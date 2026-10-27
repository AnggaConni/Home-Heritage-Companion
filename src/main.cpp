#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <math.h>
#include <time.h>
#include "music_player.h"

// LCDWiki ES3C28P target notes:
// - Display: ILI9341V, 240x320 SPI (pin mapping must be verified per board revision)
// - Touch: FT6336G I2C, address 0x38
// - LoRa: transparent UART protocol foundation for E220/E32 modules.
//   LoRa UART stays disabled until RX/TX pins are verified and configured in platformio.ini.

namespace {
constexpr uint16_t SCREEN_W = 320;
constexpr uint16_t SCREEN_H = 240;
// Native FT6336G coordinates are portrait; convert them to landscape UI coordinates.
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
lv_obj_t *showPasswordToggle = nullptr;
lv_obj_t *wifiDropdown = nullptr;
lv_obj_t *keyboard = nullptr;
lv_obj_t *indoorLabel = nullptr;
lv_obj_t *outdoorLabel = nullptr;
String outdoorResult;
String outdoorCards[4];
String outdoorRenderedResult;
const char *outdoorCardTitles[4] = {"Weather", "Air Quality", "Hazards", "Recommendations"};
uint8_t outdoorCardIndex = 0;
lv_obj_t *outdoorCard = nullptr;
lv_obj_t *outdoorCardTitle = nullptr;
lv_obj_t *outdoorCardBody = nullptr;
lv_obj_t *outdoorPageIndicator = nullptr;
volatile bool outdoorFetchRunning = false;
volatile bool outdoorFetchDone = false;
volatile bool outdoorFetchPending = false;

uint32_t lastWifiTry = 0;
uint32_t lastUiRefresh = 0;
int16_t lastTouchX = 0;
int16_t lastTouchY = 0;
bool wifiConfigured = false;
bool wifiScanInProgress = false;
volatile bool wifiGotIpEvent = false;
volatile bool wifiDisconnectedEvent = false;
volatile uint8_t wifiDisconnectReason = 0;

enum class Page : uint8_t { HOME, OUTDOOR, MUSIC, INDOOR, SETTINGS };
Page currentPage = Page::HOME;

void startSavedWifi();
void applyResourceMode(Page target);

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

void printMemoryStatus(const char *phase) {
  Serial.printf(
    "[MEM][%s] heap_free=%u heap_largest=%u psram_total=%u psram_free=%u\n",
    phase,
    static_cast<unsigned>(ESP.getFreeHeap()),
    static_cast<unsigned>(ESP.getMaxAllocHeap()),
    static_cast<unsigned>(ESP.getPsramSize()),
    static_cast<unsigned>(ESP.getFreePsram())
  );
}

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

  const uint16_t rawX = ((xHigh & 0x0F) << 8) | xLow;
  const uint16_t rawY = ((yHigh & 0x0F) << 8) | yLow;
  // Match TFT_eSPI rotation 1 (320x240 landscape). If touch axes are reversed
  // on a particular panel revision, only this transform needs calibration.
  x = rawY;
  y = (rawX < SCREEN_H) ? (SCREEN_H - 1 - rawX) : 0;
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
  showPasswordToggle = nullptr;
  wifiDropdown = nullptr;
  keyboard = nullptr;
  indoorLabel = nullptr;
  outdoorLabel = nullptr;
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

String buildDateDaysAgo(int daysAgo) {
  const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  char monthText[4] = {0};
  int day = 1, year = 2026, month = 0;
  sscanf(__DATE__, "%3s %d %d", monthText, &day, &year);
  for (int i = 0; i < 12; ++i) if (strcmp(monthText, months[i]) == 0) month = i;
  struct tm date = {};
  date.tm_year = year - 1900;
  date.tm_mon = month;
  date.tm_mday = day - daysAgo;
  const time_t stamp = mktime(&date);
  localtime_r(&stamp, &date);
  char out[11];
  strftime(out, sizeof(out), "%Y-%m-%d", &date);
  return String(out);
}

String buildDateCompact(int daysAgo) {
  String date = buildDateDaysAgo(daysAgo);
  date.replace("-", "");
  return date;
}

String fetchOutdoorApi(const String &url) {
  WiFiClientSecure client;
  client.setInsecure(); // Public APIs; HTTPS certificate validation is not provisioned in this prototype.
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4500);
  String payload;
  if (http.begin(client, url)) {
    const int code = http.GET();
    if (code == HTTP_CODE_OK) payload = http.getString();
    else Serial.printf("[outdoor] HTTP %d: %s\n", code, url.c_str());
    http.end();
  }
  return payload;
}

void buildOutdoorCards();
void renderOutdoorCard();

String fetchOutdoorData() {
  String result;
  if (WiFi.status() != WL_CONNECTED) {
    result = "Outdoor / OurHeritage\nWi-Fi is offline.\nConnect in Settings to retrieve live API data.";
  } else {
    // Default demonstration location: Jakarta. Site-specific coordinates can be added later.
    constexpr double lat = -6.2088;
    constexpr double lon = 106.8456;
    const String coords = "latitude=-6.2088&longitude=106.8456";
    const String quakeStart = buildDateDaysAgo(7);
    const String nasaStart = buildDateCompact(12);
    const String nasaEnd = buildDateCompact(2);
    const String weatherJson = fetchOutdoorApi(
      "https://api.open-meteo.com/v1/forecast?" + coords +
      "&current=temperature_2m,relative_humidity_2m,precipitation,wind_speed_10m&daily=precipitation_sum,uv_index_max&timezone=Asia%2FJakarta&forecast_days=7");
    const String airJson = fetchOutdoorApi(
      "https://air-quality-api.open-meteo.com/v1/air-quality?" + coords +
      "&current=pm2_5,pm10,uv_index,ozone,nitrogen_dioxide&timezone=Asia%2FJakarta");
    const String quakeJson = fetchOutdoorApi(
      "https://earthquake.usgs.gov/fdsnws/event/1/query?format=geojson&latitude=-6.2088&longitude=106.8456&maxradiuskm=100&starttime=" + quakeStart + "&minmagnitude=2.5");
    const String floodJson = fetchOutdoorApi(
      "https://flood-api.open-meteo.com/v1/flood?" + coords +
      "&daily=river_discharge&forecast_days=7");
    const String nasaJson = fetchOutdoorApi(
      "https://power.larc.nasa.gov/api/temporal/daily/point?parameters=ALLSKY_SFC_SW_DWN&community=RE&longitude=106.8456&latitude=-6.2088&start=" + nasaStart + "&end=" + nasaEnd + "&format=JSON");

    JsonDocument weather, air, quake, flood, nasa;
    const bool haveWeather = weatherJson.length() && !deserializeJson(weather, weatherJson);
    const bool haveAir = airJson.length() && !deserializeJson(air, airJson);
    const bool haveQuake = quakeJson.length() && !deserializeJson(quake, quakeJson);
    const bool haveFlood = floodJson.length() && !deserializeJson(flood, floodJson);
    const bool haveNasa = nasaJson.length() && !deserializeJson(nasa, nasaJson);

    result = "OUTDOOR / OURHERITAGE\nJakarta | Live public APIs\n";
    float temp = NAN, rh = NAN, rain = NAN, forecastRain = NAN, wind = NAN, pm25 = NAN, uv = NAN, solar = NAN, dischargePeak = NAN, dischargeBase = NAN;
    float maxMagnitude = NAN;
    int quakes = -1;
    if (haveWeather) {
      temp = weather["current"]["temperature_2m"] | NAN;
      rh = weather["current"]["relative_humidity_2m"] | NAN;
      rain = weather["current"]["precipitation"] | NAN;
      wind = weather["current"]["wind_speed_10m"] | NAN;
      JsonArray rainValues = weather["daily"]["precipitation_sum"].as<JsonArray>();
      for (JsonVariant v : rainValues) { float n = v | NAN; if (!isnan(n) && (isnan(forecastRain) || n > forecastRain)) forecastRain = n; }
      if (isnan(uv)) { JsonArray uvValues = weather["daily"]["uv_index_max"].as<JsonArray>(); for (JsonVariant v : uvValues) { float n = v | NAN; if (!isnan(n) && (isnan(uv) || n > uv)) uv = n; } }
    }
    if (haveAir) {
      pm25 = air["current"]["pm2_5"] | NAN;
      uv = air["current"]["uv_index"] | NAN;
    }
    if (haveQuake) {
      quakes = quake["metadata"]["count"] | 0;
      JsonArray features = quake["features"].as<JsonArray>();
      for (JsonVariant feature : features) { float mag = feature["properties"]["mag"] | NAN; if (!isnan(mag) && (isnan(maxMagnitude) || mag > maxMagnitude)) maxMagnitude = mag; }
    }
    if (haveFlood) {
      JsonArray values = flood["daily"]["river_discharge"].as<JsonArray>();
      bool first = true;
      for (JsonVariant v : values) {
        float n = v | NAN;
        if (!isnan(n)) { if (first) { dischargeBase = n; first = false; } if (isnan(dischargePeak) || n > dischargePeak) dischargePeak = n; }
      }
    }
    if (haveNasa) {
      JsonObject values = nasa["properties"]["parameter"]["ALLSKY_SFC_SW_DWN"].as<JsonObject>();
      for (JsonPair kv : values) {
        float n = kv.value().as<float>();
        if (n >= 0 && (isnan(solar) || n > solar)) solar = n;
      }
    }
    result += "Temp: " + (isnan(temp) ? String("--") : String(temp, 1) + " C");
    result += " | RH: " + (isnan(rh) ? String("--") : String(rh, 0) + "%") + "\n";
    result += "Rain now: " + (isnan(rain) ? String("--") : String(rain, 1) + " mm");
    result += " | 7d max/day: " + (isnan(forecastRain) ? String("--") : String(forecastRain, 1) + " mm");
    result += " | Wind: " + (isnan(wind) ? String("--") : String(wind, 1) + " km/h") + "\n";
    result += "PM2.5: " + (isnan(pm25) ? String("--") : String(pm25, 1) + " ug/m3");
    result += " | UV: " + (isnan(uv) ? String("--") : String(uv, 1)) + "\n";
    result += "Quakes M2.5+ within 100 km / 7 days: " + String(quakes < 0 ? 0 : quakes);
    result += " | Max M: " + (isnan(maxMagnitude) ? String("--") : String(maxMagnitude, 1)) + "\n";
    result += "Flood river discharge peak (7d): " + (isnan(dischargePeak) ? String("unavailable") : String(dischargePeak, 1) + " m3/s") + "\n";
    result += "NASA POWER solar radiation peak: " + (isnan(solar) ? String("unavailable") : String(solar, 1) + " kWh/m2/day") + "\n";
    result += "\nCONSERVATION RECOMMENDATIONS\n";
    if (!haveWeather && !haveAir) {
      result += "- Weather/Air APIs unavailable; retry when internet is stable.\n";
    } else {
      if (!isnan(rh) && rh > 80) result += "- CRITICAL humidity: inspect for condensation, mould and corrosion; verify indoor RH and activate moisture-control SOP if indoor sensors confirm the risk.\n";
      else if (!isnan(rh) && rh > 65) result += "- Elevated humidity: monitor paper, leather, textile, wood and paintings; check indoor RH and ventilation/dehumidification controls.\n";
      if (!isnan(rh) && rh < 35) result += "- Very dry outdoor air: monitor sensitive organic materials for shrinkage/embrittlement; do not change indoor RH from outdoor data alone.\n";
      if (!isnan(pm25) && pm25 >= 25) result += "- Elevated PM2.5: keep vulnerable collections protected; inspect seals/filters and avoid drawing polluted outdoor air indoors.\n";
      if ((!isnan(rain) && rain >= 2) || (!isnan(forecastRain) && forecastRain > 20)) result += "- Rain/heavy-rain signal: inspect roof, drainage, leaks, and water ingress; protect vulnerable collections from flooding.\n";
      if (!isnan(wind) && wind >= 30) result += "- Strong wind: inspect temporary structures, loose roofing, trees, and exposed site elements.\n";
      if (!isnan(uv) && uv >= 6) result += "- High UV: reduce direct sunlight exposure for light-sensitive collections.\n";
      if (quakes > 0) result += "- Recent nearby earthquakes: inspect for new cracks, falling hazards, and structural changes; follow local authority guidance.\n";
      if (!isnan(dischargePeak) && !isnan(dischargeBase) && dischargePeak > dischargeBase * 2.5 && dischargePeak > 50) result += "- HIGH flood proxy: river discharge is elevated versus the forecast baseline; check official flood alerts and move vulnerable collections away from water-ingress zones.\n";
      else if (!isnan(dischargePeak) && !isnan(dischargeBase) && dischargePeak > dischargeBase * 1.5 && dischargePeak > 20) result += "- MODERATE flood proxy: monitor drainage and official flood warnings; prepare water-ingress protection.\n";
      if (!isnan(temp) && temp > 33) result += "- High temperature: reduce heat/light exposure for sensitive collections and verify indoor temperature.\n";
      if (isnan(rh) || isnan(pm25) || isnan(rain)) result += "- Some indicators are unavailable; check API connectivity and refresh.\n";
      result += "- These are outdoor proxies, not indoor sensor readings or official emergency alerts.\n";
    }
    result += "\nSources: Open-Meteo Weather/Air/Flood, USGS, NASA POWER.";
  }
  return result;
}

void requestOutdoorData() {
  if (outdoorFetchRunning) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConfigured) {
      outdoorFetchPending = true;
      outdoorFetchDone = false;
      outdoorResult = "OUTDOOR / OURHERITAGE\nConnecting to Wi-Fi before fetching live data...";
    } else {
      outdoorFetchPending = false;
      outdoorResult = "OUTDOOR / OURHERITAGE\\nWi-Fi is not configured.\\nOpen Settings to connect, then return to Outdoor.";
      outdoorFetchDone = true;
    }
    if (outdoorCardBody) { buildOutdoorCards(); renderOutdoorCard(); }
    return;
  }
  outdoorFetchPending = false;

  // Synchronous fetch: pause audio first so SD/network activity cannot starve playback.
  outdoorFetchRunning = true;
  outdoorFetchDone = false;
  outdoorResult = "OUTDOOR / OURHERITAGE\\nFetching live data... Music paused temporarily.";
  if (outdoorCardBody) { buildOutdoorCards(); renderOutdoorCard(); }

  musicPlayerPauseForFetch();
  outdoorResult = fetchOutdoorData();
  musicPlayerResumeAfterFetch();

  outdoorFetchRunning = false;
  outdoorFetchDone = true;
  if (outdoorCardBody) {
    buildOutdoorCards();
    renderOutdoorCard();
    if (outdoorCard) lv_obj_scroll_to_y(outdoorCard, 0, LV_ANIM_OFF);
    outdoorRenderedResult = outdoorResult;
  }
}

void updateOutdoorStatus() {
  if (outdoorFetchDone && outdoorCardBody && outdoorRenderedResult != outdoorResult) {
    buildOutdoorCards();
    renderOutdoorCard();
    if (outdoorCard) lv_obj_scroll_to_y(outdoorCard, 0, LV_ANIM_OFF);
    outdoorRenderedResult = outdoorResult;
  }
}

void updateIndoorStatus() {
  if (!indoorLabel) return;
  if (!loraEnabled) {
    lv_label_set_text(indoorLabel,
      "INDOOR / MOCHI\n\nDemo mode — dummy values only.\nLoRa receiver is not configured yet.\n\nTemperature: 26.4 C\nHumidity: 58%\neCO2: --\nTVOC: --\nLight: --\n\nConnect the Mochi LoRa node and enable verified UART pins to replace these demo values with live sensor telemetry.");
    return;
  }
  if (!telemetry.received) {
    lv_label_set_text(indoorLabel, "INDOOR / MOCHI\nLoRa receiver listening.\nWaiting for a valid telemetry packet...");
    return;
  }
  const uint32_t age = (millis() - telemetry.lastPacketAt) / 1000;
  String text = "INDOOR / MOCHI\nNode: ";
  text += telemetry.node;
  text += "\nLast packet: " + String(age) + " s ago\nTemperature: ";
  text += telemetry.hasTemperature ? String(telemetry.temperatureC, 1) + " C" : "--";
  text += "\nHumidity: ";
  text += telemetry.hasHumidity ? String(telemetry.humidityPct, 1) + " %" : "--";
  text += "\neCO2: ";
  text += telemetry.hasEco2 ? String(telemetry.eco2Ppm) + " ppm" : "--";
  text += "\nTVOC: ";
  text += telemetry.hasTvoc ? String(telemetry.tvocPpb) + " ppb" : "--";
  text += "\nLight: ";
  text += telemetry.hasLux ? String(telemetry.lux, 1) + " lux" : "--";
  lv_label_set_text(indoorLabel, text.c_str());
}

void showHome();
void showSettings();
void showMusicPlayer();

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

void renderOutdoorCard() {
  if (!outdoorCardTitle || !outdoorCardBody || !outdoorPageIndicator) return;
  lv_label_set_text(outdoorCardTitle, outdoorCardTitles[outdoorCardIndex]);
  lv_label_set_text(outdoorCardBody, outdoorCards[outdoorCardIndex].length()
    ? outdoorCards[outdoorCardIndex].c_str() : "Waiting for live data...");
  char page[16];
  snprintf(page, sizeof(page), "%u / 4", static_cast<unsigned>(outdoorCardIndex + 1));
  lv_label_set_text(outdoorPageIndicator, page);
}

void buildOutdoorCards() {
  for (String &card : outdoorCards) card = "";
  outdoorCards[0] = "Jakarta • Live public APIs\n";
  outdoorCards[1] = "Air quality indicators\n";
  outdoorCards[2] = "Regional hazard context\n";
  outdoorCards[3] = "Practical actions for heritage sites\n";
  bool inRecommendations = false;
  int start = 0;
  while (start < outdoorResult.length()) {
    int end = outdoorResult.indexOf('\n', start);
    if (end < 0) end = outdoorResult.length();
    String line = outdoorResult.substring(start, end);
    line.trim();
    if (line.indexOf("CONSERVATION RECOMMENDATIONS") >= 0) inRecommendations = true;
    else if (line.startsWith("Sources:")) {
      outdoorCards[3] += "\n" + line;
      outdoorCards[2] += "\n" + line;
    } else if (line.startsWith("Temp:") || line.startsWith("Rain now:") ||
               line.startsWith("Outdoor /") || line.startsWith("OUTDOOR /") ||
               line.startsWith("Jakarta |") || line.startsWith("Wi-Fi is offline") ||
               line.startsWith("Connect in Settings") || line.startsWith("Fetching live") ||
               line.startsWith("Could not start API")) {
      outdoorCards[0] += line + "\n";
    } else if (line.startsWith("PM2.5:")) {
      outdoorCards[1] += line + "\n";
    } else if (line.startsWith("Quakes") || line.startsWith("Flood river") ||
               line.startsWith("NASA POWER")) {
      outdoorCards[2] += line + "\n";
    } else if (inRecommendations && line.startsWith("- ")) {
      outdoorCards[3] += "• " + line.substring(2) + "\n";
    } else if (line.indexOf("These are outdoor proxies") >= 0) {
      outdoorCards[3] += "\nNote: " + line + "\n";
    }
    start = end + 1;
  }
  for (String &card : outdoorCards) card.trim();
}

void showOutdoor() {
  applyResourceMode(Page::OUTDOOR);
  currentPage = Page::OUTDOOR;
  resetPageObjects();
  outdoorRenderedResult = "";
  outdoorCard = nullptr;
  outdoorCardTitle = nullptr;
  outdoorCardBody = nullptr;
  outdoorPageIndicator = nullptr;

  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, LV_SYMBOL_GPS " OUTDOOR INTELLIGENCE");
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(heading, lv_color_hex(0xE2E8F0), 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 10, 5);

  outdoorCard = lv_obj_create(root);
  lv_obj_set_size(outdoorCard, 300, 142);
  lv_obj_align(outdoorCard, LV_ALIGN_TOP_LEFT, 10, 30);
  lv_obj_set_style_radius(outdoorCard, 14, 0);
  lv_obj_set_style_bg_color(outdoorCard, lv_color_hex(0x172033), 0);
  lv_obj_set_style_border_color(outdoorCard, lv_color_hex(0x334155), 0);
  lv_obj_set_style_border_width(outdoorCard, 1, 0);
  lv_obj_set_style_pad_all(outdoorCard, 10, 0);
  lv_obj_set_scroll_dir(outdoorCard, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(outdoorCard, LV_SCROLLBAR_MODE_AUTO);

  outdoorCardTitle = lv_label_create(outdoorCard);
  lv_obj_set_style_text_font(outdoorCardTitle, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(outdoorCardTitle, lv_color_hex(0x38BDF8), 0);
  lv_obj_align(outdoorCardTitle, LV_ALIGN_TOP_LEFT, 0, 0);
  outdoorCardBody = lv_label_create(outdoorCard);
  lv_label_set_long_mode(outdoorCardBody, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(outdoorCardBody, 276);
  lv_obj_set_style_text_color(outdoorCardBody, lv_color_hex(0xCBD5E1), 0);
  lv_obj_align(outdoorCardBody, LV_ALIGN_TOP_LEFT, 0, 25);

  lv_obj_t *prev = lv_btn_create(root);
  lv_obj_set_size(prev, 70, 28);
  lv_obj_align(prev, LV_ALIGN_TOP_LEFT, 56, 176);
  lv_obj_set_style_radius(prev, 9, 0);
  lv_obj_set_style_bg_color(prev, lv_color_hex(0x334155), 0);
  lv_obj_t *prevLabel = lv_label_create(prev);
  lv_label_set_text(prevLabel, LV_SYMBOL_LEFT " Prev");
  lv_obj_center(prevLabel);
  lv_obj_add_event_cb(prev, [](lv_event_t *) {
    outdoorCardIndex = (outdoorCardIndex + 3) % 4;
    renderOutdoorCard();
    if (outdoorCard) lv_obj_scroll_to_y(outdoorCard, 0, LV_ANIM_OFF);
  }, LV_EVENT_CLICKED, nullptr);

  outdoorPageIndicator = lv_label_create(root);
  lv_obj_set_style_text_color(outdoorPageIndicator, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(outdoorPageIndicator, LV_ALIGN_TOP_MID, 0, 184);

  lv_obj_t *next = lv_btn_create(root);
  lv_obj_set_size(next, 70, 28);
  lv_obj_align(next, LV_ALIGN_TOP_RIGHT, -56, 176);
  lv_obj_set_style_radius(next, 9, 0);
  lv_obj_set_style_bg_color(next, lv_color_hex(0x0E7490), 0);
  lv_obj_t *nextLabel = lv_label_create(next);
  lv_label_set_text(nextLabel, "Next " LV_SYMBOL_RIGHT);
  lv_obj_center(nextLabel);
  lv_obj_add_event_cb(next, [](lv_event_t *) {
    outdoorCardIndex = (outdoorCardIndex + 1) % 4;
    renderOutdoorCard();
    if (outdoorCard) lv_obj_scroll_to_y(outdoorCard, 0, LV_ANIM_OFF);
  }, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *home = lv_btn_create(root);
  lv_obj_set_size(home, 86, 26);
  lv_obj_align(home, LV_ALIGN_BOTTOM_LEFT, 8, -5);
  lv_obj_set_style_radius(home, 9, 0);
  lv_obj_set_style_bg_color(home, lv_color_hex(0x334155), 0);
  lv_obj_t *homeLabel = lv_label_create(home);
  lv_label_set_text(homeLabel, LV_SYMBOL_LEFT " Home");
  lv_obj_center(homeLabel);
  lv_obj_add_event_cb(home, [](lv_event_t *) { showHome(); }, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *refresh = lv_btn_create(root);
  lv_obj_set_size(refresh, 86, 26);
  lv_obj_align(refresh, LV_ALIGN_BOTTOM_RIGHT, -8, -5);
  lv_obj_set_style_radius(refresh, 9, 0);
  lv_obj_set_style_bg_color(refresh, lv_color_hex(0x0E7490), 0);
  lv_obj_t *refreshLabel = lv_label_create(refresh);
  lv_label_set_text(refreshLabel, LV_SYMBOL_REFRESH " Refresh");
  lv_obj_center(refreshLabel);
  lv_obj_add_event_cb(refresh, [](lv_event_t *) { requestOutdoorData(); }, LV_EVENT_CLICKED, nullptr);

  for (String &card : outdoorCards) card = "";
  outdoorCards[0] = "Fetching live weather and conservation context...";
  renderOutdoorCard();
  requestOutdoorData();
}

void showIndoor() {
  applyResourceMode(Page::INDOOR);
  currentPage = Page::INDOOR;
  resetPageObjects();
  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, LV_SYMBOL_HOME " INDOOR / MOCHI");
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(heading, lv_color_hex(0x38BDF8), 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 10, 8);
  indoorLabel = lv_label_create(root);
  lv_label_set_long_mode(indoorLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(indoorLabel, SCREEN_W - 24);
  lv_obj_set_height(indoorLabel, 174);
  lv_obj_set_style_text_color(indoorLabel, lv_color_hex(0xCBD5E1), 0);
  lv_obj_set_style_bg_color(indoorLabel, lv_color_hex(0x172033), 0);
  lv_obj_set_style_bg_opa(indoorLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(indoorLabel, 10, 0);
  lv_obj_set_style_radius(indoorLabel, 12, 0);
  lv_obj_set_scrollbar_mode(indoorLabel, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_align(indoorLabel, LV_ALIGN_TOP_LEFT, 10, 34);
  updateIndoorStatus();
  makeBackButton();
}

void tile(const char *title, const char *symbol, int x, int y, lv_event_cb_t callback) {
  lv_obj_t *button = lv_btn_create(root);
  lv_obj_set_size(button, 142, 58);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_style_radius(button, 13, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x172033), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, lv_color_hex(0x334155), 0);
  lv_obj_set_style_shadow_width(button, 8, 0);
  lv_obj_set_style_shadow_color(button, lv_color_hex(0x020617), 0);
  lv_obj_set_style_shadow_opa(button, LV_OPA_40, 0);
  lv_obj_t *label = lv_label_create(button);
  String text = String(symbol) + "  " + title;
  lv_label_set_text(label, text.c_str());
  lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(0xE2E8F0), 0);
  lv_obj_center(label);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
}

void showHome() {
  // Finish building the Home UI before changing Wi-Fi state. On boot this
  // lets LVGL render the menu before the radio starts connecting.
  if (currentPage == Page::MUSIC) musicPlayerStop();
  currentPage = Page::HOME;
  resetPageObjects();
  lv_obj_set_style_bg_color(root, lv_color_hex(0x0B1120), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

  lv_obj_t *brandMark = lv_obj_create(root);
  lv_obj_set_size(brandMark, 34, 34);
  lv_obj_align(brandMark, LV_ALIGN_TOP_LEFT, 10, 8);
  lv_obj_set_style_radius(brandMark, 10, 0);
  lv_obj_set_style_bg_color(brandMark, lv_color_hex(0x0E7490), 0);
  lv_obj_set_style_border_width(brandMark, 0, 0);
  lv_obj_t *mark = lv_label_create(brandMark);
  lv_label_set_text(mark, LV_SYMBOL_HOME);
  lv_obj_set_style_text_color(mark, lv_color_hex(0xF8FAFC), 0);
  lv_obj_center(mark);

  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, "OURHERITAGE");
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(heading, lv_color_hex(0xF8FAFC), 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 52, 7);

  wifiLabel = lv_label_create(root);
  lv_obj_set_style_text_font(wifiLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(wifiLabel, lv_color_hex(0x38BDF8), 0);
  lv_obj_align(wifiLabel, LV_ALIGN_TOP_RIGHT, -10, 12);
  updateWifiLabel();

  lv_obj_t *subtitle = lv_label_create(root);
  lv_label_set_text(subtitle, "HERITAGE CARE  /  SMART COMPANION");
  lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(subtitle, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 12, 44);

  tile("OUTDOOR", LV_SYMBOL_GPS, 12, 66, [](lv_event_t *) { showOutdoor(); });
  tile("MUSIC", LV_SYMBOL_AUDIO, 166, 66, [](lv_event_t *) { showMusicPlayer(); });
  tile("INDOOR", LV_SYMBOL_HOME, 12, 132, [](lv_event_t *) { showIndoor(); });
  tile("SETTINGS", LV_SYMBOL_SETTINGS, 166, 132, [](lv_event_t *) { showSettings(); });

  lv_obj_t *footer = lv_label_create(root);
  lv_label_set_text(footer, "SYSTEM READY  •  LOCAL DEVICE");
  lv_obj_set_style_text_color(footer, lv_color_hex(0x64748B), 0);
  lv_obj_set_style_text_font(footer, &lv_font_montserrat_12, 0);
  lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -3);

  // Enable Wi-Fi only after the Home widgets are ready.
  applyResourceMode(Page::HOME);
}

void showMusicPlayer() {
  applyResourceMode(Page::MUSIC);
  currentPage = Page::MUSIC;
  // Clear page-owned pointers before musicPlayerShow() deletes the home widgets.
  // Otherwise loop() may update a stale wifiLabel and crash inside LVGL.
  resetPageObjects();
  musicPlayerShow(root, [](lv_event_t *) { showHome(); });
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

void handleShowPassword(lv_event_t *event) {
  if (!passwordField) return;
  const bool show = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
  lv_textarea_set_password_mode(passwordField, !show);
}

void handleWifiScan(lv_event_t *) {
  if (wifiScanInProgress) {
    setStatus("Scan already in progress...", 0x2563EB);
    return;
  }

  // ESP32-S3 supports 2.4 GHz only. If it is stuck trying an old saved
  // network, stop that attempt before starting a fresh scan.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(false, false);
    delay(120);
  }

  const int priorScan = WiFi.scanComplete();
  if (priorScan == WIFI_SCAN_RUNNING) {
    wifiScanInProgress = true;
    setStatus("A Wi-Fi scan is already running...", 0x2563EB);
    Serial.println("Wi-Fi scan: existing scan is still running");
    return;
  }
  WiFi.scanDelete();
  const int result = WiFi.scanNetworks(true, true, false, 300);
  Serial.printf("Wi-Fi scan start return: %d (RUNNING=%d, FAILED=%d)\n",
                result, WIFI_SCAN_RUNNING, WIFI_SCAN_FAILED);
  if (result == WIFI_SCAN_FAILED) {
    setStatus("Scan start failed. Tap Scan Wi-Fi again.", 0xDC2626);
    Serial.printf("Wi-Fi scan: could not start; status=%d\n", WiFi.status());
    return;
  }
  wifiScanInProgress = true;
  setStatus("Scanning 2.4 GHz Wi-Fi...", 0x2563EB);
  Serial.println("Wi-Fi scan started");
}

void finishWifiScan(int count) {
  wifiScanInProgress = false;
  if (!wifiDropdown) {
    WiFi.scanDelete();
    return;
  }
  if (count <= 0) {
    lv_dropdown_set_options(wifiDropdown, "No networks found");
    setStatus("No 2.4 GHz networks found. Retry near router.", 0xDC2626);
    Serial.printf("Wi-Fi scan finished: %d\n", count);
    WiFi.scanDelete();
    return;
  }

  String options;
  int firstVisible = -1;
  for (int i = 0; i < count; ++i) {
    String name = WiFi.SSID(i);
    if (name.isEmpty()) name = "(Hidden network)";
    name.replace("\n", " ");
    name.replace("\r", " ");
    if (options.length()) options += "\n";
    options += name;
    if (firstVisible < 0 && WiFi.SSID(i).length() > 0) firstVisible = i;
    Serial.printf("Wi-Fi %d: SSID='%s', RSSI=%d, channel=%d\n",
                  i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
  }
  lv_dropdown_set_options(wifiDropdown, options.c_str());
  lv_dropdown_set_selected(wifiDropdown, 0);
  if (ssidField && firstVisible >= 0) {
    String selected = WiFi.SSID(firstVisible);
    lv_textarea_set_text(ssidField, selected.c_str());
  }
  String msg = "Found " + String(count) + " networks (2.4 GHz).";
  setStatus(msg.c_str(), 0x16A34A);
  WiFi.scanDelete();
}
void handleWifiSelect(lv_event_t *) {
  if (!wifiDropdown || !ssidField) return;
  char selected[33] = {0};
  lv_dropdown_get_selected_str(wifiDropdown, selected, sizeof(selected));
  if (strcmp(selected, "No networks found") != 0 &&
      strcmp(selected, "(Hidden network)") != 0) {
    lv_textarea_set_text(ssidField, selected);
  }
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
  applyResourceMode(Page::SETTINGS);
  currentPage = Page::SETTINGS;
  resetPageObjects();
  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, LV_SYMBOL_SETTINGS " SETTINGS");
  lv_obj_set_style_text_color(heading, lv_color_hex(0x38BDF8), 0);
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 12, 8);

  ssidField = lv_textarea_create(root);
  lv_textarea_set_one_line(ssidField, true);
  lv_textarea_set_placeholder_text(ssidField, "Wi-Fi SSID");
  lv_obj_set_size(ssidField, 142, 32);
  lv_obj_set_style_bg_color(ssidField, lv_color_hex(0x172033), 0);
  lv_obj_set_style_text_color(ssidField, lv_color_hex(0xF1F5F9), 0);
  lv_obj_set_style_border_color(ssidField, lv_color_hex(0x334155), 0);
  lv_obj_align(ssidField, LV_ALIGN_TOP_LEFT, 12, 42);
  lv_obj_add_event_cb(ssidField, textareaFocused, LV_EVENT_FOCUSED, nullptr);

  passwordField = lv_textarea_create(root);
  lv_textarea_set_one_line(passwordField, true);
  lv_textarea_set_password_mode(passwordField, true);
  lv_textarea_set_placeholder_text(passwordField, "Wi-Fi password");
  lv_obj_set_size(passwordField, 142, 32);
  lv_obj_set_style_bg_color(passwordField, lv_color_hex(0x172033), 0);
  lv_obj_set_style_text_color(passwordField, lv_color_hex(0xF1F5F9), 0);
  lv_obj_set_style_border_color(passwordField, lv_color_hex(0x334155), 0);
  lv_obj_align(passwordField, LV_ALIGN_TOP_LEFT, 166, 42);
  lv_obj_add_event_cb(passwordField, textareaFocused, LV_EVENT_FOCUSED, nullptr);

  showPasswordToggle = lv_checkbox_create(root);
  lv_checkbox_set_text(showPasswordToggle, "Show password");
  lv_obj_set_style_text_color(showPasswordToggle, lv_color_hex(0xCBD5E1), 0);
  lv_obj_set_style_text_font(showPasswordToggle, &lv_font_montserrat_12, 0);
  lv_obj_align(showPasswordToggle, LV_ALIGN_TOP_LEFT, 166, 76);
  lv_obj_add_event_cb(showPasswordToggle, handleShowPassword, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *scanButton = lv_btn_create(root);
  lv_obj_set_size(scanButton, 142, 32);
  lv_obj_set_style_radius(scanButton, 9, 0);
  lv_obj_set_style_bg_color(scanButton, lv_color_hex(0x334155), 0);
  lv_obj_align(scanButton, LV_ALIGN_TOP_LEFT, 12, 108);
  lv_obj_t *scanLabel = lv_label_create(scanButton);
  lv_label_set_text(scanLabel, LV_SYMBOL_REFRESH " Scan Wi-Fi");
  lv_obj_center(scanLabel);
  lv_obj_add_event_cb(scanButton, handleWifiScan, LV_EVENT_CLICKED, nullptr);

  wifiDropdown = lv_dropdown_create(root);
  lv_dropdown_set_options(wifiDropdown, "Tap Scan Wi-Fi");
  lv_obj_set_size(wifiDropdown, 142, 32);
  lv_obj_set_style_bg_color(wifiDropdown, lv_color_hex(0x172033), 0);
  lv_obj_set_style_text_color(wifiDropdown, lv_color_hex(0xF1F5F9), 0);
  lv_obj_set_style_border_color(wifiDropdown, lv_color_hex(0x334155), 0);
  lv_obj_align(wifiDropdown, LV_ALIGN_TOP_LEFT, 166, 108);
  lv_obj_add_event_cb(wifiDropdown, handleWifiSelect, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *connectButton = lv_btn_create(root);
  lv_obj_set_size(connectButton, SCREEN_W - 24, 30);
  lv_obj_set_style_radius(connectButton, 9, 0);
  lv_obj_set_style_bg_color(connectButton, lv_color_hex(0x0E7490), 0);
  lv_obj_align(connectButton, LV_ALIGN_TOP_LEFT, 12, 146);
  lv_obj_t *connectLabel = lv_label_create(connectButton);
  lv_label_set_text(connectLabel, "Save & Connect");
  lv_obj_center(connectLabel);
  lv_obj_add_event_cb(connectButton, handleConnect, LV_EVENT_CLICKED, nullptr);

  statusLabel = lv_label_create(root);
  lv_label_set_text(statusLabel, "Wi-Fi credentials are stored locally on this device.");
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(0xCBD5E1), 0);
  lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(statusLabel, SCREEN_W - 24);
  lv_obj_align(statusLabel, LV_ALIGN_TOP_LEFT, 12, 182);

  keyboard = lv_keyboard_create(root);
  lv_obj_set_size(keyboard, SCREEN_W, 84);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(keyboard, keyboardEvent, LV_EVENT_ALL, nullptr);
  // Keep Back in the header so it never collides with status text or keyboard.
  lv_obj_t *backButton = lv_btn_create(root);
  lv_obj_set_size(backButton, 68, 30);
  lv_obj_set_style_radius(backButton, 8, 0);
  lv_obj_set_style_bg_color(backButton, lv_color_hex(0x334155), 0);
  lv_obj_align(backButton, LV_ALIGN_TOP_RIGHT, -8, 7);
  lv_obj_t *backLabel = lv_label_create(backButton);
  lv_label_set_text(backLabel, LV_SYMBOL_LEFT " Back");
  lv_obj_center(backLabel);
  lv_obj_add_event_cb(backButton, [](lv_event_t *) { showHome(); }, LV_EVENT_CLICKED, nullptr);

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
  WiFi.setSleep(false);
  if (wifiConfigured && WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid.c_str(), password.c_str());
    lastWifiTry = millis();
    Serial.printf("Wi-Fi: connecting to saved network '%s'\n", ssid.c_str());
  } else if (!wifiConfigured) {
    Serial.println("Wi-Fi: not configured; open Settings to connect.");
  }
}

void applyResourceMode(Page target) {
  // Each page owns only the services it needs. Music gets the Wi-Fi radio
  // entirely out of the way; Indoor is currently local/demo + future LoRa.
  if (currentPage == Page::MUSIC && target != Page::MUSIC) {
    musicPlayerStop();
  }

  if (target == Page::MUSIC || target == Page::INDOOR) {
    wifiScanInProgress = false;
    wifiGotIpEvent = false;
    wifiDisconnectedEvent = false;
    WiFi.scanDelete();
    WiFi.disconnect(false, false); // preserve saved credentials
    WiFi.mode(WIFI_OFF);
    Serial.printf("[RESOURCE] mode=%s wifi=OFF audio=%s\n",
                  target == Page::MUSIC ? "MUSIC" : "INDOOR",
                  target == Page::MUSIC ? "PRIORITY" : "STOPPED");
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    startSavedWifi();
    Serial.printf("[RESOURCE] mode=%s wifi=ON audio=STOPPED\n",
                  target == Page::HOME ? "HOME" :
                  target == Page::OUTDOOR ? "OUTDOOR" : "SETTINGS");
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
  printMemoryStatus("startup");
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
  tft.setRotation(1);
  tft.setSwapBytes(true);

  // Short color-bar diagnostic: proves whether TFT_eSPI can actually draw
  // pixels before LVGL takes ownership of the panel.
  Serial.println("Display test: RED");
  tft.fillScreen(TFT_RED);
  delay(1500);
  Serial.println("Display test: GREEN");
  tft.fillScreen(TFT_GREEN);
  delay(1500);
  Serial.println("Display test: BLUE");
  tft.fillScreen(TFT_BLUE);
  delay(1500);
  tft.fillScreen(TFT_BLACK);
  delay(500);
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

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      wifiGotIpEvent = true;
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      wifiDisconnectReason = info.wifi_sta_disconnected.reason;
      wifiDisconnectedEvent = true;
      Serial.printf("Wi-Fi disconnected, reason=%u\n",
                    static_cast<unsigned>(wifiDisconnectReason));
    }
  });

  setupLoRa();
  musicPlayerInit();
  showHome();
}

void loop() {
  // MP3 decoding/I2S is serviced by the dedicated FreeRTOS audio task.
  // Keep the UI loop free for LVGL flushes, touch input, and page logic.
  lv_timer_handler();
  pollLoRa();

  if (wifiScanInProgress) {
    const int scanResult = WiFi.scanComplete();
    if (scanResult != WIFI_SCAN_RUNNING) finishWifiScan(scanResult);
  }

  if (wifiGotIpEvent) {
    wifiGotIpEvent = false;
    Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
    String connected = "Connected! IP: " + WiFi.localIP().toString();
    setStatus(connected.c_str(), 0x16A34A);
    updateWifiLabel();
    updateOutdoorStatus();
    if (currentPage == Page::OUTDOOR && outdoorFetchPending) requestOutdoorData();
  }
  if (wifiDisconnectedEvent) {
    wifiDisconnectedEvent = false;
    const uint8_t reason = wifiDisconnectReason;
    Serial.printf("Wi-Fi disconnected, reason=%u\n", static_cast<unsigned>(reason));
    if (currentPage == Page::SETTINGS && wifiConfigured) {
      const char *message = "Connection failed. Check router and password.";
      switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
          message = "Network not found. Check SSID and router range.";
          break;
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
          message = "Authentication failed. Recheck Wi-Fi password.";
          break;
        case WIFI_REASON_ASSOC_FAIL:
          message = "Router rejected connection. Check router security.";
          break;
        case WIFI_REASON_BEACON_TIMEOUT:
          message = "Signal lost. Move closer to the router.";
          break;
        default:
          break;
      }
      setStatus(message, 0xDC2626);
    }
    updateWifiLabel();
    updateOutdoorStatus();
  }

  const uint32_t now = millis();
  // Low-frequency diagnostics let us compare memory during playback without
  // flooding Serial and adding avoidable work to the real-time audio loop.
  static uint32_t lastMemoryLog = 0;
  // Avoid heap diagnostics during playback; heap queries/logging can add
  // periodic jitter to the UI task even though audio now runs independently.
  if (currentPage != Page::MUSIC && now - lastMemoryLog >= 15000) {
    lastMemoryLog = now;
    printMemoryStatus("runtime");
  }

  if (currentPage != Page::MUSIC && currentPage != Page::INDOOR &&
      wifiConfigured && WiFi.status() != WL_CONNECTED &&
      now - lastWifiTry >= WIFI_RETRY_MS) {
    lastWifiTry = now;
    WiFi.reconnect();
  }

  if (now - lastUiRefresh >= UI_REFRESH_MS) {
    lastUiRefresh = now;
    updateWifiLabel();
    if (currentPage == Page::OUTDOOR) updateOutdoorStatus();
    if (currentPage == Page::INDOOR) updateIndoorStatus();
  }
  delay(1);
}
