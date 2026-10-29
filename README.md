# URS V2

Firmware for a DFRobot FireBeetle 2 ESP32-S3 board that plugs into a lab instrument's computer and shows up as a USB drive. When the instrument stops writing files, the board uploads them over WiFi to the company portal, and a small screen shows what it's doing.

## How it works

1. The instrument's computer sees the board as a USB drive (backed by an SD card) and saves files to it.
2. Once the computer has been quiet for 10 seconds, the board briefly hides the drive, scans the card, and uploads each file over HTTPS with the board's device ID.
3. Uploaded files are moved to `/backup` on the card, and the drive comes back.
4. The 2" screen shows WiFi status, upload status, and a progress bar throughout.

## What's in the repo

| Path | What it is |
| --- | --- |
| `main/main.cpp` | The main program: USB drive, SD card, WiFi, HTTPS uploads, moving files to `/backup`, and safeguards (watchdog restart, WiFi retries, skipping hidden macOS files). |
| `main/status_display.cpp` / `.h` | Drives the 2" status screen: model name, WiFi status, upload status, progress bar, and current file. |
| `main/secrets.h.example` | Template for the WiFi name, password, and portal URL. Copy it to `main/secrets.h` and fill it in. `secrets.h` is never committed. |
| `main/idf_component.yml` | Pulls in the Arduino-ESP32 framework. |
| `components/DFRobot_LcdDisplay/` | The screen maker's library for the DFR0997 display. |
| `components/arduino_tinyusb/` | The USB library that lets the board act as a USB drive. |
| `sdkconfig.defaults` | Board settings: USB drive support, HTTPS support, flash size and partitions, and long file names on the SD card. |
| `CMakeLists.txt` | Build setup. `PROJECT_VER` sets the firmware version. |

## Hardware

- **Board:** DFRobot FireBeetle 2 ESP32-S3 (DFR0975)
- **SD card (SPI):** CS 14, SCK 17, MISO 16, MOSI 15
- **Display:** DFRobot DFR0997 2.0" I2C display at address 0x2C (SDA 1, SCL 2)

## Building and flashing

Requires ESP-IDF v5.5.6.

```sh
cp main/secrets.h.example main/secrets.h   # then fill in your values
source ~/.espressif/tools/activate_idf_v5.5.6.sh
idf.py build
# unplug the board, hold BOOT while plugging it back in, then release
idf.py flash
```
