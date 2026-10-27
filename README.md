# Home Heritage Companion

Firmware for the **LCDWiki ES3C28P** ESP32-S3 2.8-inch capacitive touchscreen.

## Current implementation

This is an early, hardware-unverified firmware baseline. It now contains working software paths (not simulated readings) for:

- LVGL touchscreen dashboard with **Outdoor / OurHeritage**, **Music**, **Indoor / Mochi**, and **Settings** pages.
- Wi-Fi station connection, on-screen SSID/password entry, local credential storage with ESP32 Preferences, and reconnect attempts.
- Indoor / Mochi page with clearly labelled demo readings until LoRa pins are verified; live UART JSON telemetry parser is ready for verified wiring.
- Outdoor / OurHeritage page querying live Open-Meteo Weather, Open-Meteo Air Quality, Open-Meteo Flood, USGS Earthquake, and NASA POWER APIs for a Jakarta demo location. Includes rule-based conservation recommendations based on returned indicators; unavailable APIs are shown as unavailable rather than inventing readings.
- MP3/M4A player reads tracks from the `/Music` folder on microSD, supports an optional Shuffle toggle that randomizes upcoming tracks without immediately replaying the current track, and calls `audio.loop()` continuously; main-loop delay is set to 1 ms to keep audio playback responsive.
- Serial diagnostics at 115200 baud.

Sensor values are shown only after a valid telemetry packet is received. No fake sensor readings are generated.

## Important limitations

- **Do not flash yet without checking the exact ES3C28P revision and pin map.** The PlatformIO environment currently uses a generic ESP32-S3 DevKit board profile. Its reported flash/PSRAM variant does not automatically prove the ES3C28P hardware configuration.
- Display and touch GPIOs are based on the current working assumptions and must be verified against LCDWiki's board demo/documentation.
- LoRa UART is intentionally disabled by default because the E220/E32 module wiring and free ESP32-S3 GPIOs have not been confirmed. Set `HHC_LORA_RX_PIN` and `HHC_LORA_TX_PIN` only after verifying wiring. The receiver currently expects transparent UART mode and newline-delimited JSON; see [docs/lora-protocol.md](docs/lora-protocol.md).
- E220/E32 module mode pins, AUX behavior, frequency/channel, air data rate, addressing and regulatory settings still need to be matched between master and slave.
- Outdoor API display currently uses fixed Jakarta coordinates as a proof of concept. Site selection, location GPS, full web dashboard features, and persistent offline API cache are not yet ported to firmware.
- Audio output depends on the ES8311 / amplifier configuration and a compatible microSD card. The playback loop has been tested by the project owner; retain the 1 ms main-loop delay unless audio is retested after future changes.
- This repository is separate from the OurHeritage web application and does not copy its source.

## Hardware target to verify

- Board: LCDWiki ES3C28P (ESP32-S3)
- Display: ILI9341V, 240×320 SPI
- Touch: capacitive FT6336G over I²C
- Vendor documentation describes 16 MB flash / 8 MB PSRAM; verify the exact board revision.

Official references:
- [LCDWiki product and documentation page](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display)
- [LCDWiki Arduino demo instructions](https://www.lcdwiki.com/res/ES3C28P/2.8inch_ES3C28P_ES3N28P_arduino_Demo_Instructions.pdf)
- [ES3C28P board-support reference](https://github.com/ngttai/esp32_s3_es3c28p)

## NASA FIRMS hotspot lookup

The Outdoor Intelligence page can query NASA FIRMS VIIRS near-real-time hotspot records within 100 km of the current demo coordinates.

To configure the key locally:

1. Create `include/config.h` on your computer (this path is ignored by Git).
2. Add the following line, replacing the empty value with your own key:
   `#define HHC_FIRMS_MAP_KEY "YOUR_MAP_KEY"`
3. Build with PlatformIO.

Never commit `include/config.h`. The repository intentionally contains no live MAP_KEY. **Important:** a key compiled into ESP32 firmware can be extracted from the firmware/device, so this prevents accidental publication in source control but does not make the key truly secret. For a distributed device, prefer a small server-side proxy with rate limits rather than embedding a reusable key in firmware. If a key has already been shared publicly or exposed unintentionally, rotate it through the provider.

Hotspot records are satellite detections, not a direct measurement of smoke or a fire-confirmed emergency at the user's location. Combine them with air-quality indicators, wind context and official local advisories; never treat this feature as an official warning.

## Build

1. Install Visual Studio Code and the PlatformIO IDE extension.
2. Open this repository folder in VS Code.
3. In the PlatformIO terminal, run `pio run`.
4. Fix all compiler errors before connecting/uploading to the board.
5. Verify display, touch, flash/PSRAM and GPIOs using the official ES3C28P demo before any hardware upload.

## LoRa protocol foundation

The master receiver uses UART2 in transparent mode and accepts one JSON object per line. The schema and example packet are documented in [docs/lora-protocol.md](docs/lora-protocol.md). The current receiver is a protocol foundation only; it does not configure radio frequency, addressing or E220/E32 mode pins.

## Roadmap

1. Validate board profile, display driver, touch orientation and memory configuration.
2. Validate the master LoRa UART and agree on packet format/settings with Mochi slave.
3. Add OurHeritage content retrieval and an offline local cache.
4. Implement MP3 playback after validating the audio codec and storage interface.
5. Add telemetry history, thresholds and notifications.

## Safety

Use the board's documented power input and a suitably protected battery configuration. Do not treat SGP30 eCO2/TVOC values as a certified gas-leak or life-safety detector.
