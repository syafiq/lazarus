/*
 * Copyright(c) 2021 Fraunhofer AISEC
 * Fraunhofer-Gesellschaft zur Foerderung der angewandten Forschung e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef MBEDTLS_CONFIG_FILE

#include MBEDTLS_CONFIG_FILE

#ifdef MBEDTLS_ECDSA_C

#include "mbedtls/ecdsa.h"
#include "mbedtls/pk.h"
#include "mbedtls/hmac_drbg.h"

#include "lz_config.h"
#include "lz_crypto_common.h"
#include "lz_sha256.h"
#include "lz_ecc.h"
#include "lz_ecdsa.h"

int lz_ecdsa_sign(uint8_t *data, size_t data_length, lz_ecc_keypair *key_pair,
				  lz_ecc_signature *sig)
{
	int re = 0;

	// We first hash the message
	uint8_t hash[SHA256_DIGEST_LENGTH];
	CHECK(lz_sha256(hash, data, data_length), "Could not hash message");

	// And then sign the hash
	sig->length = 0;
	CHECK(mbedtls_pk_sign(key_pair, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig->sig,
						  (size_t *)&sig->length, lz_rand, 0),
		  "Could not sign message");

clean:
	return re;
}

int lz_ecdsa_sign_pem(uint8_t *data, size_t data_length, lz_ecc_priv_key_pem *key,
					  lz_ecc_signature *sig)
{
	int re = 0;
	lz_ecc_keypair keypair;
	CHECK(lz_pem_to_priv_key(&keypair, key), "Could not import private key.");

	CHECK(lz_ecdsa_sign(data, data_length, &keypair, sig), "Could not sign message");

clean:
	lz_free_keypair(&keypair);
	return re;
}

int lz_ecdsa_verify(uint8_t *data, size_t data_length, lz_ecc_keypair *key_pair,
					lz_ecc_signature *sig)
{
	int re = 0;

	// We first hash the message
	uint8_t hash[SHA256_DIGEST_LENGTH];
	CHECK(lz_sha256(hash, data, data_length), "Could not hash message");

	// And then verify the hash
	// TODO: Remove the CHECK from here (and just return something other than 0)
	CHECK(mbedtls_pk_verify(key_pair, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig->sig, sig->length),
		  "Could not verify message");

clean:
	return re;
}

int lz_ecdsa_verify_pub(uint8_t *data, size_t data_length, lz_ecc_keypair *keypair,
						lz_ecc_signature *sig)
{
	int re = 0;

	uint8_t hash[SHA256_DIGEST_LENGTH];
	CHECK(lz_sha256(hash, data, data_length), "Could not hash message");

	CHECK(mbedtls_ecdsa_read_signature(mbedtls_pk_ec(*keypair), hash, sizeof(hash), sig->sig,
									   sig->length),
		  "Could not verify message");
clean:
	return re;
}

int lz_ecdsa_verify_pub_pem(uint8_t *data, size_t data_length, lz_ecc_pub_key_pem *key,
							const lz_ecc_signature *sig)
{
	mbedtls_pk_context pk_context;
	mbedtls_pk_init(&pk_context);
	int re = 0;

	CHECK(mbedtls_pk_parse_public_key(&pk_context, (unsigned char *)key->key,
									  strnlen(key->key, MAX_PUB_ECP_PEM_BYTES - 1) + 1),
		  "Error parsing the public PEM key");

	uint8_t hash[SHA256_DIGEST_LENGTH];
	CHECK(lz_sha256(hash, data, data_length), "Could not hash message");

	CHECK(mbedtls_pk_verify(&pk_context, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig->sig,
							sig->length),
		  "Could not verify message");

clean:
	mbedtls_pk_free(&pk_context);

	return re;
}

#endif

#endif /* MBEDTLS_CONFIG_FILE */
