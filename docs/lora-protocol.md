# Mochi → Home Heritage Companion LoRa JSON protocol (foundation)

## Transport

- Radio modules: EBYTE E220-900T22D or E32-900T20D (select and configure the exact model on both ends).
- Mode expected by this firmware: transparent UART, not fixed/transmission mode.
- Master UART: ESP32-S3 UART2 (`Serial2`). Default UART baud in `platformio.ini`: 9600, 8N1.
- Framing: one UTF-8 JSON object followed by LF (`\n`). CRLF is also accepted. Keep each line below 383 bytes.
- Receiver GPIOs are disabled by default. Do not enable them until the actual module TX→ESP RX and module RX←ESP TX wiring and safe GPIO choices are verified.
- This firmware does not configure radio frequency, channel, air data rate, address, encryption, M0/M1 or AUX pins. Those must match the selected module and slave firmware.

## JSON fields

Required: a valid JSON object. All individual fields are optional, allowing a slave to report only sensors it has read.

| Key | Type | Meaning |
| --- | --- | --- |
| `node` | string | Node identifier, e.g. `Mochi` |
| `seq` | integer | Monotonically increasing packet sequence number |
| `uptime_ms` | integer | Slave uptime in milliseconds (accepted as an extension; not currently displayed) |
| `temperature_c` | number | SHT31 temperature in °C |
| `humidity_pct` | number | SHT31 relative humidity in percent |
| `eco2_ppm` | integer | SGP30 eCO₂ estimate in ppm |
| `tvoc_ppb` | integer | SGP30 TVOC estimate in ppb |
| `lux` | number | BH1750 illuminance in lux |
| `uv_raw` | integer | GUVA-S12SD ADC/raw value, not a calibrated UV index |

## Example packet

Send this exact JSON followed by a newline from the slave UART:

```json
{"node":"Mochi","seq":1,"uptime_ms":12000,"temperature_c":28.4,"humidity_pct":72.1,"eco2_ppm":612,"tvoc_ppb":85,"lux":143.7,"uv_raw":37}
```

The receiver parses packets with ArduinoJson, logs accepted packets to Serial at 115200 baud, and shows only received values on the Environment page. If no valid packet arrives, the UI reports waiting/no data instead of inventing readings. Data is currently held in RAM only; persistence/history is a later milestone.

## Before enabling UART

1. Confirm whether the chosen module is E220 or E32 and read its exact datasheet.
2. Verify module logic voltage, pinout, UART baud, operating mode, channel/frequency and air data rate.
3. Choose ESP32-S3 GPIOs that are genuinely available on the ES3C28P and do not conflict with the TFT, touch, flash/PSRAM, USB or other peripherals.
4. Update `HHC_LORA_RX_PIN` and `HHC_LORA_TX_PIN` in `platformio.ini` (RX means ESP32 receives from module TX; TX means ESP32 transmits to module RX).
5. Match the slave packet schema and send one JSON object per line. Test receive-only first.

This is a serial protocol baseline, not a claim that the radio link has been physically tested.
