#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <math.h>

// LCDWiki ES3C28P target notes:
// - Display: ILI9341V, 240x320 SPI (pin mapping must be verified per board revision)
// - Touch: FT6336G I2C, address 0x38
// - LoRa: transparent UART protocol foundation for E220/E32 modules.
//   LoRa UART stays disabled until RX/TX pins are verified and configured in platformio.ini.

namespace {
constexpr uint16_t SCREEN_W = 240;
constexpr uint16_t SCREEN_H = 320;
constexpr int TOUCH_SDA = 16;
constexpr int TOUCH_SCL = 15;
constexpr int TOUCH_INT = 17;
constexpr int TOUCH_RST = 18;
constexpr int TFT_BACKLIGHT = 45;
constexpr uint8_t TOUCH_ADDR = 0x38;
constexpr uint32_t WIFI_RETRY_MS = 15000;
constexpr uint32_t UI_REFRESH_MS = 1000;
constexpr uint32_t TELEMETRY_STALE_MS = 60000;
constexpr size_t LORA_LINE_MAX = 384;

TFT_eSPI tft;
Preferences prefs;
lv_disp_draw_buf_t drawBuf;
lv_color_t drawPixels[SCREEN_W * 20];
lv_disp_drv_t displayDriver;
lv_indev_drv_t inputDriver;

lv_obj_t *root = nullptr;
lv_obj_t *wifiLabel = nullptr;
lv_obj_t *statusLabel = nullptr;
lv_obj_t *ssidField = nullptr;
lv_obj_t *passwordField = nullptr;
lv_obj_t *keyboard = nullptr;
lv_obj_t *environmentLabel = nullptr;
lv_obj_t *heritageLabel = nullptr;

uint32_t lastWifiTry = 0;
uint32_t lastUiRefresh = 0;
int16_t lastTouchX = 0;
int16_t lastTouchY = 0;
bool wifiConfigured = false;
volatile bool wifiGotIpEvent = false;
volatile bool wifiDisconnectedEvent = false;

enum class Page : uint8_t { HOME, HERITAGE, MUSIC, ENVIRONMENT, SETTINGS };
Page currentPage = Page::HOME;

struct Telemetry {
  bool received = false;
  bool hasTemperature = false;
  bool hasHumidity = false;
  bool hasEco2 = false;
  bool hasTvoc = false;
  bool hasLux = false;
  bool hasUvRaw = false;
  char node[32] = "Mochi";
  uint32_t sequence = 0;
  float temperatureC = 0;
  float humidityPct = 0;
  uint16_t eco2Ppm = 0;
  uint16_t tvocPpb = 0;
  float lux = 0;
  uint16_t uvRaw = 0;
  uint32_t lastPacketAt = 0;
} telemetry;

char loraLine[LORA_LINE_MAX];
size_t loraLineLength = 0;
bool loraEnabled = false;

void flushDisplay(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *color) {
  const uint32_t width = area->x2 - area->x1 + 1;
  const uint32_t height = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, width, height);
  tft.pushColors(reinterpret_cast<uint16_t *>(color), width * height, true);
  tft.endWrite();
  lv_disp_flush_ready(driver);
}

bool readTouchPoint(uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02); // FT6336G TD_STATUS; coordinates follow at 0x03..0x06.
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, static_cast<uint8_t>(5)) != 5) return false;

  const uint8_t points = Wire.read() & 0x0F;
  const uint8_t xHigh = Wire.read();
  const uint8_t xLow = Wire.read();
  const uint8_t yHigh = Wire.read();
  const uint8_t yLow = Wire.read();
  if (points == 0) return false;

  x = ((xHigh & 0x0F) << 8) | xLow;
  y = ((yHigh & 0x0F) << 8) | yLow;
  return x < SCREEN_W && y < SCREEN_H;
}

void readTouch(lv_indev_drv_t *, lv_indev_data_t *data) {
  uint16_t x = 0;
  uint16_t y = 0;
  if (readTouchPoint(x, y)) {
    lastTouchX = static_cast<int16_t>(x);
    lastTouchY = static_cast<int16_t>(y);
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_REL;
    data->point.x = lastTouchX;
    data->point.y = lastTouchY;
  }
}

void setStatus(const char *message, uint32_t color = 0x334155) {
  if (!statusLabel) return;
  lv_label_set_text(statusLabel, message);
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(color), 0);
}

void resetPageObjects() {
  // lv_obj_clean() deletes child widgets, so never retain their old pointers.
  wifiLabel = nullptr;
  statusLabel = nullptr;
  ssidField = nullptr;
  passwordField = nullptr;
  keyboard = nullptr;
  environmentLabel = nullptr;
  heritageLabel = nullptr;
  lv_obj_clean(root);
}

void updateWifiLabel() {
  if (!wifiLabel) return;
  if (WiFi.status() == WL_CONNECTED) {
    String text = String(LV_SYMBOL_WIFI) + " " + WiFi.localIP().toString();
    lv_label_set_text(wifiLabel, text.c_str());
    lv_obj_set_style_text_color(wifiLabel, lv_color_hex(0x16A34A), 0);
  } else {
    lv_label_set_text(wifiLabel, LV_SYMBOL_WIFI " Offline");
    lv_obj_set_style_text_color(wifiLabel, lv_color_hex(0xEA580C), 0);
  }
}

void updateHeritageStatus() {
  if (!heritageLabel) return;
  if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text(heritageLabel,
      "Network: online\n\nDevice is connected to Wi-Fi. The OurHeritage content API/cache adapter is the next software milestone; this firmware does not claim to have synced content yet.");
  } else {
    lv_label_set_text(heritageLabel,
      "Network: offline\n\nThe device can still show locally received sensor telemetry. OurHeritage offline content storage/sync has not been implemented in this firmware version.");
  }
}

void updateEnvironmentStatus() {
  if (!environmentLabel) return;
  if (!loraEnabled) {
    lv_label_set_text(environmentLabel,
      "LoRa receiver: not configured\n\nThe UART receiver and JSON telemetry parser are included, but radio RX/TX GPIOs are disabled until the actual wiring is verified. No sensor values are fabricated.");
    return;
  }
  if (!telemetry.received) {
    lv_label_set_text(environmentLabel,
      "LoRa receiver: listening\n\nWaiting for a newline-terminated JSON telemetry packet from the Mochi slave...");
    return;
  }

  const uint32_t age = (millis() - telemetry.lastPacketAt) / 1000;
  String text = "Node: ";
  text += telemetry.node;
  text += "\nSequence: ";
  text += String(telemetry.sequence);
  text += "\nLast packet: ";
  text += String(age);
  text += " s ago";
  if (age > TELEMETRY_STALE_MS / 1000) text += " (STALE)";
  text += "\n\nTemperature: ";
  text += telemetry.hasTemperature ? String(telemetry.temperatureC, 1) + " C" : "--";
  text += "\nHumidity: ";
  text += telemetry.hasHumidity ? String(telemetry.humidityPct, 1) + " %" : "--";
  text += "\neCO2: ";
  text += telemetry.hasEco2 ? String(telemetry.eco2Ppm) + " ppm" : "--";
  text += "\nTVOC: ";
  text += telemetry.hasTvoc ? String(telemetry.tvocPpb) + " ppb" : "--";
  text += "\nLight: ";
  text += telemetry.hasLux ? String(telemetry.lux, 1) + " lux" : "--";
  text += "\nUV raw: ";
  text += telemetry.hasUvRaw ? String(telemetry.uvRaw) : "--";
  lv_label_set_text(environmentLabel, text.c_str());
}

void showHome();
void showSettings();

void makeBackButton() {
  lv_obj_t *back = lv_btn_create(root);
  lv_obj_set_size(back, 100, 36);
  lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 12, -8);
  lv_obj_t *label = lv_label_create(back);
  lv_label_set_text(label, LV_SYMBOL_LEFT " Home");
  lv_obj_center(label);
  lv_obj_add_event_cb(back, [](lv_event_t *) { showHome(); }, LV_EVENT_CLICKED, nullptr);
}

void showInfoPage(const char *title, const char *body, Page page) {
  currentPage = page;
  resetPageObjects();
  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, title);
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 12, 10);

  lv_obj_t *content = lv_label_create(root);
  lv_label_set_text(content, body);
  lv_label_set_long_mode(content, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(content, SCREEN_W - 24);
  lv_obj_align(content, LV_ALIGN_TOP_LEFT, 12, 54);
  makeBackButton();
}

void showHeritage() {
  currentPage = Page::HERITAGE;
  resetPageObjects();
  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, "OurHeritage");
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 12, 10);
  heritageLabel = lv_label_create(root);
  lv_label_set_long_mode(heritageLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(heritageLabel, SCREEN_W - 24);
  lv_obj_align(heritageLabel, LV_ALIGN_TOP_LEFT, 12, 52);
  updateHeritageStatus();
  makeBackButton();
}

void showEnvironment() {
  currentPage = Page::ENVIRONMENT;
  resetPageObjects();
  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, "Environment / Mochi");
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_16, 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 8, 10);
  environmentLabel = lv_label_create(root);
  lv_label_set_long_mode(environmentLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(environmentLabel, SCREEN_W - 20);
  lv_obj_align(environmentLabel, LV_ALIGN_TOP_LEFT, 10, 42);
  updateEnvironmentStatus();
  makeBackButton();
}

void tile(const char *title, const char *symbol, int x, int y, lv_event_cb_t callback) {
  lv_obj_t *button = lv_btn_create(root);
  lv_obj_set_size(button, 102, 76);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_style_radius(button, 12, 0);
  lv_obj_t *label = lv_label_create(button);
  String text = String(symbol) + "\n" + title;
  lv_label_set_text(label, text.c_str());
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(label);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
}

void showHome() {
  currentPage = Page::HOME;
  resetPageObjects();
  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, "Home Companion");
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 10, 9);

  wifiLabel = lv_label_create(root);
  lv_obj_align(wifiLabel, LV_ALIGN_TOP_RIGHT, -8, 14);
  updateWifiLabel();

  lv_obj_t *subtitle = lv_label_create(root);
  lv_label_set_text(subtitle, "OURHERITAGE | HOME");
  lv_obj_set_style_text_color(subtitle, lv_color_hex(0xB45309), 0);
  lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 12, 39);

  tile("Heritage", LV_SYMBOL_HOME, 12, 78, [](lv_event_t *) { showHeritage(); });
  tile("Music", LV_SYMBOL_AUDIO, 126, 78, [](lv_event_t *) {
    showInfoPage("Music Player",
      "MP3 playback is not enabled yet. The ES8311 audio path and microSD/SDIO interface must be verified against the ES3C28P demo before enabling playback.",
      Page::MUSIC);
  });
  tile("Environment", "ENV", 12, 166, [](lv_event_t *) { showEnvironment(); });
  tile("Settings", LV_SYMBOL_SETTINGS, 126, 166, [](lv_event_t *) { showSettings(); });

  lv_obj_t *footer = lv_label_create(root);
  lv_label_set_text(footer, "Local device | Wi-Fi when available");
  lv_obj_set_style_text_color(footer, lv_color_hex(0x64748B), 0);
  lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void keyboardEvent(lv_event_t *event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL) return;
  if (keyboard) {
    lv_keyboard_set_textarea(keyboard, nullptr);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  }
}

void textareaFocused(lv_event_t *event) {
  lv_obj_t *textarea = lv_event_get_target(event);
  if (!keyboard) return;
  lv_keyboard_set_textarea(keyboard, textarea);
  lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(keyboard);
}

void handleConnect(lv_event_t *) {
  const char *ssid = ssidField ? lv_textarea_get_text(ssidField) : "";
  const char *password = passwordField ? lv_textarea_get_text(passwordField) : "";
  if (!ssid || strlen(ssid) == 0) {
    setStatus("Enter a Wi-Fi network name (SSID).", 0xDC2626);
    return;
  }

  prefs.begin("hhc-wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("password", password ? password : "");
  prefs.end();

  wifiConfigured = true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password ? password : "");
  lastWifiTry = millis();
  setStatus("Credentials saved. Connecting...", 0x2563EB);
  updateWifiLabel();
}

void showSettings() {
  currentPage = Page::SETTINGS;
  resetPageObjects();
  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, "Wi-Fi Settings");
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 12, 8);

  ssidField = lv_textarea_create(root);
  lv_textarea_set_one_line(ssidField, true);
  lv_textarea_set_placeholder_text(ssidField, "Wi-Fi SSID");
  lv_obj_set_size(ssidField, SCREEN_W - 24, 38);
  lv_obj_align(ssidField, LV_ALIGN_TOP_LEFT, 12, 48);
  lv_obj_add_event_cb(ssidField, textareaFocused, LV_EVENT_FOCUSED, nullptr);

  passwordField = lv_textarea_create(root);
  lv_textarea_set_one_line(passwordField, true);
  lv_textarea_set_password_mode(passwordField, true);
  lv_textarea_set_placeholder_text(passwordField, "Wi-Fi password");
  lv_obj_set_size(passwordField, SCREEN_W - 24, 38);
  lv_obj_align(passwordField, LV_ALIGN_TOP_LEFT, 12, 96);
  lv_obj_add_event_cb(passwordField, textareaFocused, LV_EVENT_FOCUSED, nullptr);

  lv_obj_t *connectButton = lv_btn_create(root);
  lv_obj_set_size(connectButton, SCREEN_W - 24, 36);
  lv_obj_align(connectButton, LV_ALIGN_TOP_LEFT, 12, 144);
  lv_obj_t *connectLabel = lv_label_create(connectButton);
  lv_label_set_text(connectLabel, "Save & Connect");
  lv_obj_center(connectLabel);
  lv_obj_add_event_cb(connectButton, handleConnect, LV_EVENT_CLICKED, nullptr);

  statusLabel = lv_label_create(root);
  lv_label_set_text(statusLabel, "Wi-Fi credentials are stored locally on this device.");
  lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(statusLabel, SCREEN_W - 24);
  lv_obj_align(statusLabel, LV_ALIGN_TOP_LEFT, 12, 190);

  keyboard = lv_keyboard_create(root);
  lv_obj_set_size(keyboard, SCREEN_W, 160);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard, keyboardEvent, LV_EVENT_ALL, nullptr);
  makeBackButton();

  prefs.begin("hhc-wifi", true);
  const String savedSsid = prefs.getString("ssid", "");
  prefs.end();
  if (savedSsid.length()) lv_textarea_set_text(ssidField, savedSsid.c_str());
}

void startSavedWifi() {
  prefs.begin("hhc-wifi", true);
  const String ssid = prefs.getString("ssid", "");
  const String password = prefs.getString("password", "");
  prefs.end();
  wifiConfigured = ssid.length() > 0;
  WiFi.mode(WIFI_STA);
  if (wifiConfigured) {
    WiFi.begin(ssid.c_str(), password.c_str());
    lastWifiTry = millis();
    Serial.printf("Wi-Fi: connecting to saved network '%s'\n", ssid.c_str());
  } else {
    Serial.println("Wi-Fi: not configured; open Settings to connect.");
  }
}

bool parseTelemetryLine(const char *line) {
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, line);
  if (error || !document.is<JsonObject>()) {
    Serial.printf("LoRa: ignored invalid JSON packet (%s)\n", error ? error.c_str() : "not an object");
    return false;
  }

  // Protocol is documented in docs/lora-protocol.md. All fields are optional
  // except the JSON object itself, so nodes can send partial readings.
  if (document["node"].is<const char *>()) {
    strlcpy(telemetry.node, document["node"].as<const char *>(), sizeof(telemetry.node));
  }
  telemetry.sequence = document["seq"] | telemetry.sequence;
  telemetry.hasTemperature = !document["temperature_c"].isNull();
  telemetry.hasHumidity = !document["humidity_pct"].isNull();
  telemetry.hasEco2 = !document["eco2_ppm"].isNull();
  telemetry.hasTvoc = !document["tvoc_ppb"].isNull();
  telemetry.hasLux = !document["lux"].isNull();
  telemetry.hasUvRaw = !document["uv_raw"].isNull();

  if (telemetry.hasTemperature) telemetry.temperatureC = document["temperature_c"].as<float>();
  if (telemetry.hasHumidity) telemetry.humidityPct = document["humidity_pct"].as<float>();
  if (telemetry.hasEco2) telemetry.eco2Ppm = document["eco2_ppm"].as<uint16_t>();
  if (telemetry.hasTvoc) telemetry.tvocPpb = document["tvoc_ppb"].as<uint16_t>();
  if (telemetry.hasLux) telemetry.lux = document["lux"].as<float>();
  if (telemetry.hasUvRaw) telemetry.uvRaw = document["uv_raw"].as<uint16_t>();

  telemetry.received = true;
  telemetry.lastPacketAt = millis();
  Serial.printf("LoRa packet: node=%s seq=%lu\n", telemetry.node,
                static_cast<unsigned long>(telemetry.sequence));
  return true;
}

void pollLoRa() {
#if HHC_LORA_RX_PIN >= 0 && HHC_LORA_TX_PIN >= 0
  while (Serial2.available()) {
    const char c = static_cast<char>(Serial2.read());
    if (c == '\r') continue;
    if (c == '\n') {
      if (loraLineLength > 0) {
        loraLine[loraLineLength] = '\0';
        parseTelemetryLine(loraLine);
        loraLineLength = 0;
      }
      continue;
    }
    if (loraLineLength < sizeof(loraLine) - 1) {
      loraLine[loraLineLength++] = c;
    } else {
      // Discard an oversized line instead of parsing a truncated packet.
      loraLineLength = 0;
      Serial.println("LoRa: packet too long; discarded.");
    }
  }
#endif
}

void setupLoRa() {
#if HHC_LORA_RX_PIN >= 0 && HHC_LORA_TX_PIN >= 0
  Serial2.begin(HHC_LORA_BAUD, SERIAL_8N1, HHC_LORA_RX_PIN, HHC_LORA_TX_PIN);
  loraEnabled = true;
  Serial.printf("LoRa UART enabled: RX=%d TX=%d baud=%d\n",
                HHC_LORA_RX_PIN, HHC_LORA_TX_PIN, HHC_LORA_BAUD);
#else
  loraEnabled = false;
  Serial.println("LoRa UART disabled: verify wiring, then set HHC_LORA_RX_PIN/TX_PIN in platformio.ini.");
#endif
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nHome Heritage Companion starting");

  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(100);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  pinMode(TOUCH_INT, INPUT);

  // ES3C28P requires GPIO45 HIGH to enable the TFT backlight.
  // Enable it before initializing the panel so startup diagnostics are visible.
  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);
  delay(20);
  Serial.println("Display: backlight GPIO45 enabled; initializing TFT_eSPI");

  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);
  Serial.println("Display: TFT_eSPI initialized; registering LVGL display");

  lv_init();
  lv_disp_draw_buf_init(&drawBuf, drawPixels, nullptr, SCREEN_W * 20);
  lv_disp_drv_init(&displayDriver);
  displayDriver.hor_res = SCREEN_W;
  displayDriver.ver_res = SCREEN_H;
  displayDriver.flush_cb = flushDisplay;
  displayDriver.draw_buf = &drawBuf;
  lv_disp_drv_register(&displayDriver);

  lv_indev_drv_init(&inputDriver);
  inputDriver.type = LV_INDEV_TYPE_POINTER;
  inputDriver.read_cb = readTouch;
  lv_indev_drv_register(&inputDriver);
  root = lv_scr_act();

  WiFi.onEvent([](WiFiEvent_t event) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      wifiGotIpEvent = true;
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      wifiDisconnectedEvent = true;
    }
  });

  setupLoRa();
  showHome();
  startSavedWifi();
}

void loop() {
  lv_timer_handler();
  pollLoRa();

  if (wifiGotIpEvent) {
    wifiGotIpEvent = false;
    Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
    setStatus("Connected to Wi-Fi.", 0x16A34A);
    updateHeritageStatus();
  }
  if (wifiDisconnectedEvent) {
    wifiDisconnectedEvent = false;
    Serial.println("Wi-Fi disconnected.");
    updateHeritageStatus();
  }

  const uint32_t now = millis();
  if (wifiConfigured && WiFi.status() != WL_CONNECTED &&
      now - lastWifiTry >= WIFI_RETRY_MS) {
    lastWifiTry = now;
    WiFi.reconnect();
  }

  if (now - lastUiRefresh >= UI_REFRESH_MS) {
    lastUiRefresh = now;
    updateWifiLabel();
    if (currentPage == Page::HERITAGE) updateHeritageStatus();
    if (currentPage == Page::ENVIRONMENT) updateEnvironmentStatus();
  }
  delay(5);
}
