/**
 * @file main.c
 * @brief ESP-01 (ESP8266) USB <-> uart1 transparent flashing bridge.
 *
 * Bridges the RP2350 native USB CDC ACM port to uart1 (where the ESP-01 lives)
 * byte-for-byte, so a host tool such as esptool can talk to the ESP8266 ROM
 * bootloader straight over USB. Two features make it a usable flasher:
 *
 *   - Baud tracking: the host's CDC line coding is applied to uart1 at runtime,
 *     so esptool's high-speed flashing (e.g. --baud 460800) works.
 *   - Reset control: the host RTS line is mirrored onto the ESP reset (GP18),
 *     so esptool's `--before default_reset` resets the chip automatically. The
 *     GPIO0 bootloader strap is grounded by hand.
 *
 * @copyright Copyright (c) 2026 Centro de Inovacao EDGE
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usbd_setup.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(esp01_flasher, LOG_LEVEL_INF);

/* One ring buffer per direction; sized for a few USB full-speed packets. */
#define BRIDGE_RING_SIZE 2048
/* Bytes moved per FIFO read/fill burst inside the ISR. */
#define BRIDGE_CHUNK     64

static const struct device *const cdc_dev = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);
static const struct device *const esp_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

static const struct gpio_dt_spec esp_power = GPIO_DT_SPEC_GET(DT_NODELABEL(esp01), power_gpios);
static const struct gpio_dt_spec esp_reset = GPIO_DT_SPEC_GET(DT_NODELABEL(esp01), reset_gpios);

RING_BUF_DECLARE(rb_usb_to_esp, BRIDGE_RING_SIZE);
RING_BUF_DECLARE(rb_esp_to_usb, BRIDGE_RING_SIZE);

/**
 * @brief One side of the bridge.
 *
 * Bytes read from @ref dev are stored in @ref rx_ring; the @ref peer drains
 * that same ring through its TX path. So a port transmits out of
 * `peer->rx_ring`. @ref rx_throttled records that RX was disabled because
 * @ref rx_ring was full; the peer's TX path re-enables it once it frees space.
 */
struct bridge_port {
	const struct device *dev;
	struct ring_buf *rx_ring;
	struct bridge_port *peer;
	bool rx_throttled;
};

static struct bridge_port usb_port;
static struct bridge_port esp_port;

static void bridge_isr(const struct device *dev, void *user_data)
{
	struct bridge_port *port = user_data;
	uint8_t buf[BRIDGE_CHUNK];

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		/* RX: dev FIFO -> our rx_ring, then wake the peer's TX. */
		if (!port->rx_throttled && uart_irq_rx_ready(dev)) {
			size_t space = ring_buf_space_get(port->rx_ring);

			if (space == 0) {
				uart_irq_rx_disable(dev);
				port->rx_throttled = true;
			} else {
				int n = uart_fifo_read(dev, buf, MIN(space, sizeof(buf)));
				int put;

				if (n < 0) {
					n = 0;
				}
				put = ring_buf_put(port->rx_ring, buf, n);
				if (put < n) {
					LOG_WRN("dropped %d bytes", n - put);
				}
				if (put > 0) {
					uart_irq_tx_enable(port->peer->dev);
				}
			}
		}

		/*
		 * TX: drain the peer's rx_ring (bytes destined out of dev).
		 * Claim/finish so only the bytes the FIFO actually accepted are
		 * consumed -- uart_fifo_fill() can take fewer than offered when
		 * the FIFO fills, and dropping the remainder would corrupt large
		 * transfers (e.g. esptool flash blocks).
		 */
		if (uart_irq_tx_ready(dev)) {
			struct ring_buf *src = port->peer->rx_ring;
			uint8_t *out;
			uint32_t claimed = ring_buf_get_claim(src, &out, BRIDGE_CHUNK);

			if (claimed == 0) {
				uart_irq_tx_disable(dev);
			} else {
				int sent = uart_fifo_fill(dev, out, claimed);

				ring_buf_get_finish(src, sent > 0 ? (uint32_t)sent : 0);

				/* Freed ring space -> let the peer's RX resume. */
				if (port->peer->rx_throttled) {
					uart_irq_rx_enable(port->peer->dev);
					port->peer->rx_throttled = false;
				}
			}
		}
	}
}

/** Apply the host's CDC line coding (baud, parity, etc.) to uart1. */
static void apply_host_line_coding(const struct device *cdc)
{
	struct uart_config cfg;
	int err;

	err = uart_config_get(cdc, &cfg);
	if (err) {
		LOG_WRN("Failed to read host line coding (%d)", err);
		return;
	}

	err = uart_configure(esp_dev, &cfg);
	if (err) {
		LOG_WRN("Failed to reconfigure uart1 to %u baud (%d)", cfg.baudrate, err);
	} else {
		LOG_INF("uart1 -> %u baud", cfg.baudrate);
	}
}

static void usbd_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			if (usbd_enable(ctx)) {
				LOG_ERR("Failed to enable device support");
			}
			return;
		}
		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			if (usbd_disable(ctx)) {
				LOG_ERR("Failed to disable device support");
			}
			return;
		}
	}

	if (msg->type == USBD_MSG_CDC_ACM_LINE_CODING) {
		apply_host_line_coding(msg->dev);
	} else if (msg->type == USBD_MSG_CDC_ACM_CONTROL_LINE_STATE) {
		uint32_t rts = 0;

		/*
		 * esptool drives RTS onto the ESP reset line: RTS asserted holds
		 * the chip in reset; releasing it (with GPIO0 grounded) boots the
		 * ROM bootloader. reset-gpios is active-low, so a logical "active"
		 * (1) asserts reset.
		 */
		(void)uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_RTS, &rts);
		(void)gpio_pin_set_dt(&esp_reset, rts ? 1 : 0);
		LOG_DBG("RTS=%u -> ESP reset %s", rts, rts ? "asserted" : "released");
	}
}

int main(void)
{
	struct usbd_context *ctx;

	if (!device_is_ready(cdc_dev)) {
		LOG_ERR("CDC ACM device not ready");
		return 0;
	}
	if (!device_is_ready(esp_dev)) {
		LOG_ERR("uart1 (ESP) device not ready");
		return 0;
	}
	if (!gpio_is_ready_dt(&esp_power) || !gpio_is_ready_dt(&esp_reset)) {
		LOG_ERR("ESP control GPIOs not ready");
		return 0;
	}

	/* Power the ESP and release reset; the host RTS line drives reset from here. */
	(void)gpio_pin_configure_dt(&esp_power, GPIO_OUTPUT_ACTIVE);
	(void)gpio_pin_configure_dt(&esp_reset, GPIO_OUTPUT_INACTIVE);

	usb_port.dev = cdc_dev;
	usb_port.rx_ring = &rb_usb_to_esp;
	usb_port.peer = &esp_port;

	esp_port.dev = esp_dev;
	esp_port.rx_ring = &rb_esp_to_usb;
	esp_port.peer = &usb_port;

	ctx = flasher_usbd_init(usbd_msg_cb);
	if (ctx == NULL) {
		LOG_ERR("Failed to initialize USB device");
		return 0;
	}

	if (!usbd_can_detect_vbus(ctx)) {
		if (usbd_enable(ctx)) {
			LOG_ERR("Failed to enable USB device support");
			return 0;
		}
	}

	uart_irq_callback_user_data_set(cdc_dev, bridge_isr, &usb_port);
	uart_irq_callback_user_data_set(esp_dev, bridge_isr, &esp_port);
	uart_irq_rx_enable(cdc_dev);
	uart_irq_rx_enable(esp_dev);

	LOG_INF("ESP-01 USB <-> uart1 bridge ready");
	return 0;
}
