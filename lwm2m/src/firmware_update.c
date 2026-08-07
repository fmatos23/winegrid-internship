/*
 * Origem: baseado no exemplo oficial do Zephyr
 * (samples/net/lwm2m_client/src/firmware_update.c, ver copyright abaixo),
 * cujo Object 5 (Firmware Update) vinha como placeholder sem ligação real
 * ao MCUboot. A lógica de escrita real na partição secundária
 * (flash_img_init()/flash_img_buffered_write()), o disparo do upgrade
 * (boot_request_upgrade()) e o diagnóstico/correção do bug de OTA foram
 * escritos/reescritos nesta colaboração (Claude).
 *
 * Copyright (c) 2022 Nordic Semiconductor
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_MODULE_NAME app_fw_update
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include <zephyr/net/lwm2m.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/irq.h>
#include "modules.h"

static uint8_t firmware_buf[64];

/* Array with supported PULL firmware update protocols */
static uint8_t supported_protocol[1];

static struct flash_img_context dfu_ctx;
static bool dfu_started;

static int firmware_update_cb(uint16_t obj_inst_id,
			      uint8_t *args, uint16_t args_len)
{
	int ret;

	LOG_INF("Applying firmware update (%zu bytes written)",
		flash_img_bytes_written(&dfu_ctx));

	if (!dfu_started) {
		LOG_ERR("No firmware image was downloaded");
		lwm2m_set_u8(&LWM2M_OBJ(5, 0, 5), RESULT_UPDATE_FAILED);
		return -EINVAL;
	}

	/* This board's MCUboot build is upgrade-only (no test/revert phase -
	 * confirmed separately with the mcumgr/smp_svr PoC, where a test-boot
	 * request is rejected outright). So request the upgrade as permanent
	 * directly, same as that PoC ended up doing.
	 */
	ret = boot_request_upgrade(BOOT_UPGRADE_PERMANENT);
	dfu_started = false;
	if (ret) {
		LOG_ERR("boot_request_upgrade failed: %d", ret);
		lwm2m_set_u8(&LWM2M_OBJ(5, 0, 5), RESULT_UPDATE_FAILED);
		return ret;
	}

	lwm2m_set_u8(&LWM2M_OBJ(5, 0, 3), STATE_IDLE);
	lwm2m_set_u8(&LWM2M_OBJ(5, 0, 5), RESULT_SUCCESS);

	LOG_INF("Rebooting to apply the new image");
	sys_reboot(SYS_REBOOT_WARM);

	return 0;
}

static void *firmware_get_buf(uint16_t obj_inst_id, uint16_t res_id,
			      uint16_t res_inst_id, size_t *data_len)
{
	*data_len = sizeof(firmware_buf);
	return firmware_buf;
}

static int firmware_block_received_cb(uint16_t obj_inst_id, uint16_t res_id,
				      uint16_t res_inst_id, uint8_t *data,
				      uint16_t data_len, bool last_block,
				      size_t total_size, size_t offset)
{
	int ret;

	if (offset == 0) {
		LOG_INF("Starting firmware download, total size %zu", total_size);
		ret = flash_img_init(&dfu_ctx);
		if (ret) {
			LOG_ERR("flash_img_init failed: %d", ret);
			return ret;
		}
		dfu_started = true;
	}

	/* No irq_lock() workaround needed here: the crash this used to cause
	 * (flash write + Wi-Fi ISR -> illegal instruction) was Zephyr issue
	 * #77952 - esp_intr_noniram_disable() was never being called during
	 * flash ops on 3.7.0, a regression from 3.6. Fixed upstream by PR
	 * #78121 (soc/espressif/esp32/soc.c + hal_espressif bump), applied
	 * locally to this checkout. Verified: full ~680KB image over Wi-Fi,
	 * zero crashes, clean MCUboot permanent swap (see notas_estagio.md).
	 */
	ret = flash_img_buffered_write(&dfu_ctx, data, data_len, last_block);
	if (ret) {
		LOG_ERR("flash_img_buffered_write failed: %d", ret);
		dfu_started = false;
		return -ENOSPC;
	}

	LOG_INF("FIRMWARE: BLOCK RECEIVED: offset:%zd len:%u last_block:%d (%zu bytes written)",
		offset, data_len, last_block, flash_img_bytes_written(&dfu_ctx));

	return 0;
}

static int firmware_cancel_cb(const uint16_t obj_inst_id)
{
	LOG_INF("FIRMWARE: Update canceled");
	dfu_started = false;
	return 0;
}

void init_firmware_update(void)
{
	/* setup data buffer for block-wise transfer */
	lwm2m_register_pre_write_callback(&LWM2M_OBJ(5, 0, 0), firmware_get_buf);
	lwm2m_firmware_set_write_cb(firmware_block_received_cb);

	/* register cancel callback */
	lwm2m_firmware_set_cancel_cb(firmware_cancel_cb);

	/* Register the update trigger regardless of PULL support: this is
	 * push mode (the server writes bytes directly into the PACKAGE
	 * resource via CoAP block-wise transfer), and the "Update" exec
	 * resource (5/0/2) always routes through this callback either way -
	 * the upstream sample only wired it up inside the PULL_SUPPORT
	 * ifdef, which left push-mode updates with no handler at all.
	 */
	lwm2m_firmware_set_update_cb(firmware_update_cb);

	if (IS_ENABLED(CONFIG_LWM2M_FIRMWARE_UPDATE_PULL_SUPPORT)) {
		lwm2m_create_res_inst(&LWM2M_OBJ(5, 0, 8, 0));
		lwm2m_set_res_buf(&LWM2M_OBJ(5, 0, 8, 0), &supported_protocol[0],
					 sizeof(supported_protocol[0]),
					 sizeof(supported_protocol[0]), 0);
	}
}
