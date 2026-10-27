# Home Heritage Companion

Firmware and experimentation project for the **LCDWiki ES3C28P**, an ESP32-S3 board with a 2.8-inch capacitive touchscreen and onboard audio hardware.

> **Project status:** early-stage proof of concept. The firmware has software paths for the dashboard and connected APIs, but hardware behavior depends on verifying the exact board revision, pin mapping, display/touch setup, audio codec, and storage configuration. A successful compile does not by itself confirm that every peripheral works on the physical board.

## Board overview

The project includes photos of the board's front and back, plus a pin-interface schematic in the `thum/` directory.

### Front of the board

![LCDWiki ES3C28P front board](thum/frontboard.png)

### Back of the board

![LCDWiki ES3C28P back board](thum/backboard.png)

### Pin-interface schematic

![LCDWiki ES3C28P pin interface schematic](<thum/shcema pin interface.png>)

For the vendor's product photos and documentation, see the [LCDWiki 2.8-inch ESP32-S3 Display — Product Picture](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display#Product_Picture).

For background on using audio-capable LCD boards with Arduino Audio Tools, see the [Arduino Audio Tools wiki: LCD Boards with Audio](https://github.com/pschatzmann/arduino-audio-tools/wiki/LCD-Boards-with-Audio).

## Features in the current firmware

- **LVGL touchscreen UI** with Outdoor / OurHeritage, Music, Indoor / Mochi, and Settings pages.
- **Wi-Fi setup** with on-screen SSID/password entry, local credential storage using ESP32 Preferences, and reconnect attempts.
- **Outdoor Intelligence proof of concept** using Open-Meteo Weather, Air Quality and Flood data, USGS earthquake information, NASA POWER, and NASA FIRMS VIIRS near-real-time hotspot records around a fixed Jakarta demo location.
- **Rule-based recommendations** based on available indicators. Unavailable API responses should be displayed as unavailable rather than treated as real measurements.
- **Music player** intended to play MP3 files from the `/Music` directory on microSD, with play/pause, previous/next, volume and shuffle controls. Audio behavior and track auto-advance still need to be confirmed on the target hardware.
- **Serial diagnostics** at 115200 baud.

The Indoor / Mochi page may show labelled demo values until the LoRa wiring is verified. Live telemetry should only be treated as real after a valid packet has been received.

## Hardware reference

| Item | Current reference |
|---|---|
| Board | LCDWiki ES3C28P, ESP32-S3 |
| Display | ILI9341V, 240 × 320 SPI (verify exact board/revision) |
| Touch | FT6336G capacitive touch over I²C (verify against the board documentation) |
| Audio | ES8311/audio amplifier path; verify codec and amplifier configuration |
| Memory | Vendor documentation lists 16 MB flash and 8 MB OPI PSRAM; verify the exact revision |

### References

- [LCDWiki product page and documentation](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display)
- [LCDWiki Product Picture](https://www.lcdwiki.com/2.8inch_ESP32-S3_Display#Product_Picture)
- [LCDWiki Arduino demo instructions (PDF)](https://www.lcdwiki.com/res/ES3C28P/2.8inch_ES3C28P_ES3N28P_arduino_Demo_Instructions.pdf)
- [ES3C28P board-support reference](https://github.com/ngttai/esp32_s3_es3c28p)
- [Arduino Audio Tools — LCD Boards with Audio](https://github.com/pschatzmann/arduino-audio-tools/wiki/LCD-Boards-with-Audio)

**Important:** Confirm display/touch GPIOs, board revision, flash/PSRAM configuration, audio wiring and microSD behavior using the official board demo and schematic before flashing or relying on the device.

## Build with PlatformIO

1. Install Visual Studio Code and the PlatformIO IDE extension.
2. Open this repository folder in VS Code.
3. In the PlatformIO terminal, run:

   ```powershell
   pio run
   ```

4. Resolve compiler errors before uploading firmware.
5. Before flashing, validate the exact board revision and test the display, touch, audio codec, microSD and relevant GPIOs.

The configured PlatformIO environment is `es3c28p`. A successful build verifies compilation only; it is not a substitute for a physical-board test.

## NASA FIRMS API key configuration

The Outdoor Intelligence page can query NASA FIRMS VIIRS near-real-time hotspot records around the current demo coordinates. The key is intentionally excluded from the repository.

1. In the project, open the `include/` folder.
2. Create a new file named `config.h`, so the path is `include/config.h`.
3. Add the following line, replacing the placeholder with your own NASA FIRMS MAP key:

   ```cpp
   #pragma once
   #define HHC_FIRMS_MAP_KEY "YOUR_MAP_KEY"
   ```

4. Save the file and run `pio run`.

The repository's `.gitignore` excludes `include/config.h`. Keep the real key in that local file and **do not commit it**. A key compiled into ESP32 firmware may still be extracted from a firmware image or device; this setup prevents accidental publication in source control but does not make the key secret. For a distributed device, consider a server-side proxy with rate limiting. If a key has been exposed, revoke or replace it if the provider supports that.

Hotspot records are satellite detections, not a direct measurement of smoke and not confirmation of an emergency at the user's precise location. Interpret them alongside air-quality readings, wind context and official local advisories. This feature is not an official warning system.

## LoRa / Indoor-Mochi notes

LoRa UART support is intentionally disabled by default until the E220/E32 module wiring and available ESP32-S3 pins are verified. Only set `HHC_LORA_RX_PIN` and `HHC_LORA_TX_PIN` after checking the physical wiring.

The receiver foundation expects transparent UART mode and newline-delimited JSON. The current code does not configure the radio's frequency, channel, addressing, air data rate or E220/E32 mode pins. These settings must be matched between master and slave and must comply with local regulations.

See [docs/lora-protocol.md](docs/lora-protocol.md) for the packet schema and example.

## Known limitations

- Outdoor API queries currently use fixed Jakarta coordinates for a proof of concept; GPS/site selection and a persistent offline API cache are not yet ported.
- API availability depends on network access and each provider's service behavior.
- Audio depends on the ES8311/amplifier configuration and compatible microSD media; test playback and auto-next on the physical device.
- The exact display, touch, flash/PSRAM and GPIO settings must be verified against the board revision.
- This repository is separate from the OurHeritage web application and does not copy its source.
- Sensor readings should only be treated as live measurements when they come from valid telemetry.


## Planned extension: Mochi Early Warning Companion (Slave)

<details>
<summary><strong>Show the planned Mochi slave build guide (parts, wiring and assembly)</strong></summary>

> **Status: planned documentation, not a verified wiring design.** The component list and GPIO assignments below are a working draft. Confirm the exact ESP32-S3 CAM board pinout, LoRa module variant, display breakout, power module and sensor voltage requirements before connecting power. Do not treat this draft as a validated schematic.

### Project overview

- **Difficulty:** Intermediate; suitable for beginner-to-intermediate makers with careful supervision.
- **Estimated assembly time:** Approximately 1–2 hours, excluding debugging and firmware development.
- **Build target:** One Mochi sensor/display slave node.
- **Purpose:** Read environmental sensors, display Mochi status/expressions, and periodically send telemetry to the Home Heritage Companion master over a LoRa UART link.

### Parts list

| No. | Component | Specification / notes | Qty. |
|---:|---|---|---:|
| 1 | ESP32-S3 CAM board | Preferably N16R8 (16 MB flash, 8 MB PSRAM), USB Type-C; verify the exact board | 1 |
| 2 | UART LoRa module | EBYTE E220-900T22D or E32-900T20D; select a legal, locally permitted frequency variant | 1 |
| 3 | LoRa antenna | Antenna matched to the module frequency, with the correct connector; IPEX-to-SMA pigtail if required | 1 |
| 4 | SPI TFT display | ST7735, 1.8-inch, 128 × 160 RGB | 1 |
| 5 | GY-SHT31 sensor | Temperature and relative humidity, I²C | 1 |
| 6 | SGP30 sensor | TVOC and eCO₂ estimates, I²C | 1 |
| 7 | BH1750 sensor | Ambient light / lux, I²C | 1 |
| 8 | GUVA-S12SD UV sensor | Analog output; check the breakout's supply and output-voltage range | 1 |
| 9 | Battery charger and boost module | USB Type-C charging and regulated 5 V output; verify its cell configuration and protection features | 1 |
| 10 | 18650 Li-ion cell(s) | Protected, reputable cells with compatible charger; capacity and cell count depend on the selected power module | 1–2 |
| 11 | Main power switch | Rated for the expected current and voltage | 1 |
| 12 | Tactile push buttons | Approximately 6 × 6 mm: expression and menu/status interaction | 2 |

### Tools and accessories

- Dupont jumper wires (female-to-female and male-to-female).
- Small breadboard for initial tests (optional).
- Perfboard, soldering iron and solder for a permanent assembly.
- A compatible 18650 holder, if required by the selected power module.
- Heat-shrink tubing and suitable insulation for exposed connections.
- Multimeter for checking polarity, voltage and continuity before powering the circuit.

### Critical safety checks

1. **LoRa antenna:** Attach an antenna matched to the module and operating frequency before transmitting. Follow the module manufacturer's instructions; transmitting without a suitable antenna can damage the RF output stage.
2. **Battery and charger:** Do not assume that cells must be wired in parallel or that any USB-C boost board supports two cells. Follow the exact power module's documentation for cell count, series/parallel topology, charging, protection and output current. Never connect a 2-cell series pack to an input intended for a single Li-ion cell.
3. **Voltage compatibility:** Check every module's allowed supply voltage and logic levels. Do not connect a 5 V signal to an ESP32-S3 GPIO unless the interface is explicitly protected for it.
4. **Power-off wiring:** Disconnect the battery and USB supply before changing wiring. Check for shorts and verify regulated output voltage with a multimeter before connecting the ESP32-S3.
5. **Radio compliance:** Use a frequency and transmit settings permitted in your country. The example module names do not by themselves establish that 915 MHz is legal for every deployment.

### Draft wiring map — verify before use

The GPIO numbers below are **proposed assignments only**, not a confirmed pinout. Check the ESP32-S3 CAM board schematic and ensure that the pins are not already used by the camera, onboard peripherals, boot strapping, flash/PSRAM or other board circuitry.

#### 1. Power

- Battery holder → the charger/boost module's documented battery terminals.
- Regulated module output (+) → the board's documented 5 V / VIN input, only if that input accepts the measured output.
- Regulated module output (−) → GND.
- Keep all grounds common where required by the interface design.
- Do not connect a battery directly to the ESP32-S3 3.3 V pin.

#### 2. LoRa UART

| LoRa module pin | Proposed ESP32-S3 connection |
|---|---|
| TXD | GPIO 44 (UART RX) |
| RXD | GPIO 43 (UART TX) |
| GND | GND |
| VCC | The voltage explicitly supported by the exact LoRa module variant |

UART TX and RX are crossed: module TXD goes to MCU RX, and module RXD goes to MCU TX. Check the module datasheet for supply voltage, logic levels, AUX and mode-pin requirements before wiring.

#### 3. ST7735 SPI display

| Display pin | Proposed ESP32-S3 connection |
|---|---|
| VCC | 3.3 V, if supported by the exact display breakout |
| GND | GND |
| SCL / SCK | GPIO 12 |
| SDA / MOSI | GPIO 11 |
| CS | GPIO 10 |
| DC / A0 | GPIO 14 |
| RES / RST | GPIO 21 |
| BLK / LED | The breakout's documented backlight supply |

These are draft assignments. Confirm the breakout's pin labels, logic levels and backlight requirements.

#### 4. I²C sensor array

The SHT31, SGP30 and BH1750 can share an I²C bus if their addresses are compatible and their electrical requirements are met.

| Signal | Proposed connection |
|---|---|
| SDA on all three sensors | GPIO 8 |
| SCL on all three sensors | GPIO 9 |
| VCC | 3.3 V, if supported by each breakout |
| GND | Common GND |

Check I²C addresses, pull-up resistors and sensor-board voltage compatibility before connecting all three modules.

#### 5. Analog UV sensor

| GUVA-S12SD signal | Proposed connection |
|---|---|
| VCC | The voltage specified for the exact sensor breakout |
| GND | GND |
| OUT | GPIO 1, only after confirming it is ADC-capable and the output stays within the ESP32-S3 ADC input range |

Do not assume that a sensor breakout's analog output is safe for the MCU simply because its supply is compatible.

#### 6. Interaction buttons

| Button | Proposed connection |
|---|---|
| Expression button | GPIO 47 to GND |
| Menu/status button | GPIO 48 to GND |

If using `INPUT_PULLUP`, connect each button between its GPIO and GND, and implement debounce handling in firmware. Confirm both pins are available on the specific board.

### Assembly and first-power-on checklist

1. Read the documentation for the exact ESP32-S3 CAM board, LoRa module, display and power module.
2. Assemble and test the power path separately. Measure the regulated output before connecting electronics.
3. Connect the LoRa module and matched antenna with power disconnected.
4. Wire the display and test it with a minimal ST7735 example before integrating the full firmware.
5. Connect the I²C sensors and scan the bus to confirm that each expected sensor responds.
6. Test the UV sensor's analog output and confirm the voltage is within the ESP32-S3 ADC input range.
7. Connect the two buttons and test their GPIO states and debounce behavior.
8. Integrate the sensors, display and UART firmware one subsystem at a time.
9. Confirm that Mochi displays status correctly and sends valid newline-delimited JSON telemetry to the master.
10. Test battery charging, protection, temperature and runtime under supervision before any unattended deployment.

### Integration with the master

The master-side LoRa receiver is a protocol foundation and expects transparent UART mode with one JSON object per line. The slave firmware, radio settings, packet schema and master receiver must agree before telemetry can be considered operational.

See [LoRa protocol notes](docs/lora-protocol.md). The radio's frequency, channel, addressing, air data rate, mode pins and AUX behavior must be configured for the exact module and comply with local regulations.

</details>

## Roadmap

1. Verify board revision, display driver, touch orientation, flash/PSRAM and GPIO configuration.
2. Validate audio codec output, microSD mounting, track selection and end-of-track behavior.
3. Verify LoRa UART wiring and align packet/radio settings with the Mochi slave.
4. Extend OurHeritage content retrieval and offline caching.
5. Add telemetry history, thresholds and carefully scoped notifications.

## Safety

Use the board's documented power input and a suitably protected battery configuration. SGP30 eCO2/TVOC values are not a certified gas-leak detector or life-safety measurement. NASA FIRMS detections and other online data should not be treated as a substitute for official emergency alerts.
