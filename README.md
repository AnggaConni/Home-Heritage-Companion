# Home Heritage Companion

Firmware for the **LCDWiki ES3C28P** ESP32-S3 2.8-inch capacitive touchscreen.

## Goal
A local-first home companion with a native touchscreen dashboard, online/offline status, OurHeritage content synchronization, MP3 playback, and LoRa telemetry from the Mochi sensor slave.

## Milestone 1 status

The repository contains an initial **software scaffold** for the UI and Wi-Fi flow. It is not yet validated for physical hardware. In particular, display/touch initialization must be replaced with a known-compatible ES3C28P board-support implementation before flashing.

## Hardware target
- Board: LCDWiki ES3C28P (ESP32-S3)
- Display: ILI9341V, 240×320 SPI
- Touch: capacitive FT6336G over I²C
- Flash/PSRAM: vendor documentation describes 16 MB flash / 8 MB PSRAM

Official references:
- [LCDWiki product and documentation page](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display)
- [LCDWiki Arduino demo instructions](https://www.lcdwiki.com/res/ES3C28P/2.8inch_ES3C28P_ES3N28P_arduino_Demo_Instructions.pdf)
- [ES3C28P board-support reference](https://github.com/ngttai/esp32_s3_es3c28p)

## Development
1. Install [Visual Studio Code](https://code.visualstudio.com/).
2. Install the PlatformIO IDE extension.
3. Clone this repository and open the folder.
4. Build first, review compiler output, then validate the exact board driver before uploading.

## Simulation
Wokwi can validate selected ESP32-S3 logic and compatible peripherals, but it does not fully emulate this board's FT6336G touch, ES8311 audio codec, SDIO microSD interface, battery circuit, or LoRa hardware. Separate physical validation is required.

## Roadmap
1. Native UI + Wi-Fi status/configuration
2. OurHeritage content sync and offline cache
3. MP3 player using the onboard audio path and local storage
4. LoRa receiver for Mochi telemetry
5. Sensor history and notifications

## Safety and scope
Do not upload a generic display configuration until the ES3C28P panel and touch driver are verified. Use a suitable protected 3.7 V Li-ion/LiPo battery and the board's intended battery connector; confirm polarity and power instructions in the vendor manual. This firmware repository is separate from the OurHeritage web application and does not copy its source.
