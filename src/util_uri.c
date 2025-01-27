/*
 * Copyright (c) 2001 Markus Friedl
 * Copyright (c) 2002 Juha Yrjölä
 * Copyright (c) 2002 Olaf Kirch
 * Copyright (c) 2003 Kevin Stefanik
 * Copyright (c) 2016-2025 Michał Trojnara <Michal.Trojnara@stunnel.org>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "util.h"
#include <stdio.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#define strncasecmp _strnicmp
#endif

/* switch to legacy call if get0 variant is not available */
#ifndef HAVE_X509_GET0_NOTBEFORE
#	define X509_get0_notBefore X509_get_notBefore
#endif

#ifndef HAVE_X509_GET0_NOTAFTER
#	define X509_get0_notAfter X509_get_notAfter
#endif

/******************************************************************************/
/* helpers                                                                    */
/******************************************************************************/

static char *dump_hex(unsigned char *val, const size_t len)
{
	int i, j = 0, size = 2 * len + 1;
	char *hexbuf = OPENSSL_malloc((size_t)size);

	if (!hexbuf)
		return NULL;
	for (i = 0; i < len; i++) {
#ifdef WIN32
		j += sprintf_s(hexbuf + j, size - j, "%02X", val[i]);
#else
		j += sprintf(hexbuf + j, "%02X", val[i]);
#endif /* WIN32 */
	}
	return hexbuf;
}

static char *dump_expiry(const PKCS11_CERT *cert)
{
	BIO *bio;
	const ASN1_TIME *exp;
	char *buf = NULL, *result;
	int len = 0;

	if (!cert || !cert->x509 || !(exp = X509_get0_notAfter(cert->x509)))
		return strdup("No expiry information available");

	if ((bio = BIO_new(BIO_s_mem())) == NULL)
		return NULL; /* Memory allocation failure */

	/* Print the expiry date into the BIO */
	if (ASN1_TIME_print(bio, exp) <= 0) {
		BIO_free(bio);
		return NULL; /* Failed to format expiry date */
	}
	/* Retrieve the data from the BIO */
	len = BIO_get_mem_data(bio, &buf);

	result = OPENSSL_strndup((const char *)buf, (size_t)len);
	BIO_free(bio);

	return result;
}

static int hex_to_bin(ENGINE_CTX *ctx,
		const char *in, char *out, size_t *outlen)
{
	size_t left, count = 0;

	if (!in || *in == '\0') {
		*outlen = 0;
		return 1;
	}

	left = *outlen;

	while (*in != '\0') {
		int byte = 0, nybbles = 2;

		while (nybbles-- && *in && *in != ':') {
			char c;
			byte <<= 4;
			c = *in++;
			if ('0' <= c && c <= '9')
				c -= '0';
			else if ('a' <= c && c <= 'f')
				c = c - 'a' + 10;
			else if ('A' <= c && c <= 'F')
				c = c - 'A' + 10;
			else {
				ctx_log(ctx, LOG_ERR,
					"hex_to_bin(): invalid char '%c' in hex string\n",
					c);
				*outlen = 0;
				return 0;
			}
			byte |= c;
		}
		if (*in == ':')
			in++;
		if (left == 0) {
			ctx_log(ctx, LOG_ERR, "hex_to_bin(): hex string too long\n");
			*outlen = 0;
			return 0;
		}
		out[count++] = byte;
		left--;
	}

	*outlen = count;
	return 1;
}

/******************************************************************************/
/* PIN handling                                                               */
/******************************************************************************/

/* Free PIN storage in secure way. */
static void ctx_destroy_pin(ENGINE_CTX *ctx)
{
	if (ctx->pin) {
		OPENSSL_cleanse(ctx->pin, ctx->pin_length);
		OPENSSL_free(ctx->pin);
		ctx->pin = NULL;
		ctx->pin_length = 0;
		ctx->forced_pin = 0;
	}
}

/* Get the PIN via asking user interface. The supplied call-back data are
 * passed to the user interface implemented by an application. Only the
 * application knows how to interpret the call-back data.
 * A (strdup'ed) copy of the PIN code will be stored in the pin variable. */
static int ctx_get_pin(ENGINE_CTX *ctx, const char *token_label, UI_METHOD *ui_method, void *callback_data)
{
	UI *ui;
	char *prompt;

	/* call ui to ask for a pin */
	ui = UI_new_method(ui_method);
	if (!ui) {
		ctx_log(ctx, LOG_ERR, "UI_new failed\n");
		return 0;
	}
	if (callback_data)
		UI_add_user_data(ui, callback_data);

	ctx_destroy_pin(ctx);
	ctx->pin = OPENSSL_malloc(MAX_PIN_LENGTH+1);
	if (!ctx->pin)
		return 0;
	memset(ctx->pin, 0, MAX_PIN_LENGTH+1);
	ctx->pin_length = MAX_PIN_LENGTH;
	prompt = UI_construct_prompt(ui, "PKCS#11 token PIN", token_label);
	if (!prompt) {
		return 0;
	}
	if (UI_dup_input_string(ui, prompt,
			UI_INPUT_FLAG_DEFAULT_PWD, ctx->pin, 4, MAX_PIN_LENGTH) <= 0) {
		ctx_log(ctx, LOG_ERR, "UI_dup_input_string failed\n");
		UI_free(ui);
		OPENSSL_free(prompt);
		return 0;
	}
	OPENSSL_free(prompt);

	if (UI_process(ui)) {
		ctx_log(ctx, LOG_ERR, "UI_process failed\n");
		UI_free(ui);
		return 0;
	}
	UI_free(ui);
	return 1;
}

/* Return 1 if the user has already logged in */
static int slot_logged_in(ENGINE_CTX *ctx, PKCS11_SLOT *slot) {
	int logged_in = 0;

	/* Check if already logged in to avoid resetting state */
	if (PKCS11_is_logged_in(slot, 0, &logged_in) != 0) {
		ctx_log(ctx, LOG_WARNING, "Unable to check if already logged in\n");
		return 0;
	}
	return logged_in;
}

/*
 * Log-into the token if necessary.
 *
 * @slot is PKCS11 slot to log in
 * @tok is PKCS11 token to log in (??? could be derived as @slot->token)
 * @ui_method is OpenSSL user interface which is used to ask for a password
 * @callback_data are application data to the user interface
 * @return 1 on success, 0 on error.
 */
static int ctx_login(ENGINE_CTX *ctx, PKCS11_SLOT *slot, PKCS11_TOKEN *tok,
		UI_METHOD *ui_method, void *callback_data)
{
	if (!(ctx->force_login || tok->loginRequired) || slot_logged_in(ctx, slot))
		return 1;

	/* If the token has a secure login (i.e., an external keypad),
	 * then use a NULL PIN. Otherwise, obtain a new PIN if needed. */
	if (tok->secureLogin && !ctx->forced_pin) {
		/* Free the PIN if it has already been
		 * assigned (i.e, cached by ctx_get_pin) */
		ctx_destroy_pin(ctx);
	} else if (!ctx->pin) {
		ctx->pin = OPENSSL_malloc(MAX_PIN_LENGTH+1);
		ctx->pin_length = MAX_PIN_LENGTH;
		if (ctx->pin == NULL) {
			ctx_log(ctx, LOG_ERR, "Could not allocate memory for PIN\n");
			return 0;
		}
		memset(ctx->pin, 0, MAX_PIN_LENGTH+1);
		if (!ctx_get_pin(ctx, tok->label, ui_method, callback_data)) {
			ctx_destroy_pin(ctx);
			ctx_log(ctx, LOG_ERR, "No PIN code was entered\n");
			return 0;
		}
	}

	/* Now login in with the (possibly NULL) PIN */
	if (PKCS11_login(slot, 0, ctx->pin)) {
		/* Login failed, so free the PIN if present */
		ctx_destroy_pin(ctx);
		ctx_log(ctx, LOG_ERR, "Login failed\n");
		return 0;
	}
	return 1;
}

/******************************************************************************/
/* URI parsing                                                                */
/******************************************************************************/

/* parse string containing slot and id information */
static int parse_slot_id_string(ENGINE_CTX *ctx,
		const char *slot_id, int *slot,
		char *id, size_t *id_len, char **label)
{
	int n, i;

	/* support for several formats */
#define HEXDIGITS "01234567890ABCDEFabcdef"
#define DIGITS "0123456789"

	/* first: pure hex number (id, slot is undefined) */
	if (strspn(slot_id, HEXDIGITS) == strlen(slot_id)) {
		/* ah, easiest case: only hex. */
		if ((strlen(slot_id) + 1) / 2 > *id_len) {
			ctx_log(ctx, LOG_ERR, "ID string too long!\n");
			return 0;
		}
		*slot = -1;
		return hex_to_bin(ctx, slot_id, id, id_len);
	}

	/* second: slot:id. slot is an digital int. */
	if (sscanf(slot_id, "%d", &n) == 1) {
		i = strspn(slot_id, DIGITS);

		if (slot_id[i] != ':') {
			ctx_log(ctx, LOG_ERR, "Could not parse string!\n");
			return 0;
		}
		i++;
		if (slot_id[i] == 0) {
			*slot = n;
			*id_len = 0;
			return 1;
		}
		if (strspn(slot_id + i, HEXDIGITS) + i != strlen(slot_id)) {
			ctx_log(ctx, LOG_ERR, "Could not parse string!\n");
			return 0;
		}
		/* ah, rest is hex */
		if ((strlen(slot_id) - i + 1) / 2 > *id_len) {
			ctx_log(ctx, LOG_ERR, "ID string too long!\n");
			return 0;
		}
		*slot = n;
		return hex_to_bin(ctx, slot_id + i, id, id_len);
	}

	/* third: id_<id>, slot is undefined */
	if (strncmp(slot_id, "id_", 3) == 0) {
		if (strspn(slot_id + 3, HEXDIGITS) + 3 != strlen(slot_id)) {
			ctx_log(ctx, LOG_ERR, "Could not parse string!\n");
			return 0;
		}
		/* ah, rest is hex */
		if ((strlen(slot_id) - 3 + 1) / 2 > *id_len) {
			ctx_log(ctx, LOG_ERR, "ID string too long!\n");
			return 0;
		}
		*slot = -1;
		return hex_to_bin(ctx, slot_id + 3, id, id_len);
	}

	/* label_<label>, slot is undefined */
	if (strncmp(slot_id, "label_", 6) == 0) {
		*slot = -1;
		*label = OPENSSL_strdup(slot_id + 6);
		*id_len = 0;
		return *label != NULL;
	}

	/* last try: it has to be slot_<slot> and then "-id_<cert>" */

	if (strncmp(slot_id, "slot_", 5) != 0) {
		ctx_log(ctx, LOG_ERR, "Format not recognized!\n");
		return 0;
	}

	/* slot is an digital int. */
	if (sscanf(slot_id + 5, "%d", &n) != 1) {
		ctx_log(ctx, LOG_ERR, "Could not decode slot number!\n");
		return 0;
	}

	i = strspn(slot_id + 5, DIGITS);

	if (slot_id[i + 5] == 0) {
		*slot = n;
		*id_len = 0;
		return 1;
	}

	if (slot_id[i + 5] != '-') {
		ctx_log(ctx, LOG_ERR, "Could not parse string!\n");
		return 0;
	}

	i = 5 + i + 1;

	/* now followed by "id_" */
	if (strncmp(slot_id + i, "id_", 3) == 0) {
		if (strspn(slot_id + i + 3, HEXDIGITS) + 3 + i != strlen(slot_id)) {
			ctx_log(ctx, LOG_ERR, "Could not parse string!\n");
			return 0;
		}
		/* ah, rest is hex */
		if ((strlen(slot_id) - i - 3 + 1) / 2 > *id_len) {
			ctx_log(ctx, LOG_ERR, "ID string too long!\n");
			return 0;
		}
		*slot = n;
		return hex_to_bin(ctx, slot_id + i + 3, id, id_len);
	}

	/* ... or "label_" */
	if (strncmp(slot_id + i, "label_", 6) == 0) {
		*slot = n;
		*label = OPENSSL_strdup(slot_id + i + 6);
		*id_len = 0;
		return *label != NULL;
	}

	ctx_log(ctx, LOG_ERR, "Could not parse string!\n");
	return 0;
}

static int parse_uri_attr_len(ENGINE_CTX *ctx,
		const char *attr, int attrlen, char *field,
		size_t *field_len)
{
	size_t max = *field_len, outlen = 0;
	int ret = 1;

	while (ret && attrlen && outlen < max) {
		if (*attr == '%') {
			if (attrlen < 3) {
				ret = 0;
			} else {
				char tmp[3];
				size_t l = 1;

				tmp[0] = attr[1];
				tmp[1] = attr[2];
				tmp[2] = 0;
				ret = hex_to_bin(ctx, tmp, &field[outlen++], &l);
				attrlen -= 3;
				attr += 3;
			}

		} else {
			field[outlen++] = *(attr++);
			attrlen--;
		}
	}
	if (attrlen && outlen == max)
		ret = 0;

	if (ret)
		*field_len = outlen;

	return ret;
}

static int parse_uri_attr(ENGINE_CTX *ctx,
		const char *attr, int attrlen, char **field)
{
	int ret = 1;
	size_t outlen = attrlen + 1;
	char *out = OPENSSL_malloc(outlen);

	if (!out)
		return 0;

	ret = parse_uri_attr_len(ctx, attr, attrlen, out, &outlen);

	if (ret) {
		out[outlen] = 0;
		*field = out;
	} else {
		OPENSSL_free(out);
	}

	return ret;
}


static int read_from_file(ENGINE_CTX *ctx,
	const char *path, char *field, size_t *field_len)
{
	BIO *fp;
	char *txt;

	fp = BIO_new_file(path, "r");
	if (!fp) {
		ctx_log(ctx, LOG_ERR, "Could not open file %s\n", path);
		return 0;
	}

	txt = OPENSSL_malloc(*field_len + 1); /* + 1 for '\0' */
	if (BIO_gets(fp, txt, *field_len + 1) > 0) {
		memcpy(field, txt, *field_len);
		*field_len = strlen(txt);
	} else {
		*field_len = 0;
	}
	OPENSSL_free(txt);

	BIO_free(fp);
	return 1;
}

static int parse_pin_source(ENGINE_CTX *ctx,
		const char *attr, int attrlen, char *field,
		size_t *field_len)
{
	char *val;
	int ret = 1;

	if (!parse_uri_attr(ctx, attr, attrlen, &val)) {
		return 0;
	}

	if (!strncasecmp((const char *)val, "file:", 5)) {
		ret = read_from_file(ctx, (const char *)(val + 5), field, field_len);
	} else if (*val == '|') {
		ret = 0;
		ctx_log(ctx, LOG_ERR, "Unsupported pin-source syntax\n");
	/* 'pin-source=/foo/bar' is commonly used */
	} else {
		ret = read_from_file(ctx, (const char *)val, field, field_len);
	}
	OPENSSL_free(val);

	return ret;
}

static int parse_pkcs11_uri(ENGINE_CTX *ctx,
		const char *uri, PKCS11_TOKEN **p_tok,
		char *id, size_t *id_len, char *pin, size_t *pin_len,
		char **label)
{
	PKCS11_TOKEN *tok;
	char *newlabel = NULL;
	const char *end, *p;
	int rv = 1, id_set = 0, pin_set = 0;

	tok = OPENSSL_malloc(sizeof(PKCS11_TOKEN));
	if (!tok) {
		ctx_log(ctx, LOG_ERR, "Could not allocate memory for token info\n");
		return 0;
	}
	memset(tok, 0, sizeof(PKCS11_TOKEN));

	/* We are only ever invoked if the string starts with 'pkcs11:' */
	end = uri + 6;
	while (rv && end[0] && end[1]) {
		p = end + 1;
		end = strpbrk(p, ";?&");
		if (!end)
			end = p + strlen(p);

		if (!strncmp(p, "model=", 6)) {
			p += 6;
			rv = parse_uri_attr(ctx, p, end - p, &tok->model);
		} else if (!strncmp(p, "manufacturer=", 13)) {
			p += 13;
			rv = parse_uri_attr(ctx, p, end - p, &tok->manufacturer);
		} else if (!strncmp(p, "token=", 6)) {
			p += 6;
			rv = parse_uri_attr(ctx, p, end - p, &tok->label);
		} else if (!strncmp(p, "serial=", 7)) {
			p += 7;
			rv = parse_uri_attr(ctx, p, end - p, &tok->serialnr);
		} else if (!strncmp(p, "object=", 7)) {
			p += 7;
			rv = parse_uri_attr(ctx, p, end - p, &newlabel);
		} else if (!strncmp(p, "id=", 3)) {
			p += 3;
			rv = parse_uri_attr_len(ctx, p, end - p, id, id_len);
			id_set = 1;
		} else if (!strncmp(p, "pin-value=", 10)) {
			p += 10;
			rv = pin_set ? 0 : parse_uri_attr_len(ctx, p, end - p, pin, pin_len);
			pin_set = 1;
		} else if (!strncmp(p, "pin-source=", 11)) {
			p += 11;
			rv = pin_set ? 0 : parse_pin_source(ctx, p, end - p, pin, pin_len);
			pin_set = 1;
		} else if (!strncmp(p, "type=", 5) || !strncmp(p, "object-type=", 12)) {
			p = strchr(p, '=') + 1;

			if ((end - p == 4 && !strncmp(p, "cert", 4)) ||
					(end - p == 6 && !strncmp(p, "public", 6)) ||
					(end - p == 7 && !strncmp(p, "private", 7))) {
				/* Actually, just ignore it */
			} else {
				ctx_log(ctx, LOG_ERR, "Unknown object type\n");
				rv = 0;
			}
		} else {
			rv = 0;
		}
	}

	if (!id_set)
		*id_len = 0;
	if (!pin_set)
		*pin_len = 0;

	if (rv) {
		*label = newlabel;
		*p_tok = tok;
	} else {
		OPENSSL_free(tok);
		tok = NULL;
		OPENSSL_free(newlabel);
	}

	return rv;
}

/******************************************************************************/
/* Utilities common to public, private key and certificate handling           */
/******************************************************************************/

static void *ctx_try_load_object(ENGINE_CTX *ctx,
		const char *object_typestr,
		void *(*match_func)(ENGINE_CTX *, PKCS11_TOKEN *,
				const char *, size_t, const char *),
		const char *object_uri, const int login,
		UI_METHOD *ui_method, void *callback_data)
{
	PKCS11_SLOT *slot;
	PKCS11_SLOT *found_slot = NULL, **matched_slots = NULL;
	PKCS11_TOKEN *match_tok = NULL;
	unsigned int n, m;
	char *obj_id = NULL, *hexbuf = NULL;
	size_t obj_id_len = 0;
	char *obj_label = NULL;
	char tmp_pin[MAX_PIN_LENGTH+1];
	size_t tmp_pin_len = MAX_PIN_LENGTH;
	int slot_nr = -1;
	char flags[64];
	size_t matched_count = 0;
	void *object = NULL;

	if (object_uri && *object_uri) {
		obj_id_len = strlen(object_uri) + 1;
		obj_id = OPENSSL_malloc(obj_id_len);
		if (!obj_id) {
			ctx_log(ctx, LOG_ERR, "Could not allocate memory for ID\n");
			goto cleanup;
		}
		if (!strncasecmp(object_uri, "pkcs11:", 7)) {
			n = parse_pkcs11_uri(ctx, object_uri, &match_tok,
				obj_id, &obj_id_len, tmp_pin, &tmp_pin_len, &obj_label);
			if (!n) {
				ctx_log(ctx, LOG_ERR,
					"The %s ID is not a valid PKCS#11 URI\n"
					"The PKCS#11 URI format is defined by RFC7512\n",
					object_typestr);
				ENGerr(ENG_F_CTX_LOAD_OBJECT, ENG_R_INVALID_ID);
				goto cleanup;
			}
			if (tmp_pin_len > 0 && tmp_pin[0] != 0) {
				tmp_pin[tmp_pin_len] = 0;
				if (!ctx_ctrl_set_pin(ctx, tmp_pin)) {
					goto cleanup;
				}
			}
			if (obj_id_len != 0) {
				hexbuf = dump_hex((unsigned char *)obj_id, obj_id_len);
			}
			ctx_log(ctx, LOG_NOTICE, "Looking in slots for %s %s login:%s%s%s%s\n",
				object_typestr,
				login ? "with" : "without",
				hexbuf ? " id=" : "",
				hexbuf ? hexbuf : "",
				obj_label ? " label=" : "",
				obj_label ? obj_label : "");
			OPENSSL_free(hexbuf);
		} else {
			n = parse_slot_id_string(ctx, object_uri, &slot_nr,
				obj_id, &obj_id_len, &obj_label);
			if (!n) {
				ctx_log(ctx, LOG_ERR,
					"The %s ID is not a valid PKCS#11 URI\n"
					"The PKCS#11 URI format is defined by RFC7512\n"
					"The legacy ENGINE_pkcs11 ID format is also "
					"still accepted for now\n",
					object_typestr);
				ENGerr(ENG_F_CTX_LOAD_OBJECT, ENG_R_INVALID_ID);
				goto cleanup;
			}
			if (obj_id_len != 0) {
				hexbuf = dump_hex((unsigned char *)obj_id, obj_id_len);
			}
			ctx_log(ctx, LOG_NOTICE, "Looking in slot %d for %s %s login:%s%s%s%s\n",
				slot_nr, object_typestr,
				login ? "with" : "without",
				hexbuf ? " id=" : "",
				hexbuf ? hexbuf : "",
				obj_label ? " label=" : "",
				obj_label ? obj_label : "");
			OPENSSL_free(hexbuf);
		}
	}

	matched_slots = (PKCS11_SLOT **)calloc(ctx->slot_count,
		sizeof(PKCS11_SLOT *));
	if (!matched_slots) {
		ctx_log(ctx, LOG_ERR, "Could not allocate memory for slots\n");
		goto cleanup;
	}

	for (n = 0; n < ctx->slot_count; n++) {
		slot = ctx->slot_list + n;
		flags[0] = '\0';
		if (slot->token) {
			if (!slot->token->initialized)
				strcat(flags, "uninitialized, ");
			else if (!slot->token->userPinSet)
				strcat(flags, "no pin, ");
			if (slot->token->loginRequired)
				strcat(flags, "login, ");
			if (slot->token->readOnly)
				strcat(flags, "ro, ");
		} else {
			strcpy(flags, "no token, ");
		}
		if ((m = strlen(flags)) != 0) {
			flags[m - 2] = '\0';
		}

		if (slot_nr != -1 &&
			slot_nr == (int)PKCS11_get_slotid_from_slot(slot)) {
			found_slot = slot;
		}

		if (match_tok && slot->token &&
				(!match_tok->label ||
					!strcmp(match_tok->label, slot->token->label)) &&
				(!match_tok->manufacturer ||
					!strcmp(match_tok->manufacturer, slot->token->manufacturer)) &&
				(!match_tok->serialnr ||
					!strcmp(match_tok->serialnr, slot->token->serialnr)) &&
				(!match_tok->model ||
					!strcmp(match_tok->model, slot->token->model))) {
			found_slot = slot;
		}
		ctx_log(ctx, LOG_NOTICE, "- [%lu] %-25.25s  %-36s  (%s)\n",
			PKCS11_get_slotid_from_slot(slot),
			slot->description, flags,
			slot->token->label[0] ? slot->token->label : "no label");

		/* Ignore slots without tokens. Thales HSM (and potentially
		 * other modules) allow objects on uninitialized tokens. */
		if (found_slot && found_slot->token) {
			matched_slots[matched_count] = found_slot;
			matched_count++;
		}
		found_slot = NULL;
	}

	if (matched_count == 0) {
		if (match_tok) {
			ctx_log(ctx, LOG_ERR, "No matching token was found for %s\n",
				object_typestr);
			goto cleanup;
		}

		/* If the legacy slot ID format was used */
		if (slot_nr != -1) {
			ctx_log(ctx, LOG_ERR, "The %s was not found on slot %d\n", object_typestr, slot_nr);
			goto cleanup;
		} else {
			found_slot = PKCS11_find_token(ctx->pkcs11_ctx,
								ctx->slot_list, ctx->slot_count);
			/* Ignore slots without tokens. Thales HSM (and potentially
			 * other modules) allow objects on uninitialized tokens. */
			if (found_slot && found_slot->token) {
				matched_slots[matched_count] = found_slot;
				matched_count++;
			} else {
				ctx_log(ctx, LOG_ERR, "No tokens found\n");
				goto cleanup;
			}
		}
	}

	/* In several tokens certificates are marked as private */
	if (login) {
		/* Only try to login if a single slot matched to avoiding trying
		 * the PIN against all matching slots */

		if (matched_count == 1) {
			slot = matched_slots[0];
			if (!slot->token) {
				ctx_log(ctx, LOG_ERR, "Empty slot found:  %s\n", slot->description);
				goto cleanup; /* failed */
			}
			ctx_log(ctx, LOG_NOTICE, "Found slot:  %s\n", slot->description);
			ctx_log(ctx, LOG_NOTICE, "Found token: %s\n", slot->token->label[0]?
				slot->token->label : "no label");

			/* Only try to login if login is required */
			if (slot->token->loginRequired || ctx->force_login) {
				if (!ctx_login(ctx, slot, slot->token, ui_method, callback_data)) {
					ctx_log(ctx, LOG_ERR, "Login to token failed, returning NULL...\n");
					goto cleanup; /* failed */
				}
			}
		} else {
			/* Multiple matching slots */
			size_t init_count = 0;
			size_t uninit_count = 0;
			PKCS11_SLOT **init_slots = NULL, **uninit_slots = NULL;

			init_slots = (PKCS11_SLOT **)calloc(ctx->slot_count, sizeof(PKCS11_SLOT *));
			if (!init_slots) {
				ctx_log(ctx, LOG_ERR, "Could not allocate memory for slots\n");
				goto cleanup; /* failed */
			}
			uninit_slots = (PKCS11_SLOT **)calloc(ctx->slot_count, sizeof(PKCS11_SLOT *));
			if (!uninit_slots) {
				ctx_log(ctx, LOG_ERR, "Could not allocate memory for slots\n");
				free(init_slots);
				goto cleanup; /* failed */
			}

			for (m = 0; m < matched_count; m++) {
				slot = matched_slots[m];
				if (!slot->token) {
					ctx_log(ctx, LOG_INFO, "Empty slot found:  %s\n", slot->description);
					continue; /* skipped */
				}
				if (slot->token->initialized) {
					init_slots[init_count] = slot;
					init_count++;
				} else {
					uninit_slots[uninit_count] = slot;
					uninit_count++;
				}
			}

			/* Initialized tokens */
			if (init_count == 1) {
				slot = init_slots[0];
				ctx_log(ctx, LOG_NOTICE, "Found slot:  %s\n", slot->description);
				ctx_log(ctx, LOG_NOTICE, "Found token: %s\n", slot->token->label[0]?
					slot->token->label : "no label");

				/* Only try to login if login is required */
				if (slot->token->loginRequired || ctx->force_login) {
					if (!ctx_login(ctx, slot, slot->token, ui_method, callback_data)) {
						ctx_log(ctx, LOG_ERR, "Login to token failed, returning NULL...\n");
						free(init_slots);
						free(uninit_slots);
						goto cleanup; /* failed */
					}
				}
			} else {
				/* Multiple slots with initialized token */
				if (init_count > 1) {
					ctx_log(ctx, LOG_WARNING, "Multiple matching slots (%zu);"
						" will not try to login\n", init_count);
				}
				for (m = 0; m < init_count; m++) {
					slot = init_slots[m];
					ctx_log(ctx, LOG_WARNING, "- [%u] %s: %s\n", m + 1,
						slot->description? slot->description:
						"(no description)",
						(slot->token && slot->token->label)?
						slot->token->label: "no label");
				}
				free(init_slots);

				/* Uninitialized tokens, user PIN is unset */
				for (m = 0; m < uninit_count; m++) {
					slot = uninit_slots[m];
					ctx_log(ctx, LOG_NOTICE, "Found slot:  %s\n", slot->description);
					ctx_log(ctx, LOG_NOTICE, "Found token: %s\n", slot->token->label[0]?
						slot->token->label : "no label");
					object = match_func(ctx, slot->token, obj_id, obj_id_len, obj_label);
					if (object) {
						free(uninit_slots);
						goto cleanup; /* success */
					}
				}
				free(uninit_slots);
				goto cleanup; /* failed */
			}
		}
		object = match_func(ctx, slot->token, obj_id, obj_id_len, obj_label);

	} else {
		/* Find public object */
		for (n = 0; n < matched_count; n++) {
			slot = matched_slots[n];
			if (!slot->token) {
				ctx_log(ctx, LOG_INFO, "Empty slot found:  %s\n", slot->description);
				break;
			}
			ctx_log(ctx, LOG_NOTICE, "Found slot:  %s\n", slot->description);
			ctx_log(ctx, LOG_NOTICE, "Found token: %s\n", slot->token->label[0]?
				slot->token->label : "no label");
			object = match_func(ctx, slot->token, obj_id, obj_id_len, obj_label);
			if (object)
				break; /* success */
		}
	}

cleanup:
	/* Free the searched token data */
	if (match_tok) {
		OPENSSL_free(match_tok->model);
		OPENSSL_free(match_tok->manufacturer);
		OPENSSL_free(match_tok->serialnr);
		OPENSSL_free(match_tok->label);
		OPENSSL_free(match_tok);
	}

	if (obj_label)
		OPENSSL_free(obj_label);
	if (matched_slots)
		free(matched_slots);
	if (obj_id)
		OPENSSL_free(obj_id);
	return object;
}

static void *ctx_load_object(ENGINE_CTX *ctx,
		const char *object_typestr,
		void *(*match_func)(ENGINE_CTX *, PKCS11_TOKEN *,
				const char *, size_t, const char *),
		const char *object_uri, UI_METHOD *ui_method, void *callback_data)
{
	void *obj = NULL;

	if (!ctx->force_login) {
		ERR_clear_error();
		obj = ctx_try_load_object(ctx, object_typestr, match_func,
			object_uri, 0, ui_method, callback_data);
	}

	if (!obj) {
		/* Try again with login */
		ERR_clear_error();
		obj = ctx_try_load_object(ctx, object_typestr, match_func,
			object_uri, 1, ui_method, callback_data);
		if (!obj) {
			ctx_log(ctx, LOG_ERR, "The %s was not found at: %s\n",
				object_typestr, object_uri);
		}
	}

	return obj;
}

/******************************************************************************/
/* Certificate handling                                                       */
/******************************************************************************/

static PKCS11_CERT *cert_cmp(PKCS11_CERT *a, PKCS11_CERT *b)
{
	const ASN1_TIME *a_time, *b_time;
	int pday, psec;

	/* the best certificate exists */
	if (!a || !a->x509) {
		return b;
	}
	if (!b || !b->x509) {
		return a;
	}

	a_time = X509_get0_notAfter(a->x509);
	b_time = X509_get0_notAfter(b->x509);

	/* the best certificate expires last */
	if (ASN1_TIME_diff(&pday, &psec, a_time, b_time)) {
		if (pday < 0 || psec < 0) {
			return a;
		} else if (pday > 0 || psec > 0) {
			return b;
		}
	}

	/* deterministic tie break */
	if (X509_cmp(a->x509, b->x509) < 1) {
		return b;
	} else {
		return a;
	}
}

static void *match_cert(ENGINE_CTX *ctx, PKCS11_TOKEN *tok,
		const char *obj_id, size_t obj_id_len, const char *obj_label)
{
	PKCS11_CERT *certs, *selected_cert = NULL;
	PKCS11_CERT cert_template = {0};
	unsigned int m, cert_count;
	const char *which;
	char *hexbuf, *expiry;

	errno = 0;
	cert_template.label = obj_label ? OPENSSL_strdup(obj_label) : NULL;
	if (errno != 0) {
		ctx_log(ctx, LOG_ERR, "%s", strerror(errno));
		goto cleanup;
	}
	if (obj_id_len) {
		cert_template.id = OPENSSL_malloc(obj_id_len);
		if (!cert_template.id) {
			ctx_log(ctx, LOG_ERR, "Could not allocate memory for ID\n");
			goto cleanup;
		}
		memcpy(cert_template.id, obj_id, obj_id_len);
		cert_template.id_len = obj_id_len;
	}

	if (PKCS11_enumerate_certs_ext(tok, &cert_template, &certs, &cert_count)) {
		ctx_log(ctx, LOG_ERR, "Unable to enumerate certificates\n");
		goto cleanup;
	}
	if (cert_count == 0) {
		ctx_log(ctx, LOG_INFO, "No certificate found.\n");
		goto cleanup;
	}
	ctx_log(ctx, LOG_NOTICE, "Found %u certificate%s:\n", cert_count, cert_count == 1 ? "" : "s");
	if (obj_id_len != 0 || obj_label) {
		which = "longest expiry matching";
		for (m = 0; m < cert_count; m++) {
			PKCS11_CERT *k = certs + m;

			hexbuf = dump_hex((unsigned char *)k->id, k->id_len);
			expiry = dump_expiry(k);
			ctx_log(ctx, LOG_NOTICE, "  %2u    %s%s%s%s%s%s\n", m + 1,
				hexbuf ? " id=" : "",
				hexbuf ? hexbuf : "",
				k->label ? " label=" : "",
				k->label ? k->label : "",
				expiry ? " expiry=" : "",
				expiry ? expiry : "");
			OPENSSL_free(hexbuf);
			OPENSSL_free(expiry);

			if (obj_label && obj_id_len != 0) {
				if (k->label && strcmp(k->label, obj_label) == 0 &&
						k->id_len == obj_id_len &&
						memcmp(k->id, obj_id, obj_id_len) == 0) {
					selected_cert = cert_cmp(selected_cert, k);
				}
			} else if (obj_label && !obj_id_len) {
				if (k->label && strcmp(k->label, obj_label) == 0) {
					selected_cert = cert_cmp(selected_cert, k);
				}
			} else if (obj_id_len && !obj_label) {
				if (k->id_len == obj_id_len &&
						memcmp(k->id, obj_id, obj_id_len) == 0) {
					selected_cert = cert_cmp(selected_cert, k);
				}
			}
		}
	} else {
		which = "first (with id present)";
		for (m = 0; m < cert_count; m++) {
			PKCS11_CERT *k = certs + m;

			hexbuf = dump_hex((unsigned char *)k->id, k->id_len);
			expiry = dump_expiry(k);
			ctx_log(ctx, LOG_NOTICE, "  %2u    %s%s%s%s%s%s\n", m + 1,
				hexbuf ? " id=" : "",
				hexbuf ? hexbuf : "",
				k->label ? " label=" : "",
				k->label ? k->label : "",
				expiry ? " expiry=" : "",
				expiry ? expiry : "");
			OPENSSL_free(hexbuf);
			OPENSSL_free(expiry);

			if (!selected_cert && k->id && *k->id) {
				selected_cert = k; /* Use the first certificate with nonempty id */
			}
		}
		if (!selected_cert) {
			which = "first";
			selected_cert = certs; /* Use the first certificate */
		}
	}

	if (selected_cert) {
		hexbuf = dump_hex((unsigned char *)selected_cert->id, selected_cert->id_len);
		expiry = dump_expiry(selected_cert);
		ctx_log(ctx, LOG_NOTICE, "Returning %s certificate:%s%s%s%s%s%s\n", which,
			hexbuf ? " id=" : "",
			hexbuf ? hexbuf : "",
			selected_cert->label ? " label=" : "",
			selected_cert->label ? selected_cert->label : "",
			expiry ? " expiry=" : "",
			expiry ? expiry : "");
		OPENSSL_free(hexbuf);
		OPENSSL_free(expiry);
	} else {
		ctx_log(ctx, LOG_ERR, "No matching certificate returned.\n");
	}

cleanup:
	OPENSSL_free(cert_template.label);
	OPENSSL_free(cert_template.id);
	return selected_cert;
}

X509 *util_get_cert_from_uri(ENGINE_CTX *ctx, const char *object_uri,
	UI_METHOD *ui_method, void *callback_data)
{
	PKCS11_CERT *cert;

	cert = ctx_load_object(ctx, "certificate", match_cert, object_uri,
		ui_method, callback_data);
	return cert ? X509_dup(cert->x509) : NULL;
}

/******************************************************************************/
/* Private and public key handling                                            */
/******************************************************************************/

static void *match_key(ENGINE_CTX *ctx, const char *key_type,
		PKCS11_KEY *keys, unsigned int key_count,
		const char *obj_id, size_t obj_id_len, const char *obj_label)
{
	PKCS11_KEY *selected_key = NULL;
	unsigned int m;
	const char *which;
	char *hexbuf;

	if (key_count == 0) {
		ctx_log(ctx, LOG_INFO, "No %s key found.\n", key_type);
		return NULL;
	}
	ctx_log(ctx, LOG_NOTICE, "Found %u %s key%s:\n", key_count, key_type,
		key_count == 1 ? "" : "s");

	if (obj_id_len != 0 || obj_label) {
		which = "last matching";
		for (m = 0; m < key_count; m++) {
			PKCS11_KEY *k = keys + m;

			hexbuf = dump_hex((unsigned char *)k->id, k->id_len);
			ctx_log(ctx, LOG_NOTICE, "  %2u %c%c%s%s%s%s\n", m + 1,
				k->isPrivate ? 'P' : ' ',
				k->needLogin ? 'L' : ' ',
				hexbuf ? " id=" : "",
				hexbuf ? hexbuf : "",
				k->label ? " label=" : "",
				k->label ? k->label : "");
			OPENSSL_free(hexbuf);

			if (obj_label && obj_id_len != 0) {
				if (k->label && strcmp(k->label, obj_label) == 0 &&
						k->id_len == obj_id_len &&
						memcmp(k->id, obj_id, obj_id_len) == 0) {
					selected_key = k;
				}
			} else if (obj_label && !obj_id_len) {
				if (k->label && strcmp(k->label, obj_label) == 0) {
					selected_key = k;
				}
			} else if (obj_id_len && !obj_label) {
				if (k->id_len == obj_id_len &&
						memcmp(k->id, obj_id, obj_id_len) == 0) {
					selected_key = k;
				}
			}
		}
	} else {
		which = "first";
		selected_key = keys; /* Use the first key */
	}

	if (selected_key) {
		hexbuf = dump_hex((unsigned char *)selected_key->id, selected_key->id_len);
		ctx_log(ctx, LOG_NOTICE, "Returning %s %s key:%s%s%s%s\n", which, key_type,
			hexbuf ? " id=" : "",
			hexbuf ? hexbuf : "",
			selected_key->label ? " label=" : "",
			selected_key->label ? selected_key->label : "");
		OPENSSL_free(hexbuf);
	} else {
		ctx_log(ctx, LOG_ERR, "No matching %s key returned.\n", key_type);
	}

	return selected_key;
}

static void *match_key_int(ENGINE_CTX *ctx, PKCS11_TOKEN *tok,
		const unsigned int isPrivate, const char *obj_id, size_t obj_id_len, const char *obj_label)
{
	PKCS11_KEY *keys;
	PKCS11_KEY key_template = {0};
	unsigned int key_count;
	void *ret = NULL;

	key_template.isPrivate = isPrivate;
	errno = 0;
	key_template.label = obj_label ? OPENSSL_strdup(obj_label) : NULL;
	if (errno != 0) {
		ctx_log(ctx, LOG_ERR, "%s", strerror(errno));
		goto cleanup;
	}
	if (obj_id_len) {
		key_template.id = OPENSSL_malloc(obj_id_len);
		if (!key_template.id) {
			ctx_log(ctx, LOG_ERR, "Could not allocate memory for ID\n");
			goto cleanup;
		}
		memcpy(key_template.id, obj_id, obj_id_len);
		key_template.id_len = obj_id_len;
	}

	/* Make sure there is at least one private key on the token */
	if (key_template.isPrivate != 0 && PKCS11_enumerate_keys_ext(tok, (const PKCS11_KEY *) &key_template, &keys, &key_count)) {
		ctx_log(ctx, LOG_ERR, "Unable to enumerate private keys\n");
		goto cleanup;
	}
	else if (key_template.isPrivate == 0 && PKCS11_enumerate_public_keys_ext(tok, (const PKCS11_KEY *) &key_template, &keys, &key_count)) {
		ctx_log(ctx, LOG_ERR, "Unable to enumerate public keys\n");
		goto cleanup;
	}
	ret = match_key(ctx, key_template.isPrivate ? "private" : "public", keys, key_count, obj_id, obj_id_len, obj_label);
cleanup:
	OPENSSL_free(key_template.label);
	OPENSSL_free(key_template.id);
	return ret;
}

static void *match_public_key(ENGINE_CTX *ctx, PKCS11_TOKEN *tok,
		const char *obj_id, size_t obj_id_len, const char *obj_label)
{
	return match_key_int(ctx, tok, 0, obj_id, obj_id_len, obj_label);
}

static void *match_private_key(ENGINE_CTX *ctx, PKCS11_TOKEN *tok,
		const char *obj_id, size_t obj_id_len, const char *obj_label)
{
	return match_key_int(ctx, tok, 1, obj_id, obj_id_len, obj_label);
}

EVP_PKEY *util_get_pubkey_from_uri(ENGINE_CTX *ctx, const char *s_key_id,
		UI_METHOD *ui_method, void *callback_data)
{
	PKCS11_KEY *key;

	key = ctx_load_object(ctx, "public key", match_public_key,
		s_key_id, ui_method, callback_data);
	return key ? PKCS11_get_public_key(key) : NULL;
}

EVP_PKEY *util_get_privkey_from_uri(ENGINE_CTX *ctx, const char *s_key_id,
		UI_METHOD *ui_method, void *callback_data)
{
	PKCS11_KEY *key = ctx_load_object(ctx, "private key", match_private_key,
		s_key_id, ui_method, callback_data);
	return key ? PKCS11_get_private_key(key) : NULL;
}

/* vim: set noexpandtab: */