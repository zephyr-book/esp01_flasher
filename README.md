# ZBook ESP-01 USB ↔ UART flashing bridge

Firmware for the **ZBook** board (RP2350B) that turns the board into a
**transparent USB-serial bridge to the on-board ESP-01 (ESP8266)**, so you can
reflash the ESP8266 with `esptool` straight over the board's USB port — no
external USB-UART adapter needed.

The bridge connects the RP2350 **native USB CDC ACM** port to **`uart1`**
(TX=GP20, RX=GP21), where the ESP module lives. It is built on the **Zephyr**
RTOS, and the board itself comes from the [`zephyr-book/zbook`](https://github.com/zephyr-book/zbook)
Zephyr module pulled in by `west.yml`.

> This app deliberately does **not** use the `zbook_wifi` shield / ESP-AT
> driver — that would claim `uart1` and reset the modem. Here `uart1` stays a
> plain UART the bridge drives byte-for-byte.

---

## What it does

- **Transparent bridge** — bytes flow `USB CDC ACM ⇄ uart1` in both directions,
  interrupt-driven with a ring buffer per direction.
- **Baud tracking** — the host's CDC line coding is applied to `uart1` at
  runtime (`CONFIG_UART_USE_RUNTIME_CONFIGURE`), so esptool's high-speed
  flashing (e.g. `--baud 460800`) works, not just 115200.
- **Reset control** — the host **RTS** line is mirrored onto the ESP **reset**
  line (GP18, active-low), so esptool's `--before default-reset` resets the ESP
  automatically.
- **You ground GPIO0 by hand** — the bootloader strap is not driven by the
  firmware. Hold ESP **GPIO0 → GND** while resetting to enter the ROM bootloader.

The ESP **power** line (GP19) is driven on and **reset** released at boot, so
the module runs normally until a host asserts RTS.

---

## Build & flash the RP2350

Build the bridge firmware (base target — **no** `--shield`, **no** `-S`):

```bash
west build -p always -b zbook/rp2350b/m33
```

Flash it onto the RP2350 over SWD with the Raspberry Pi build of OpenOCD (the
stock one lacks the `rp2350` target), exactly as for any ZBook firmware:

```bash
west flash \
  --openocd ~/.local/bin/usr/local/bin/openocd \
  --openocd-search ~/.local/openocd
```

`just flash` wraps this with the paths set at the top of the `justfile`. You can
also drag the generated `build/zephyr/zephyr.uf2` onto the RP2350 UF2 drive.

> **Workspace setup** (first time): this repo is a west app inside the
> `zephyr-book-workspace`. Run `west update` from the workspace once so Zephyr,
> the modules and the `zbook` board module are fetched. Run build/flash commands
> from inside this directory.

---

## Flashing the ESP-01

> **Prerequisite:** the host needs [esptool](https://github.com/espressif/esptool)
> **v5 or newer** — install with `pipx install esptool` (or `pip install esptool`).

The ESP-01 AT firmware is **not** a single image — it is several binaries written
at fixed offsets. The matching ESP8266 NONOS SDK 3.0.6 **Nano AT (512+512)** set
for a **1 MB** flash is vendored under `ESP8266_NONOS_SDK-3.0.6/bin/`, and the
`just flash-at` recipe writes each one to the right place.

1. Build & flash the bridge firmware onto the RP2350 (above), then connect the
   board over USB. It enumerates as a CDC ACM serial port, e.g.
   `/dev/cu.usbmodemXXXX` (macOS) or `/dev/ttyACM0` (Linux).
2. **Hold ESP `GPIO0` to `GND`** (jumper to ground) to select the bootloader.
3. Flash. RTS is wired to the ESP reset, so `--before default-reset` drops the
   chip into the bootloader for you:

   ```bash
   just flash-at                            # uses port /dev/tty.usbmodem11401
   just flash-at port=/dev/cu.usbmodemXXXX  # or override the port
   ```

   `just flash-at` runs the following from `ESP8266_NONOS_SDK-3.0.6/bin/`:

   ```bash
   esptool --chip esp8266 --port /dev/cu.usbmodemXXXX --baud 460800 \
       --before default-reset --after hard-reset \
       write-flash --flash-size 1MB --flash-mode dio --flash-freq 40m \
       0x00000 boot_v1.7.bin \
       0x01000 ./at/512+512/user1.1024.new.2.bin \
       0xfc000 esp_init_data_default_v08.bin \
       0x7e000 blank.bin \
       0xfe000 blank.bin
   ```

   - Smoke test first: `esptool --chip esp8266 --port /dev/cu.usbmodemXXXX flash-id`.
   - Lower `esp_baud` in the `justfile` if 460800 is flaky (115200 always works).
   - If Wi-Fi misbehaves afterwards, swap `esp_init_data_default_v08.bin` for the
     `…_v05.bin`; if the chip boot-loops, retry with `--flash-mode dout`.
4. When done, **remove the GPIO0 jumper** and let `--after hard-reset` (RTS) or a
   power cycle restart the ESP into the freshly-flashed AT firmware.

### If auto-reset misbehaves

Some hosts/timings don't drive RTS the way the ESP expects through a USB bridge.
Keep GPIO0 grounded, drop the ESP into the bootloader by hand (power-cycle, or
pulse RTS from your terminal), then run the same flash with auto-reset disabled —
from `ESP8266_NONOS_SDK-3.0.6/bin/`:

```bash
esptool --chip esp8266 --port /dev/cu.usbmodemXXXX \
    --before no-reset --after no-reset \
    write-flash --flash-size 1MB --flash-mode dio --flash-freq 40m \
    0x00000 boot_v1.7.bin \
    0x01000 ./at/512+512/user1.1024.new.2.bin \
    0xfc000 esp_init_data_default_v08.bin \
    0x7e000 blank.bin \
    0xfe000 blank.bin
```

---

## Diagnostics

Firmware log/console is on **UART0** (115200 baud) — separate from the USB
bridge port. Connect a serial monitor there to see messages like
`uart1 -> 460800 baud` (baud changes) and `ESP-01 USB <-> uart1 bridge ready`.

---

## Repository structure

```
esp01_flasher/
├── app.overlay              # USB CDC ACM node + ESP power/reset (GP19/GP18)
├── dts/bindings/            # binding for the esp01 control-lines node
├── include/usbd_setup.h
├── src/
│   ├── main.c               #   bridge: ISR pump, baud tracking, RTS→reset
│   └── usbd_setup.c         #   minimal device_next USB device (CDC ACM)
├── prj.conf                 # USB device_next + CDC ACM + UART line-ctrl/runtime cfg
├── CMakeLists.txt           # globs src/*.c; board comes from the zbook module
└── west.yml                 # manifest: Zephyr + modules + zephyr-book/zbook
```
