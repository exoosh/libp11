/*
 * Copyright (C) 2018 Anderson Toshiyuki Sasaki
 * Copyright (c) 2018 Red Hat, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* this code extensively uses deprecated features, so warnings are useless */
#define OPENSSL_SUPPRESS_DEPRECATED

#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/engine.h>
#include <openssl/conf.h>

static void display_openssl_errors(int l)
{
	const char *file;
	char buf[120];
	int e, line;

	if (ERR_peek_error() == 0)
		return;
	fprintf(stderr, "At main.c:%d:\n", l);

	while ((e = ERR_get_error_line(&file, &line))) {
		ERR_error_string(e, buf);
		fprintf(stderr, "- SSL %s: %s:%d\n", buf, file, line);
	}
}

int main(int argc, char **argv)
{
	EVP_PKEY *private_key, *public_key;
	EVP_PKEY_CTX *pkey_ctx;
	EVP_MD_CTX *md_ctx;

	const EVP_MD *digest_algo;

	char *private_key_name, *public_key_name;

	unsigned char sig[4096];
	size_t sig_len;

	unsigned char md[128];
	size_t md_len;
	unsigned digest_len;

	char *key_pass = NULL;
	const char *module_path, *efile;

	ENGINE *e;

	int ret;

	if (argc < 5) {
		fprintf(stderr, "usage: %s [PIN] [CONF] [private key URL] [public key URL] [module]\n", argv[0]);
		fprintf(stderr, "\n");
		exit(1);
	}

	key_pass = argv[1];
	efile = argv[2];
	private_key_name = argv[3];
	public_key_name = argv[4];
	module_path = argv[5];

	ret = CONF_modules_load_file(efile, "engines", 0);
	if (ret <= 0) {
		fprintf(stderr, "cannot load %s\n", efile);
		display_openssl_errors(__LINE__);
		exit(1);
	}

	ENGINE_add_conf_module();
#if OPENSSL_VERSION_NUMBER>=0x10100000
	OPENSSL_init_crypto(OPENSSL_INIT_ADD_ALL_CIPHERS \
		| OPENSSL_INIT_ADD_ALL_DIGESTS \
		| OPENSSL_INIT_LOAD_CONFIG, NULL);
#else
	OpenSSL_add_all_algorithms();
	OpenSSL_add_all_digests();
	ERR_load_crypto_strings();
#endif
	ERR_clear_error();

	ENGINE_load_builtin_engines();
	e = ENGINE_by_id("pkcs11");
	if (e == NULL) {
		display_openssl_errors(__LINE__);
		exit(1);
	}

	if (!ENGINE_ctrl_cmd_string(e, "DEBUG_LEVEL", "7", 0)) {
		display_openssl_errors(__LINE__);
		exit(1);
	}

	if (!ENGINE_ctrl_cmd_string(e, "MODULE_PATH", module_path, 0)) {
		display_openssl_errors(__LINE__);
		exit(1);
	}

	if (!ENGINE_init(e)) {
		display_openssl_errors(__LINE__);
		exit(1);
	}
	/*
	 * ENGINE_init() returned a functional reference, so free the structural
	 * reference from ENGINE_by_id().
	 */
	ENGINE_free(e);

	if (key_pass && !ENGINE_ctrl_cmd_string(e, "PIN", key_pass, 0)) {
		display_openssl_errors(__LINE__);
		exit(1);
	}

	private_key = ENGINE_load_private_key(e, private_key_name, NULL, NULL);
	if (private_key == NULL) {
		fprintf(stderr, "cannot load: %s\n", private_key_name);
		display_openssl_errors(__LINE__);
		exit(1);
	}

	public_key = ENGINE_load_public_key(e, public_key_name, NULL, NULL);
	if (public_key == NULL) {
		fprintf(stderr, "cannot load: %s\n", public_key_name);
		display_openssl_errors(__LINE__);
		exit(1);
	}

	digest_algo = EVP_get_digestbyname("sha256");

#define TEST_DATA "test data"

	md_ctx = EVP_MD_CTX_create();
	if (EVP_DigestInit(md_ctx, digest_algo) <= 0) {
		display_openssl_errors(__LINE__);
		exit(1);
	}

	if (EVP_DigestUpdate(md_ctx, TEST_DATA, sizeof(TEST_DATA)) <= 0) {
		display_openssl_errors(__LINE__);
		exit(1);
	}

	digest_len = sizeof(md);
	if (EVP_DigestFinal(md_ctx, md, &digest_len) <= 0) {
		display_openssl_errors(__LINE__);
		exit(1);
	}
	md_len = digest_len;

	EVP_MD_CTX_destroy(md_ctx);

	/* Sign the hash */
	pkey_ctx = EVP_PKEY_CTX_new(private_key, e);

	if (pkey_ctx == NULL) {
		fprintf(stderr, "Could not create context\n");
		display_openssl_errors(__LINE__);
		exit(1);
	}

	if (EVP_PKEY_sign_init(pkey_ctx) <= 0) {
		fprintf(stderr, "Could not init signature\n");
		display_openssl_errors(__LINE__);
		exit(1);
	}

	if (EVP_PKEY_CTX_set_rsa_padding(pkey_ctx, RSA_PKCS1_PSS_PADDING) <= 0) {
		fprintf(stderr, "Could not set padding\n");
		display_openssl_errors(__LINE__);
		exit(1);
	}

	if (EVP_PKEY_CTX_set_signature_md(pkey_ctx, digest_algo) <= 0) {
		fprintf(stderr, "Could not set message digest algorithm\n");
		display_openssl_errors(__LINE__);
		exit(1);
	}

	sig_len = sizeof(sig);
	if (EVP_PKEY_sign(pkey_ctx, sig, &sig_len, md,
			EVP_MD_size(digest_algo)) <= 0) {
		display_openssl_errors(__LINE__);
		exit(1);
	}

	EVP_PKEY_CTX_free(pkey_ctx);
	EVP_PKEY_free(private_key);

	printf("Signature created\n");

	pkey_ctx = EVP_PKEY_CTX_new(public_key, e);

	if (pkey_ctx == NULL) {
		fprintf(stderr, "Could not create context\n");
		display_openssl_errors(__LINE__);
		exit(1);
	}

	if (EVP_PKEY_verify_init(pkey_ctx) <= 0) {
		fprintf(stderr, "Could not init verify\n");
		display_openssl_errors(__LINE__);
		exit(1);
	}

	if (EVP_PKEY_CTX_set_rsa_padding(pkey_ctx, RSA_PKCS1_PSS_PADDING) <= 0) {
		fprintf(stderr, "Could not set padding\n");
		display_openssl_errors(__LINE__);
		exit(1);
	}

	if (EVP_PKEY_CTX_set_signature_md(pkey_ctx, digest_algo) <= 0) {
		fprintf(stderr, "Could not set message digest algorithm\n");
		display_openssl_errors(__LINE__);
		exit(1);
	}

	ret = EVP_PKEY_verify(pkey_ctx, sig, sig_len, md, md_len);
	if (ret < 0) {
		display_openssl_errors(__LINE__);
		exit(1);
	}

	EVP_PKEY_CTX_free(pkey_ctx);
	EVP_PKEY_free(public_key);

	if (ret == 1) {
		printf("Signature verified\n");
	}
	else {
		printf("Verification failed\n");
		display_openssl_errors(__LINE__);
		exit(1);
	}

	/* Free the functional reference from ENGINE_init */
	ENGINE_finish(e);
	CONF_modules_unload(1);
	return 0;
}

/* vim: set noexpandtab: */
