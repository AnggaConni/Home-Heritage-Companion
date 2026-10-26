# Home Heritage Companion

Firmware for the **LCDWiki ES3C28P** ESP32-S3 touchscreen.

## Goal
A local-first home companion with a native touchscreen dashboard, online/offline status, OurHeritage content sync, MP3 playback, and LoRa telemetry from the Mochi sensor slave.

## Milestone 1
- Native UI scaffold
- Wi-Fi status and setup
- Future modules clearly separated

## Hardware
- LCDWiki ES3C28P / ESP32-S3
- 2.8-inch 240x320 TFT
- Capacitive FT6336G touch

## Important
This is a project starter. Confirm board-specific display/touch drivers, pin mapping, flash/PSRAM settings, audio and microSD wiring before uploading firmware. Wokwi cannot fully emulate the physical board. Do not treat the generic starter as tested hardware firmware.

## Development
Install VS Code and PlatformIO IDE. Clone this repo and build before upload.

OurHeritage remains in its own repository; this firmware repo does not copy its web application code.