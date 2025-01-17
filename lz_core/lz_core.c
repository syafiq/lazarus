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

#include <time.h>
#include <stdio.h>

#include "lz_common.h"
#include "mbedtls/ecdsa.h"
#include "lz_ecc.h"
#include "lz_hmac.h"
#include "lz_ecdsa.h"
#include "lz_x509.h"
#include "lz_sha256.h"

#include "lzport_flash.h"
#include "lzport_memory.h"
#include "lzport_debug_output.h"

#include "lz_core.h"
#include "lz_update.h"
#include "lz_awdt.h"

__attribute__((section(".CP_CODE"))) volatile const uint8_t lz_cpatcher_code[LZ_CPATCHER_CODE_SIZE];
__attribute__((section(".UD_CODE"))) volatile const uint8_t lz_udownloader_code[LZ_UD_CODE_SIZE];
__attribute__((section(".APP_CODE"))) volatile const uint8_t app_code[LZ_APP_CODE_SIZE];

static lz_core_boot_params_t *lz_core_boot_params = (lz_core_boot_params_t *)&lz_img_boot_params;

static LZ_RESULT lz_get_staging_elem_content(hdr_type_t elem_type, uint8_t **content);
static LZ_RESULT lz_core_get_next_layer_addrs(boot_mode_t boot_mode,
											  const lz_img_hdr_t **boot_image_hdr,
											  const uint8_t **boot_image_code,
											  const lz_img_meta_t **img_meta);
static LZ_RESULT lz_core_derive_dev_auth(uint8_t *dev_auth, uint32_t dev_auth_length,
										 lz_ecc_keypair *lz_dev_id);

boot_mode_t lz_core_run(void)
{
	bool initial_boot;
	lz_ecc_keypair lz_dev_id_keypair;
	boot_mode_t boot_mode;
	uint8_t next_layer_digest[SHA256_DIGEST_LENGTH];

	// Check whether DICEpp passed valid boot parameters
	if (!lz_core_boot_params_valid()) {
		dbgprint(DBG_ERR, "PANIC: Lazarus corrupted boot parameters.\n");
		lz_error_handler();
	}

	// Derive DeviceID keypair based on CDI_prime provided via boot parameters
	if (lz_core_derive_device_id(&lz_dev_id_keypair) != LZ_SUCCESS) {
		dbgprint(DBG_ERR, "ERROR: Failed to derive DeviceID key pair\n");
		lz_error_handler();
	}

	// Check whether the system boots for the very first time
	initial_boot = lz_core_is_initial_boot();

	if (initial_boot) {
		dbgprint(DBG_INFO, "INFO: Initial boot of Lazarus, erase Lazarus Data Store "
						   "and Staging Area\n");

		// Erase the Lazarus data store and the staging area. This is necessary on the NXP
		// in order to be able to write to it
		if (lz_core_erase_lz_data_store() != LZ_SUCCESS) {
			dbgprint(DBG_ERR, "ERROR: Failed to erase Lazarus data store\n");
			lz_error_handler();
		}
		if (lz_core_erase_staging_area() != LZ_SUCCESS) {
			dbgprint(DBG_ERR, "ERROR: Failed to erase staging area\n");
			lz_error_handler();
		}

		// DICEpp provides static_symm at first boot. Store it in flash to be read by
		// the hub during provisioning. Afterwards, static_symm is wiped
		if (lz_core_store_static_symm() != LZ_SUCCESS) {
			dbgprint(DBG_ERR, "ERROR: Lazarus Core could not store static_symm for later "
							  "encrypting and signing.\n");
			lz_error_handler();
		}

		// On initial boot, the image meta data must be stored for the first time
		if (lz_update_img_meta_data() != LZ_SUCCESS) {
			dbgprint(DBG_ERR, "ERROR: Failed to update image meta data\n");
			lz_error_handler();
		}
	} else {
		// On subsequent boots, we must ensure, that static_symm is not present anymore
		// TODO The encryption and signing of static_symm as described in the paper:
		// X := sig(enc(dev_uuid|static_symm)^Hub_pub)^DevideID_priv)
		// could be implemented here if online provisioning shall be supported in the
		// future
		if (lz_core_wipe_static_symm() != LZ_SUCCESS) {
			dbgprint(DBG_ERR, "ERROR: Failed to wipe static_symm\n");
			lz_error_handler();
		}
	}

	// Check whether we have a new Lazarus Core: either after an update, or because it runs for the
	// very first time

	bool lz_core_updated = lz_core_is_updated(&lz_dev_id_keypair);
	// bool lz_core_updated = lz_core_is_updated(&device_id_pk, lz_keypair_to_public(&lz_dev_id_keypair));

	if (lz_core_updated) {
		dbgprint(
			DBG_INFO,
			"INFO: New DeviceID public key, this Lazarus Core version runs for the first time.\n");

		// If so, create a new DeviceID CSR and store the new pubkey and CSR.
		// This CSR is either signed via provisioning during the first time, or with the
		// Lazarus update protocol
		if (lz_core_create_device_id_csr(initial_boot, &lz_dev_id_keypair) != LZ_SUCCESS) {
			dbgprint(DBG_ERR, "ERROR: Lazarus Core could not store DeviceID pubkey and CSR.\n");
			lz_error_handler();
		}
	}

	// The hub is responsible for flashing the signed binaries and the trust anchors structure onto
	// the device if the device is not provisioned yet (happens via provisioning script)
	if (!lz_core_is_provisioning_complete()) {
		dbgprint(DBG_WARN, "WARN: Device is not provisioned yet. This normal during the very first "
						   "boot. Blocking and waiting for the device to be provisioned..\n");
		for (;;)
			;
	} else {
		dbgprint(DBG_INFO, "INFO: Device is provisioned\n");
	}

	// Check if there are staging elements on the staging area. This might be tickets of updates.
	// If there are no elements, we need to boot into the update downloader to get a boot ticket
	// from the hub in order to boot into the firmware. If there are elements present, we might
	// need to apply updates and may boot directly into the app if a boot ticket is present.
	if (lz_get_num_staging_elems() == 0) {
		boot_mode = LZ_UDOWNLOADER;
	} else {
		// Check for updates
		if (lz_std_updates_pending() == LZ_SUCCESS) {
			// Verify and apply updates
			lz_apply_updates();
		}

		if (lz_update_img_meta_data() != LZ_SUCCESS) {
			dbgprint(DBG_ERR, "ERROR: Failed to update image meta data\n");
			lz_error_handler();
		}

		if (lz_verified_core_update_pending() == LZ_SUCCESS) {
			boot_mode = LZ_CPATCHER;
		} else if (lz_has_valid_staging_element(BOOT_TICKET) == LZ_SUCCESS) {
			boot_mode = APP;
		} else {
			boot_mode = LZ_UDOWNLOADER;
		}
	}

	// Determine deferral time based on deferral ticket in staging area
	uint32_t deferral_time;
	if (lz_get_deferral_time(&deferral_time) != LZ_SUCCESS) {
		dbgprint(DBG_WARN,
				 "WARN: Could not find valid deferral ticket, using default value "
				 "%ds.\n",
				 DEFAULT_WDT_TIMOUT_s);
		deferral_time = DEFAULT_WDT_TIMOUT_s;
	}

	// Trusted boot: verify the next layer. If verification of the App fails, a new App will
	// be fetched from the hub ('dominance principle'). If the verification of the Core Patcher
	// or Update Downloader fails, the device is bricked.
	bool firmware_update_necessary = false;
	if (lz_core_verify_next_layer(boot_mode, next_layer_digest) != LZ_SUCCESS) {
		if (boot_mode == APP) {
			dbgprint(DBG_ERR, "ERROR: Verification of App failed, require App update..\n");

			// Verification of the app failed, switch boot-mode to Update Downloader and
			// indicate that a new firmware is required
			boot_mode = LZ_UDOWNLOADER;
			firmware_update_necessary = true;
		} else {
			dbgprint(DBG_ERR, "FATAL: Verification of UD or UM failed. This is not recoverable.\n");
			lz_error_handler();
		}
	}

	lz_ecc_priv_key_pem pem;
	lz_priv_key_to_pem(&lz_dev_id_keypair, &pem);
	uint8_t digest[SHA256_DIGEST_LENGTH];
	if (lz_sha256_two_parts(digest, next_layer_digest, sizeof(next_layer_digest), &pem,
							sizeof(pem)) < 0) {
		dbgprint(DBG_ERR, "ERROR: Failed to derive digest from next layer and DeviceID\n");
		return false;
	}

	// Create the volatile AliasID key pair based on measuring the next layer
	lz_ecc_keypair lz_alias_id_keypair;
	if (lz_core_derive_alias_id_keypair(digest, &lz_alias_id_keypair) != LZ_SUCCESS) {
		dbgprint(
			DBG_ERR,
			"ERROR: Failed to calculate and store alias credentials into next layer's parameters");
		return false;
	}

	// Create the boot parameters for the next layer depending on the boot mode
	if (lz_core_provide_params_ram(boot_mode, lz_core_updated, firmware_update_necessary,
								   &lz_alias_id_keypair, &lz_dev_id_keypair) != LZ_SUCCESS) {
		dbgprint(DBG_ERR, "PANIC: Could not create boot parameters for next layer.\n");
		lz_error_handler();
	}

	// Initialize the AWDT. Once initialized, it can never be stopped again. The firmware
	// will have to fetch boot tickets always in time to prevent a device reset
	lz_awdt_init(deferral_time);
	if (lz_awdt_last_reset_awdt()) {
		dbgprint(DBG_WARN, "WARN: Last device reset was through expired AWDT\n");
	}

	// Attention: after the de-init, adding dbgprint will cause a HardFault
	dbgprint(DBG_INFO, "INFO: Launching next layer...\n");

	// Deinitialize peripherals
	lzport_rng_deinit();

	initial_boot = false;
	// TODO: Set new device id key to 0
	secure_zero_memory(next_layer_digest, sizeof(next_layer_digest));
	deferral_time = 0;

	return boot_mode;
}

/**
 * Create DeviceID key pair from CDI''
 * @param pubKey The derived DeviceID public key
 * @param privKey The derived DeviceID private key
 * @return LZ_SUCCESS if successful, otherwise LZ_ERROR
 */
LZ_RESULT lz_core_derive_device_id(lz_ecc_keypair *device_id_keypair)
{
	dbgprint(DBG_INFO, "INFO: Generating DeviceID key pair\n");
	if (lz_derive_ecc_keypair(device_id_keypair, lz_core_boot_params->info.cdi_prime,
							  sizeof(lz_core_boot_params->info.cdi_prime))) {
		dbgprint(DBG_ERR, "ERROR: Failed to derive DeviceID key pair (device_id_keypair)\n");
		return LZ_ERROR;
	}

	dbgprint(DBG_INFO, "INFO: Done with generating mbedtls key\n");

	return LZ_SUCCESS;
}

// Looks for a valid deferral ticket on the staging area.
// On success, the function returns true and writes the deferral time to <deferral_time>
LZ_RESULT lz_get_deferral_time(uint32_t *deferral_time)
{
	LZ_RESULT result;

	dbgprint(DBG_INFO, "INFO: Searching for deferral ticket on staging area\n");

	if ((result = lz_has_valid_staging_element(DEFERRAL_TICKET)) != LZ_SUCCESS) {
		goto exit;
	}

	if ((result = lz_get_staging_elem_content(DEFERRAL_TICKET, (uint8_t **)&deferral_time)) !=
		LZ_SUCCESS) {
		goto exit;
	}

	result = LZ_SUCCESS;

exit:
	return result;
}

LZ_RESULT lz_core_get_next_layer_addrs(boot_mode_t boot_mode, const lz_img_hdr_t **boot_image_hdr,
									   const uint8_t **boot_image_code,
									   const lz_img_meta_t **img_meta)
{
	switch (boot_mode) {
	case APP:
		if (boot_image_hdr != NULL) {
			*boot_image_hdr = (lz_img_hdr_t *)&lz_app_hdr;
		}
		if (boot_image_code != NULL) {
			*boot_image_code = (uint8_t *)app_code;
		}
		if (img_meta != NULL) {
			*img_meta = (lz_img_meta_t *)&lz_data_store.config_data.img_info.app_meta;
		}
		break;
	case LZ_CPATCHER:
		if (boot_image_hdr != NULL) {
			*boot_image_hdr = (lz_img_hdr_t *)&lz_cpatcher_hdr;
		}
		if (boot_image_code != NULL) {
			*boot_image_code = (uint8_t *)lz_cpatcher_code;
		}
		if (img_meta != NULL) {
			*img_meta = (lz_img_meta_t *)&lz_data_store.config_data.img_info.um_meta;
		}
		break;
	case LZ_UDOWNLOADER:
		if (boot_image_hdr != NULL) {
			*boot_image_hdr = (lz_img_hdr_t *)&lz_udownloader_hdr;
		}
		if (boot_image_code != NULL) {
			*boot_image_code = (uint8_t *)lz_udownloader_code;
		}
		if (img_meta != NULL) {
			*img_meta = (lz_img_meta_t *)&lz_data_store.config_data.img_info.ud_meta;
		}
		break;
	default:
		dbgprint(DBG_ERR, "ERROR: Unknown boot mode.\n");
		return LZ_ERROR;
	}
	return LZ_SUCCESS;
}

// Verifies the next layer to be booted
// Returns the verification result, and writes the verified digest to <next_layer_digest> if not null
LZ_RESULT lz_core_verify_next_layer(boot_mode_t boot_mode, uint8_t *next_layer_digest)
{
	const lz_img_hdr_t *boot_image_hdr;
	const uint8_t *boot_image_code;
	const lz_img_meta_t *img_meta;
	LZ_RESULT result;

	if ((result = lz_core_get_next_layer_addrs(boot_mode, &boot_image_hdr, &boot_image_code,
											   &img_meta)) != LZ_SUCCESS) {
		dbgprint(DBG_ERR, "ERROR: Could not get header and code information of next layer.\n");
		return result;
	}

	result = lz_core_verify_image(boot_image_hdr, boot_image_code, img_meta, next_layer_digest);

	return result;
}

/**
 * Wipe static_symm from flash
 * @return LZ_SUCCESS if successful, otherwise LZ_ERROR
 */
LZ_RESULT lz_core_wipe_static_symm(void)
{
	// Check if static_symm is already wiped
	if (lz_is_mem_zero((const void *)&lz_data_store.config_data.static_symm_info.static_symm,
					   sizeof(lz_data_store.config_data.static_symm_info.static_symm))) {
		dbgprint(DBG_INFO, "INFO: static_symm already wiped\n");
		return LZ_SUCCESS;
	}

	// Create a copy of the config data area in RAM
	lz_config_data_t config_data_cpy;
	memcpy((void *)&config_data_cpy, (void *)&lz_data_store.config_data, sizeof(config_data_cpy));

	// Zero static_symm
	secure_zero_memory(&config_data_cpy.static_symm_info.static_symm,
					   sizeof(config_data_cpy.static_symm_info.static_symm));

	config_data_cpy.static_symm_info.magic = LZ_MAGIC;

	// Write config back to flash
	if (!(lzport_flash_write((uint32_t)&lz_data_store.config_data, (uint8_t *)&config_data_cpy,
							 sizeof(lz_data_store.config_data)))) {
		dbgprint(DBG_ERR, "ERROR: Failed to wipe static_symm\n");
		return LZ_ERROR;
	}

	dbgprint(DBG_INFO, "INFO: Successfully wiped static_symm\n");

	return LZ_SUCCESS;
}

LZ_RESULT lz_core_derive_dev_auth(uint8_t *dev_auth, uint32_t dev_auth_length,
								  lz_ecc_keypair *lz_dev_id)
{
	uint8_t digest_dev_auth[MAX_PUB_ECP_PEM_BYTES + LEN_UUID_V4_BIN];

	if (dev_auth_length < SHA256_DIGEST_LENGTH) {
		dbgprint(DBG_ERR, "ERROR: Provided dev_auth too small\n");
		return LZ_ERROR;
	}

	// Setting everything to 0
	memset(digest_dev_auth, 0, sizeof(digest_dev_auth));

	// Concatenate devID public and dev_uuid to calculate dev_auth
	lz_pub_key_to_pem(lz_dev_id, (lz_ecc_pub_key_pem *)digest_dev_auth);
	memcpy(digest_dev_auth + MAX_PUB_ECP_PEM_BYTES, lz_core_boot_params->info.dev_uuid,
		   LEN_UUID_V4_BIN);

	// Compute dev auth
	if (lz_hmac_sha256(dev_auth, digest_dev_auth, sizeof(digest_dev_auth),
					   (uint8_t *)&(lz_core_boot_params->info.core_auth),
					   sizeof(lz_core_boot_params->info.core_auth))) {
		dbgprint(DBG_ERR, "ERROR: Creating dev_auth failed.\n");
		return LZ_ERROR;
	}

	return LZ_SUCCESS;
}

// Calculates the alias key pair and stores it in alias_creds
LZ_RESULT lz_core_derive_alias_id_keypair(uint8_t *digest, lz_ecc_keypair *lz_alias_id_keypair)
{
	dbgprint(DBG_INFO, "INFO: Generating Alias Identity\n");

	if (lz_derive_ecc_keypair(lz_alias_id_keypair, digest, sizeof(digest))) {
		dbgprint(DBG_ERR, "ERROR: Failed to derive alias id key pair (device_id_keypair)\n");
		return LZ_ERROR;
	}

	dbgprint(DBG_INFO, "INFO: Successfully generated alias keypair\n");

	return LZ_SUCCESS;
}

LZ_RESULT lz_core_create_cert_store(boot_mode_t boot_mode, lz_ecc_keypair *alias_keypair,
									lz_ecc_keypair *device_id_keypair)
{
	const lz_img_hdr_t *boot_image_hdr;
	uint32_t rem_length = 0;

	// We require information from the header of the next layer to be loaded
	if (lz_core_get_next_layer_addrs(boot_mode, &boot_image_hdr, NULL, NULL) != LZ_SUCCESS) {
		dbgprint(DBG_ERR, "ERROR: Could not retrieve next layer's image header address.\n");
		return LZ_ERROR;
	}

	// Create a cert with the device_id_key as issuer and the alias_id as subject
	// Then sign that thing and store it somewhere

	lz_x509_cert_info info;
	info.issuer.common_name = "DeviceID";
	info.issuer.org = "Lazarus";
	info.issuer.country = "DE";
	info.subject.common_name = "AliasID";
	info.subject.org = "Lazarus";
	info.subject.country = "DE";

	lz_ecc_pub_key_pem alias_keypair_pem;
	lz_pub_key_to_pem(alias_keypair, &alias_keypair_pem);
	if (lz_set_serial_number_cert(&info, (unsigned char *)&alias_keypair_pem,
								  sizeof(alias_keypair_pem)) != 0) {
		dbgprint(DBG_ERR, "ERROR: lz_set_serial_number_cert failed.\n");
		return LZ_ERROR;
	}
	// Create the cert store with all certificates
	memset((void *)&lz_img_cert_store, 0x00, sizeof(lz_img_cert_store));

	// Store DeviceID pubkey
	// Write the public key to the cert_store
	lz_pub_key_to_pem(device_id_keypair, (lz_ecc_pub_key_pem *)&lz_img_cert_store.info.dev_pub_key);

	// Provide backend public key to upper layers
	memcpy((void *)&lz_img_cert_store.info.management_pub_key,
		   (void *)&lz_data_store.trust_anchors.info.management_pub_key,
		   sizeof(lz_img_cert_store.info.management_pub_key));

	// Now, load the certificate chain into the certBag of the Image Cert Store
	// Start with issued root certificate stored in Lazarus Data Store
	if ((lz_img_cert_store.info.cursor +
		 lz_data_store.trust_anchors.info.certTable[INDEX_LZ_CERTSTORE_HUB].size) >
		sizeof(lz_img_cert_store.certBag)) {
		dbgprint(DBG_ERR, "ERROR: ImgCertStore overflow (INDEX_IMG_CERTSTORE_HUB).\n");
		return false;
	}

	if (lz_data_store.trust_anchors.info.certTable[INDEX_LZ_CERTSTORE_HUB].size != 0) {
		memcpy(
			(void *)&lz_img_cert_store.certBag[lz_img_cert_store.info.cursor],
			(void *)&lz_data_store.trust_anchors
				.certBag[lz_data_store.trust_anchors.info.certTable[INDEX_LZ_CERTSTORE_HUB].start],
			lz_data_store.trust_anchors.info.certTable[INDEX_LZ_CERTSTORE_HUB].size);

		lz_img_cert_store.info.certTable[INDEX_IMG_CERTSTORE_HUB].start =
			lz_img_cert_store.info.cursor;
		lz_img_cert_store.info.certTable[INDEX_IMG_CERTSTORE_HUB].size =
			lz_data_store.trust_anchors.info.certTable[INDEX_LZ_CERTSTORE_HUB].size;
		lz_img_cert_store.info.cursor +=
			lz_data_store.trust_anchors.info.certTable[INDEX_LZ_CERTSTORE_HUB].size;
		lz_img_cert_store.certBag[lz_img_cert_store.info.cursor++] = '\0';
	}

	// Load issued or self-signed DeviceID certificate from Lazarus Data Store
	if ((lz_img_cert_store.info.cursor +
		 lz_data_store.trust_anchors.info.certTable[INDEX_LZ_CERTSTORE_DEVICEID].size) >
		sizeof(lz_img_cert_store.certBag)) {
		dbgprint(DBG_ERR, "ERROR: ImgCertStore overflow (INDEX_IMG_CERTSTORE_DEVICEID).\n");
		return LZ_ERROR;
	}

	memcpy(
		(void *)&lz_img_cert_store.certBag[lz_img_cert_store.info.cursor],
		(void *)&lz_data_store.trust_anchors
			.certBag[lz_data_store.trust_anchors.info.certTable[INDEX_LZ_CERTSTORE_DEVICEID].start],
		lz_data_store.trust_anchors.info.certTable[INDEX_LZ_CERTSTORE_DEVICEID].size);
	lz_img_cert_store.info.certTable[INDEX_IMG_CERTSTORE_DEVICEID].start =
		lz_img_cert_store.info.cursor;
	lz_img_cert_store.info.certTable[INDEX_IMG_CERTSTORE_DEVICEID].size =
		lz_data_store.trust_anchors.info.certTable[INDEX_LZ_CERTSTORE_DEVICEID].size;
	lz_img_cert_store.info.cursor +=
		lz_data_store.trust_anchors.info.certTable[INDEX_LZ_CERTSTORE_DEVICEID].size;
	lz_img_cert_store.certBag[lz_img_cert_store.info.cursor++] = '\0';

	// Finally, load the volatile AliasID certificate
	rem_length = sizeof(lz_img_cert_store.certBag) - lz_img_cert_store.info.cursor;
	if (lz_write_cert_to_pem(
			&info, alias_keypair, device_id_keypair,
			(unsigned char *)&lz_img_cert_store.certBag[lz_img_cert_store.info.cursor],
			rem_length) != 0) {
		dbgprint(
			DBG_ERR,
			"ERROR: lz_write_cert_to_pem failed. ImgCertStore overflow likely (INDEX_IMG_CERTSTORE_ALIASID).\n");
		return LZ_ERROR;
	}
	rem_length = strlen((const char *)&lz_img_cert_store.certBag[lz_img_cert_store.info.cursor]);

	lz_img_cert_store.info.certTable[INDEX_IMG_CERTSTORE_ALIASID].start =
		lz_img_cert_store.info.cursor;
	lz_img_cert_store.info.certTable[INDEX_IMG_CERTSTORE_ALIASID].size = rem_length;
	lz_img_cert_store.info.cursor += rem_length;
	lz_img_cert_store.certBag[lz_img_cert_store.info.cursor++] = '\0';

	// Set the magic value after everything went well
	lz_img_cert_store.info.magic = LZ_MAGIC;

	return LZ_SUCCESS;
}

// This function provides all required boot parameters for the next layer as fixed structures at fixed locations in RAM.
LZ_RESULT lz_core_provide_params_ram(boot_mode_t boot_mode, bool lz_core_updated,
									 bool firmware_update_necessary,
									 lz_ecc_keypair *lz_alias_id_keypair,
									 lz_ecc_keypair *lz_dev_id_keypair)
{
	// Create a temporary boot parameters structure on the stack, later write it to the designated area.
	// We cannot directly write, because the area overlaps with Lazarus Core's boot parameters,
	// which we still require until the end of this function.
	lz_img_boot_params_info_t img_boot_params_info_cpy = { 0 };

	lz_pub_key_to_pem(lz_alias_id_keypair, &img_boot_params_info_cpy.alias_id_keypair_pub);
	lz_priv_key_to_pem(lz_alias_id_keypair, &img_boot_params_info_cpy.alias_id_keypair_priv);

	// App and UD get next nonce for retrieving boot/deferral tickets, UM doesn't need it
	// App currently also gets dev_uuid TODO check if that is OK or if another identifier must be
	// used, see also comment below (need to know principle)
	if (boot_mode == LZ_UDOWNLOADER || boot_mode == APP) {
		memcpy(&img_boot_params_info_cpy.dev_uuid, &(lz_core_boot_params->info.dev_uuid),
			   LEN_UUID_V4_BIN);
		memcpy(&img_boot_params_info_cpy.next_nonce, &(lz_core_boot_params->info.next_nonce),
			   sizeof(img_boot_params_info_cpy.next_nonce));
	}

	// The App should not be able to issue a Lazarus Core re-association so it does not get
	// the current nonce, dev_auth and sig_enc_static_symm (need to know principle)
	if (boot_mode == LZ_UDOWNLOADER || boot_mode == LZ_CPATCHER) {
		memcpy(&img_boot_params_info_cpy.dev_uuid, &(lz_core_boot_params->info.dev_uuid),
			   LEN_UUID_V4_BIN);
		memcpy(&img_boot_params_info_cpy.cur_nonce, &(lz_core_boot_params->info.cur_nonce),
			   sizeof(img_boot_params_info_cpy.cur_nonce));

		// Derive dev_auth and write it to the parameters.
		if (lz_core_derive_dev_auth(img_boot_params_info_cpy.dev_auth,
									sizeof(img_boot_params_info_cpy.dev_auth),
									lz_dev_id_keypair) != LZ_SUCCESS) {
			dbgprint(DBG_ERR,
					 "ERROR: Failed to calculate and store dev_auth into next layer's parameters");
			return LZ_ERROR;
		}

		// Indicate that device reassociation protocol must be carried out because lazarus core
		// was updated
		img_boot_params_info_cpy.dev_reassociation_necessary = lz_core_updated;
		img_boot_params_info_cpy.firmware_update_necessary = firmware_update_necessary;
	}

	// UD gets network credentials from Lazarus Data Store, when present
	if (boot_mode == LZ_UDOWNLOADER) {
		if (lz_data_store.config_data.nw_info.magic == LZ_MAGIC) {
			memcpy((void *)&img_boot_params_info_cpy.nw_data,
				   (void *)&lz_data_store.config_data.nw_info,
				   sizeof(img_boot_params_info_cpy.nw_data));
		}
	}

	// Set magic value, structure is complete
	img_boot_params_info_cpy.magic = LZ_MAGIC;

	// Now, directly write the ImgCertStore structure to RAM, which does not overlap with Lazarus
	// Core's boot parameters
	if (lz_core_create_cert_store(boot_mode, lz_alias_id_keypair, lz_dev_id_keypair) !=
		LZ_SUCCESS) {
		dbgprint(DBG_ERR, "ERROR: Failed to setup certificate store for next layer");
		return LZ_ERROR;
	}

	// At this point, Lazarus Core doesn't need its boot parameters anymore, so zero out the area
	// and then write next layer's parameter
	secure_zero_memory((void *)&lz_img_boot_params, sizeof(lz_img_boot_params));
	memcpy((void *)&lz_img_boot_params.info, &img_boot_params_info_cpy,
		   sizeof(lz_img_boot_params.info));

	return LZ_SUCCESS;
}

/**
 * Create DeviceID Certificate Signing Request and store it in flash
 */
LZ_RESULT lz_core_create_device_id_csr(bool first_boot, lz_ecc_keypair *device_id_keypair)
{
	// Create a trust anchors structure to RAM, rewrite and flash when finished
	trust_anchors_t ta_copy = { 0 };

	dbgprint(DBG_INFO, "INFO: Generating new DeviceID certificate.\n");

	if (!first_boot) {
		// Write the contents of the existing trust anchors structure to the copy
		memcpy((void *)&ta_copy, (void *)&lz_data_store.trust_anchors, sizeof(ta_copy));
	} else {
		// Leave 0xff in the certBag to be able to further write it without erasing the full page
		// (not made use of so far)
		memset(ta_copy.certBag, 0xff, sizeof(ta_copy.certBag));
	}

	// Store new DeviceID public key
	lz_pub_key_to_pem(device_id_keypair, &ta_copy.info.dev_pub_key);
	lz_x509_csr_info info;
	info.subject.common_name = "DeviceID";
	info.subject.country = "DE";
	info.subject.org = "Lazarus";

	if (lz_set_serial_number_csr(&info, (unsigned char *)&ta_copy.info.dev_pub_key,
								 sizeof(ta_copy.info.dev_pub_key)) != 0) {
		dbgprint(DBG_ERR, "ERROR: lz_set_serial_number_csr failed.\n");
		return LZ_ERROR;
	}

	uint32_t length = 0;

	// Produce a PEM-formatted output from the DER encoded certificate for the certBag
	if (first_boot) {
		// certBag must be empty, so we have the full space available
		length = sizeof(ta_copy.certBag);
	} else {
		if (ta_copy.info.cursor == 0) {
			dbgprint(DBG_ERR, "ERROR: Cursor is zero. Previous DeviceID CSR was not correctly "
							  "stored.\n");
			return LZ_ERROR;
		}

		if (lz_core_is_provisioning_complete()) {
			ta_copy.info.cursor = ta_copy.info.certTable[INDEX_LZ_CERTSTORE_DEVICEID].start;
			length = ta_copy.info.certTable[INDEX_LZ_CERTSTORE_DEVICEID].size;
		} else {
			ta_copy.info.cursor = 0;
			length = sizeof(ta_copy.certBag);
		}
	}

	if (lz_write_csr_to_pem(&info, device_id_keypair, &ta_copy.certBag[ta_copy.info.cursor],
							length) < 0) {
		dbgprint(DBG_ERR, "ERROR: lz_write_csr_to_pem failed.\n");
		return LZ_ERROR;
	}
	length = strlen((char *)&ta_copy.certBag[ta_copy.info.cursor]);
	ta_copy.info.certTable[INDEX_LZ_CERTSTORE_DEVICEID].start = ta_copy.info.cursor;
	ta_copy.info.certTable[INDEX_LZ_CERTSTORE_DEVICEID].size = (uint16_t)length;
	ta_copy.info.cursor += length;
	ta_copy.certBag[ta_copy.info.cursor++] = '\0';

	// Persist the new copy of the trust anchors structure in the Lazarus Data Store
	if (!(lzport_flash_write((uint32_t)&lz_data_store.trust_anchors, (uint8_t *)&ta_copy,
							 sizeof(lz_data_store.trust_anchors)))) {
		dbgprint(DBG_ERR, "ERROR: Failed to flash DeviceID CSR\n");
		return LZ_ERROR;
	}
	dbgprint(DBG_ERR, "INFO: Successfully written csr to trust anchors.\n");

	return LZ_SUCCESS;
}

LZ_RESULT lz_core_erase_lz_data_store(void)
{
	lz_data_store_t temp_store;
	memset(&temp_store, 0xFF, sizeof(lz_data_store_t));
	if (!lzport_flash_write((uint32_t)&lz_data_store, (uint8_t *)&temp_store,
							sizeof(lz_data_store_t))) {
		return LZ_ERROR;
	}
	return LZ_SUCCESS;
}

LZ_RESULT lz_core_erase_staging_area(void)
{
	uint8_t temp[512];
	memset(temp, 0xFF, 512);
	uint8_t *p = (uint8_t *)&lz_staging_area;
	for (int i = 0; i < LZ_STAGING_AREA_NUM_PAGES; i++) {
		if (!lzport_flash_write((uint32_t)p, temp, 512)) {
			dbgprint(DBG_ERR, "ERROR: Failed to erase staging area (page %d, addr %x)\n", i, p);
			return LZ_ERROR;
		}
		p += 512;
	}
	return LZ_SUCCESS;
}

// Check if derived DeviceID matches with the stored identity.
// DeviceID may only change when Lazarus Core was updated
bool lz_core_is_updated(lz_ecc_keypair *lz_dev_id_keypair)
{
	lz_ecc_keypair old_key;
	if (lz_pem_to_pub_key(
			&old_key, (lz_ecc_pub_key_pem *)&lz_data_store.trust_anchors.info.dev_pub_key) != 0) {
		return 1;
	}
	int re = lz_compare_public_key(lz_keypair_to_public(&old_key),
								   lz_keypair_to_public(lz_dev_id_keypair));
	lz_free_keypair(&old_key);

	return re;
}

bool lz_core_is_initial_boot(void)
{
	return lz_core_boot_params->info.initial_boot;
}

/**
 * Store static_symm. May only be called during the initial Lazarus Core boot
 * @return
 */
LZ_RESULT lz_core_store_static_symm(void)
{
	// Creates the initial CONFIG_DATA structure in RAM, then writes it to flash
	lz_config_data_t cfg_data_cpy;
	memset(&cfg_data_cpy, 0xFF, sizeof(lz_config_data_t));

	// Write static_symm, which is provided on first boot to Lazarus Core's boot params
	memcpy(&cfg_data_cpy.static_symm_info.static_symm, &(lz_core_boot_params->info.static_symm),
		   sizeof(cfg_data_cpy.static_symm_info.static_symm));
	// Write dev_uuid
	memcpy(&cfg_data_cpy.static_symm_info.dev_uuid, &(lz_core_boot_params->info.dev_uuid),
		   sizeof(cfg_data_cpy.static_symm_info.dev_uuid));

	// Set the magic value to indicate the initialization of the struct
	cfg_data_cpy.static_symm_info.magic = LZ_MAGIC;

	if (!(lzport_flash_write((uint32_t)&lz_data_store.config_data, (uint8_t *)&cfg_data_cpy,
							 sizeof(lz_data_store.config_data)))) {
		dbgprint(DBG_ERR, "ERROR: lzport_flash_write failed.\n");
		return LZ_ERROR;
	}
	return LZ_SUCCESS;
}

// Returns true if the provided boot parameters have the magic value set
bool lz_core_boot_params_valid(void)
{
	return lz_core_boot_params->info.magic == LZ_MAGIC;
}

bool lz_core_is_provisioning_complete(void)
{
	return ((lz_data_store.trust_anchors.info.magic == LZ_MAGIC) &&
			(lz_udownloader_hdr.hdr.content.magic == LZ_MAGIC) &&
			(lz_cpatcher_hdr.hdr.content.magic == LZ_MAGIC) &&
			(lz_core_hdr.hdr.content.magic == LZ_MAGIC));
}

LZ_RESULT lz_has_staging_elem_type(hdr_type_t elem_type)
{
	lz_auth_hdr_t *staging_hdr = NULL;
	return lz_get_staging_hdr(elem_type, &staging_hdr, lz_core_boot_params->info.cur_nonce);
}

LZ_RESULT lz_core_verify_staging_elem_hdr_sig(const lz_auth_hdr_t *hdr, uint8_t *payload)
{
	uint8_t digest[SHA256_DIGEST_LENGTH];

	// Hash the staging element's payload
	if (lz_sha256(digest, payload, hdr->content.payload_size) != 0) {
		dbgprint(DBG_ERR, "ERROR: lz_sha256 failed.\n");
		return LZ_ERROR;
	}

	// Verify the computed hash against the hash in the header
	if (memcmp(digest, hdr->content.digest, sizeof(digest)) != 0) {
		dbgprint(DBG_WARN, "ERROR: Staging element digest mismatch\n");
		return LZ_ERROR;
	}

	if (lz_ecdsa_verify_pub_pem(
			(uint8_t *)&hdr->content, sizeof(hdr->content),
			(lz_ecc_pub_key_pem *)&lz_data_store.trust_anchors.info.management_pub_key,
			&hdr->signature) != 0) {
		dbgprint(DBG_ERR, "ERROR: GEN - Failed to verify staging element header signature\n");
		return LZ_ERROR;
	}

	dbgprint(DBG_INFO, "INFO: Success! Staging element's signature valid.\n");

	return LZ_SUCCESS;
}

LZ_RESULT lz_verify_staging_header(const lz_auth_hdr_t *staging_element_hdr, uint8_t *payload)
{
	if (lz_core_verify_staging_elem_hdr(staging_element_hdr, payload,
										lz_core_boot_params->info.cur_nonce) != LZ_SUCCESS) {
		dbgprint(DBG_ERR, "ERROR: Staging element header verification failed.");
		return LZ_ERROR;
	}

	if ((staging_element_hdr->content.type == LZ_CORE_UPDATE) ||
		(staging_element_hdr->content.type == LZ_UDOWNLOADER_UPDATE) ||
		(staging_element_hdr->content.type == LZ_CPATCHER_UPDATE) ||
		(staging_element_hdr->content.type == APP_UPDATE) ||
		(staging_element_hdr->content.type == DEVICE_ID_REASSOC_RES) ||
		(staging_element_hdr->content.type == CONFIG_UPDATE)) {
		return LZ_SUCCESS;
	}

	return LZ_ERROR;
}

LZ_RESULT lz_core_verify_staging_elem_hdr(const lz_auth_hdr_t *hdr, uint8_t *payload,
										  uint8_t *nonce)
{
	// Check sanity of header
	if (hdr->content.magic != LZ_MAGIC) {
		dbgprint(DBG_ERR, "ERROR: Staging element header corrupted\n");
		return LZ_ERROR;
	}

	// Payload must be larger than 0
	if (hdr->content.payload_size == 0) {
		dbgprint(DBG_ERR, "ERROR: Staging element size is 0\n");
		return LZ_ERROR;
	}

	dbgprint(DBG_INFO, "INFO: Element %s with size %d bytes (0x%x bytes)\n",
			 HDR_TYPE_STRING[hdr->content.type], hdr->content.payload_size,
			 hdr->content.payload_size);

	dbgprint(DBG_VERB, "Payload digest: ");
	for (uint32_t i = 0; i < SHA256_DIGEST_LENGTH; i++) {
		dbgprint(DBG_VERB, "%02x ", hdr->content.digest[i]);
	}

	// Nonce must match with input provided nonce
	if (memcmp(&(hdr->content.nonce), nonce, sizeof(hdr->content.nonce))) {
		dbgprint(DBG_ERR, "ERROR: Staging element's nonce incorrect\n");
		return LZ_ERROR;
	}

	// Verify the signature of the staging element header
	if (lz_core_verify_staging_elem_hdr_sig(hdr, payload) != LZ_SUCCESS) {
		dbgprint(DBG_ERR, "ERROR: Staging element header verification failed.\n");
		return LZ_ERROR;
	}

	dbgprint(DBG_INFO, "INFO: Element successfully verified (Nonce, digest and signature)\n");

	return LZ_SUCCESS;
}

LZ_RESULT lz_core_verify_image(const lz_img_hdr_t *image_hdr, const uint8_t *image_code,
							   const lz_img_meta_t *image_meta, uint8_t *image_digest_out)
{
	uint8_t digest[SHA256_DIGEST_LENGTH];

	if (image_hdr->hdr.content.magic != LZ_MAGIC) {
		dbgprint(DBG_ERR, "ERROR: Image header invalid (MAGIC)\n");
		return LZ_ERROR;
	}

	if (!(image_code == (uint8_t *)(((uint32_t)image_hdr) + (image_hdr->hdr.content.hdr_size)))) {
		dbgprint(DBG_ERR, "ERROR: Unexpected boot image start address.\n");
		return LZ_ERROR;
	}

	// Compute the digest of the next layer's image
	if (lz_sha256(digest, image_code, image_hdr->hdr.content.size) != 0) {
		dbgprint(DBG_ERR, "ERROR: lz_sha256 failed.\n");
		return LZ_ERROR;
	}

	// Compare it with the digest stored in the header
	if (memcmp(digest, image_hdr->hdr.content.digest, sizeof(digest)) != 0) {
		dbgprint(DBG_ERR,
				 "ERROR: Next layer digest mismatch. Layer %s, size %d, version %d, "
				 "issue time %s\n",
				 image_hdr->hdr.content.name, image_hdr->hdr.content.size,
				 image_hdr->hdr.content.version,
				 asctime(gmtime((time_t *)&(image_hdr->hdr.content.issue_time))));
		dbgprint_data((uint8_t *)image_hdr->hdr.content.digest, SHA256_DIGEST_LENGTH, "Digest");
		return LZ_ERROR;
	}

	if (lz_ecdsa_verify_pub_pem(
			(uint8_t *)&image_hdr->hdr.content, sizeof(image_hdr->hdr.content),
			(lz_ecc_pub_key_pem *)&lz_data_store.trust_anchors.info.code_auth_pub_key,
			&image_hdr->hdr.signature) != 0) {
		dbgprint(DBG_ERR, "ERROR: Failed to verify image signature with code signing key\n");
		return LZ_ERROR;
	}

	dbgprint(DBG_INFO, "INFO: Successfully verified image signature with code auth key.\n");
	dbgprint(DBG_INFO, "INFO: Checking image's version numbers.\n");

	// Detect rollback attacks. The first time an image is deployed onto the device,
	// Lazarus Core persists its metadata, so it has to be present at this point in time.
	if (image_meta->magic != LZ_MAGIC) {
		dbgprint(DBG_ERR, "ERROR: Stored image info is invalid.");
		return false;
	}

	dbgprint(DBG_INFO, "INFO: Verifying meta data of image %s\n", image_hdr->hdr.content.name);

	char *date_last_issued = asctime(gmtime((time_t *)&(image_meta->last_issue_time)));
	date_last_issued[24] = '\0';
	dbgprint(DBG_INFO, "INFO: Expected: Version of min. %d.%d, issued min. (UTC): %s.\n",
			 image_meta->lastVersion >> 16, image_meta->lastVersion & 0x0000ffff, date_last_issued);

	char *date = asctime(gmtime((time_t *)&image_hdr->hdr.content.issue_time));
	date[24] = '\0';
	dbgprint(DBG_INFO, "INFO: Actual: Version %d.%d, issued (UTC): %s.\n",
			 image_hdr->hdr.content.version >> 16, image_hdr->hdr.content.version & 0x0000ffff,
			 date);

	// Check against stored metadata
	if (image_meta->lastVersion > image_hdr->hdr.content.version ||
		image_meta->last_issue_time > image_hdr->hdr.content.issue_time) {
		dbgprint(DBG_ERR, "ERROR: Failed to verify image because of version roll-back\n");
		return LZ_ERROR;
	}

	dbgprint(DBG_INFO, "INFO: Image version and issue time check succeeded.\n");

	// Write the digest to the out parameter in case a pointer was provided
	if (image_digest_out) {
		memcpy(image_digest_out, digest, SHA256_DIGEST_LENGTH);
	}

	return LZ_SUCCESS;
}

LZ_RESULT lz_has_valid_staging_element(hdr_type_t hdr_type)
{
	lz_auth_hdr_t *staging_hdr = NULL;

	// Search for a header of the specified type
	LZ_RESULT result =
		lz_get_staging_hdr(hdr_type, &staging_hdr, lz_core_boot_params->info.cur_nonce);

	// And verify it
	if (result == LZ_SUCCESS) {
		dbgprint(DBG_INFO, "INFO: Found requested staging element %s, verifying it...\n",
				 HDR_TYPE_STRING[hdr_type]);

		if (lz_core_verify_staging_elem_hdr(staging_hdr,
											(((uint8_t *)staging_hdr) + sizeof(lz_auth_hdr_t)),
											lz_core_boot_params->info.cur_nonce) != LZ_SUCCESS) {
			result = LZ_ERROR;
		}
	}

	return result;
}

void lz_get_curr_nonce(uint8_t *nonce)
{
	memcpy(nonce, lz_core_boot_params->info.cur_nonce, LEN_NONCE);
}

uint32_t lz_get_num_staging_elems(void)
{
	uint32_t staging_area_size = sizeof(lz_staging_area.content);
	uint32_t cursor = 0;
	uint8_t num_elements = 0;
	lz_auth_hdr_t *hdr;

	// Cursor holds the current position in the staging area
	while (cursor < staging_area_size) {
		hdr = (lz_auth_hdr_t *)(((uint32_t)&lz_staging_area.content) + cursor);

		// Check whether header is sane
		if (hdr->content.magic != LZ_MAGIC) {
			dbgprint(DBG_INFO, "INFO: Staging area contains %d elements\n", num_elements,
					 hdr->content.magic);
			goto exit;
		}

		num_elements++;

		// Move the cursor to the next header
		cursor += (sizeof(lz_auth_hdr_t) + hdr->content.payload_size);
	}

exit:
	return num_elements;
}

static LZ_RESULT lz_get_staging_elem_content(hdr_type_t elem_type, uint8_t **content)
{
	lz_auth_hdr_t *hdr = NULL;

	if (lz_get_staging_hdr(elem_type, &hdr, lz_core_boot_params->info.cur_nonce) == LZ_SUCCESS) {
		*content = (((uint8_t *)hdr) + sizeof(lz_auth_hdr_t));
		return LZ_SUCCESS;
	} else {
		*content = NULL;
		return LZ_NOT_FOUND;
	}
}
