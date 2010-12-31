/*
 * card-authentic.c: Support for the Oberthur smart cards 
 * 	with PKI applet AuthentIC v3.1
 * 	
 * Copyright (C) 2010  Viktor Tarasov <vtarasov@opentrust.com>
 * 			OpenTrust <www.opentrust.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <string.h>
#include <stdlib.h>

#include "internal.h"
#include "asn1.h"
#include "cardctl.h"
#include "opensc.h"
#include "pkcs15.h"
#include "iso7816.h"
/* #include "hash-strings.h" */
#include "authentic.h"

#ifndef ENABLE_OPENSSL
#error "Need OpenSSL"
#endif

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/pkcs12.h>
#include <openssl/x509v3.h>
#include <openssl/sha.h>

#define AUTHENTIC_CARD_DEFAULT_FLAGS (SC_CARD_FLAG_RNG	\
		| SC_ALGORITHM_ONBOARD_KEY_GEN		\
		| SC_ALGORITHM_RSA_PAD_ISO9796		\
		| SC_ALGORITHM_RSA_PAD_PKCS1		\
		| SC_ALGORITHM_RSA_HASH_NONE		\
		| SC_ALGORITHM_RSA_HASH_SHA1		\
		| SC_ALGORITHM_RSA_HASH_SHA256)

#define AUTHENTIC_READ_BINARY_LENGTH_MAX 0xE7

/* generic iso 7816 operations table */
static const struct sc_card_operations *iso_ops = NULL;

/* our operations table with overrides */
static struct sc_card_operations authentic_ops;

static struct sc_card_driver authentic_drv = {
	"Oberthur AuthentIC v3.1", "authentic", &authentic_ops,
	NULL, 0, NULL
};

/*
 * FIXME: use dynamic allocation for the PIN data to reduce memory usage
 * actually size of 'authentic_private_data' 140kb
 */
struct authentic_private_data {
	struct sc_pin_cmd_data pins[8];
	unsigned char pins_sha1[8][SHA_DIGEST_LENGTH];

	struct sc_authentic_cplc cplc;
};

static struct sc_atr_table authentic_known_atrs[] = {
	{ "3B:DD:18:00:81:31:FE:45:80:F9:A0:00:00:00:77:01:00:70:0A:90:00:8B", NULL, 
		"Oberthur AuthentIC 3.2.2", SC_CARD_TYPE_OBERTHUR_AUTHENTIC_3_2,  0, NULL },
	{ NULL, NULL, NULL, 0, 0, NULL }
};

unsigned char aid_AuthentIC_3_2[] = {
	0xA0,0x00,0x00,0x00,0x77,0x01,0x00,0x70,0x0A,0x10,0x00,0xF1,0x00,0x00,0x01,0x00
};

static int authentic_select_file(struct sc_card *card, const struct sc_path *path, struct sc_file **file_out);
static int authentic_process_fci(struct sc_card *card, struct sc_file *file, const unsigned char *buf, size_t buflen);
static int authentic_get_serialnr(struct sc_card *card, struct sc_serial_number *serial);
static int authentic_pin_get_policy (struct sc_card *card, struct sc_pin_cmd_data *data);
static int authentic_pin_is_verified(struct sc_card *card, struct sc_pin_cmd_data *pin_cmd, int *tries_left);
static int authentic_select_mf(struct sc_card *card, struct sc_file **file_out);
static int authentic_card_ctl(struct sc_card *card, unsigned long cmd, void *ptr);
static void authentic_debug_select_file(struct sc_card *card, const struct sc_path *path);

static int
authentic_update_blob(struct sc_context *ctx, unsigned tag, unsigned char *data, size_t data_len,
		unsigned char **blob, size_t *blob_size)
{
	unsigned char *pp = NULL;
	int offs = 0, sz;

	if (data_len == 0)
		return SC_SUCCESS;

	sz = data_len + 2;

	if (tag > 0xFF)
		sz++;

	if (data_len > 0x7F && data_len < 0x100)
		sz++;
	else if (data_len >= 0x100)
		sz += 2;
	
	pp = realloc(*blob, *blob_size + sz);
	if (!pp)
		LOGN_FUNC_RETURN(ctx, SC_ERROR_MEMORY_FAILURE);
	
	if (tag > 0xFF)
		*(pp + *blob_size + offs++) = (tag >> 8) & 0xFF;
	*(pp + *blob_size + offs++) = tag & 0xFF;

	if (data_len >= 0x100) {
		*(pp + *blob_size + offs++) = 0x82;
		*(pp + *blob_size + offs++) = (data_len >> 8) & 0xFF;
	}
	else if (data_len > 0x7F)   {
		*(pp + *blob_size + offs++) = 0x81;
	}
	*(pp + *blob_size + offs++) = data_len & 0xFF;

	memcpy(pp + *blob_size + offs, data, data_len);

	*blob_size += sz;
	*blob = pp;

	return SC_SUCCESS;
}


static int 
authentic_parse_size(unsigned char *in, size_t *out)
{
	if (!in || !out)
		return SC_ERROR_INVALID_ARGUMENTS;

	if (*in < 0x80)   {
		*out = *in;
		return 1;
	}
	else if (*in == 0x81)   {
		*out = *(in + 1);
		return 2;
	}
	else if (*in == 0x82)   {
		*out = *(in + 1) * 0x100 + *(in + 2);
		return 3;
	}

	return SC_ERROR_INVALID_DATA;
}


static int 
authentic_get_tagged_data(struct sc_context *ctx, unsigned char *in, size_t in_len, 
		unsigned in_tag, unsigned char **out, size_t *out_len)
{
	size_t size_len, tag_len, offs, size;
	unsigned tag;

	if (!out || !out_len)
		LOGN_FUNC_RETURN(ctx, SC_ERROR_INVALID_ARGUMENTS);

	for (offs = 0; offs < in_len; )   {
		if ((*(in + offs) == 0x7F) || (*(in + offs) == 0x5F))   {
			tag = *(in + offs) * 0x100 + *(in + offs + 1);
			tag_len = 2;
		}
		else   {
			tag = *(in + offs);
			tag_len = 1;
		}
	
		size_len = authentic_parse_size(in + offs + tag_len, &size);
		LOGN_TEST_RET(ctx, size_len, "parse error: invalid size data");

		if (tag == in_tag)   {
			*out = in + offs + tag_len + size_len;
			*out_len = size;

			return SC_SUCCESS;
		}

		offs += tag_len + size_len + size;
	}

	return SC_ERROR_ASN1_OBJECT_NOT_FOUND;
}


static int
authentic_decode_pubkey_rsa(struct sc_context *ctx, unsigned char *blob, size_t blob_len, 
		struct sc_pkcs15_prkey **out_key)
{
	struct sc_pkcs15_prkey_rsa *key;
	unsigned char *data;
	size_t data_len;
	int rv;

	LOGN_FUNC_CALLED(ctx);

	if (!out_key)
		LOGN_FUNC_RETURN(ctx, SC_ERROR_INVALID_ARGUMENTS);

	if (!(*out_key))   {
		*out_key = calloc(1, sizeof(struct sc_pkcs15_prkey));

		if (!(*out_key))
			LOGN_TEST_RET(ctx, SC_ERROR_OUT_OF_MEMORY, "Cannot callocate pkcs15 private key");

		(*out_key)->algorithm = SC_ALGORITHM_RSA;
	}
	else if (*out_key && (*out_key)->algorithm != SC_ALGORITHM_RSA)   {
		LOGN_FUNC_RETURN(ctx, SC_ERROR_INVALID_DATA);
	}

	key = &(*out_key)->u.rsa;

	rv = authentic_get_tagged_data(ctx, blob, blob_len, AUTHENTIC_TAG_RSA_PUBLIC, &data, &data_len);
	LOGN_TEST_RET(ctx, rv, "cannot get public key SDO data");

	blob = data;
	blob_len = data_len;

	/* Get RSA public modulus */
	rv = authentic_get_tagged_data(ctx, blob, blob_len, AUTHENTIC_TAG_RSA_PUBLIC_MODULUS, &data, &data_len);
	LOGN_TEST_RET(ctx, rv, "cannot get public key SDO data");

	if (key->modulus.data)
		free(key->modulus.data);
	key->modulus.data = calloc(1, data_len);
	if (!key->modulus.data)
		LOGN_TEST_RET(ctx, SC_ERROR_OUT_OF_MEMORY, "Cannot callocate modulus BN");
	memcpy(key->modulus.data, data, data_len);
	key->modulus.len = data_len;

	/* Get RSA public exponent */
	rv = authentic_get_tagged_data(ctx, blob, blob_len, AUTHENTIC_TAG_RSA_PUBLIC_EXPONENT, &data, &data_len);
	LOGN_TEST_RET(ctx, rv, "cannot get public key SDO data");

	if (key->exponent.data)
		free(key->exponent.data);
	key->exponent.data = calloc(1, data_len);
	if (!key->exponent.data)
		LOGN_TEST_RET(ctx, SC_ERROR_OUT_OF_MEMORY, "Cannot callocate modulus BN");
	memcpy(key->exponent.data, data, data_len);
	key->exponent.len = data_len;

	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_parse_credential_data(struct sc_context *ctx, struct sc_pin_cmd_data *pin_cmd, 
		unsigned char *blob, size_t blob_len)
{
	unsigned char *data;
	size_t data_len;
	int rv, ii;
	unsigned tag = AUTHENTIC_TAG_CREDENTIAL | pin_cmd->pin_reference;

	rv = authentic_get_tagged_data(ctx, blob, blob_len, tag, &blob, &blob_len);
	LOGN_TEST_RET(ctx, rv, "cannot get credential data");

	rv = authentic_get_tagged_data(ctx, blob, blob_len, AUTHENTIC_TAG_CREDENTIAL_TRYLIMIT, &data, &data_len);
	LOGN_TEST_RET(ctx, rv, "cannot get try limit");
	pin_cmd->pin1.max_tries = *data;

	rv = authentic_get_tagged_data(ctx, blob, blob_len, AUTHENTIC_TAG_DOCP_MECH, &data, &data_len);
	LOGN_TEST_RET(ctx, rv, "cannot get PIN type");
	if (*data == 0)
		pin_cmd->pin_type = SC_AC_CHV;
	else if (*data >= 2 && *data <= 7)
		pin_cmd->pin_type = SC_AC_AUT;
	else
		LOGN_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "unsupported Credential type");

	rv = authentic_get_tagged_data(ctx, blob, blob_len, AUTHENTIC_TAG_DOCP_ACLS, &data, &data_len);
	LOGN_TEST_RET(ctx, rv, "failed to get ACLs");
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "data_len:%i", data_len);
	if (data_len == 10)   {
		for (ii=0; ii<5; ii++)   {
			unsigned char acl = *(data + ii*2);
			unsigned char cred_id = *(data + ii*2 + 1);
			unsigned sc = acl * 0x100 + cred_id;

			sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "%i: SC:%X", ii, sc);
			if (!sc)
				continue;

			/* pin_cmd->pin1.acls[ii].raw_value = sc; */

			if (acl & AUTHENTIC_AC_SM_MASK)   {
				pin_cmd->pin1.acls[ii].method = SC_AC_SCB;
				pin_cmd->pin1.acls[ii].key_ref = sc;
			}
			else if (acl!=0xFF && cred_id)   {
				sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "%i: ACL(method:SC_AC_CHV,id:%i)", ii, cred_id);
				pin_cmd->pin1.acls[ii].method = SC_AC_CHV;
				pin_cmd->pin1.acls[ii].key_ref = cred_id;
			}
			else   {
				pin_cmd->pin1.acls[ii].method = SC_AC_NEVER;
				pin_cmd->pin1.acls[ii].key_ref = 0;
			}
		}
	}

	rv = authentic_get_tagged_data(ctx, blob, blob_len, AUTHENTIC_TAG_CREDENTIAL_PINPOLICY, &data, &data_len);
	if (!rv)   {
		blob = data;
		blob_len = data_len;

		rv = authentic_get_tagged_data(ctx, blob, blob_len, AUTHENTIC_TAG_CREDENTIAL_PINPOLICY_MAXLENGTH, &data, &data_len);
		LOGN_TEST_RET(ctx, rv, "failed to get PIN max.length value");
		pin_cmd->pin1.max_length = *data;

		rv = authentic_get_tagged_data(ctx, blob, blob_len, AUTHENTIC_TAG_CREDENTIAL_PINPOLICY_MINLENGTH, &data, &data_len);
		LOGN_TEST_RET(ctx, rv, "failed to get PIN min.length value");
		pin_cmd->pin1.min_length = *data;
	}

	return SC_SUCCESS;
}


static int
authentic_get_cplc(struct sc_card *card)
{
	struct authentic_private_data *prv_data = (struct authentic_private_data *) card->drv_data;
	struct sc_apdu apdu;
	int rv, ii;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0xCA, 0x9F, 0x7F);
	for (ii=0;ii<2;ii++)   {
		apdu.le = 0x2D;
		apdu.resplen = sizeof(prv_data->cplc.value);
		apdu.resp = prv_data->cplc.value;

        	rv = sc_transmit_apdu(card, &apdu);
        	LOGN_TEST_RET(card->ctx, rv, "APDU transmit failed");
        	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
		if (rv != SC_ERROR_CLASS_NOT_SUPPORTED)
			break;

		apdu.cla = 0x80;
	}
        LOGN_TEST_RET(card->ctx, rv, "'GET CPLC' error");
	
	prv_data->cplc.len = 0x2D;
	return SC_SUCCESS;
}


static int
authentic_select_aid(struct sc_card *card, unsigned char *aid, size_t aid_len, 
		unsigned char *out, size_t *out_len)
{
	struct sc_apdu apdu;
	unsigned char apdu_resp[SC_MAX_APDU_BUFFER_SIZE];
	int rv;

	/* Select Card Manager (to deselect previously selected application) */
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xA4, 0x04, 0x00);
	apdu.lc = aid_len;
	apdu.data = aid;
	apdu.datalen = aid_len;
	apdu.resplen = sizeof(apdu_resp);
	apdu.resp = apdu_resp;

	rv = sc_transmit_apdu(card, &apdu);
	LOGN_TEST_RET(card->ctx, rv, "APDU transmit failed");
	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
	LOGN_TEST_RET(card->ctx, rv, "Cannot select AID");

	if (out && out_len)   {
		if (*out_len < apdu.resplen)
			LOGN_TEST_RET(card->ctx, SC_ERROR_BUFFER_TOO_SMALL, "Cannot select AID");
		memcpy(out, apdu.resp, apdu.resplen);
	}

	return SC_SUCCESS;
}


static int
authentic_match_card(struct sc_card *card)
{
	struct sc_context *ctx = card->ctx;
	int i;

	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "try to match card with ATR %s", sc_dump_hex(card->atr, card->atr_len));
	i = _sc_match_atr(card, authentic_known_atrs, &card->type);
	if (i < 0)   {
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "card not matched");
		return 0;
	}

	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "'%s' card matched", authentic_known_atrs[i].name);
	return 1;
}


static int
authentic_init_oberthur_authentic_3_2(struct sc_card *card)
{
	struct sc_context *ctx = card->ctx;
	unsigned char resp[0x100];
	size_t resp_len;
	unsigned int flags;
	int rv = 0;

	LOGN_FUNC_CALLED(ctx);

	flags = AUTHENTIC_CARD_DEFAULT_FLAGS;

	_sc_card_add_rsa_alg(card, 1024, flags, 0x10001);
	_sc_card_add_rsa_alg(card, 2048, flags, 0x10001);

	card->caps = SC_CARD_CAP_RNG;
	card->caps |= SC_CARD_CAP_RSA_2048;
	card->caps |= SC_CARD_CAP_APDU_EXT; 
	card->caps |= SC_CARD_CAP_USE_FCI_AC;

	resp_len = sizeof(resp);
	rv = authentic_select_aid(card, aid_AuthentIC_3_2, sizeof(aid_AuthentIC_3_2), NULL, NULL);
	LOGN_TEST_RET(ctx, rv, "AuthentIC application select error");

	rv = authentic_select_mf(card, NULL);
	LOGN_TEST_RET(ctx, rv, "MF selection error");
	
	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_init(struct sc_card *card)
{
	struct sc_context *ctx = card->ctx;
	int ii, rv = SC_ERROR_NO_CARD_SUPPORT;

	LOGN_FUNC_CALLED(ctx);
	for(ii=0;authentic_known_atrs[ii].atr;ii++)   {
		if (card->type == authentic_known_atrs[ii].type)   {
			card->name = authentic_known_atrs[ii].name;
			card->flags = authentic_known_atrs[ii].flags;
			break;
		}
	}

	if (!authentic_known_atrs[ii].atr)
		LOGN_FUNC_RETURN(ctx, SC_ERROR_NO_CARD_SUPPORT);

	card->cla  = 0x00;
	card->drv_data = (struct authentic_private_data *) calloc(sizeof(struct authentic_private_data), 1);
	if (!card->drv_data)
		LOGN_FUNC_RETURN(ctx, SC_ERROR_OUT_OF_MEMORY);
#if 0	
	card->flags |= SC_CARD_FLAG_ZERO_LENGTH_PIN_ACCEPTED;
#endif

	if (card->type == SC_CARD_TYPE_OBERTHUR_AUTHENTIC_3_2)
		rv = authentic_init_oberthur_authentic_3_2(card);

	if (!rv)
		rv = authentic_get_serialnr(card, NULL);

	LOGN_FUNC_RETURN(ctx, rv);
}


#if 0
static int 
authentic_read_binary_long(struct sc_card *card, unsigned int offs, 
		unsigned char *buf, size_t count, unsigned long flags)
{
	struct sc_context *ctx = card->ctx;
	const struct sc_acl_entry *entry = NULL;
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "authentic_sc_read_binary(card:%p) offs %i; count %i\n", card, offs, count);
	if (offs > 0x7fff)
		LOGN_TEST_RET(ctx, SC_ERROR_OFFSET_TOO_LARGE, "Invalid arguments");

	if (count == 0)
		LOGN_FUNC_RETURN(ctx, SC_SUCCESS);

	authentic_debug_select_file(card, NULL);

	if (card->cache.valid  && card->cache.current_ef)   {
		entry = sc_file_get_acl_entry(card->cache.current_ef, SC_AC_OP_READ);
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "READ method/reference %X/%X\n", entry->method, entry->key_ref);
		if (entry->method == SC_AC_SCB)   {
			rv = sm_read_binary(card, entry->key_ref, offs, count ? count : card->cache.current_ef->size, buf, count);
			LOGN_FUNC_RETURN(ctx, rv);
		}
	}

	LOGN_FUNC_RETURN(ctx, SC_SUCCESS);
}


static int
authentic_update_binary_long(struct sc_card *card, unsigned int offs,
		const unsigned char *buff, size_t count, unsigned long flags)
{
	struct sc_context *ctx = card->ctx;
	const struct sc_acl_entry *entry = NULL;
	int rv;
	
	LOGN_FUNC_CALLED(ctx);
	if (count == 0)
		return SC_SUCCESS;

	authentic_debug_select_file(card, NULL);

	if (card->cache.valid && card->cache.current_ef)   {
		entry = sc_file_get_acl_entry(card->cache.current_ef, SC_AC_OP_UPDATE);
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "UPDATE method/reference %X/%X\n", entry->method, entry->key_ref);
		if (entry->method == SC_AC_SCB)   {
			rv = sm_update_binary(card, entry->key_ref, offs, buff, count);
			LOGN_FUNC_RETURN(ctx, rv);
		}
	}

	LOGN_FUNC_RETURN(ctx, SC_SUCCESS);
}
#endif

static int
authentic_erase_binary(struct sc_card *card, unsigned int offs, size_t count, unsigned long flags)
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu *apdus = NULL, *cur_apdu = NULL;
	size_t sz, rest;
	int rv;
	unsigned char *buf_zero = NULL;

	LOGN_FUNC_CALLED(ctx);
	if (!count)
		LOGN_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "'ERASE BINARY' with ZERO count not supported");

	if (card->cache.valid && card->cache.current_ef) 
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "current_ef(type=%i) %s", card->cache.current_ef->path.type, 
				sc_print_path(&card->cache.current_ef->path));

	buf_zero = calloc(1, count);
	if (!buf_zero)
		LOGN_TEST_RET(ctx, SC_ERROR_MEMORY_FAILURE, "cannot allocate buff 'zero'");

	rv = sc_update_binary(card, offs, buf_zero, count, flags);
	free(buf_zero);
	LOGN_TEST_RET(ctx, rv, "'ERASE BINARY' failed");

	LOGN_FUNC_RETURN(ctx, SC_SUCCESS);
}


static int 
authentic_resize_file(struct sc_card *card, unsigned file_id, unsigned new_size)
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu apdu;
	unsigned char data[6] = {
		0x62, 0x04, 0x80, 0x02, 0xFF, 0xFF
	};
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "try to set file size to %i bytes", new_size);

	data[4] = (new_size >> 8) & 0xFF;
	data[5] = new_size & 0xFF;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xDB, (file_id >> 8) & 0xFF, file_id & 0xFF);
	apdu.data = data;
	apdu.datalen = sizeof(data);
	apdu.lc = sizeof(data);

	rv = sc_transmit_apdu(card, &apdu);
	LOGN_TEST_RET(ctx, rv, "APDU transmit failed");
	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
	LOGN_TEST_RET(ctx, rv, "resize file failed");

	if (card->cache.valid && card->cache.current_ef && card->cache.current_ef->id == file_id)
		card->cache.current_ef->size = new_size;

	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_set_current_files(struct sc_card *card, struct sc_path *path,
		unsigned char *resp, size_t resplen, struct sc_file **file_out)
{
	struct sc_context *ctx = card->ctx;
	struct sc_file *file = NULL;
	int rv;

	LOGN_FUNC_CALLED(ctx);
	if (resplen)   {
		switch (resp[0]) {
		case 0x62:
		case 0x6F:
			file = sc_file_new();
			if (file == NULL)
				LOGN_FUNC_RETURN(ctx, SC_ERROR_OUT_OF_MEMORY);
			if (path)
				file->path = *path;

			rv = authentic_process_fci(card, file, resp, resplen);
			LOGN_TEST_RET(ctx, rv, "cannot set 'current file': FCI process error");

			break;
		default:
			LOGN_FUNC_RETURN(ctx, SC_ERROR_UNKNOWN_DATA_RECEIVED);
		}

		if (file->type == SC_FILE_TYPE_DF)   {
			struct sc_path cur_df_path;

			memset(&cur_df_path, 0, sizeof(cur_df_path));
			if (card->cache.valid && card->cache.current_df)   {
				cur_df_path = card->cache.current_df->path;
				sc_file_free(card->cache.current_df);
			}
			card->cache.current_df = NULL;
			sc_file_dup(&card->cache.current_df, file);

			if (cur_df_path.len)   {
				memcpy(card->cache.current_df->path.value + cur_df_path.len, 
						card->cache.current_df->path.value, 
						card->cache.current_df->path.len);
				memcpy(card->cache.current_df->path.value, cur_df_path.value, cur_df_path.len);
				card->cache.current_df->path.len += cur_df_path.len;
			}

			if (card->cache.current_ef)   {
				sc_file_free(card->cache.current_ef);
				card->cache.current_ef = NULL;
			}

			card->cache.valid = 1;
		}
		else   {
			if (card->cache.current_ef)
				sc_file_free(card->cache.current_ef);
			card->cache.current_ef = NULL;
			sc_file_dup(&card->cache.current_ef, file);
		}
		
		if (file_out)
			*file_out = file;
		else
			sc_file_free(file);
	}

	LOGN_FUNC_RETURN(ctx, SC_SUCCESS);
}


static int 
authentic_select_mf(struct sc_card *card, struct sc_file **file_out)
{
	struct sc_context *ctx = card->ctx;
	struct sc_path mfpath;
	int rv;

	struct sc_apdu apdu;
	unsigned char rbuf[SC_MAX_APDU_BUFFER_SIZE];

	LOGN_FUNC_CALLED(ctx);

	memset(&mfpath, 0, sizeof(struct sc_path));
	sc_format_path("3F00", &mfpath);
	mfpath.type = SC_PATH_TYPE_PATH;

	if (card->cache.valid == 1 
			&& card->cache.current_df 
			&& card->cache.current_df->path.len == 2 
			&& !memcmp(card->cache.current_df->path.value, "\x3F\x00", 2))   {
		if (file_out)
			sc_file_dup(file_out, card->cache.current_df);
	
		LOGN_FUNC_RETURN(ctx, SC_SUCCESS);
	}

	sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0xA4, 0x00, 0x00);
		
	apdu.resp = rbuf;
	apdu.resplen = sizeof(rbuf);

	rv = sc_transmit_apdu(card, &apdu);
	LOGN_TEST_RET(ctx, rv, "APDU transmit failed");
	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
	LOGN_TEST_RET(ctx, rv, "authentic_select_file() check SW failed");

	if (card->cache.valid == 1)   {
		if (card->cache.current_df)
			sc_file_free(card->cache.current_df);
		card->cache.current_df = NULL;

		if (card->cache.current_ef)
			sc_file_free(card->cache.current_ef);
		card->cache.current_ef = NULL;
	}

	rv = authentic_set_current_files(card, &mfpath, apdu.resp, apdu.resplen, file_out);
	LOGN_TEST_RET(ctx, rv, "authentic_select_file() cannot set 'current_file'");

	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_reduce_path(struct sc_card *card, struct sc_path *path)
{
	struct sc_context *ctx = card->ctx;
	struct sc_path in_path, cur_path;
	int offs;

	LOGN_FUNC_CALLED(ctx);

	if (path->len <= 2 || path->type == SC_PATH_TYPE_DF_NAME || !path)
		LOGN_FUNC_RETURN(ctx, SC_SUCCESS);

	if (!card->cache.valid || !card->cache.current_df)
		LOGN_FUNC_RETURN(ctx, 0);

	in_path = *path;
	cur_path = card->cache.current_df->path;

	if (!memcmp(cur_path.value, "\x3F\x00", 2) && memcmp(in_path.value, "\x3F\x00", 2))   {
		memcpy(in_path.value + 2, in_path.value, in_path.len);
		memcpy(in_path.value, "\x3F\x00", 2);
		in_path.len += 2;
	}

	for (offs=0; offs < in_path.len && offs < cur_path.len; offs += 2)   {
		if (cur_path.value[offs] != in_path.value[offs])
			break;
		if (cur_path.value[offs + 1] != in_path.value[offs + 1])
			break;
	}

	memcpy(in_path.value, in_path.value + offs, sizeof(in_path.value) - offs);
	in_path.len -= offs;
	*path = in_path;

	LOGN_FUNC_RETURN(ctx, offs);
}


static void 
authentic_debug_select_file(struct sc_card *card, const struct sc_path *path)
{
	struct sc_context *ctx = card->ctx;
	struct sc_card_cache *cache = &card->cache;

	if (path)
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "try to select path(type:%i) %s", path->type, sc_print_path(path));

	if (!cache->valid)
		return;

	if (cache->current_df)
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "current_df(type=%i) %s", cache->current_df->path.type, sc_print_path(&cache->current_df->path));
	else
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "current_df empty");

	if (cache->current_ef)
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "current_ef(type=%i) %s", cache->current_ef->path.type, sc_print_path(&cache->current_ef->path));
	else
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "current_ef empty");
}


static int
authentic_is_selected(struct sc_card *card, const struct sc_path *path, struct sc_file **file_out)
{
	if (!path->len)   {
		if (file_out && card->cache.valid && card->cache.current_df)
			sc_file_dup(file_out, card->cache.current_df);
		return SC_SUCCESS;
	}
	else if (path->len == 2 && card->cache.valid && card->cache.current_ef)   {
		if (!memcmp(card->cache.current_ef->path.value, path->value, 2))   {
			if (file_out)
				sc_file_dup(file_out, card->cache.current_ef);
			return SC_SUCCESS;
		}
	}

	return SC_ERROR_FILE_NOT_FOUND;
}


static int
authentic_select_file(struct sc_card *card, const struct sc_path *path,
		 struct sc_file **file_out)
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu apdu;
	struct sc_path lpath;
	unsigned char rbuf[SC_MAX_APDU_BUFFER_SIZE];
	int pathlen, rv;

	LOGN_FUNC_CALLED(ctx);
	authentic_debug_select_file(card, path);

	memcpy(&lpath, path, sizeof(struct sc_path));

	rv = authentic_reduce_path(card, &lpath);
	LOGN_TEST_RET(ctx, rv, "reduce path error");

	if (lpath.len >= 2 && lpath.value[0] == 0x3F && lpath.value[1] == 0x00)   {
		rv = authentic_select_mf(card, file_out);
		LOGN_TEST_RET(ctx, rv, "cannot select MF");

		memcpy(&lpath.value[0], &lpath.value[2], lpath.len - 2);
		lpath.len -=  2;

		if (!lpath.len)
			LOGN_FUNC_RETURN(ctx, SC_SUCCESS);
	}

	if (lpath.type == SC_PATH_TYPE_PATH && (lpath.len == 2))
		lpath.type = SC_PATH_TYPE_FILE_ID;

	rv = authentic_is_selected(card, &lpath, file_out);
	if (!rv) 
		LOGN_FUNC_RETURN(ctx, SC_SUCCESS);

	pathlen = lpath.len;
	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0xA4, 0x00, 0x00);

	if (card->type != SC_CARD_TYPE_OBERTHUR_AUTHENTIC_3_2)
		LOGN_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Unsupported card");

	if (lpath.type == SC_PATH_TYPE_FILE_ID)   {
		apdu.p1 = 0x00;
	}
	else if (lpath.type == SC_PATH_TYPE_PATH)  {
		apdu.p1 = 0x08;
	}
	else if (lpath.type == SC_PATH_TYPE_FROM_CURRENT)  {
		apdu.p1 = 0x09;
	}
	else if (lpath.type == SC_PATH_TYPE_DF_NAME)   {
		apdu.p1 = 4;
	}
	else if (lpath.type == SC_PATH_TYPE_PARENT)   {
		apdu.p1 = 0x03;
		pathlen = 0;
		apdu.cse = SC_APDU_CASE_2_SHORT;
	}
	else   {
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "Invalid PATH type: 0x%X", lpath.type);
		LOGN_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "authentic_select_file() invalid PATH type");
	}

	apdu.lc = pathlen;
	apdu.data = lpath.value;
	apdu.datalen = pathlen;

	if (apdu.cse == SC_APDU_CASE_4_SHORT || apdu.cse == SC_APDU_CASE_2_SHORT)   {
		apdu.resp = rbuf;
		apdu.resplen = sizeof(rbuf);
		apdu.le = 256;
	}

	rv = sc_transmit_apdu(card, &apdu);
	LOGN_TEST_RET(ctx, rv, "APDU transmit failed");
	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
	LOGN_TEST_RET(ctx, rv, "authentic_select_file() check SW failed");

	rv = authentic_set_current_files(card, &lpath, apdu.resp, apdu.resplen, file_out);
	LOGN_TEST_RET(ctx, rv, "authentic_select_file() cannot set 'current_file'");

	LOGN_FUNC_RETURN(ctx, SC_SUCCESS);
}

#if 0
static int
authentic_apdus_allocate(struct sc_apdu **head, struct sc_apdu **new)
{
	struct sc_apdu *allocated_apdu = NULL, *tmp_apdu = NULL;

	if (!head)
		return SC_ERROR_INVALID_ARGUMENTS; 

	allocated_apdu = calloc(1, sizeof(struct sc_apdu));
	if (!allocated_apdu)
		return SC_ERROR_MEMORY_FAILURE;

	if (*head == NULL)
		*head = allocated_apdu;

	if (new)
		*new = allocated_apdu;

	tmp_apdu = *head;
	while(tmp_apdu->next) 
		tmp_apdu = tmp_apdu->next;

	tmp_apdu->next = allocated_apdu;

	return 0;
}


static void
authentic_apdus_free(struct sc_apdu *apdu)
{
	while(apdu)   {
		struct sc_apdu *tmp_apdu = apdu->next;
		free(apdu);
		apdu = tmp_apdu;
	}
}	


static int 
authentic_read_binary(struct sc_card *card, unsigned int idx, 
		unsigned char *buf, size_t count, unsigned long flags)
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu *apdus = NULL, *cur_apdu = NULL;
	size_t sz, rest;
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "offs:%i,count:%i,max_recv_size:%i", idx, count, card->max_recv_size);

	rest = count;
	while(rest)   {
		if (authentic_apdus_allocate(&apdus, &cur_apdu))
			LOGN_TEST_RET(ctx, SC_ERROR_MEMORY_FAILURE, "cannot allocate APDU");

		sz = rest > 256 ? 256 : rest;
		sc_format_apdu(card, cur_apdu, SC_APDU_CASE_2_SHORT, 0xB0, (idx >> 8) & 0x7F, idx & 0xFF);
		cur_apdu->le = sz;
		cur_apdu->resplen = count;
		cur_apdu->resp = buf;

		idx += sz;
		rest -= sz;
	}

	rv = sc_transmit_apdu(card, apdus);
	if (!rv)
		rv = sc_check_sw(card, apdus->sw1, apdus->sw2);
	if (!rv)
		count = apdus->resplen;

	authentic_apdus_free(apdus);

	LOGN_TEST_RET(ctx, rv, "authentic_read_binary() failed");
	LOGN_FUNC_RETURN(ctx, count);
}


static int 
authentic_write_binary(struct sc_card *card, unsigned int idx, 
		const unsigned char *buf, size_t count, unsigned long flags)
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu *apdus = NULL, *cur_apdu = NULL;
	size_t sz, rest;
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "offs:%i,count:%i,max_send_size:%i", idx, count, card->max_send_size);

	rest = count;
	while(rest)   {
		if (authentic_apdus_allocate(&apdus, &cur_apdu))
			LOGN_TEST_RET(ctx, SC_ERROR_MEMORY_FAILURE, "cannot allocate APDU");

		sz = rest > 255 ? 255 : rest;
		sc_format_apdu(card, cur_apdu, SC_APDU_CASE_3_SHORT, 0xD0, (idx >> 8) & 0x7F, idx & 0xFF);
		cur_apdu->lc = sz;
		cur_apdu->datalen = sz;
		cur_apdu->data = buf + count - rest;

		idx += sz;
		rest -= sz;
	}

	rv = sc_transmit_apdu(card, apdus);
	if (!rv)
		rv = sc_check_sw(card, apdus->sw1, apdus->sw2);

	authentic_apdus_free(apdus);

	LOGN_TEST_RET(ctx, rv, "authentic_write_binary() failed");
	LOGN_FUNC_RETURN(ctx, count);
}


static int 
authentic_update_binary(struct sc_card *card, unsigned int idx, 
		const unsigned char *buf, size_t count, unsigned long flags)
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu *apdus = NULL, *cur_apdu = NULL;
	size_t sz, rest;
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "offs:%i,count:%i,max_send_size:%i", idx, count, card->max_send_size);

	rest = count;
	while(rest)   {
		if (authentic_apdus_allocate(&apdus, &cur_apdu))
			LOGN_TEST_RET(ctx, SC_ERROR_MEMORY_FAILURE, "cannot allocate APDU");

		sz = rest > 255 ? 255 : rest;
		sc_format_apdu(card, cur_apdu, SC_APDU_CASE_3_SHORT, 0xD6, (idx >> 8) & 0x7F, idx & 0xFF);
		cur_apdu->lc = sz;
		cur_apdu->datalen = sz;
		cur_apdu->data = buf + count - rest;

		idx += sz;
		rest -= sz;
	}

	rv = sc_transmit_apdu(card, apdus);
	if (!rv)
		rv = sc_check_sw(card, apdus->sw1, apdus->sw2);

	authentic_apdus_free(apdus);

	LOGN_TEST_RET(ctx, rv, "authentic_write_binary() failed");
	LOGN_FUNC_RETURN(ctx, count);
}
#endif

static int
authentic_process_fci(struct sc_card *card, struct sc_file *file,
		 const unsigned char *buf, size_t buflen)
{
	struct sc_context *ctx = card->ctx;
	size_t taglen;
	int rv, ii;
	const unsigned char *acls = NULL, *tag = NULL;
	unsigned char ops_DF[8] = {
		SC_AC_OP_CREATE, SC_AC_OP_DELETE, SC_AC_OP_CRYPTO, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};
	unsigned char ops_EF[8] = {
		SC_AC_OP_READ, SC_AC_OP_DELETE, SC_AC_OP_UPDATE, SC_AC_OP_RESIZE, 0xFF, 0xFF, 0xFF, 0xFF
	};
	unsigned char acls_NEVER[8] = {
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
	};

	LOGN_FUNC_CALLED(ctx);

	tag = sc_asn1_find_tag(card->ctx,  buf, buflen, 0x6F, &taglen);
	if (tag != NULL) {
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "  FCP length %i\n", taglen);
		buf = tag;
		buflen = taglen;
	}

	tag = sc_asn1_find_tag(card->ctx,  buf, buflen, 0x62, &taglen);
	if (tag != NULL) {
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "  FCP length %i\n", taglen);
		buf = tag;
		buflen = taglen;
	}

	rv = iso_ops->process_fci(card, file, buf, buflen);
	LOGN_TEST_RET(ctx, rv, "ISO parse FCI failed");

	if (!file->sec_attr_len)   {
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "ACLs not found in data(%i) %s", buflen, sc_dump_hex(buf, buflen));
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "Path:%s; Type:%X; PathType:%X", sc_print_path(&file->path), file->type, file->path.type);
		if (file->path.type == SC_PATH_TYPE_DF_NAME || file->type == SC_FILE_TYPE_DF)   {
			acls = acls_NEVER;
			file->type = SC_FILE_TYPE_DF;
		}
		else   {
			LOGN_TEST_RET(ctx, SC_ERROR_OBJECT_NOT_FOUND, "ACLs tag missing");
		}
	}

	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "ACL data(%i):%s", file->sec_attr_len, sc_dump_hex(file->sec_attr, file->sec_attr_len));
	for (ii = 0; ii < file->sec_attr_len / 2; ii++)  {
		unsigned char op = file->type == SC_FILE_TYPE_DF ? ops_DF[ii] : ops_EF[ii];
		unsigned char acl = *(file->sec_attr + ii*2);
		unsigned char cred_id = *(file->sec_attr + ii*2 + 1);
		unsigned sc = acl * 0x100 + cred_id; 

		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "ACL(%i) op 0x%X, acl %X:%X", ii, op, acl, cred_id);
		if (op == 0xFF)
			;
		else if (!acl && !cred_id)
			sc_file_add_acl_entry(file, op, SC_AC_NONE, 0);
		else if (acl == 0xFF)
			sc_file_add_acl_entry(file, op, SC_AC_NEVER, 0);
		else if (acl & AUTHENTIC_AC_SM_MASK)
			sc_file_add_acl_entry(file, op, SC_AC_SCB, sc);
		else if (cred_id)
			sc_file_add_acl_entry(file, op, SC_AC_CHV, cred_id);
		else
			sc_file_add_acl_entry(file, op, SC_AC_NEVER, 0);
	}

	LOGN_FUNC_RETURN(ctx, 0);
}


static int
authentic_fcp_encode(struct sc_card *card, struct sc_file *file, unsigned char *out, size_t out_len)
{
	struct sc_context *ctx = card->ctx;
	unsigned char buf[0x80];
	size_t ii, offs;
	unsigned char ops_ef[4] = { SC_AC_OP_READ, SC_AC_OP_DELETE, SC_AC_OP_UPDATE, SC_AC_OP_RESIZE };
	unsigned char ops_df[3] = { SC_AC_OP_CREATE, SC_AC_OP_DELETE, SC_AC_OP_CRYPTO };
	unsigned char *ops = file->type == SC_FILE_TYPE_DF ? ops_df : ops_ef;
	size_t ops_len = file->type == SC_FILE_TYPE_DF ? 3 : 4;

	LOGN_FUNC_CALLED(ctx);

	offs = 0;
	buf[offs++] = ISO7816_TAG_FCP_SIZE;
	buf[offs++] = 2;
	buf[offs++] = (file->size >> 8) & 0xFF;
	buf[offs++] = file->size & 0xFF;

	buf[offs++] = ISO7816_TAG_FCP_TYPE;
	buf[offs++] = 1;
	buf[offs++] = file->type == SC_FILE_TYPE_DF ? ISO7816_FILE_TYPE_DF : ISO7816_FILE_TYPE_TRANSPARENT_EF;

	buf[offs++] = ISO7816_TAG_FCP_ID;
	buf[offs++] = 2;
	buf[offs++] = (file->id >> 8) & 0xFF;
	buf[offs++] = file->id & 0xFF;

	buf[offs++] = ISO7816_TAG_FCP_ACLS;
	buf[offs++] = ops_len * 2;
	for (ii=0; ii < ops_len; ii++) {
		const struct sc_acl_entry *entry;
		
		entry = sc_file_get_acl_entry(file, ops[ii]);
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "acl entry(method:%X,ref:%X)", entry->method, entry->key_ref);

		if (entry->method == SC_AC_NEVER)   {
			/* TODO: After development change for 0xFF */
			buf[offs++] = 0x00;
			buf[offs++] = 0x00;
		}
		else if (entry->method == SC_AC_NONE)   {
			buf[offs++] = 0x00;
			buf[offs++] = 0x00;
		}
		else if (entry->method == SC_AC_CHV)   {
			if (!(entry->key_ref & AUTHENTIC_V3_CREDENTIAL_ID_MASK) 
					|| (entry->key_ref & ~AUTHENTIC_V3_CREDENTIAL_ID_MASK))
				LOGN_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Non supported Credential Reference");
			buf[offs++] = 0x00;
			buf[offs++] = 0x01 << (entry->key_ref - 1);
		}
		else
			LOGN_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Non supported AC method");
	}

	if (out)   {
		if (out_len < offs) 
			LOGN_TEST_RET(ctx, SC_ERROR_BUFFER_TOO_SMALL, "Buffer too small to encode FCP");
		memcpy(out, buf, offs); 
	}

	LOGN_FUNC_RETURN(ctx, offs);
}


static int
authentic_create_file(struct sc_card *card, struct sc_file *file)
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu apdu;
	unsigned char sbuf[0x100];
	size_t sbuf_len;
	struct sc_path path;
	int rv;

	LOGN_FUNC_CALLED(ctx);

	if (file->type != SC_FILE_TYPE_WORKING_EF)
		LOGN_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Creation of the file with of this type is not supported");

	authentic_debug_select_file(card, &file->path);

	sbuf_len = authentic_fcp_encode(card, file, sbuf + 2, sizeof(sbuf)-2);
	LOGN_TEST_RET(ctx, sbuf_len, "FCP encode error");

	sbuf[0] = ISO7816_TAG_FCP;
	sbuf[1] = sbuf_len;

	if (card->cache.valid  && card->cache.current_df)   {
		const struct sc_acl_entry *entry = sc_file_get_acl_entry(card->cache.current_df, SC_AC_OP_CREATE);

		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "CREATE method/reference %X/%X\n", entry->method, entry->key_ref);
		if (entry->method == SC_AC_SCB)   {
#if 0
			rv = sm_create_file(card, entry->key_ref, sbuf, sbuf_len + 2);
			LOGN_FUNC_RETURN(ctx, rv);
#else
			LOGN_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Not yet");
#endif
		}
	}

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xE0, 0, 0);
	apdu.data = sbuf;
	apdu.datalen = sbuf_len + 2;
	apdu.lc = sbuf_len + 2;

	rv = sc_transmit_apdu(card, &apdu);
	LOGN_TEST_RET(ctx, rv, "APDU transmit failed");
	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
	LOGN_TEST_RET(ctx, rv, "authentic_create_file() create file error");

	path = file->path;
	memcpy(path.value, path.value + path.len - 2, 2);
	path.len = 2;
	rv = authentic_set_current_files(card, &path, sbuf, sbuf_len + 2, NULL);
	LOGN_TEST_RET(ctx, rv, "authentic_select_file() cannot set 'current_file'");

	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_delete_file(struct sc_card *card, const struct sc_path *path)
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu apdu;
	unsigned char p1;
	int rv, ii;

	LOGN_FUNC_CALLED(ctx);

	if (!path)
		LOGN_FUNC_RETURN(ctx, SC_ERROR_INVALID_ARGUMENTS);

	for (ii=0, p1 = 0x02; ii<2; ii++, p1 = 0x01)   {
		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xE4, p1, 0x00);
		apdu.data = path->value + path->len - 2;
		apdu.datalen = 2;
		apdu.lc = 2;

		rv = sc_transmit_apdu(card, &apdu);
		LOGN_TEST_RET(ctx, rv, "APDU transmit failed");
		rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
		if (rv != SC_ERROR_FILE_NOT_FOUND || p1 != 0x02)
			break;
	}
	LOGN_TEST_RET(ctx, rv, "Delete file failed");

	if (card->cache.valid && card->cache.current_ef)   {
		sc_file_free(card->cache.current_ef);
		card->cache.current_ef = NULL;
	}

	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_chv_verify_pinpad(struct sc_card *card, struct sc_pin_cmd_data *pin_cmd, int *tries_left)
{
	struct sc_context *ctx = card->ctx;
	unsigned char ffs[0x100];
	struct sc_pin_cmd_pin *pin1 = &pin_cmd->pin1;
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "Verify PIN(ref:%i) with pin-pad", pin_cmd->pin_reference);

	rv = authentic_pin_is_verified(card, pin_cmd, tries_left);
	if (!rv)
		LOGN_FUNC_RETURN(ctx, rv);

	if (!card->reader || !card->reader->ops || !card->reader->ops->perform_verify)   {
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "Reader not ready for PIN PAD");
		LOGN_FUNC_RETURN(ctx, SC_ERROR_READER);
	}

	pin1->len = pin1->min_length;
	pin1->max_length = 8;

	memset(ffs, pin1->pad_char, sizeof(ffs));
	pin1->data = ffs;

	pin_cmd->cmd = SC_PIN_CMD_VERIFY;
	pin_cmd->flags |= SC_PIN_CMD_USE_PINPAD;

	rv = iso_ops->pin_cmd(card, pin_cmd, tries_left);

	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_chv_verify(struct sc_card *card, struct sc_pin_cmd_data *pin_cmd,
		int *tries_left)
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu apdu;
	struct sc_pin_cmd_pin *pin1 = &pin_cmd->pin1;
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "CHV PIN reference %i, pin1(%p,len:%i)", pin_cmd->pin_reference, pin1->data, pin1->len);

	if (pin1->data && !pin1->len)   {
		sc_format_apdu(card, &apdu, SC_APDU_CASE_1, 0x20, 0, pin_cmd->pin_reference);
	}
	else if (pin1->data && pin1->len)   {
		unsigned char pin_buff[SC_MAX_APDU_BUFFER_SIZE];
		size_t pin_len;

		memcpy(pin_buff, pin1->data, pin1->len);
		pin_len = pin1->len;

		if (pin1->pad_length && pin_cmd->flags & SC_PIN_CMD_NEED_PADDING)   {
			memset(pin_buff + pin1->len, pin1->pad_char, pin1->pad_length - pin1->len);
			pin_len = pin1->pad_length;
		}

		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x20, 0, pin_cmd->pin_reference);
		apdu.data = pin_buff;
		apdu.datalen = pin_len;
		apdu.lc = pin_len;
	}
	else if ((card->reader->capabilities & SC_READER_CAP_PIN_PAD) && !pin1->data && !pin1->len)   {
		rv = authentic_chv_verify_pinpad(card, pin_cmd, tries_left);
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "authentic_chv_verify() authentic_chv_verify_pinpad returned %i", rv);
		LOGN_FUNC_RETURN(ctx, rv);
	}
	else   {
		LOGN_FUNC_RETURN(ctx, SC_ERROR_NOT_SUPPORTED);
	}

	rv = sc_transmit_apdu(card, &apdu);
	LOGN_TEST_RET(ctx, rv, "APDU transmit failed");

	if (apdu.sw1 == 0x63 && (apdu.sw2 & 0xF0) == 0xC0)   {
		pin1->tries_left = apdu.sw2 & 0x0F;
		if (tries_left)
			*tries_left = apdu.sw2 & 0x0F;
	}

	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);

	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_pin_is_verified(struct sc_card *card, struct sc_pin_cmd_data *pin_cmd_data,
		int *tries_left)
{
	struct sc_context *ctx = card->ctx;
	struct sc_pin_cmd_data pin_cmd;
	int rv;

	LOGN_FUNC_CALLED(ctx);

	if (pin_cmd_data->pin_type != SC_AC_CHV) 
		LOGN_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "PIN type is not supported for the verification");

	pin_cmd = *pin_cmd_data;
	pin_cmd.pin1.data = (unsigned char *)"";
	pin_cmd.pin1.len = 0;
		
	rv = authentic_chv_verify(card, &pin_cmd, tries_left);

	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_pin_verify(struct sc_card *card, struct sc_pin_cmd_data *pin_cmd)
{
	struct sc_context *ctx = card->ctx;
	struct authentic_private_data *prv_data = (struct authentic_private_data *) card->drv_data;
	unsigned char pin_sha1[SHA_DIGEST_LENGTH];
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "PIN(type:%X,reference:%X,data:%p,length:%i)", 
			pin_cmd->pin_type, pin_cmd->pin_reference, pin_cmd->pin1.data, pin_cmd->pin1.len);

	if (pin_cmd->pin1.data && !pin_cmd->pin1.len)   {
		pin_cmd->pin1.tries_left = -1;
		rv = authentic_pin_is_verified(card, pin_cmd, &pin_cmd->pin1.tries_left);
		LOGN_FUNC_RETURN(ctx, rv);
	}

	if (pin_cmd->pin1.data)
		SHA1(pin_cmd->pin1.data, pin_cmd->pin1.len, pin_sha1);
	else
		SHA1("", 0, pin_sha1);

	if (!memcmp(pin_sha1, prv_data->pins_sha1[pin_cmd->pin_reference], SHA_DIGEST_LENGTH))   {
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "Already verified");
		LOGN_FUNC_RETURN(ctx, SC_SUCCESS);
	}

	memset(prv_data->pins_sha1[pin_cmd->pin_reference], 0, sizeof(prv_data->pins_sha1[0]));

	rv = authentic_pin_get_policy(card, pin_cmd);
	LOGN_TEST_RET(ctx, rv, "Get 'PIN policy' error");

	if (pin_cmd->pin1.len > pin_cmd->pin1.max_length)
		LOGN_TEST_RET(ctx, SC_ERROR_INVALID_PIN_LENGTH, "PIN policy check failed");

	pin_cmd->pin1.tries_left = -1;	
	rv = authentic_chv_verify(card, pin_cmd, &pin_cmd->pin1.tries_left);
	LOGN_TEST_RET(ctx, rv, "PIN CHV verification error");

	memcpy(prv_data->pins_sha1[pin_cmd->pin_reference], pin_sha1, SHA_DIGEST_LENGTH);
	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_pin_change_pinpad(struct sc_card *card, unsigned reference, int *tries_left)
{
	struct sc_context *ctx = card->ctx;
	struct sc_pin_cmd_data pin_cmd;
	unsigned char pin1_data[SC_MAX_APDU_BUFFER_SIZE], pin2_data[SC_MAX_APDU_BUFFER_SIZE];
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "CHV PINPAD PIN reference %i", reference);

	if (!card->reader || !card->reader->ops || !card->reader->ops->perform_verify)   {
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "Reader not ready for PIN PAD");
		LOGN_FUNC_RETURN(ctx, SC_ERROR_READER);
	}

	memset(&pin_cmd, 0, sizeof(pin_cmd));
	pin_cmd.pin_type = SC_AC_CHV;
	pin_cmd.pin_reference = reference;
	pin_cmd.cmd = SC_PIN_CMD_CHANGE;
	pin_cmd.flags |= SC_PIN_CMD_USE_PINPAD | SC_PIN_CMD_NEED_PADDING;

	rv = authentic_pin_get_policy(card, &pin_cmd);
	LOGN_TEST_RET(ctx, rv, "Get 'PIN policy' error");
	
	memset(pin1_data, pin_cmd.pin1.pad_char, sizeof(pin1_data));
	pin_cmd.pin1.data = pin1_data;

	pin_cmd.pin1.len = pin_cmd.pin1.min_length;
	pin_cmd.pin1.max_length = 8;

	memcpy(&pin_cmd.pin2, &pin_cmd.pin1, sizeof(pin_cmd.pin1));
	memset(pin2_data, pin_cmd.pin2.pad_char, sizeof(pin2_data));
	pin_cmd.pin2.data = pin2_data;

	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "PIN1 lengths max/min/pad: %i/%i/%i", pin_cmd.pin1.max_length, pin_cmd.pin1.min_length, 
			pin_cmd.pin1.pad_length);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "PIN2 lengths max/min/pad: %i/%i/%i", pin_cmd.pin2.max_length, pin_cmd.pin2.min_length, 
			pin_cmd.pin2.pad_length);

	rv = iso_ops->pin_cmd(card, &pin_cmd, tries_left);

	LOGN_FUNC_RETURN(ctx, rv);
}

		
static int
authentic_pin_change(struct sc_card *card, struct sc_pin_cmd_data *data, int *tries_left)
{
	struct sc_context *ctx = card->ctx;
	struct authentic_private_data *prv_data = (struct authentic_private_data *) card->drv_data;
	struct sc_apdu apdu;
	unsigned char pin_data[SC_MAX_APDU_BUFFER_SIZE];
	size_t offs;
	int rv;

	rv = authentic_pin_get_policy(card, data);
	LOGN_TEST_RET(ctx, rv, "Get 'PIN policy' error");
	
	memset(prv_data->pins_sha1[data->pin_reference], 0, sizeof(prv_data->pins_sha1[0]));

	if (!data->pin1.data && !data->pin1.len && &data->pin2.data && !data->pin2.len)   {
		if (!(card->reader->capabilities & SC_READER_CAP_PIN_PAD))
			LOGN_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "PIN pad not supported");
		rv = authentic_pin_change_pinpad(card, data->pin_reference, tries_left);
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "authentic_pin_cmd(SC_PIN_CMD_CHANGE) chv_change_pinpad returned %i", rv);
		LOGN_FUNC_RETURN(ctx, rv);
	}

	if (card->max_send_size && data->pin1.len + data->pin2.len > card->max_send_size)
		LOGN_TEST_RET(ctx, SC_ERROR_INVALID_PIN_LENGTH, "APDU transmit failed");

	memset(pin_data, data->pin1.pad_char, sizeof(pin_data));
	offs = 0;
	if (data->pin1.data && data->pin1.len)   {
		memcpy(pin_data, data->pin1.data, data->pin1.len);
		offs += data->pin1.pad_length;
	}
	if (data->pin2.data && data->pin2.len)
		memcpy(pin_data + offs, data->pin2.data, data->pin2.len);

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x24, offs ? 0x00 : 0x01, data->pin_reference);
	apdu.data = pin_data;
	apdu.datalen = offs + data->pin1.pad_length;
	apdu.lc = offs + data->pin1.pad_length;
			
	rv = sc_transmit_apdu(card, &apdu);
	LOGN_TEST_RET(ctx, rv, "APDU transmit failed");
	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);

	LOGN_FUNC_RETURN(ctx, rv);
}
	

static int
authentic_chv_set_pinpad(struct sc_card *card, unsigned char reference)
{
	struct sc_context *ctx = card->ctx;
	struct sc_pin_cmd_data pin_cmd;
	unsigned char pin_data[0x100];
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "Set CHV PINPAD PIN reference %i", reference);

	if (!card->reader || !card->reader->ops || !card->reader->ops->perform_verify)   {
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "Reader not ready for PIN PAD");
		LOGN_FUNC_RETURN(ctx, SC_ERROR_READER);
	}

	memset(&pin_cmd, 0, sizeof(pin_cmd));
	pin_cmd.pin_type = SC_AC_CHV;
	pin_cmd.pin_reference = reference;
	pin_cmd.cmd = SC_PIN_CMD_UNBLOCK;
	pin_cmd.flags |= SC_PIN_CMD_USE_PINPAD | SC_PIN_CMD_NEED_PADDING;

	rv = authentic_pin_get_policy(card, &pin_cmd);
	LOGN_TEST_RET(ctx, rv, "Get 'PIN policy' error");

	memset(pin_data, pin_cmd.pin1.pad_char, sizeof(pin_data));
	pin_cmd.pin1.data = pin_data;

	pin_cmd.pin1.len = pin_cmd.pin1.min_length;
        pin_cmd.pin1.max_length = 8;

	memcpy(&pin_cmd.pin2, &pin_cmd.pin1, sizeof(pin_cmd.pin1));
	memset(&pin_cmd.pin1, 0, sizeof(pin_cmd.pin1));

	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "PIN2 max/min/pad %i/%i/%i", 
			pin_cmd.pin2.max_length, pin_cmd.pin2.min_length, pin_cmd.pin2.pad_length);
	rv = iso_ops->pin_cmd(card, &pin_cmd, NULL);

	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_pin_get_policy (struct sc_card *card, struct sc_pin_cmd_data *data)   
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu apdu;
	unsigned char rbuf[0x100];
	int ii, rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "get PIN(type:%X,ref:%X)", data->pin_type, data->pin_reference);
  
	sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0xCA, 0x5F, data->pin_reference);
	for (ii=0;ii<2;ii++)   {
        	apdu.le = sizeof(rbuf);
        	apdu.resp = rbuf;
        	apdu.resplen = sizeof(rbuf);

        	rv = sc_transmit_apdu(card, &apdu);
        	LOGN_TEST_RET(ctx, rv, "APDU transmit failed");
        	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);

		if (rv != SC_ERROR_CLASS_NOT_SUPPORTED)
			break;

		apdu.cla = 0x80;
	}
        LOGN_TEST_RET(ctx, rv, "'GET DATA' error");

	rv = authentic_parse_credential_data(ctx, data, apdu.resp, apdu.resplen);
        LOGN_TEST_RET(ctx, rv, "Cannot parse credential data");

	data->pin1.encoding = SC_PIN_ENCODING_ASCII;
	data->pin1.offset = 5;
	data->pin1.pad_char = 0xFF;
	data->pin1.pad_length = data->pin1.max_length;

	data->flags |= SC_PIN_CMD_NEED_PADDING;

	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "PIN policy: size max/min/pad %i/%i/%i, tries max/left %i/%i",
				data->pin1.max_length, data->pin1.min_length, data->pin1.pad_length,
				data->pin1.max_tries, data->pin1.tries_left);

	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_pin_reset(struct sc_card *card, struct sc_pin_cmd_data *data, int *tries_left)
{
	struct sc_context *ctx = card->ctx;
	struct authentic_private_data *prv_data = (struct authentic_private_data *) card->drv_data;
	struct sc_file *save_current = NULL;
	struct sc_pin_cmd_data pin_cmd, puk_cmd;
	struct sc_apdu apdu;
	unsigned reference;
	int rv, ii;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "reset PIN (ref:%i,lengths %i/%i)", data->pin_reference, data->pin1.len, data->pin2.len);
 
	memset(prv_data->pins_sha1[data->pin_reference], 0, sizeof(prv_data->pins_sha1[0]));

	memset(&pin_cmd, 0, sizeof(pin_cmd));
	pin_cmd.pin_reference = data->pin_reference;
	pin_cmd.pin_type = data->pin_type;

	rv = authentic_pin_get_policy(card, &pin_cmd);
	LOGN_TEST_RET(ctx, rv, "Get 'PIN policy' error");

	if (pin_cmd.pin1.acls[AUTHENTIC_ACL_NUM_PIN_RESET].method == SC_AC_CHV)   {
		for (ii=0;ii<8;ii++)   {
			unsigned char mask = 0x01 << ii;
			if (pin_cmd.pin1.acls[AUTHENTIC_ACL_NUM_PIN_RESET].key_ref & mask)   {
				memset(&puk_cmd, 0, sizeof(puk_cmd));
				puk_cmd.pin_reference = ii + 1;

				rv = authentic_pin_get_policy(card, &puk_cmd);
				LOGN_TEST_RET(ctx, rv, "Get 'PIN policy' error");

				if (puk_cmd.pin_type == SC_AC_CHV)
					break;
			}
		}
		if (ii < 8)   {
			puk_cmd.pin1.data = data->pin1.data;
			puk_cmd.pin1.len = data->pin1.len;

			rv = authentic_pin_verify(card, &puk_cmd);

			if (tries_left && rv == SC_ERROR_PIN_CODE_INCORRECT)
				*tries_left = puk_cmd.pin1.tries_left;

			LOGN_TEST_RET(ctx, rv, "Cannot verify PUK");
		}
	}

	reference = data->pin_reference;
	if (data->pin2.len)   {
		unsigned char pin_data[SC_MAX_APDU_BUFFER_SIZE];

		memset(pin_data, pin_cmd.pin1.pad_char, sizeof(pin_data));
		memcpy(pin_data, data->pin2.data, data->pin2.len);

		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x2C, 0x02, reference);
		apdu.data = pin_data;
		apdu.datalen = pin_cmd.pin1.pad_length;
		apdu.lc = pin_cmd.pin1.pad_length;
			
		rv = sc_transmit_apdu(card, &apdu);
		LOGN_TEST_RET(ctx, rv, "APDU transmit failed");
		rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
		LOGN_TEST_RET(ctx, rv, "PIN cmd failed");
	}
	else if (data->pin2.data) {
		sc_format_apdu(card, &apdu, SC_APDU_CASE_1, 0x2C, 3, reference);
			
		rv = sc_transmit_apdu(card, &apdu);
		LOGN_TEST_RET(ctx, rv, "APDU transmit failed");
		rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
		LOGN_TEST_RET(ctx, rv, "PIN cmd failed");
	}
	else   {
		rv = authentic_chv_set_pinpad(card, reference);
		LOGN_TEST_RET(ctx, rv, "Failed to set PIN with pin-pad");
	}

	if (save_current)   {
		struct sc_file *dummy_file = NULL;

		rv = authentic_select_file(card, &save_current->path, &dummy_file);
		LOGN_TEST_RET(ctx, rv, "Cannot return to saved PATH");
	}
	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_pin_cmd(struct sc_card *card, struct sc_pin_cmd_data *data, int *tries_left)
{
	struct sc_context *ctx = card->ctx;
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "PIN-CMD:%X,PIN(type:%X,ret:%i),PIN1(%p,len:%i),PIN2(%p,len:%i)", data->cmd, data->pin_type, 
			data->pin_reference, data->pin1.data, data->pin1.len, data->pin2.data, data->pin2.len);
  
	switch (data->cmd)   {
	case SC_PIN_CMD_VERIFY:
		rv = authentic_pin_verify(card, data);
		break;
	case SC_PIN_CMD_CHANGE:
		rv = authentic_pin_change(card, data, tries_left);
		break;
	case SC_PIN_CMD_UNBLOCK:
		rv = authentic_pin_reset(card, data, tries_left);
		break;
	case SC_PIN_CMD_GET_INFO:
		rv = authentic_pin_get_policy(card, data);
		break;
	default:
		LOGN_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Unupported PIN command");
	}

	if (rv == SC_ERROR_PIN_CODE_INCORRECT && tries_left)
		*tries_left = data->pin1.tries_left;

	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_get_serialnr(struct sc_card *card, struct sc_serial_number *serial)
{
	struct sc_context *ctx = card->ctx;
	struct authentic_private_data *prv_data = (struct authentic_private_data *) card->drv_data;
	int rv;

	LOGN_FUNC_CALLED(ctx);
	if (!card->serialnr.len)   {
		rv = authentic_get_cplc(card);
		LOGN_TEST_RET(ctx, rv, "get CPLC data error");

		card->serialnr.len = 4;
		memcpy(card->serialnr.value, prv_data->cplc.value + 15, 4);
	
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "serial %02X%02X%02X%02X", 
				card->serialnr.value[0], card->serialnr.value[1], 
				card->serialnr.value[2], card->serialnr.value[3]);
	}

	if (serial)
		memcpy(serial, &card->serialnr, sizeof(*serial));

	LOGN_FUNC_RETURN(ctx, SC_SUCCESS);
}


/* 'GET CHALLENGE' returns always 24 bytes */
static int 
authentic_get_challenge(struct sc_card *card, unsigned char *rnd, size_t len)
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu apdu;
	unsigned char rbuf[0x18];
	int rv, nn;

	LOGN_FUNC_CALLED(ctx);
	if (!rnd)
		return SC_ERROR_INVALID_ARGUMENTS;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0x84, 0x00, 0x00);
	apdu.resp = rbuf;
	apdu.resplen = sizeof(rbuf);
	apdu.le = sizeof(rbuf);

	while (len > 0) {
		rv = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, rv, "APDU transmit failed");
		rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
		LOGN_TEST_RET(ctx, rv, "PIN cmd failed");

		if (apdu.resplen != sizeof(rbuf))
			return sc_check_sw(card, apdu.sw1, apdu.sw2);

		nn = len > apdu.resplen ? apdu.resplen : len;
		memcpy(rnd, apdu.resp, nn);
		len -= nn;
		rnd += nn;
	}

	LOGN_FUNC_RETURN(ctx, SC_SUCCESS);
}


int
authentic_manage_sdo_encode_prvkey(struct sc_card *card, struct sc_pkcs15_prkey *prvkey,
			unsigned char **out, size_t *out_len)
{
	struct sc_context *ctx = card->ctx;
	struct sc_pkcs15_prkey_rsa rsa; 
	unsigned char *blob = NULL, *blob01 = NULL;
	size_t blob_len = 0, blob01_len = 0;
	int rv;

	if (!prvkey || !out || !out_len)
		LOGN_TEST_RET(ctx, SC_ERROR_INVALID_ARGUMENTS, "Invalid arguments");
	if (prvkey->algorithm != SC_ALGORITHM_RSA)
		LOGN_TEST_RET(ctx, SC_ERROR_INVALID_DATA, "Invalid SDO operation");

	rsa = prvkey->u.rsa; 
	/* Encode private RSA key part */
	rv = authentic_update_blob(ctx, AUTHENTIC_TAG_RSA_PRIVATE_P, rsa.p.data, rsa.p.len, &blob, &blob_len);
	LOGN_TEST_RET(ctx, rv, "SDO RSA P encode error");

	rv = authentic_update_blob(ctx, AUTHENTIC_TAG_RSA_PRIVATE_Q, rsa.q.data, rsa.q.len, &blob, &blob_len);
	LOGN_TEST_RET(ctx, rv, "SDO RSA Q encode error");

	rv = authentic_update_blob(ctx, AUTHENTIC_TAG_RSA_PRIVATE_PQ, rsa.iqmp.data, rsa.iqmp.len, &blob, &blob_len);
	LOGN_TEST_RET(ctx, rv, "SDO RSA PQ encode error");

	rv = authentic_update_blob(ctx, AUTHENTIC_TAG_RSA_PRIVATE_DP1, rsa.dmp1.data, rsa.dmp1.len, &blob, &blob_len);
	LOGN_TEST_RET(ctx, rv, "SDO RSA DP1 encode error");

	rv = authentic_update_blob(ctx, AUTHENTIC_TAG_RSA_PRIVATE_DQ1, rsa.dmq1.data, rsa.dmq1.len, &blob, &blob_len);
	LOGN_TEST_RET(ctx, rv, "SDO RSA DQ1 encode error");

	rv = authentic_update_blob(ctx, AUTHENTIC_TAG_RSA_PRIVATE, blob, blob_len, &blob01, &blob01_len);
	LOGN_TEST_RET(ctx, rv, "SDO RSA Private encode error");

	free (blob);
	blob = NULL;
	blob_len = 0;

	/* Encode public RSA key part */
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "modulus.len:%i blob_len:%i", rsa.modulus.len, blob_len);
	rv = authentic_update_blob(ctx, AUTHENTIC_TAG_RSA_PUBLIC_MODULUS, rsa.modulus.data, rsa.modulus.len, &blob, &blob_len);
	LOGN_TEST_RET(ctx, rv, "SDO RSA Modulus encode error");

	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "exponent.len:%i blob_len:%i", rsa.exponent.len, blob_len);
	rv = authentic_update_blob(ctx, AUTHENTIC_TAG_RSA_PUBLIC_EXPONENT, rsa.exponent.data, rsa.exponent.len, &blob, &blob_len);
	LOGN_TEST_RET(ctx, rv, "SDO RSA Exponent encode error");

	rv = authentic_update_blob(ctx, AUTHENTIC_TAG_RSA_PUBLIC, blob, blob_len, &blob01, &blob01_len);
	LOGN_TEST_RET(ctx, rv, "SDO RSA Private encode error");

	free (blob);

	rv = authentic_update_blob(ctx, AUTHENTIC_TAG_RSA, blob01, blob01_len, out, out_len);
	LOGN_TEST_RET(ctx, rv, "SDO RSA encode error");

	free(blob01);

	LOGN_FUNC_RETURN(ctx, rv);
}


int
authentic_manage_sdo_encode(struct sc_card *card, struct sc_authentic_sdo *sdo, unsigned long cmd,
			unsigned char **out, size_t *out_len)
{
	struct sc_context *ctx = card->ctx;
	unsigned char *data = NULL;
	size_t data_len = 0;
	unsigned char data_tag = AUTHENTIC_TAG_DOCP;
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "encode SDO operation (cmd:%lX,mech:%X,id:%X)", cmd, sdo->docp.mech, sdo->docp.id);

	if (!out || !out_len)
		LOGN_TEST_RET(ctx, SC_ERROR_INVALID_ARGUMENTS, "Invalid arguments");

	rv = authentic_update_blob(ctx, AUTHENTIC_TAG_DOCP_MECH, &sdo->docp.mech, sizeof(sdo->docp.mech), 
			&data, &data_len);
	LOGN_TEST_RET(ctx, rv, "DOCP MECH encode error");

	rv = authentic_update_blob(ctx, AUTHENTIC_TAG_DOCP_ID, &sdo->docp.id, sizeof(sdo->docp.id),
			&data, &data_len);
	LOGN_TEST_RET(ctx, rv, "DOCP ID encode error");

	if (cmd == SC_CARDCTL_AUTHENTIC_SDO_CREATE)   {
		rv = authentic_update_blob(ctx, AUTHENTIC_TAG_DOCP_ACLS, sdo->docp.acl_data, sdo->docp.acl_data_len, 
				&data, &data_len);
		LOGN_TEST_RET(ctx, rv, "DOCP ACLs encode error");

		if (sdo->docp.security_parameter)  {
			rv = authentic_update_blob(ctx, AUTHENTIC_TAG_DOCP_SCP, 
					&sdo->docp.security_parameter, sizeof(sdo->docp.security_parameter), 
					&data, &data_len);
			LOGN_TEST_RET(ctx, rv, "DOCP ACLs encode error");
		}
		if (sdo->docp.usage_counter[0] || sdo->docp.usage_counter[1])  {
			rv = authentic_update_blob(ctx, AUTHENTIC_TAG_DOCP_USAGE_COUNTER, 
					sdo->docp.usage_counter, sizeof(sdo->docp.usage_counter), 
					&data, &data_len);
			LOGN_TEST_RET(ctx, rv, "DOCP ACLs encode error");
		}
	}
	else if (cmd == SC_CARDCTL_AUTHENTIC_SDO_STORE)   {
		if (sdo->docp.mech == AUTHENTIC_MECH_CRYPTO_RSA1024 
				|| sdo->docp.mech == AUTHENTIC_MECH_CRYPTO_RSA1280
				|| sdo->docp.mech == AUTHENTIC_MECH_CRYPTO_RSA1536 
				|| sdo->docp.mech == AUTHENTIC_MECH_CRYPTO_RSA1792
				|| sdo->docp.mech == AUTHENTIC_MECH_CRYPTO_RSA2048)   {
			rv = authentic_manage_sdo_encode_prvkey(card, sdo->data.prvkey, &data, &data_len);
			LOGN_TEST_RET(ctx, rv, "SDO RSA encode error");
		}
		else  {
			LOGN_TEST_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Cryptographic object unsupported for encoding");
		}
	}
	else if (cmd == SC_CARDCTL_AUTHENTIC_SDO_GENERATE)   {
		if (sdo->data.prvkey)   {
		        rv = authentic_update_blob(ctx, AUTHENTIC_TAG_RSA_PUBLIC_EXPONENT, 
					sdo->data.prvkey->u.rsa.exponent.data, sdo->data.prvkey->u.rsa.exponent.len, 
					&data, &data_len);
		        LOGN_TEST_RET(ctx, rv, "SDO RSA Exponent encode error");
		}

		data_tag = AUTHENTIC_TAG_RSA_GENERATE_DATA;
	}
	else if (cmd != SC_CARDCTL_AUTHENTIC_SDO_DELETE)   {
		LOGN_TEST_RET(ctx, SC_ERROR_INVALID_DATA, "Invalid SDO operation");
	}

	rv = authentic_update_blob(ctx, data_tag, data, data_len, out, out_len);
	LOGN_TEST_RET(ctx, rv, "SDO DOCP encode error");

	free(data);

	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "encoded SDO operation data %s", sc_dump_hex(*out, *out_len));
	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_manage_sdo_generate(struct sc_card *card, struct sc_authentic_sdo *sdo)
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu apdu;
	unsigned char rbuf[0x400];
	unsigned char *data = NULL;
	size_t data_len = 0;
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "Generate SDO(mech:%X,id:%X)",  sdo->docp.mech, sdo->docp.id);

	rv = authentic_manage_sdo_encode(card, sdo, SC_CARDCTL_AUTHENTIC_SDO_GENERATE, &data, &data_len);
	LOGN_TEST_RET(ctx, rv, "Cannot encode SDO data");
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "encoded SDO length %i", data_len);

	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0x47, 0x00, 0x00);
	apdu.data = data;
	apdu.datalen = data_len;
	apdu.lc = data_len;
	apdu.resp = rbuf;
        apdu.resplen = sizeof(rbuf);
	apdu.le = 0x100;

	rv = sc_transmit_apdu(card, &apdu);
	LOGN_TEST_RET(ctx, rv, "APDU transmit failed");
	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
	LOGN_TEST_RET(ctx, rv, "authentic_sdo_create() SDO put data error");

	rv = authentic_decode_pubkey_rsa(ctx, apdu.resp, apdu.resplen, &sdo->data.prvkey);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, rv, "cannot decode public key");

	free(data);
	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_manage_sdo(struct sc_card *card, struct sc_authentic_sdo *sdo, unsigned long cmd)
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu apdu;
	unsigned char rbuf[0x400];
	unsigned char *data = NULL;
	size_t data_len = 0, save_max_send = card->max_send_size;
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "SDO(cmd:%lX,mech:%X,id:%X)", cmd, sdo->docp.mech, sdo->docp.id);

	rv = authentic_manage_sdo_encode(card, sdo, cmd, &data, &data_len);
	LOGN_TEST_RET(ctx, rv, "Cannot encode SDO data");
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "encoded SDO length %i", data_len);

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xDB, 0x3F, 0xFF);
	apdu.data = data;
	apdu.datalen = data_len;
	apdu.lc = data_len;
	apdu.flags |= SC_APDU_FLAGS_CHAINING;

	if (card->max_send_size > 255)
		card->max_send_size = 255;
	rv = sc_transmit_apdu(card, &apdu);
	card->max_send_size = save_max_send;
	LOGN_TEST_RET(ctx, rv, "APDU transmit failed");

	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
	LOGN_TEST_RET(ctx, rv, "authentic_sdo_create() SDO put data error");

	free(data);
	LOGN_FUNC_RETURN(ctx, rv);
}


static int
authentic_card_ctl(struct sc_card *card, unsigned long cmd, void *ptr)
{
	struct sc_context *ctx = card->ctx;
	struct sc_authentic_sdo *sdo = (struct sc_authentic_sdo *) ptr;

	switch (cmd) {
	case SC_CARDCTL_GET_SERIALNR:
		return authentic_get_serialnr(card, (struct sc_serial_number *)ptr);
	case SC_CARDCTL_AUTHENTIC_SDO_CREATE:
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "CARDCTL SDO_CREATE: sdo(mech:%X,id:%X)", sdo->docp.mech, sdo->docp.id);
		return authentic_manage_sdo(card, (struct sc_authentic_sdo *) ptr, cmd);
	case SC_CARDCTL_AUTHENTIC_SDO_DELETE:
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "CARDCTL SDO_DELETE: sdo(mech:%X,id:%X)", sdo->docp.mech, sdo->docp.id);
		return authentic_manage_sdo(card, (struct sc_authentic_sdo *) ptr, cmd);
	case SC_CARDCTL_AUTHENTIC_SDO_STORE:
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "CARDCTL SDO_STORE: sdo(mech:%X,id:%X)", sdo->docp.mech, sdo->docp.id);
		return authentic_manage_sdo(card, (struct sc_authentic_sdo *) ptr, cmd);
	case SC_CARDCTL_AUTHENTIC_SDO_GENERATE:
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "CARDCTL SDO_GENERATE: sdo(mech:%X,id:%X)", sdo->docp.mech, sdo->docp.id);
		return authentic_manage_sdo_generate(card, (struct sc_authentic_sdo *) ptr);
	}
	return SC_ERROR_NOT_SUPPORTED;
}


static int 
authentic_set_security_env(struct sc_card *card,
		const struct sc_security_env *env, int se_num)   
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu apdu;
	unsigned char cse_crt_dst[] = {
		0x80, 0x01, AUTHENTIC_ALGORITHM_RSA_PKCS1,
		0x83, 0x01, env->key_ref[0] & ~AUTHENTIC_OBJECT_REF_FLAG_LOCAL, 
	};
	unsigned char cse_crt_ct[] = {
		0x80, 0x01, AUTHENTIC_ALGORITHM_RSA_PKCS1,
		0x83, 0x01, env->key_ref[0] & ~AUTHENTIC_OBJECT_REF_FLAG_LOCAL, 
	};
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "set SE#%i(op:0x%X,algo:0x%X,algo_ref:0x%X,flags:0x%X), key_ref:0x%X", 
			se_num, env->operation, env->algorithm, env->algorithm_ref, env->algorithm_flags, env->key_ref[0]);
	switch (env->operation)  {
	case SC_SEC_OPERATION_SIGN:
		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, 0x41, AUTHENTIC_TAG_CRT_DST);
		apdu.data = cse_crt_dst;
		apdu.datalen = sizeof(cse_crt_dst);
		apdu.lc = sizeof(cse_crt_dst);
		break;
	case SC_SEC_OPERATION_DECIPHER:
		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, 0x41, AUTHENTIC_TAG_CRT_CT);
		apdu.data = cse_crt_ct;
		apdu.datalen = sizeof(cse_crt_ct);
		apdu.lc = sizeof(cse_crt_ct);
		break;
	default:
		LOGN_FUNC_RETURN(ctx, SC_ERROR_NOT_SUPPORTED);
	}
#if 0
	apdu.flags |= SC_APDU_FLAGS_CAN_WAIT;
#endif

	rv = sc_transmit_apdu(card, &apdu);
	LOGN_TEST_RET(ctx, rv, "APDU transmit failed");
	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
	LOGN_TEST_RET(ctx, rv, "MSE restore error");

	LOGN_FUNC_RETURN(ctx, rv);
}


static int 
authentic_decipher(struct sc_card *card, const unsigned char *in, size_t in_len,
		unsigned char *out, size_t out_len)
{
	struct sc_context *ctx = card->ctx;
	struct sc_apdu apdu;
	unsigned char resp[SC_MAX_APDU_BUFFER_SIZE];
	int rv;

	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "crgram_len %i;  outlen %i\n", in_len, out_len);
	if (!out || !out_len || in_len > SC_MAX_APDU_BUFFER_SIZE) 
		LOGN_FUNC_RETURN(ctx, SC_ERROR_INVALID_ARGUMENTS);
	
	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0x2A, 0x80, 0x86);
	apdu.flags |= SC_APDU_FLAGS_CHAINING;
	apdu.data = in;
	apdu.datalen = in_len;
	apdu.lc = in_len;
	apdu.resp = resp;
	apdu.resplen = sizeof(resp);
	apdu.le = in_len - (in_len % 8);
	
	rv = sc_transmit_apdu(card, &apdu);
	LOGN_TEST_RET(ctx, rv, "APDU transmit failed");
	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
	LOGN_TEST_RET(ctx, rv, "Card returned error");

	if (out_len > apdu.resplen)
		out_len = apdu.resplen;
	
	memcpy(out, apdu.resp, out_len);
	rv = out_len;

	LOGN_FUNC_RETURN(ctx, rv);
}


/*  
 *  Remote Access 
 */
#if 0
static int
authentic_encode_answer(struct sc_context *ctx, struct sc_remote_apdu *in_apdus, 
		unsigned char *out, size_t *out_len)
{
	struct sc_remote_apdu *p_apdu = in_apdus;
	char ticket[SC_MAX_APDU_BUFFER_SIZE * 2 + 32];
	char *answer = NULL;
	unsigned sw = 0;
	int ii;

	printf("TODO: use ASN1 encoder\n");
	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "encode_answer: out %p/%p", out, out_len);

	answer = calloc(1, 16);
	if (!answer)
		LOGN_TEST_RET(ctx, SC_ERROR_MEMORY_FAILURE, "encode_answer: answer allocate failed");
	strcpy(answer, "DATA=");

	memset(ticket, 0, sizeof(ticket));
	for (p_apdu = in_apdus, ii=0; p_apdu; p_apdu = p_apdu->next, ii++)   {
		unsigned char tmp_buff[0x200];
		size_t len, offs = sizeof(tmp_buff);

		sw = p_apdu->apdu.sw1 * 0x100 + p_apdu->apdu.sw2;

		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "%i: encode_answer: SW %X; resplen %i", ii, sw, p_apdu->apdu.resplen);
		if (p_apdu->apdu.resplen)   {
			offs -= p_apdu->apdu.resplen;
			memcpy(tmp_buff + offs--, p_apdu->apdu.resp, p_apdu->apdu.resplen);

			*(tmp_buff + offs--) = p_apdu->apdu.resplen & 0xFF;
			if (p_apdu->apdu.resplen > 0xFF)
				*(tmp_buff + offs--) = (p_apdu->apdu.resplen >> 8) & 0xFF;

			if (p_apdu->apdu.resplen > 0xFF)
				*(tmp_buff + offs--) = 0x82;
			else if (p_apdu->apdu.resplen > 0x7F)
				*(tmp_buff + offs--) = 0x81;

			*(tmp_buff + offs--) = SM_RESPONSE_CONTEXT_DATA_TAG;
		}
		else   {
			offs--;
		}

		*(tmp_buff + offs--) = p_apdu->apdu.sw2;
		*(tmp_buff + offs--) = p_apdu->apdu.sw1;
		if (p_apdu->apdu.sw1 & 0x80)   {
			*(tmp_buff + offs--) = 0x00;
			*(tmp_buff + offs--) = 3;
		}
		else   {
			*(tmp_buff + offs--) = 2;
		}
		*(tmp_buff + offs--) = SC_ASN1_TAG_INTEGER;

		*(tmp_buff + offs--) = ii + 1;
		*(tmp_buff + offs--) = 1;
		*(tmp_buff + offs--) = SC_ASN1_TAG_INTEGER;

		len = sizeof(tmp_buff) - offs - 1;

		*(tmp_buff + offs--) = len & 0xFF;

		if (len >= 0x80 && len < 0x100)   {
			*(tmp_buff + offs--) = 0x81;
		}
		else   if (len >= 0x100) {
			*(tmp_buff + offs--) = (len > 8) & 0xFF;
			*(tmp_buff + offs--) = 0x82;
		}

		*(tmp_buff + offs) = SM_RESPONSE_CONTEXT_TAG;

		len = sizeof(tmp_buff) - offs;
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "%i: EncodedAnswer(%i) %s\n", ii, len, sc_dump_hex(tmp_buff + offs, len));
		
		answer = realloc(answer, strlen(answer) + len*2 + 16);
		if (!answer)
			LOGN_TEST_RET(ctx, SC_ERROR_MEMORY_FAILURE, "encode_answer: answer (re)allocate failed");

		sprintf(answer + strlen(answer), "%s", sc_dump_hex(tmp_buff + offs, len));
	}

	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "encode_answer: SW:%X,ticket:%s", sw, ticket);
	if (strlen(ticket))   {
		answer = realloc(answer, strlen(answer) + strlen(ticket) + 16);
		if (!answer)
			LOGN_TEST_RET(ctx, SC_ERROR_MEMORY_FAILURE, "encode_answer: answer (re)allocate for ticket failed");
		sprintf(answer + strlen(answer), ";%s", ticket);
	}

	answer = realloc(answer, strlen(answer) + 16);
	if (!answer)
		LOGN_TEST_RET(ctx, SC_ERROR_MEMORY_FAILURE, "encode_answer: answer (re)allocate failed");
	sprintf(answer + strlen(answer), ";SW=%04X", sw);

	if (out && out_len)   {
		if (strlen(answer) + 1 > *out_len)   {
			sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "encode_answer: buffer too small: need %i, have %i", strlen(answer) + 1, *out_len);
			LOGN_FUNC_RETURN(ctx, SC_ERROR_BUFFER_TOO_SMALL);
		}

		strcpy((char *)out, answer);
		*out_len = strlen(answer);
	}
	else if (out_len)   {
		*out_len = 0;
	}

	free(answer);

	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "encode_answer: returns '%s'", out);
	LOGN_FUNC_RETURN(ctx, SC_SUCCESS);
}


static int 
authentic_external_apdus(struct sc_card *card, char *encoded_apdus, 
		unsigned char *out, size_t *out_len)
{
	struct sc_context *ctx = card->ctx;
	int rv = 0, rvv = 0;
	size_t _out_len = 0;

	printf("TODO: implement non card specific procedures -- common procedures for IAS/ECC and AuthentIC\n");
	LOGN_FUNC_CALLED(ctx);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "external_apdus: out(%p) %p", out_len, out);
	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "external_apdus: encoded_apdus:'%s'", encoded_apdus);

	if (out && out_len)   {
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "external_apdus: out_len:%i", *out_len);
		memset(out, 0, *out_len);
		_out_len = *out_len;
	}

	do   {
		struct sc_remote_apdu *remote_apdus = NULL, *current = NULL;
		char header[0x200];

		if (encoded_apdus && strlen(encoded_apdus))   {
			rv = sc_hash_decode_remote_apdus(card->ctx, encoded_apdus, &remote_apdus);
			if (rv)
				break;

			if (!remote_apdus)
				break;

			current = remote_apdus;
			while(current)   {
				rv = sc_transmit_apdu(card, &current->apdu);
				if (rv)   {
					sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "external_apdus: transmit APDU error %i", rv);
					break;
				}

	  			rv = sc_check_sw(card, current->apdu.sw1, current->apdu.sw2);
				if (rv && !current->fatal)   {
					sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "external_apdus: non fatal APDU error %i", rv);
					rv = 0;
				}

				if (rv)   {
					sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "external_apdus: APDU error %i", rv);
					break;
				}

				current = current->next;
			}
		
			sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "external_apdus: out before encode %p/%p", out, out_len);
			rvv = authentic_encode_answer(card->ctx, remote_apdus, out, out_len);
			if (rvv)   {   
				sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "external_apdus: encode answer error %i", rvv);
				if (rv!=0)
					rv = rvv;
			}

			sc_remote_apdu_free(remote_apdus);
			remote_apdus = NULL;
		}

		memset(header, 0, sizeof(header));
		sprintf(header + strlen(header), "SERIAL=%s;", sc_dump_hex(card->serialnr.value,card->serialnr.len));

		if (out && out_len)   {
			int str_len = strlen((char *)out);

			if (strlen(header) > _out_len - str_len)   {
				sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "external_apdus: unsufficient buffer; need/have %i/%i", 
						strlen(header) + str_len, _out_len);
				LOGN_TEST_RET(card->ctx, SC_ERROR_BUFFER_TOO_SMALL, "authentic_external_apdus() buffer too small");
			}

			sprintf((char *)(out + str_len), "%s%s", str_len ? ";" : "", header);
		}

		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "external_apdus returns '%s'", out);
	} while (0);

	LOGN_FUNC_RETURN(ctx, rv);
}
#endif

static int
authentic_finish(struct sc_card *card)
{
	struct sc_context *ctx = card->ctx;

	LOGN_FUNC_CALLED(ctx);
	if (card->drv_data)
		free(card->drv_data);
	card->drv_data = NULL;
	LOGN_FUNC_RETURN(ctx, SC_SUCCESS);
}


static struct sc_card_driver *
sc_get_driver(void)
{
	struct sc_card_driver *iso_drv = sc_get_iso7816_driver();

	if (!iso_ops)
		iso_ops = iso_drv->ops;

	authentic_ops = *iso_ops;

	authentic_ops.match_card = authentic_match_card;
	authentic_ops.init = authentic_init;
	authentic_ops.finish = authentic_finish;
	/*
	authentic_ops.read_binary = authentic_read_binary;
	authentic_ops.write_binary = authentic_write_binary;
	authentic_ops.update_binary = authentic_update_binary;
	*/
	authentic_ops.erase_binary = authentic_erase_binary;
	/* authentic_ops.resize_file = authentic_resize_file; */
	authentic_ops.select_file = authentic_select_file;
	/* get_response: Untested */
	authentic_ops.get_challenge = authentic_get_challenge;
	authentic_ops.set_security_env = authentic_set_security_env;
	/* decipher: Untested */
	authentic_ops.decipher = authentic_decipher;
	/* authentic_ops.compute_signature = authentic_compute_signature; */
	authentic_ops.create_file = authentic_create_file;
	authentic_ops.delete_file = authentic_delete_file;
	authentic_ops.card_ctl = authentic_card_ctl;
	authentic_ops.process_fci = authentic_process_fci;
	authentic_ops.pin_cmd = authentic_pin_cmd;

	/*
	authentic_ops.external_apdus = authentic_external_apdus;
	authentic_ops.update_binary_long = authentic_update_binary_long;
	authentic_ops.read_binary_long = authentic_read_binary_long;
	*/
	return &authentic_drv;
}

struct sc_card_driver *
sc_get_authentic_driver(void)
{
	return sc_get_driver();
}