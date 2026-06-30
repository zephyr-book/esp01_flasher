/**
 * @file usbd_setup.h
 * @brief Minimal USB device (device_next) context for the CDC ACM bridge.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP01_FLASHER_USBD_SETUP_H
#define ESP01_FLASHER_USBD_SETUP_H

#include <zephyr/usb/usbd.h>

/**
 * @brief Build and initialize the USB device context (CDC ACM class).
 *
 * Registers the string/configuration descriptors and all enabled USB classes,
 * then calls usbd_init(). The caller is responsible for usbd_enable().
 *
 * @param msg_cb USBD message callback (line coding / control line / VBUS), or
 *               NULL to skip registration.
 * @return Pointer to the initialized context, or NULL on failure.
 */
struct usbd_context *flasher_usbd_init(usbd_msg_cb_t msg_cb);

#endif /* ESP01_FLASHER_USBD_SETUP_H */
