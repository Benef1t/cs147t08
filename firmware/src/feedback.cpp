#include "feedback.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

#include "config.h"

static Adafruit_SSD1306 oled(128, 64, &Wire, -1);

// ---------------- Sounds ----------------
namespace sounds {
const Note flip[1] = {{1760, 35}};
const Note lock[2] = {{660, 90}, {440, 140}};
const Note unlock[4] = {{523, 110}, {659, 110}, {784, 110}, {1047, 260}};
const Note denied[2] = {{220, 120}, {0, 60}};
const Note boot[2] = {{880, 60}, {1320, 80}};
}  // namespace sounds

static const Note kAlarm[4] = {{988, 180}, {0, 90}, {988, 180}, {0, 550}};

void Buzzer::begin() {
  ledcSetup(LEDC_BUZZER_CH, 2000, 10);
  ledcAttachPin(PIN_BUZZER, LEDC_BUZZER_CH);
  ledcWrite(LEDC_BUZZER_CH, 0);
}

void Buzzer::tone(uint16_t f) {
  if (f == 0) {
    ledcWrite(LEDC_BUZZER_CH, 0);
  } else {
    ledcWriteTone(LEDC_BUZZER_CH, f);  // 50% duty square wave
  }
}

void Buzzer::play(const Note *notes, uint8_t n) {
  n_ = min<uint8_t>(n, 8);
  memcpy(notes_, notes, n_ * sizeof(Note));
  idx_ = 0;
  noteStart_ = millis();
  tone(n_ ? notes_[0].freq : 0);
}

void Buzzer::setAlarm(bool on) {
  if (on == alarm_) return;
  alarm_ = on;
  if (on) {
    play(kAlarm, 4);
  } else {
    n_ = 0;
    tone(0);
  }
}

void Buzzer::update(uint32_t now) {
  if (n_ == 0) return;
  if (now - noteStart_ < notes_[idx_].ms) return;
  idx_++;
  if (idx_ >= n_) {
    if (alarm_) {
      play(kAlarm, 4);  // loop the alarm pattern
    } else {
      n_ = 0;
      tone(0);
    }
    return;
  }
  noteStart_ = now;
  tone(notes_[idx_].freq);
}

// ---------------- Display ----------------
bool Feedback::begin(TwoWire &wire, uint8_t addr) {
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  buzzer.begin();
  displayOk_ = oled.begin(SSD1306_SWITCHCAPVCC, addr);
  if (displayOk_) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.display();
  }
  return displayOk_;
}

void Feedback::leds(const UiState &ui, uint32_t now) {
  const bool blink = (now / 250) % 2;
  bool r = false, y = false, g = false;
  switch (ui.screen) {
    case Screen::Locked:
      r = ui.alarm ? blink : true;
      if (ui.flip.attempt) y = ui.flip.paused ? blink : true;
      break;
    case Screen::Unlocked:
    case Screen::Open:
      g = true;
      break;
    case Screen::SensorError:
      r = blink;
      y = !blink;
      break;
    default:
      break;
  }
  digitalWrite(PIN_LED_RED, r);
  digitalWrite(PIN_LED_YELLOW, y);
  digitalWrite(PIN_LED_GREEN, g);
}

void Feedback::topBar(const UiState &ui) {
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print(ui.clock);
  if (ui.alarm) oled.print(" ALARM");
  const char *status = !ui.wifi ? "no wifi" : (!ui.serverOk ? "no srv" : "online");
  char right[20];
  if (ui.queued) snprintf(right, sizeof right, "Q%u %s", ui.queued, status);
  else snprintf(right, sizeof right, "%s", status);
  oled.setCursor(128 - 6 * strlen(right), 0);
  oled.print(right);
  oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);
}

// A box seen from the side, rotated by angleDeg; the thick edge is the lid.
void Feedback::drawBox(int cx, int cy, float angleDeg) {
  const float a = angleDeg * PI / 180.f, c = cosf(a), s = sinf(a);
  const float hw = 13, hh = 8;
  float px[4] = {-hw, hw, hw, -hw}, py[4] = {-hh, -hh, hh, hh};
  int x[4], y[4];
  for (int i = 0; i < 4; i++) {
    x[i] = cx + (int)(px[i] * c - py[i] * s);
    y[i] = cy + (int)(px[i] * s + py[i] * c);
  }
  for (int i = 0; i < 4; i++) oled.drawLine(x[i], y[i], x[(i + 1) % 4], y[(i + 1) % 4], SSD1306_WHITE);
  // lid: second line just inside the top edge
  float lx0 = -hw + 2, lx1 = hw - 2, ly = -hh + 2;
  oled.drawLine(cx + (int)(lx0 * c - ly * s), cy + (int)(lx0 * s + ly * c), cx + (int)(lx1 * c - ly * s),
                cy + (int)(lx1 * s + ly * c), SSD1306_WHITE);
}

static void centered(const char *txt, int y, uint8_t size) {
  oled.setTextSize(size);
  int w = strlen(txt) * 6 * size;
  oled.setCursor((128 - w) / 2, y);
  oled.print(txt);
}

void Feedback::render(const UiState &ui, uint32_t now) {
  if (!displayOk_) return;
  oled.clearDisplay();
  const uint32_t t = now - ui.screenSince;

  switch (ui.screen) {
    case Screen::Boot:
      centered("RiseQuest", 18, 2);
      centered("starting...", 44, 1);
      break;

    case Screen::SensorError:
      topBar(ui);
      centered("SENSOR ERROR", 20, 1);
      centered("check MPU-6050", 34, 1);
      centered("wiring (SDA21 SCL22)", 46, 1);
      break;

    case Screen::Open:
      topBar(ui);
      centered("OPEN", 16, 2);
      centered("store phone, close lid", 38, 1);
      if ((now / 600) % 2) centered("then press button", 50, 1);
      break;

    case Screen::Unlocked: {  // celebration for a few seconds after unlocking
      topBar(ui);
      for (int r = (t / 40) % 24, k = 0; k < 3; k++, r += 8) {
        if (r < 40) oled.drawCircle(64, 38, r, SSD1306_WHITE);
      }
      oled.fillRect(16, 30, 96, 18, SSD1306_BLACK);
      centered("UNLOCKED!", 32, 2);
      break;
    }

    case Screen::Locked: {
      topBar(ui);
      const FlipStatus &f = ui.flip;
      if (!f.attempt) {
        // Idle locked: animated box flip as an instruction.
        float ang = (t % 2000) < 1000 ? ((t % 1000) / 1000.f) * 180.f : 180.f;
        drawBox(26, 38, ang);
        oled.setTextSize(1);
        oled.setCursor(52, 18);
        oled.print("LOCKED");
        oled.setCursor(52, 32);
        oled.print("flip me for");
        oled.setCursor(52, 44);
        oled.printf("%lus to open", (unsigned long)(ui.requiredMs / 1000));
      } else {
        char buf[24];
        snprintf(buf, sizeof buf, "%.1f/%lus", f.activeMs / 1000.f, (unsigned long)(ui.requiredMs / 1000));
        centered(buf, 14, 2);
        // progress bar
        oled.drawRect(4, 34, 120, 10, SSD1306_WHITE);
        oled.fillRect(6, 36, (116 * f.percent) / 100, 6, SSD1306_WHITE);
        oled.setTextSize(1);
        oled.setCursor(4, 52);
        oled.printf("flips %u/%u", f.flips, ui.minFlips);
        // face indicator arrow
        const int ax = 116, ay = 55;
        if (f.face == Face::Up) oled.fillTriangle(ax - 5, ay + 4, ax + 5, ay + 4, ax, ay - 5, SSD1306_WHITE);
        else if (f.face == Face::Down) oled.fillTriangle(ax - 5, ay - 5, ax + 5, ay - 5, ax, ay + 4, SSD1306_WHITE);
        else oled.drawCircle(ax, ay, 4, SSD1306_WHITE);
        if (f.paused && (now / 300) % 2) {
          oled.fillRect(10, 12, 108, 20, SSD1306_BLACK);
          centered("KEEP FLIPPING!", 18, 1);
        } else if (f.shaking) {
          oled.setCursor(64, 52);
          oled.print("steady!");
        }
      }
      break;
    }
  }

  if (ui.toast && (int32_t)(ui.toastUntil - now) > 0) {
    oled.fillRect(0, 22, 128, 20, SSD1306_BLACK);
    oled.drawRect(0, 22, 128, 20, SSD1306_WHITE);
    centered(ui.toast, 28, 1);
  }
  if (ui.alarm) oled.invertDisplay((now / 500) % 2);
  else oled.invertDisplay(false);
  oled.display();
}
