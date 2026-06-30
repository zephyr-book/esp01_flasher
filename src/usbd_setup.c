/**
 * @file usbd_setup.c
 * @brief Minimal USB device (device_next) context for the CDC ACM bridge.
 *
 * Distilled from samples/subsys/usb/common/sample_usbd_init.c: a single
 * full-speed configuration exposing the CDC ACM class, with literal VID/PID and
 * string descriptors instead of the sample's Kconfig knobs.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usbd_setup.h"

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(usbd_setup, LOG_LEVEL_INF);

/*
 * Zephyr project test VID/PID. Fine for a local bench tool; obtain a real
 * VID/PID before distributing hardware.
 */
#define FLASHER_USBD_VID 0x2fe3
#define FLASHER_USBD_PID 0x0001

/* Bus-powered, no remote wakeup. */
#define FLASHER_USBD_ATTRIBUTES 0
/* Reported max draw, in 2 mA units (250 mA). */
#define FLASHER_USBD_MAX_POWER  125

USBD_DEVICE_DEFINE(flasher_usbd, DEVICE_DT_GET(DT_NODELABEL(usbd)), FLASHER_USBD_VID,
		   FLASHER_USBD_PID);

USBD_DESC_LANG_DEFINE(flasher_lang);
USBD_DESC_MANUFACTURER_DEFINE(flasher_mfr, "Centro de Inovacao EDGE");
USBD_DESC_PRODUCT_DEFINE(flasher_product, "ZBook ESP-01 USB-UART bridge");
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(flasher_sn)));

USBD_DESC_CONFIG_DEFINE(flasher_fs_cfg_desc, "FS Configuration");

USBD_CONFIGURATION_DEFINE(flasher_fs_config, FLASHER_USBD_ATTRIBUTES, FLASHER_USBD_MAX_POWER,
			  &flasher_fs_cfg_desc);

struct usbd_context *flasher_usbd_init(usbd_msg_cb_t msg_cb)
{
	int err;

	err = usbd_add_descriptor(&flasher_usbd, &flasher_lang);
	if (err) {
		LOG_ERR("Failed to add language descriptor (%d)", err);
		return NULL;
	}

	err = usbd_add_descriptor(&flasher_usbd, &flasher_mfr);
	if (err) {
		LOG_ERR("Failed to add manufacturer descriptor (%d)", err);
		return NULL;
	}

	err = usbd_add_descriptor(&flasher_usbd, &flasher_product);
	if (err) {
		LOG_ERR("Failed to add product descriptor (%d)", err);
		return NULL;
	}

	IF_ENABLED(CONFIG_HWINFO, (err = usbd_add_descriptor(&flasher_usbd, &flasher_sn);))
	if (err) {
		LOG_ERR("Failed to add serial number descriptor (%d)", err);
		return NULL;
	}

	err = usbd_add_configuration(&flasher_usbd, USBD_SPEED_FS, &flasher_fs_config);
	if (err) {
		LOG_ERR("Failed to add Full-Speed configuration (%d)", err);
		return NULL;
	}

	err = usbd_register_all_classes(&flasher_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		LOG_ERR("Failed to register classes (%d)", err);
		return NULL;
	}

	/* CDC ACM owns an Interface Association Descriptor: use the IAD triple. */
	usbd_device_set_code_triple(&flasher_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02,
				    0x01);

	if (msg_cb != NULL) {
		err = usbd_msg_register_cb(&flasher_usbd, msg_cb);
		if (err) {
			LOG_ERR("Failed to register message callback (%d)", err);
			return NULL;
		}
	}

	err = usbd_init(&flasher_usbd);
	if (err) {
		LOG_ERR("Failed to initialize device support (%d)", err);
		return NULL;
	}

	return &flasher_usbd;
}
