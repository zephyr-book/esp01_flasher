alias c := clean
alias b := build
alias f := flash
alias fa := flash-at

openocd_bin := "~/.local/bin/usr/local/bin/openocd"
openodc_dir := "~/.local/openocd"

# ESP-01 (ESP8266) AT firmware flashing -- 1MB flash, Nano AT (512+512).
esp_port := "/dev/tty.usbmodem11401"
esp_baud := "460800"
sdk_bin := "ESP8266_NONOS_SDK-3.0.6/bin"

clean:
    rip build

build:
    west build -b zbook/rp2350b/m33

build-wifi:
    west build -b zbook/rp2350b/m33 --shield zbook_wifi

flash:
    west flash --openocd {{ openocd_bin }} --openocd-search {{ openodc_dir }}

erase:
    {{ openocd_bin }} -f interface/cmsis-dap.cfg -f target/rp2350.cfg -c "init; reset halt; flash erase_sector 0 0 last; shutdown"

# Flash ESP-01 AT firmware over the USB<->uart1 bridge. Ground ESP GPIO0 to GND first.
flash-at port=esp_port:
    cd {{ sdk_bin }} && esptool --chip esp8266 --port {{ port }} --baud {{ esp_baud }} \
        --before default-reset --after hard-reset \
        write-flash --flash-size 1MB --flash-mode dio --flash-freq 40m \
        0x00000 boot_v1.7.bin \
        0x01000 ./at/512+512/user1.1024.new.2.bin \
        0xfc000 esp_init_data_default_v08.bin \
        0x7e000 blank.bin \
        0xfe000 blank.bin
