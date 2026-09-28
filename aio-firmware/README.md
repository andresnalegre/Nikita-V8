# AIO ESP32 firmware — Nikita Marauder v2.0.0

Everything needed to understand, reflash, or rebuild the ESP32 firmware that runs on
the **SecureTechware 3-in-1 AIO (V1.4)** board plugged into the Flipper's GPIO. This
lives inside Nikita-V8 (an official repo) so the external working folders
(`nikita-marauder/`, `3in1-AIO-Expansion-Board-FlipperZero/`) can be deleted.

## The board
- 3 radios, ONE active at a time (mode switch + LED): 🟢 green = **ESP32-S2 / WiFi**,
  🔵 blue = **CC1101 / Sub-GHz**, 🔴 red = **nRF24 / 2.4GHz**, middle = flash mode.
  Plus an **RX/TX** switch — leave on **TX**.
- ESP32-S2R2, 4 MB flash, native USB. **No Bluetooth** (S2 is WiFi-only; the "BLE" the
  box markets is the nRF24, driven by the Flipper).
- CC1101 and nRF24 are wired to the **Flipper's SPI** — driven by the Flipper's own
  Sub-GHz / nrf tools, NOT by the ESP32.

## What v2.0.0 is
Marauder **v1.17.0** (justcallmekoko) + our `src/NikitaBridge.{h,cpp}`, versioned
**v2.0.0**. NikitaBridge adds a UART heartbeat `[NIKITA-AIO:UP:v2.0.0]` every 3 s so
the Flipper can show **GPIO UP! / GPIO DOWN**. (A WiFi command server was intentionally
NOT added — an open network command executor is an RCE surface. Nikita drives the board
through the Flipper's WIFI-app mailbox instead.)

Marauder is driven from the Flipper via the WIFI app mailbox:
`/ext/apps_data/nikita_wifi/cmd` (write a command) → `/ext/apps_data/nikita_wifi/last.log`
(read the output).

## Reflash (only if ever needed — the board is normally DONE)
Prebuilt binaries are in `firmware/`. Put the board in download mode:
switch = middle, **plug the USB-C while holding RT+BT together, release RT, then BT**.
```bash
PY=<Nikita-V8>/toolchain/arm64-darwin/bin/python3   # any python with esptool works
$PY -m esptool --chip esp32s2 --port /dev/cu.usbmodem01 --before no_reset --after hard_reset \
  write_flash -z --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x1000 firmware/bootloader.bin \
  0x8000 firmware/partitions.bin \
  0xe000 firmware/boot_app0.bin \
  0x10000 firmware/nikita-marauder-v2.0.0.bin
```
`firmware/board-stock-v1.0.0-4MB.bin` is the full 4 MB backup of what the board shipped
with (restore with `write_flash 0x0 board-stock-v1.0.0-4MB.bin`).

## Rebuild from source (if you change NikitaBridge)
1. `git clone --branch v1.17.0 https://github.com/justcallmekoko/ESP32Marauder`
2. In `esp32_marauder/configs.h`: enable `#define MARAUDER_FLIPPER` and set
   `#define MARAUDER_VERSION "v2.0.0"`.
3. Copy `src/NikitaBridge.h` and `src/NikitaBridge.cpp` into `esp32_marauder/`.
4. In `esp32_marauder.ino`: add `#include "NikitaBridge.h"`, call `nikitaBridge.begin();`
   at the end of `setup()`, and `nikitaBridge.loop();` at the top of `loop()`.
5. Bundled libs it needs (arduino-cli): LinkedList, ArduinoJson, MicroNMEA,
   Adafruit NeoPixel, AsyncTCP, EspSoftwareSerial, and ESP32Ping (from
   github.com/marian-craciunescu/ESP32Ping into the sketch's libraries/).
6. Build with **core esp32 2.0.11** (NOT 3.x — it drops the deauth symbols):
```bash
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s2:CDCOnBoot=default,PartitionScheme=huge_app,FlashSize=4M,PSRAM=enabled" \
  --build-property "compiler.c.elf.extra_flags=-Wl,-zmuldefs" \
  --libraries ESP32Marauder/libraries  ESP32Marauder/esp32_marauder
```
Critical flags: **`CDCOnBoot=default`** (Serial = UART0 = the Flipper link; `cdc` sends
Marauder over USB and the Flipper never hears it), **`PSRAM=enabled`** (without it the
S2R2 crashes on scanap — EXCCAUSE 0x1d), and **`-Wl,-zmuldefs`** (Marauder redefines
`ieee80211_raw_frame_sanity_check`).
