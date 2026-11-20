#pragma once
// User feedback: SSD1306 OLED animations, three LEDs and a passive buzzer.
#include <Arduino.h>
#include <Wire.h>

#include "flip_detector.h"

enum class Screen : uint8_t { Boot, Open, Locked, Unlocked, SensorError };

struct UiState {
  Screen screen = Screen::Boot;
  FlipStatus flip;
  uint32_t requiredMs = 10000;
  uint16_t minFlips = 6;
  bool alarm = false;
  bool wifi = false;
  bool serverOk = false;
  uint16_t queued = 0;
  bool timeValid = false;
  char clock[6] = "--:--";
  uint32_t screenSince = 0;   // millis() when the screen last changed
  const char *toast = nullptr; // short message shown for a moment
  uint32_t toastUntil = 0;
};

struct Note {
  uint16_t freq;  // 0 = rest
  uint16_t ms;
};

class Buzzer {
 public:
  void begin();
  void play(const Note *notes, uint8_t n);  // interrupts whatever is playing
  void setAlarm(bool on);
  void update(uint32_t now);

 private:
  void tone(uint16_t f);
  Note notes_[8];
  uint8_t n_ = 0, idx_ = 0;
  uint32_t noteStart_ = 0;
  bool alarm_ = false;
};

namespace sounds {
extern const Note flip[1];
extern const Note lock[2];
extern const Note unlock[4];
extern const Note denied[2];
extern const Note boot[2];
}  // namespace sounds

class Feedback {
 public:
  bool begin(TwoWire &wire, uint8_t addr);
  void render(const UiState &ui, uint32_t now);  // OLED, call ~10 Hz
  void leds(const UiState &ui, uint32_t now);    // call every loop
  bool displayOk() const { return displayOk_; }
  Buzzer buzzer;

 private:
  void topBar(const UiState &ui);
  void drawBox(int cx, int cy, float angleDeg);
  bool displayOk_ = false;
};
