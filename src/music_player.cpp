#include "music_player.h"

#include <Wire.h>
#include <SD_MMC.h>
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/Disk/AudioSourceSDMMC.h"

namespace {
constexpr int PA_ENABLE = 1;
constexpr uint8_t MAX_TRACKS = 80;

AudioSourceSDMMC source("/Music", ".mp3");
AudioBoardStream kit(ESP32S3HosyondDisplay);
MP3DecoderHelix decoder;
AudioPlayer player(source, kit, decoder);

String tracks[MAX_TRACKS];
size_t trackCount = 0;
int selectedTrack = -1;
int shuffleOrder[MAX_TRACKS];
size_t shufflePos = 0;
bool shuffleEnabled = false;
bool sdReady = false;
bool audioReady = false;
bool playing = false;
bool paused = false;
bool suppressTrackEvent = false;
volatile bool trackEnded = false;
uint8_t volumePercent = 40;

lv_obj_t *statusLabel = nullptr;
lv_obj_t *nowPlayingLabel = nullptr;
lv_obj_t *trackDropdown = nullptr;
lv_obj_t *playButton = nullptr;
lv_obj_t *volumeSlider = nullptr;
lv_obj_t *shuffleButton = nullptr;

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
    const String name = displayName(tracks[selectedTrack]);
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
      if (lower.endsWith(".mp3")) {
        if (!path.startsWith("/")) path = String("/Music/") + path;
        tracks[trackCount++] = path;
        Serial.printf("[music] track: %s\n", path.c_str());
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  Serial.printf("[music] tracks=%u\n", static_cast<unsigned>(trackCount));
}

void setSelectedTrack(int index) {
  if (!trackCount) { selectedTrack = -1; return; }
  if (index < 0) index = static_cast<int>(trackCount) - 1;
  if (index >= static_cast<int>(trackCount)) index = 0;
  selectedTrack = index;
  refreshTitle();
  if (trackDropdown) {
    suppressTrackEvent = true;
    lv_dropdown_set_selected(trackDropdown, static_cast<uint16_t>(selectedTrack));
    suppressTrackEvent = false;
  }
}

void rebuildShuffleOrder() {
  size_t count = 0;
  for (size_t i = 0; i < trackCount; ++i) {
    if (static_cast<int>(i) != selectedTrack) shuffleOrder[count++] = static_cast<int>(i);
  }
  // Fisher-Yates: each remaining track appears once before the bag is rebuilt.
  for (size_t i = count; i > 1; --i) {
    const size_t j = static_cast<size_t>(random(i));
    const int tmp = shuffleOrder[i - 1];
    shuffleOrder[i - 1] = shuffleOrder[j];
    shuffleOrder[j] = tmp;
  }
  shufflePos = 0;
}

void updateShuffleButton() {
  if (!shuffleButton) return;
  lv_obj_t *label = lv_obj_get_child(shuffleButton, 0);
  if (label) lv_label_set_text(label, LV_SYMBOL_SHUFFLE);
  lv_obj_set_style_bg_color(shuffleButton,
      lv_color_hex(shuffleEnabled ? 0x047857 : 0x334155), 0);
}

void onTrackEOF(AudioPlayer &) {
  // Keep the EOF callback lightweight; perform UI and file operations in musicPlayerLoop().
  trackEnded = true;
}

void startSelected();

void advanceTrackAfterEOF() {
  if (!playing || paused || !trackEnded) return;
  trackEnded = false;
  if (!trackCount) {
    playing = false;
    setStatus("Playback ended; no tracks available.", 0xCBD5E1);
    return;
  }
  if (shuffleEnabled && trackCount > 1) {
    if (shufflePos >= trackCount - 1) rebuildShuffleOrder();
    setSelectedTrack(shuffleOrder[shufflePos++]);
  } else {
    setSelectedTrack(selectedTrack + 1);
  }
  startSelected();
}

void startSelected() {
  if (!audioReady) { setStatus("Audio board initialization failed.", 0xDC2626); return; }
  if (!sdReady) { setStatus("microSD mount failed.", 0xDC2626); return; }
  if (!trackCount) { setStatus("No MP3 files in /Music.", 0xDC2626); return; }
  if (selectedTrack < 0) setSelectedTrack(0);
  // IMPORTANT: setPath() selects and activates the requested stream. Calling
  // begin() immediately afterward reinitializes the source and resets selection
  // to stream index 0, which made Next/Shuffle update only the UI title.
  trackEnded = false;
  player.stop();
  const bool opened = player.setPath(tracks[selectedTrack].c_str());
  if (!opened) {
    playing = false;
    paused = false;
    setStatus("Could not open MP3 file.", 0xDC2626);
    return;
  }
  playing = true;
  paused = false;
  if (playButton) lv_label_set_text(lv_obj_get_child(playButton, 0), LV_SYMBOL_PAUSE);
  refreshTitle();
  setStatus("Playing from microSD.", 0x16A34A);
  Serial.printf("[music] play: %s\n", tracks[selectedTrack].c_str());
}

void onTrackChanged(lv_event_t *) {
  if (suppressTrackEvent || !trackDropdown || !trackCount) return;
  setSelectedTrack(lv_dropdown_get_selected(trackDropdown));
  setStatus("Track selected. Press Play.");
}

void onPlay(lv_event_t *) {
  if (!audioReady) { setStatus("Audio board initialization failed.", 0xDC2626); return; }
  if (playing) {
    paused = !paused;
    if (paused) player.stop();
    else startSelected();
    if (playButton) lv_label_set_text(lv_obj_get_child(playButton, 0), paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    setStatus(paused ? "Paused (restarts track on Play)." : "Playing from microSD.", 0x16A34A);
  } else {
    startSelected();
  }
}

void onNext(lv_event_t *) {
  if (!trackCount) { setStatus("No tracks in /Music.", 0xDC2626); return; }
  if (shuffleEnabled && trackCount > 1) {
    if (shufflePos >= trackCount - 1) rebuildShuffleOrder();
    setSelectedTrack(shuffleOrder[shufflePos++]);
  } else {
    setSelectedTrack(selectedTrack + 1);
  }
  startSelected();
}
void onShuffle(lv_event_t *) {
  shuffleEnabled = !shuffleEnabled;
  shufflePos = 0;
  if (shuffleEnabled) rebuildShuffleOrder();
  updateShuffleButton();
  setStatus(shuffleEnabled ? "Shuffle on: tracks won't repeat until the cycle ends."
                           : "Shuffle off: sequential playback.");
}
void onPrevious(lv_event_t *) {
  if (!trackCount) { setStatus("No tracks in /Music.", 0xDC2626); return; }
  setSelectedTrack(selectedTrack - 1);
  startSelected();
}
void onVolume(lv_event_t *event) {
  volumePercent = static_cast<uint8_t>(lv_slider_get_value(lv_event_get_target(event)));
  kit.setVolume(volumePercent / 100.0f);
  char text[32];
  snprintf(text, sizeof(text), "Volume: %u%%", volumePercent);
  setStatus(text);
}
} // namespace

void musicPlayerInit() {
  pinMode(PA_ENABLE, OUTPUT);
  digitalWrite(PA_ENABLE, HIGH); // amplifier muted while initializing
  Wire.begin(16, 15);

  auto config = kit.defaultConfig(TX_MODE);
  config.sdmmc_active = true; // AudioBoardStream initializes the board's 4-bit SD_MMC pins
  config.sample_rate = 44100;
  config.channels = 2;
  config.bits_per_sample = 16;
  audioReady = kit.begin(config);
  if (audioReady) {
    kit.setVolume(volumePercent / 100.0f);
    digitalWrite(PA_ENABLE, LOW);
    sdReady = true; // SD_MMC mount is owned by the board's begin()
  } else {
    Serial.println("[music] AudioBoardStream begin failed");
    // Independently check the card so the UI can report both failures accurately.
    sdReady = SD_MMC.cardType() != CARD_NONE;
  }
  player.setAutoNext(false); // Our EOF handler advances sequentially or through the shuffle bag.
  player.setOnEOFCallback(onTrackEOF);
  scanFolder();
}

void musicPlayerLoop() {
  if (playing && !paused) {
    player.copy();
    advanceTrackAfterEOF();
  }
}

void musicPlayerStop() {
  if (playing) player.stop();
  playing = false;
  paused = false;
  if (playButton) lv_label_set_text(lv_obj_get_child(playButton, 0), LV_SYMBOL_PLAY);
}

void musicPlayerPauseForFetch() {
  if (playing && !paused) {
    player.stop();
    paused = true;
    if (playButton) lv_label_set_text(lv_obj_get_child(playButton, 0), LV_SYMBOL_PLAY);
  }
}
void musicPlayerResumeAfterFetch() {
  if (playing && paused) {
    paused = false;
    player.play();
    if (playButton) lv_label_set_text(lv_obj_get_child(playButton, 0), LV_SYMBOL_PAUSE);
  }
}

void musicPlayerShow(lv_obj_t *root, lv_event_cb_t backCallback) {
  lv_obj_clean(root);
  statusLabel = nullptr;
  nowPlayingLabel = nullptr;
  trackDropdown = nullptr;
  playButton = nullptr;
  volumeSlider = nullptr;
  shuffleButton = nullptr;

  lv_obj_t *heading = lv_label_create(root);
  lv_label_set_text(heading, "MUSIC PLAYER");
  lv_obj_set_style_text_color(heading, lv_color_hex(0xE2E8F0), 0);
  lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
  lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 12, 8);

  lv_obj_t *sub = lv_label_create(root);
  lv_label_set_text(sub, "ES8311  |  microSD  |  MP3");
  lv_obj_set_style_text_color(sub, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 12, 34);

  nowPlayingLabel = lv_label_create(root);
  lv_label_set_long_mode(nowPlayingLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_width(nowPlayingLabel, 296);
  lv_obj_set_style_text_color(nowPlayingLabel, lv_color_hex(0xF8FAFC), 0);
  lv_obj_align(nowPlayingLabel, LV_ALIGN_TOP_LEFT, 12, 56);
  refreshTitle();

  trackDropdown = lv_dropdown_create(root);
  lv_obj_set_size(trackDropdown, 220, 34);
  lv_obj_align(trackDropdown, LV_ALIGN_TOP_LEFT, 12, 82);
  if (trackCount) {
    String options;
    for (size_t i = 0; i < trackCount; ++i) {
      if (i) options += "\n";
      options += displayName(tracks[i]);
    }
    lv_dropdown_set_options(trackDropdown, options.c_str());
    if (selectedTrack >= 0) lv_dropdown_set_selected(trackDropdown, selectedTrack);
  } else {
    lv_dropdown_set_options(trackDropdown, "No MP3 files found");
  }
  lv_obj_add_event_cb(trackDropdown, onTrackChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  shuffleButton = lv_btn_create(root);
  lv_obj_set_size(shuffleButton, 64, 34);
  lv_obj_align(shuffleButton, LV_ALIGN_TOP_LEFT, 244, 82);
  lv_obj_set_style_radius(shuffleButton, 7, 0);
  lv_obj_t *shuffleLabel = lv_label_create(shuffleButton);
  lv_label_set_text(shuffleLabel, LV_SYMBOL_SHUFFLE);
  lv_obj_center(shuffleLabel);
  lv_obj_add_event_cb(shuffleButton, onShuffle, LV_EVENT_CLICKED, nullptr);
  updateShuffleButton();

  const uint32_t buttonColors[3] = {0x334155, 0x0E7490, 0x334155};
  const char *buttonTexts[3] = {
    LV_SYMBOL_PREV,
    playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY,
    LV_SYMBOL_NEXT
  };
  const int buttonWidths[3] = {64, 64, 64};
  const int buttonXs[3] = {28, 128, 228};
  lv_obj_t *buttons[3] = {};
  for (int i = 0; i < 3; ++i) {
    buttons[i] = lv_btn_create(root);
    lv_obj_set_size(buttons[i], buttonWidths[i], 36);
    lv_obj_align(buttons[i], LV_ALIGN_TOP_LEFT, buttonXs[i], 126);
    lv_obj_set_style_bg_color(buttons[i], lv_color_hex(buttonColors[i]), 0);
    lv_obj_set_style_bg_color(buttons[i], lv_color_hex(0x475569), LV_STATE_PRESSED);
    lv_obj_set_style_radius(buttons[i], 8, 0);
    lv_obj_t *label = lv_label_create(buttons[i]);
    lv_label_set_text(label, buttonTexts[i]);
    lv_obj_center(label);
  }
  lv_obj_add_event_cb(buttons[0], onPrevious, LV_EVENT_CLICKED, nullptr);
  for (int i = 0; i < 3; ++i) {
    lv_obj_set_style_text_font(lv_obj_get_child(buttons[i], 0), &lv_font_montserrat_20, 0);
  }
  playButton = buttons[1];
  lv_obj_add_event_cb(buttons[1], onPlay, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(buttons[2], onNext, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *volumeLabel = lv_label_create(root);
  lv_label_set_text(volumeLabel, "VOLUME");
  lv_obj_set_style_text_color(volumeLabel, lv_color_hex(0xCBD5E1), 0);
  lv_obj_align(volumeLabel, LV_ALIGN_TOP_LEFT, 12, 174);
  volumeSlider = lv_slider_create(root);
  lv_obj_set_size(volumeSlider, 212, 16);
  lv_obj_align(volumeSlider, LV_ALIGN_TOP_LEFT, 80, 174);
  lv_slider_set_range(volumeSlider, 0, 100);
  lv_slider_set_value(volumeSlider, volumePercent, LV_ANIM_OFF);
  lv_obj_add_event_cb(volumeSlider, onVolume, LV_EVENT_VALUE_CHANGED, nullptr);

  statusLabel = lv_label_create(root);
  lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(statusLabel, 296);
  lv_obj_set_style_text_color(statusLabel, lv_color_hex(0xCBD5E1), 0);
  lv_obj_align(statusLabel, LV_ALIGN_TOP_LEFT, 12, 202);
  if (!audioReady) setStatus("Audio board initialization failed.", 0xDC2626);
  else if (!sdReady) setStatus("microSD mount failed.", 0xDC2626);
  else if (!trackCount) setStatus("No MP3 files in /Music.", 0xDC2626);
  else setStatus("Ready to play.", 0x16A34A);

  lv_obj_t *back = lv_btn_create(root);
  lv_obj_set_size(back, 100, 28);
  lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 12, -4);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x334155), 0);
  lv_obj_t *backLabel = lv_label_create(back);
  lv_label_set_text(backLabel, "HOME");
  lv_obj_center(backLabel);
  lv_obj_add_event_cb(back, backCallback, LV_EVENT_CLICKED, nullptr);
}
