#include "music_player.h"

#include <Wire.h>
#include <SD_MMC.h>
#include <Audio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace {
constexpr uint8_t CODEC_ADDR = 0x18;
constexpr int PA_ENABLE = 1;       // FM8002E amplifier is enabled LOW
constexpr int I2S_MCLK = 4;
constexpr int I2S_BCLK = 5;
constexpr int I2S_LRCK = 7;
constexpr int I2S_DOUT = 8;
constexpr int SD_CLK = 38;
constexpr int SD_CMD = 40;
constexpr int SD_D0 = 39;
constexpr int SD_D1 = 41;
constexpr int SD_D2 = 48;
constexpr int SD_D3 = 47;
constexpr size_t MAX_TRACKS = 80;

Audio audio;
enum class AudioCmd : uint8_t { PLAY, PAUSE_RESUME, STOP, VOLUME };
struct AudioMsg {
  AudioCmd cmd;
  int value;
  char path[192];
};
QueueHandle_t audioQueue = nullptr;
TaskHandle_t audioTaskHandle = nullptr;
String tracks[MAX_TRACKS];
size_t trackCount = 0;
int selectedTrack = -1;
int shuffleOrder[MAX_TRACKS];
size_t shufflePosition = 0;
bool shuffleEnabled = false;
bool sdReady = false;
bool codecReady = false;
bool playing = false;
bool paused = false;
bool pausedForFetch = false;
uint8_t volumePercent = 40;

lv_obj_t *statusLabel = nullptr;
lv_obj_t *nowPlayingLabel = nullptr;
lv_obj_t *trackDropdown = nullptr;
lv_obj_t *playButton = nullptr;
lv_obj_t *shuffleButton = nullptr;
lv_obj_t *volumeSlider = nullptr;

bool writeCodec(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(CODEC_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool initCodec() {
  Wire.beginTransmission(CODEC_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("[music] ES8311 not detected at 0x18");
    return false;
  }

  // ES3C28P audio reference: I2S clocks come from ESP32-S3.
  writeCodec(0x00, 0x1F);
  delay(20);
  writeCodec(0x00, 0x80);
  writeCodec(0x01, 0x3F);
  writeCodec(0x02, 0x00);
  writeCodec(0x03, 0x10);
  writeCodec(0x04, 0x10);
  writeCodec(0x05, 0x00);
  writeCodec(0x06, 0x03);
  writeCodec(0x07, 0x00);
  writeCodec(0x08, 0xFF);
  writeCodec(0x09, 0x0C);
  writeCodec(0x0A, 0x4C);
  writeCodec(0x0B, 0x00);
  writeCodec(0x0C, 0x00);
  writeCodec(0x0D, 0x01);
  writeCodec(0x0E, 0x02);
  writeCodec(0x0F, 0x44);
  writeCodec(0x10, 0x1F);
  writeCodec(0x11, 0x7F);
  writeCodec(0x12, 0x00);
  writeCodec(0x13, 0x10);
  writeCodec(0x14, 0x1A);
  writeCodec(0x31, 0x00);
  writeCodec(0x32, 0xBF);
  writeCodec(0x33, 0xBF);
  writeCodec(0x37, 0x08);
  delay(40);
  Serial.println("[music] ES8311 initialized");
  return true;
}

String displayName(const String &path) {
  const int slash = path.lastIndexOf('/');
  return slash >= 0 ? path.substring(slash + 1) : path;
}

void setStatus(const char *message, uint32_t color = 0xCBD5E1) {
  if (!statusLabel) return;
  lv_label_set_text(statusLabel, message);
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(color), 0);
}

void refreshTitle() {
  if (!nowPlayingLabel) return;
  if (selectedTrack >= 0 && selectedTrack < static_cast<int>(trackCount)) {
    String name = displayName(tracks[selectedTrack]);
    lv_label_set_text(nowPlayingLabel, name.c_str());
  } else {
    lv_label_set_text(nowPlayingLabel, "Nothing playing");
  }
}

void scanFolder() {
  trackCount = 0;
  if (!sdReady) return;
  File dir = SD_MMC.open("/Music");
  if (!dir || !dir.isDirectory()) {
    Serial.println("[music] /Music folder not found");
    if (dir) dir.close();
    return;
  }

  File entry = dir.openNextFile();
  while (entry && trackCount < MAX_TRACKS) {
    if (!entry.isDirectory()) {
      String path = String(entry.name());
      String lower = path;
      lower.toLowerCase();
      if (lower.endsWith(".mp3") || lower.endsWith(".m4a")) {
        if (!path.startsWith("/")) path = String("/Music/") + path;
        tracks[trackCount++] = path;
        Serial.printf("[music] found: %s\n", path.c_str());
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  Serial.printf("[music] %u supported tracks\n", static_cast<unsigned>(trackCount));
}

void shufflePlaylist(bool keepCurrentFirst) {
  for (size_t i = 0; i < trackCount; ++i) shuffleOrder[i] = static_cast<int>(i);
  // Fisher-Yates: randomize the upcoming playlist order.
  for (size_t i = trackCount; i > 1; --i) {
    const size_t j = static_cast<size_t>(random(static_cast<long>(i)));
    const int tmp = shuffleOrder[i - 1];
    shuffleOrder[i - 1] = shuffleOrder[j];
    shuffleOrder[j] = tmp;
  }
  shufflePosition = 0;
  if (keepCurrentFirst && selectedTrack >= 0 && selectedTrack < static_cast<int>(trackCount)) {
    for (size_t i = 0; i < trackCount; ++i) {
      if (shuffleOrder[i] == selectedTrack) {
        const int tmp = shuffleOrder[0];
        shuffleOrder[0] = shuffleOrder[i];
        shuffleOrder[i] = tmp;
        break;
      }
    }
  }
}

void updateShuffleButton() {
  if (!shuffleButton) return;
  lv_obj_t *label = lv_obj_get_child(shuffleButton, 0);
  if (label) lv_label_set_text(label, shuffleEnabled ? "Shuffle: ON" : "Shuffle: OFF");
  lv_obj_set_style_bg_color(shuffleButton,
    lv_color_hex(shuffleEnabled ? 0x15803D : 0x475569), LV_PART_MAIN);
}

int nextTrackIndex(bool forward) {
  if (!trackCount) return -1;
  if (!shuffleEnabled) {
    return forward
      ? (selectedTrack + 1) % static_cast<int>(trackCount)
      : (selectedTrack <= 0 ? static_cast<int>(trackCount - 1) : selectedTrack - 1);
  }

  if (!trackCount) return -1;
  bool found = false;
  for (size_t i = 0; i < trackCount; ++i) {
    if (shuffleOrder[i] == selectedTrack) {
      shufflePosition = i;
      found = true;
      break;
    }
  }
  if (!found) {
    shufflePlaylist(true);
    return trackCount > 1 ? shuffleOrder[1] : shuffleOrder[0];
  }

  if (forward) {
    if (shufflePosition + 1 >= trackCount) {
      const int previousTrack = selectedTrack;
      shufflePlaylist(false);
      if (trackCount > 1 && shuffleOrder[0] == previousTrack) {
        const int tmp = shuffleOrder[0];
        shuffleOrder[0] = shuffleOrder[1];
        shuffleOrder[1] = tmp;
      }
      shufflePosition = 0;
    } else {
      ++shufflePosition;
    }
  } else {
    if (shufflePosition == 0) shufflePosition = trackCount - 1;
    else --shufflePosition;
  }
  return shuffleOrder[shufflePosition];
}

void onShuffle(lv_event_t *) {
  shuffleEnabled = !shuffleEnabled;
  if (shuffleEnabled) {
    shufflePlaylist(true);
    setStatus("Shuffle enabled: upcoming tracks randomized.", 0x16A34A);
  } else {
    setStatus("Shuffle disabled: normal playlist order.");
  }
  updateShuffleButton();
}

void sendAudio(AudioCmd cmd, int value = 0, const char *path = nullptr) {
  if (!audioQueue) {
    Serial.println("[music] audio command dropped: task queue not ready");
    return;
  }
  AudioMsg message = {};
  message.cmd = cmd;
  message.value = value;
  if (path) strlcpy(message.path, path, sizeof(message.path));
  if (xQueueSend(audioQueue, &message, pdMS_TO_TICKS(10)) != pdTRUE) {
    Serial.printf("[music] audio command queue full (cmd=%u)\n", static_cast<unsigned>(cmd));
  }
}

void audioTask(void *) {
  AudioMsg message;
  Serial.println("[music] dedicated audio task started on core 0");
  for (;;) {
    while (xQueueReceive(audioQueue, &message, 0) == pdTRUE) {
      switch (message.cmd) {
        case AudioCmd::PLAY:
          audio.stopSong();
          audio.connecttoFS(SD_MMC, message.path);
          break;
        case AudioCmd::PAUSE_RESUME:
          audio.pauseResume();
          break;
        case AudioCmd::STOP:
          audio.stopSong();
          break;
        case AudioCmd::VOLUME:
          audio.setVolume(message.value);
          break;
      }
    }
    audio.loop();
    vTaskDelay(1);
  }
}

void startSelected() {
  if (!codecReady) { setStatus("ES8311 codec not detected.", 0xDC2626); return; }
  if (!sdReady) { setStatus("microSD mount failed.", 0xDC2626); return; }
  if (!trackCount) { setStatus("No MP3/M4A files in /Music.", 0xDC2626); return; }
  if (selectedTrack < 0 || selectedTrack >= static_cast<int>(trackCount)) selectedTrack = 0;

  const String path = tracks[selectedTrack];
  sendAudio(AudioCmd::PLAY, 0, path.c_str());
  playing = true;
  paused = false;
  if (playButton) lv_label_set_text(lv_obj_get_child(playButton, 0), "Pause");
  refreshTitle();
  setStatus("Opening track from microSD...", 0x16A34A);
  Serial.printf("[music] play: %s\n", path.c_str());
}

void onTrackChanged(lv_event_t *) {
  if (!trackDropdown || !trackCount) return;
  const uint16_t index = lv_dropdown_get_selected(trackDropdown);
  if (index < trackCount) {
    selectedTrack = index;
    refreshTitle();
    setStatus("Track selected. Press Play.");
  }
}

void onPlay(lv_event_t *) {
  if (playing) {
    sendAudio(AudioCmd::PAUSE_RESUME);
    paused = !paused;
    if (playButton) lv_label_set_text(lv_obj_get_child(playButton, 0), paused ? "Play" : "Pause");
    setStatus(paused ? "Paused" : "Playing from microSD", 0x16A34A);
  } else {
    startSelected();
  }
}

void onNext(lv_event_t *) {
  if (!trackCount) { setStatus("No tracks in /Music.", 0xDC2626); return; }
  selectedTrack = nextTrackIndex(true);
  if (trackDropdown) lv_dropdown_set_selected(trackDropdown, selectedTrack);
  startSelected();
}

void onPrevious(lv_event_t *) {
  if (!trackCount) { setStatus("No tracks in /Music.", 0xDC2626); return; }
  selectedTrack = nextTrackIndex(false);
  if (trackDropdown) lv_dropdown_set_selected(trackDropdown, selectedTrack);
  startSelected();
}

void onVolume(lv_event_t *event) {
  volumePercent = static_cast<uint8_t>(lv_slider_get_value(lv_event_get_target(event)));
  sendAudio(AudioCmd::VOLUME, map(volumePercent, 0, 100, 0, 21));
}

void onVolumeReleased(lv_event_t *) {
  char text[32];
  snprintf(text, sizeof(text), "Volume: %u%%", volumePercent);
  setStatus(text);
}
} // namespace

void musicPlayerInit() {
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
  sdReady = SD_MMC.begin("/sdcard", false);
  if (!sdReady) sdReady = SD_MMC.begin("/sdcard", true);
  Serial.printf("[music] SD_MMC mount: %s\n", sdReady ? "OK" : "FAILED");

  // Keep initialization compatible with the ESP32-audioI2S API resolved by PlatformIO.
  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_PIN_NO_CHANGE, I2S_MCLK);
  codecReady = initCodec();
  pinMode(PA_ENABLE, OUTPUT);
  digitalWrite(PA_ENABLE, codecReady ? LOW : HIGH);
  audio.setVolume(map(volumePercent, 0, 100, 0, 21));
  randomSeed(static_cast<unsigned long>(esp_random()));
  scanFolder();

  audioQueue = xQueueCreate(8, sizeof(AudioMsg));
  if (!audioQueue) {
    Serial.println("[music] ERROR: failed to create audio command queue");
    return;
  }
  const BaseType_t taskCreated = xTaskCreatePinnedToCore(
    audioTask, "audio", 12288, nullptr, 3, &audioTaskHandle, 0);
  if (taskCreated != pdPASS) {
    Serial.println("[music] ERROR: failed to create audio task");
    vQueueDelete(audioQueue);
    audioQueue = nullptr;
  }
}

void musicPlayerStop() {
  // Stop decoding and release the active stream before another page takes over.
  sendAudio(AudioCmd::STOP);
  playing = false;
  paused = false;
  pausedForFetch = false;
  Serial.println("[music] stopped; resource mode released");
}

void musicPlayerLoop() {
  // Audio is serviced by the dedicated FreeRTOS task.
}

void musicPlayerPauseForFetch() {
  pausedForFetch = playing && !paused;
  if (pausedForFetch) {
    sendAudio(AudioCmd::PAUSE_RESUME);
    paused = true;
    if (playButton) lv_label_set_text(lv_obj_get_child(playButton, 0), "Play");
    setStatus("Music paused while Outdoor data loads.", 0xD97706);
  }
}

void musicPlayerResumeAfterFetch() {
  if (pausedForFetch && playing && paused) {
    sendAudio(AudioCmd::PAUSE_RESUME);
    paused = false;
    if (playButton) lv_label_set_text(lv_obj_get_child(playButton, 0), "Pause");
    setStatus("Outdoor data loaded. Music resumed.", 0x16A34A);
  }
  pausedForFetch = false;
}

void musicPlayerShow(lv_obj_t *root, lv_event_cb_t backCallback) {
  lv_obj_clean(root);
  statusLabel = nullptr;
  nowPlayingLabel = nullptr;
  trackDropdown = nullptr;
  playButton = nullptr;
  shuffleButton = nullptr;
  volumeSlider = nullptr;

  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, LV_SYMBOL_AUDIO " NOW PLAYING");
  lv_obj_set_style_text_color(heading, lv_color_hex(0x38BDF8), 0);
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 12, 8);

  nowPlayingLabel = lv_label_create(root);
  lv_label_set_long_mode(nowPlayingLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_width(nowPlayingLabel, 296);
  lv_obj_set_style_text_color(nowPlayingLabel, lv_color_hex(0xF8FAFC), 0);
  lv_obj_align(nowPlayingLabel, LV_ALIGN_TOP_LEFT, 12, 38);
  refreshTitle();

  trackDropdown = lv_dropdown_create(root);
  lv_obj_set_size(trackDropdown, 296, 34);
  lv_obj_set_style_bg_color(trackDropdown, lv_color_hex(0x172033), 0);
  lv_obj_set_style_text_color(trackDropdown, lv_color_hex(0xF1F5F9), 0);
  lv_obj_set_style_border_color(trackDropdown, lv_color_hex(0x334155), 0);
  lv_obj_align(trackDropdown, LV_ALIGN_TOP_LEFT, 12, 64);
  if (trackCount) {
    String options;
    for (size_t i = 0; i < trackCount; ++i) {
      if (i) options += "\n";
      options += displayName(tracks[i]);
    }
    lv_dropdown_set_options(trackDropdown, options.c_str());
    if (selectedTrack >= 0) lv_dropdown_set_selected(trackDropdown, selectedTrack);
  } else {
    lv_dropdown_set_options(trackDropdown, "No songs found");
  }
  lv_obj_add_event_cb(trackDropdown, onTrackChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *previous = lv_btn_create(root);
  lv_obj_set_size(previous, 54, 34);
  lv_obj_set_style_radius(previous, 9, 0);
  lv_obj_set_style_bg_color(previous, lv_color_hex(0x334155), 0);
  lv_obj_align(previous, LV_ALIGN_TOP_LEFT, 8, 108);
  lv_obj_t *previousLabel = lv_label_create(previous);
  lv_label_set_text(previousLabel, "Prev");
  lv_obj_center(previousLabel);
  lv_obj_add_event_cb(previous, onPrevious, LV_EVENT_CLICKED, nullptr);

  playButton = lv_btn_create(root);
  lv_obj_set_size(playButton, 68, 34);
  lv_obj_set_style_radius(playButton, 9, 0);
  lv_obj_set_style_bg_color(playButton, lv_color_hex(0x0E7490), 0);
  lv_obj_align(playButton, LV_ALIGN_TOP_LEFT, 68, 108);
  lv_obj_t *playLabel = lv_label_create(playButton);
  lv_label_set_text(playLabel, playing ? "Pause" : "Play");
  lv_obj_center(playLabel);
  lv_obj_add_event_cb(playButton, onPlay, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *next = lv_btn_create(root);
  lv_obj_set_size(next, 54, 34);
  lv_obj_set_style_radius(next, 9, 0);
  lv_obj_set_style_bg_color(next, lv_color_hex(0x334155), 0);
  lv_obj_align(next, LV_ALIGN_TOP_LEFT, 142, 108);
  lv_obj_t *nextLabel = lv_label_create(next);
  lv_label_set_text(nextLabel, "Next");
  lv_obj_center(nextLabel);
  lv_obj_add_event_cb(next, onNext, LV_EVENT_CLICKED, nullptr);

  shuffleButton = lv_btn_create(root);
  lv_obj_set_size(shuffleButton, 104, 34);
  lv_obj_set_style_radius(shuffleButton, 9, 0);
  lv_obj_align(shuffleButton, LV_ALIGN_TOP_RIGHT, -8, 108);
  lv_obj_t *shuffleLabel = lv_label_create(shuffleButton);
  lv_label_set_text(shuffleLabel, "Shuffle: OFF");
  lv_obj_center(shuffleLabel);
  lv_obj_add_event_cb(shuffleButton, onShuffle, LV_EVENT_CLICKED, nullptr);
  updateShuffleButton();

  lv_obj_t *volumeLabel = lv_label_create(root);
  lv_label_set_text(volumeLabel, "Volume");
  lv_obj_align(volumeLabel, LV_ALIGN_TOP_LEFT, 12, 151);
  volumeSlider = lv_slider_create(root);
  lv_obj_set_size(volumeSlider, 212, 16);
  lv_obj_set_style_bg_color(volumeSlider, lv_color_hex(0x334155), LV_PART_MAIN);
  lv_obj_set_style_bg_color(volumeSlider, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
  lv_obj_align(volumeSlider, LV_ALIGN_TOP_LEFT, 80, 151);
  lv_slider_set_range(volumeSlider, 0, 100);
  lv_slider_set_value(volumeSlider, volumePercent, LV_ANIM_OFF);
  lv_obj_add_event_cb(volumeSlider, onVolume, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(volumeSlider, onVolumeReleased, LV_EVENT_RELEASED, nullptr);

  statusLabel = lv_label_create(root);
  lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(statusLabel, 296);
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(0xCBD5E1), 0);
  lv_obj_align(statusLabel, LV_ALIGN_TOP_LEFT, 12, 177);
  if (!codecReady) setStatus("ES8311 codec not detected.", 0xDC2626);
  else if (!sdReady) setStatus("microSD mount failed.", 0xDC2626);
  else if (!trackCount) setStatus("No MP3/M4A files in /Music.", 0xDC2626);
  else setStatus("Ready to play", 0x16A34A);

  lv_obj_t *back = lv_btn_create(root);
  lv_obj_set_size(back, 100, 36);
  lv_obj_set_style_radius(back, 9, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x334155), 0);
  lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 12, -8);
  lv_obj_t *backLabel = lv_label_create(back);
  lv_label_set_text(backLabel, "Home");
  lv_obj_center(backLabel);
  lv_obj_add_event_cb(back, backCallback, LV_EVENT_CLICKED, nullptr);
}
