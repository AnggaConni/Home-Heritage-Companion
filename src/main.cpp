#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

// Milestone 1 starter. Display pin/driver and FT6336G touch setup are NOT
// validated for the LCDWiki ES3C28P revision yet. Do not flash until aligned
// with the board's vendor configuration.

static constexpr uint16_t SCREEN_W = 240;
static constexpr uint16_t SCREEN_H = 320;
static constexpr uint32_t WIFI_RETRY_MS = 15000;

TFT_eSPI tft;
Preferences prefs;
static lv_disp_draw_buf_t drawBuf;
static lv_color_t pixels[SCREEN_W * 20];
static lv_disp_drv_t dispDrv;
static lv_indev_drv_t inputDrv;
static lv_obj_t *root;
static lv_obj_t *wifiLabel;
static lv_obj_t *statusLabel;
static lv_obj_t *ssidField;
static lv_obj_t *passwordField;
static uint32_t lastWifiTry = 0;

void flushDisplay(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors(reinterpret_cast<uint16_t *>(color), w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(drv);
}

// Touch is a deliberate stub until the FT6336G driver and exact pins are verified.
void readTouch(lv_indev_drv_t *, lv_indev_data_t *data) {
  data->state = LV_INDEV_STATE_REL;
  data->point.x = 0;
  data->point.y = 0;
}

void setStatus(const char *text, uint32_t color = 0x334155) {
  if (statusLabel) {
    lv_label_set_text(statusLabel, text);
    lv_obj_set_style_text_color(statusLabel, lv_color_hex(color), 0);
  }
}

void updateWifi() {
  if (!wifiLabel) return;
  if (WiFi.status() == WL_CONNECTED) {
    String s = String(LV_SYMBOL_WIFI) + " " + WiFi.localIP().toString();
    lv_label_set_text(wifiLabel, s.c_str());
    lv_obj_set_style_text_color(wifiLabel, lv_color_hex(0x16A34A), 0);
  } else {
    lv_label_set_text(wifiLabel, LV_SYMBOL_WIFI " Offline");
    lv_obj_set_style_text_color(wifiLabel, lv_color_hex(0xEA580C), 0);
  }
}

void showHome();

void makeBackButton() {
  lv_obj_t *back = lv_btn_create(root);
  lv_obj_set_size(back, 100, 36);
  lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 12, -8);
  lv_obj_t *label = lv_label_create(back);
  lv_label_set_text(label, LV_SYMBOL_LEFT " Home");
  lv_obj_center(label);
  lv_obj_add_event_cb(back, [](lv_event_t *) { showHome(); }, LV_EVENT_CLICKED, nullptr);
}

void showPage(const char *title, const char *body) {
  lv_obj_clean(root);
  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, title);
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 12, 12);
  lv_obj_t *content = lv_label_create(root);
  lv_label_set_text(content, body);
  lv_label_set_long_mode(content, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(content, SCREEN_W - 24);
  lv_obj_align(content, LV_ALIGN_TOP_LEFT, 12, 54);
  makeBackButton();
}

void showSettings();

void handleConnect(lv_event_t *) {
  const char *ssid = lv_textarea_get_text(ssidField);
  const char *password = lv_textarea_get_text(passwordField);
  if (!ssid || strlen(ssid) == 0) {
    setStatus("Please enter Wi-Fi SSID.", 0xDC2626);
    return;
  }
  prefs.begin("hhc-wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("password", password);
  prefs.end();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  lastWifiTry = millis();
  setStatus("Connecting...", 0x2563EB);
  updateWifi();
}

void showSettings() {
  lv_obj_clean(root);
  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, "Wi-Fi Settings");
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 12, 8);

  ssidField = lv_textarea_create(root);
  lv_textarea_set_one_line(ssidField, true);
  lv_textarea_set_placeholder_text(ssidField, "Wi-Fi SSID");
  lv_obj_set_size(ssidField, SCREEN_W - 24, 38);
  lv_obj_align(ssidField, LV_ALIGN_TOP_LEFT, 12, 48);

  passwordField = lv_textarea_create(root);
  lv_textarea_set_one_line(passwordField, true);
  lv_textarea_set_password_mode(passwordField, true);
  lv_textarea_set_placeholder_text(passwordField, "Wi-Fi password");
  lv_obj_set_size(passwordField, SCREEN_W - 24, 38);
  lv_obj_align(passwordField, LV_ALIGN_TOP_LEFT, 12, 96);

  lv_obj_t *connect = lv_btn_create(root);
  lv_obj_set_size(connect, SCREEN_W - 24, 40);
  lv_obj_align(connect, LV_ALIGN_TOP_LEFT, 12, 146);
  lv_obj_t *label = lv_label_create(connect);
  lv_label_set_text(label, "Save & Connect");
  lv_obj_center(label);
  lv_obj_add_event_cb(connect, handleConnect, LV_EVENT_CLICKED, nullptr);

  statusLabel = lv_label_create(root);
  lv_label_set_text(statusLabel, "Credentials stay on this device.");
  lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(statusLabel, SCREEN_W - 24);
  lv_obj_align(statusLabel, LV_ALIGN_TOP_LEFT, 12, 198);
  makeBackButton();
}

void tile(const char *title, const char *symbol, int x, int y, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_btn_create(root);
  lv_obj_set_size(btn, 102, 76);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_t *label = lv_label_create(btn);
  String s = String(symbol) + "\n" + title;
  lv_label_set_text(label, s.c_str());
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(label);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
}

void showHome() {
  lv_obj_clean(root);
  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, "Home Companion");
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 10, 9);

  wifiLabel = lv_label_create(root);
  lv_obj_align(wifiLabel, LV_ALIGN_TOP_RIGHT, -8, 14);
  updateWifi();

  lv_obj_t *sub = lv_label_create(root);
  lv_label_set_text(sub, "OURHERITAGE | HOME");
  lv_obj_set_style_text_color(sub, lv_color_hex(0xB45309), 0);
  lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 12, 39);

  tile("Heritage", LV_SYMBOL_HOME, 12, 78, [](lv_event_t *) {
    showPage("OurHeritage", "Content sync will be added in Milestone 2.\n\nOffline mode will show the last locally saved content.");
  });
  tile("Music", LV_SYMBOL_AUDIO, 126, 78, [](lv_event_t *) {
    showPage("Music Player", "MP3 playback is planned. Verify the board audio codec and microSD interface before implementing playback.");
  });
  tile("Environment", LV_SYMBOL_TINT, 12, 166, [](lv_event_t *) {
    showPage("Environment", "Mochi slave telemetry will be added later. No live sensor readings are shown until a slave connects.");
  });
  tile("Settings", LV_SYMBOL_SETTINGS, 126, 166, [](lv_event_t *) { showSettings(); });

  lv_obj_t *footer = lv_label_create(root);
  lv_label_set_text(footer, "Local-first | Online when available");
  lv_obj_set_style_text_color(footer, lv_color_hex(0x64748B), 0);
  lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void startSavedWifi() {
  prefs.begin("hhc-wifi", true);
  String ssid = prefs.getString("ssid", "");
  String password = prefs.getString("password", "");
  prefs.end();
  WiFi.mode(WIFI_STA);
  if (ssid.length()) {
    WiFi.begin(ssid.c_str(), password.c_str());
    lastWifiTry = millis();
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  lv_init();
  lv_disp_draw_buf_init(&drawBuf, pixels, nullptr, SCREEN_W * 20);
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = SCREEN_W;
  dispDrv.ver_res = SCREEN_H;
  dispDrv.flush_cb = flushDisplay;
  dispDrv.draw_buf = &drawBuf;
  lv_disp_drv_register(&dispDrv);

  lv_indev_drv_init(&inputDrv);
  inputDrv.type = LV_INDEV_TYPE_POINTER;
  inputDrv.read_cb = readTouch;
  lv_indev_drv_register(&inputDrv);

  root = lv_scr_act();
  WiFi.onEvent([](WiFiEvent_t event) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.printf("Wi-Fi IP: %s\n", WiFi.localIP().toString().c_str());
      updateWifi();
      setStatus("Connected.", 0x16A34A);
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.println("Wi-Fi disconnected.");
      updateWifi();
    }
  });

  showHome();
  startSavedWifi();
  Serial.println("Home Heritage Companion milestone 1 scaffold.");
}

void loop() {
  lv_timer_handler();
  delay(5);
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 15000) {
    lastWifiTry = millis();
    prefs.begin("hhc-wifi", true);
    String ssid = prefs.getString("ssid", "");
    prefs.end();
    if (ssid.length()) WiFi.reconnect();
  }
  static uint32_t lastRefresh = 0;
  if (millis() - lastRefresh > 2000) {
    lastRefresh = millis();
    updateWifi();
  }
}
