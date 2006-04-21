/*
 * Copyright (c) 2005 - 2006 Kungliga Tekniska H�gskolan
 * (Royal Institute of Technology, Stockholm, Sweden). 
 * All rights reserved. 
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 *
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution. 
 *
 * 3. Neither the name of the Institute nor the names of its contributors 
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE 
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS 
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE. 
 */

#include "hx_locl.h"
RCSID("$Id$");

struct ks_file {
    hx509_certs certs;
};

struct header {
    char *header;
    char *value;
    struct header *next;
};

static int
add_headers(struct header **headers, const char *header, const char *value)
{
    struct header *h;
    h = calloc(1, sizeof(*h));
    if (h == NULL)
	return ENOMEM;
    h->header = strdup(header);
    if (h->header == NULL) {
	free(h);
	return ENOMEM;
    }
    h->value = strdup(value);
    if (h->value == NULL) {
	free(h->header);
	free(h);
	return ENOMEM;
    }

    h->next = *headers;
    *headers = h;

    return 0;
}

static void
free_headers(struct header *headers)
{
    struct header *h;
    while (headers) {
	h = headers;
	headers = headers->next;
	free(h->header);
	free(h->value);
	free(h);
    }
}

static const char *
find_header(const struct header *headers, const char *header)
{
    while(headers) {
	if (strcmp(header, headers->header) == 0)
	    return headers->value;
	headers = headers->next;
    }
    return NULL;
}

/*
 *
 */

static int
parse_certificate(hx509_context context, struct hx509_collector *c, 
		  const struct header *headers,
		  const void *data, size_t len)
{
    hx509_cert cert;
    Certificate t;
    size_t size;
    int ret;

    ret = decode_Certificate(data, len, &t, &size);
    if (ret)
	return ret;

    ret = hx509_cert_init(context, &t, &cert);
    free_Certificate(&t);
    if (ret)
	return ret;

    ret = _hx509_collector_certs_add(context, c, cert);
    hx509_cert_free(cert);
    return ret;
}

static int
parse_rsa_private_key(hx509_context context, struct hx509_collector *c, 
		      const struct header *headers,
		      const void *data, size_t len)
{
    heim_octet_string keydata;
    int ret;
    const char *enc;

    enc = find_header(headers, "Proc-Type");
    if (enc) {
	const char *dek;
	char *type, *iv;
	ssize_t ssize, size;
	void *ivdata;
	const EVP_CIPHER *cipher;
	void *key = "foobar";
	size_t keylen = 6;
	
	if (strcmp(enc, "4,ENCRYPTED") != 0)
	    return EINVAL;

	dek = find_header(headers, "DEK-Info");
	if (dek == NULL)
	    return EINVAL;

	type = strdup(dek);
	if (type == NULL)
	    return ENOMEM;

	iv = strchr(type, ',');
	if (iv)
	    *iv++ = '\0';

	/* printf("type: %s iv: %s\n", type, iv); */
	/* XXX handle use EVP_BytesToKey() and decrypt */

	size = strlen(iv);
	ivdata = malloc(size);
	if (ivdata == NULL) {
	    free(type);
	    return ENOMEM;
	}

	cipher = EVP_aes_256_cbc();

#define PKCS5_SALT_LEN 8

	ssize = hex_decode(iv, ivdata, size);
	if (ssize < 0 || ssize < PKCS5_SALT_LEN || ssize < EVP_CIPHER_iv_length(cipher)) {
	    free(ivdata);
	    free(type);
	    return EINVAL;
	}
	
	ret = EVP_BytesToKey(cipher, EVP_md5(), ivdata, key, keylen,
			     1, key, NULL);
	free(type);
	if (ret != 1) {
	    free(ivdata);
	    return EINVAL;
	}

	keydata.data = malloc(len);
	keydata.length = len;

	{
	    EVP_CIPHER_CTX ctx;
	    EVP_CIPHER_CTX_init(&ctx);
	    EVP_CipherInit_ex(&ctx, cipher, NULL, key, ivdata, 0);
	    EVP_Cipher(&ctx, keydata.data, data, len);
	    EVP_CIPHER_CTX_cleanup(&ctx);
	}	
	free(ivdata);

    } else {
	keydata.data = rk_UNCONST(data);
	keydata.length = len;
    }

    ret = _hx509_collector_private_key_add(c,
					   hx509_signature_rsa(),
					   NULL,
					   &keydata,
					   NULL);
    if (keydata.data != data)
	free(keydata.data);

    return ret;
}


struct pem_formats {
    const char *name;
    int (*func)(hx509_context, struct hx509_collector *, 
		const struct header *, const void *, size_t);
} formats[] = {
    { "CERTIFICATE", parse_certificate },
    { "RSA PRIVATE KEY", parse_rsa_private_key }
};


static int
parse_pem_file(hx509_context context, 
	       const char *fn,
	       struct hx509_collector *c,
	       int *found_data)
{
    struct header *headers = NULL;
    char *type = NULL;
    void *data = NULL;
    size_t len = 0;
    char buf[1024];
    int i, ret;
    FILE *f;


    enum { BEFORE, SEARCHHEADER, INHEADER, INDATA, DONE } where;

    where = BEFORE;
    *found_data = 0;

    if ((f = fopen(fn, "r")) == NULL)
	return ENOENT;

    ret = 0;

    while (fgets(buf, sizeof(buf), f) != NULL) {
	char *p;

	i = strcspn(buf, "\n");
	if (buf[i] == '\n') {
	    buf[i] = '\0';
	    if (i > 0)
		i--;
	}
	if (buf[i] == '\r') {
	    buf[i] = '\0';
	    if (i > 0)
		i--;
	}
	    
	switch (where) {
	case BEFORE:
	    if (strncmp("-----BEGIN ", buf, 11) == 0) {
		type = strdup(buf + 11);
		if (type == NULL)
		    break;
		*found_data = 1;
		where = SEARCHHEADER;
	    }
	    break;
	case SEARCHHEADER:
	    p = strchr(buf, ':');
	    if (p == NULL) {
		where = INDATA;
		goto indata;
	    }
	    /* FALLTHOUGH */
	case INHEADER:
	    if (buf[0] == '\0') {
		where = INDATA;
		break;
	    }
	    p = strchr(buf, ':');
	    if (p) {
		*p++ = '\0';
		while (isspace((int)*p))
		    p++;
		add_headers(&headers, buf, p);
	    }
	    break;
	case INDATA:
	indata:
	    if (strncmp("-----END ", buf, 9) == 0) {
		where = DONE;
		break;
	    }

	    p = malloc(i);
	    i = base64_decode(buf, p);
	    
	    data = erealloc(data, len + i);
	    memcpy(((char *)data) + len, p, i);
	    free(p);
	    len += i;
	    break;
	case DONE:
	    abort();
	}

	if (where == DONE) {
	    int i;

	    ret = EINVAL;
	    for (i = 0; i < sizeof(formats)/sizeof(formats[0]); i++) {
		const char *p = formats[i].name;
		if (strncmp(type, p, strlen(p)) == 0)
		    ret = (*formats[i].func)(context, c, headers, data, len);
	    }
	    free(data);
	    data = NULL;
	    free(type);
	    type = NULL;
	    where = BEFORE;
	    free_headers(headers);
	    headers = NULL;
	    if (ret)
		break;
	}
    }

    fclose(f);

    if (where != BEFORE)
	ret = EINVAL;
    if (data)
	free(data);
    if (type)
	free(type);
    if (headers)
	free_headers(headers);

    return ret;
}

/*
 *
 */

static int
file_init(hx509_context context,
	  hx509_certs certs, void **data, int flags, 
	  const char *residue, hx509_lock lock)
{
    char *files = NULL, *p, *pnext;
    int ret;
    struct ks_file *f;
    struct hx509_collector *c;

    *data = NULL;

    if (lock == NULL)
	lock = _hx509_empty_lock;

    c = _hx509_collector_alloc(context, lock);
    if (c == NULL)
	return ENOMEM;

    f = calloc(1, sizeof(*f));
    if (f == NULL) {
	ret = ENOMEM;
	goto out;
    }

    files = strdup(residue);
    if (files == NULL) {
	ret = ENOMEM;
	goto out;
    }

    for (p = files; p != NULL; p = pnext) {
	int found_data;

	pnext = strchr(p, ',');
	if (pnext)
	    *pnext++ = '\0';
	
	ret = parse_pem_file(context, p, c, &found_data);
	if (ret)
	    goto out;

	if (!found_data) {
	    size_t length;
	    void *data;
	    int ret;

	    ret = _hx509_map_file(p, &data, &length, NULL);
	    if (ret)
		goto out;

	    ret = parse_certificate(context, c, NULL, data, length);
	    _hx509_unmap_file(data, length);
	    if (ret)
		goto out;
	}
    }

    ret = _hx509_collector_collect(context, c, &f->certs);
    if (ret == 0)
	*data = f;
    else
	free(f);
out:
    free(files);

    _hx509_collector_free(c);

    return ret;
}

static int
file_free(hx509_certs certs, void *data)
{
    struct ks_file *f = data;
    hx509_certs_free(&f->certs);
    free(f);
    return 0;
}



static int 
file_iter_start(hx509_context context,
		hx509_certs certs, void *data, void **cursor)
{
    struct ks_file *f = data;
    return hx509_certs_start_seq(context, f->certs, cursor);
}

static int
file_iter(hx509_context context,
	  hx509_certs certs, void *data, void *iter, hx509_cert *cert)
{
    struct ks_file *f = data;
    return hx509_certs_next_cert(context, f->certs, iter, cert);
}

static int
file_iter_end(hx509_context context,
	      hx509_certs certs,
	      void *data,
	      void *cursor)
{
    struct ks_file *f = data;
    return hx509_certs_end_seq(context, f->certs, cursor);
}


static struct hx509_keyset_ops keyset_file = {
    "FILE",
    0,
    file_init,
    file_free,
    NULL,
    NULL,
    file_iter_start,
    file_iter,
    file_iter_end
};

void
_hx509_ks_file_register(hx509_context context)
{
    _hx509_ks_register(context, &keyset_file);
}
