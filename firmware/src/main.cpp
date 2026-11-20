// RiseQuest: a motion-unlocked phone storage box (CS147 Group 8)
//
//   OPEN  --button (phone stored, lid closed)-->  LOCKED
//   LOCKED --10 s of repeated face-up/face-down flips-->  UNLOCKED (celebrate) --> OPEN
//
// Core 1 (this loop): sensing, flip detection, latch, OLED/LEDs/buzzer, alarm.
// Core 0 (net task):  Wi-Fi, NTP, offline queue + uploads, config, signed OTA.
#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "config.h"
#include "feedback.h"
#include "flip_detector.h"
#include "imu.h"
#include "latch.h"
#include "net.h"
#include "settings.h"

enum class State : uint8_t { Open, Locked, Celebrate };

static Imu imu;
static Latch latch;
static Feedback fb;
static FlipDetector detector;
static DeviceSettings cfg;
static UiState ui;
static ImuSample sample;

static State state = State::Open;
static uint32_t stateSince = 0;
static bool imuWasOk = true;
static uint32_t lastImuMs = 0, lastImuRetry = 0, lastDisplayMs = 0, lastLiveMs = 0, lastClockMs = 0;

// button
static bool btnStable = false, btnLast = false;
static uint32_t btnChangedMs = 0;

// alarm
static bool alarmActive = false;
static uint32_t alarmStartMs = 0;
static int lastAlarmYday = -1;

static const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

static void setState(State s) {
  state = s;
  stateSince = millis();
  ui.screenSince = stateSince;
}

static void toast(const char *msg, uint32_t ms = 1500) {
  ui.toast = msg;
  ui.toastUntil = millis() + ms;
}

// ---------------- actions ----------------
static void stopAlarm(const char *reason) {
  if (!alarmActive) return;
  alarmActive = false;
  fb.buzzer.setAlarm(false);
  char d[48];
  snprintf(d, sizeof d, "{\"reason\":\"%s\"}", reason);
  net::event("alarm_stop", d);
}

static void doLock() {
  detector.reset();
  latch.lock();
  setState(State::Locked);
  fb.buzzer.play(sounds::lock, 2);
  net::event("lock", "{\"source\":\"button\"}");
}

static void doUnlock(const char *source) {
  latch.unlock();
  setState(State::Celebrate);
  stopAlarm(source);
  fb.buzzer.play(sounds::unlock, 4);
  char d[64];
  snprintf(d, sizeof d, "{\"source\":\"%s\",\"cycles\":%lu}", source, (unsigned long)latch.cycles());
  net::event("unlock", d);
}

static void onButton() {
  if (state == State::Locked) {
    toast("flip the box to open");
    fb.buzzer.play(sounds::denied, 2);
    return;
  }
  if (alarmActive) {  // first press while open silences the alarm
    stopAlarm("button");
    return;
  }
  doLock();
}

// ---------------- periodic work ----------------
static void pollButton(uint32_t now) {
  const bool raw = digitalRead(PIN_BUTTON) == LOW;
  if (raw != btnLast) {
    btnLast = raw;
    btnChangedMs = now;
  }
  if (now - btnChangedMs >= BUTTON_DEBOUNCE_MS && raw != btnStable) {
    btnStable = raw;
    if (btnStable) onButton();
  }
}

static void pollImu(uint32_t now) {
  if (now - lastImuMs < IMU_PERIOD_MS) return;
  lastImuMs = now;

  if (!imu.ok()) {
    if (now - lastImuRetry > 2000) {
      lastImuRetry = now;
      imu.begin(Wire, MPU_ADDR);
    }
  } else if (imu.read(sample, IMU_UP_AXIS, IMU_UP_INVERT) && state == State::Locked) {
    const FlipStatus &f = detector.update(sample.ax, sample.ay, sample.az, now);
    char d[96];
    if (f.evFlip) fb.buzzer.play(sounds::flip, 1);
    if (f.evStart) net::event("attempt_start", "{}");
    if (f.evAbandon) {
      snprintf(d, sizeof d, "{\"active_ms\":%lu,\"wall_ms\":%lu,\"flips\":%u}", (unsigned long)f.abandonActiveMs,
               (unsigned long)f.abandonWallMs, f.abandonFlips);
      net::event("attempt_abandon", d);
    }
    if (f.evComplete) {
      snprintf(d, sizeof d, "{\"active_ms\":%lu,\"wall_ms\":%lu,\"flips\":%u,\"rejected\":%u}",
               (unsigned long)f.activeMs, (unsigned long)f.wallMs, f.flips, f.rejected);
      net::event("attempt_complete", d);
      doUnlock("flips");
    }
  }

  const bool ok = imu.ok();
  if (ok != imuWasOk) {
    imuWasOk = ok;
    net::event(ok ? "sensor_ok" : "sensor_error", "{\"sensor\":\"mpu6050\"}");
  }
}

static void pollAlarm(uint32_t now) {
  if (now - lastClockMs < 1000) return;
  lastClockMs = now;

  ui.timeValid = net::timeValid();
  if (!ui.timeValid) {
    strcpy(ui.clock, "--:--");
    return;
  }
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  snprintf(ui.clock, sizeof ui.clock, "%02d:%02d", lt.tm_hour, lt.tm_min);

  if (cfg.alarmEnabled && !alarmActive && lt.tm_hour == cfg.alarmHour && lt.tm_min == cfg.alarmMinute &&
      lt.tm_yday != lastAlarmYday) {
    lastAlarmYday = lt.tm_yday;
    alarmActive = true;
    alarmStartMs = now;
    fb.buzzer.setAlarm(true);
    char d[48];
    snprintf(d, sizeof d, "{\"time\":\"%02u:%02u\"}", cfg.alarmHour, cfg.alarmMinute);
    net::event("alarm_start", d);
  }
  if (alarmActive && now - alarmStartMs > ALARM_MAX_MS) stopAlarm("timeout");
}

static void pollNetwork() {
  DeviceSettings s;
  if (net::takeNewSettings(s)) {
    cfg = s;
    store::saveSettings(cfg);
    detector.setConfig(cfg.flipConfig());
    net::applySettings(cfg);
    char d[48];
    snprintf(d, sizeof d, "{\"version\":%lu}", (unsigned long)cfg.version);
    net::event("config_applied", d);
    toast("settings updated");
  }
  int cmdId;
  if (net::takeRemoteRelease(cmdId)) {
    char d[40];
    snprintf(d, sizeof d, "{\"command_id\":%d}", cmdId);
    net::event("remote_release", d);
    if (state == State::Locked) doUnlock("remote");
  }
  net::setOtaAllowed(state == State::Open && !alarmActive);
}

static void publishLive(uint32_t now) {
  if (now - lastLiveMs < LIVE_PERIOD_MS) return;
  lastLiveMs = now;
  const FlipStatus &f = detector.status();
  LiveSample l;
  l.ax = sample.ax;
  l.ay = sample.ay;
  l.az = sample.az;
  l.tilt = sample.tiltDeg;
  l.pitch = sample.pitchDeg;
  l.roll = sample.rollDeg;
  l.face = f.face;
  const bool locked = state == State::Locked;
  l.percent = locked ? f.percent : 0;
  l.activeMs = locked ? f.activeMs : 0;
  l.flips = locked ? f.flips : 0;
  l.attempt = locked && f.attempt;
  l.paused = locked && f.paused;
  l.shaking = f.shaking;
  l.locked = latch.locked();
  l.alarm = alarmActive;
  l.imuOk = imu.ok();
  l.latchCycles = latch.cycles();
  net::setLive(l);
}

static void updateUi(uint32_t now) {
  if (state == State::Celebrate && now - stateSince > 3000) setState(State::Open);

  Screen s = Screen::Open;
  if (state == State::Locked) s = imu.ok() ? Screen::Locked : Screen::SensorError;
  else if (state == State::Celebrate) s = Screen::Unlocked;
  ui.screen = s;
  ui.flip = detector.status();
  ui.requiredMs = detector.config().requiredMs;
  ui.minFlips = detector.config().minFlips;
  ui.alarm = alarmActive;
  ui.wifi = net::wifiUp();
  ui.serverOk = net::serverOk();
  ui.queued = net::queued();

  fb.leds(ui, now);
  if (now - lastDisplayMs >= DISPLAY_PERIOD_MS) {
    lastDisplayMs = now;
    fb.render(ui, now);
  }
}

// ---------------- Arduino entry points ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  const esp_reset_reason_t reason = esp_reset_reason();

  store::begin();
  const uint32_t bootId = store::nextBootId();
  if (!store::loadSettings(cfg)) store::saveSettings(cfg);
  detector.setConfig(cfg.flipConfig());

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  fb.begin(Wire, OLED_ADDR);
  ui.screen = Screen::Boot;
  fb.render(ui, millis());

  imu.begin(Wire, MPU_ADDR);
  imuWasOk = imu.ok();

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  btnLast = btnStable = digitalRead(PIN_BUTTON) == LOW;

  // Keep the box locked across reboots and power loss.
  const bool wasLocked = store::loadLocked();
  latch.begin(wasLocked);
  setState(wasLocked ? State::Locked : State::Open);

  esp_task_wdt_init(WDT_TIMEOUT_S, true);  // hung loop -> reboot (reason is logged)
  esp_task_wdt_add(nullptr);

  net::begin(bootId, cfg);
  char d[100];
  snprintf(d, sizeof d, "{\"reason\":\"%s\",\"fw\":%d,\"locked\":%d,\"imu\":%d,\"oled\":%d}",
           resetReasonName(reason), FW_VERSION, wasLocked, imu.ok(), fb.displayOk());
  net::event("boot", d);
  if (!imu.ok()) net::event("sensor_error", "{\"sensor\":\"mpu6050\"}");

  Serial.printf("RiseQuest fw %d boot #%lu (%s) imu=%d whoami=0x%02x oled=%d locked=%d\n", FW_VERSION,
                (unsigned long)bootId, resetReasonName(reason), imu.ok(), imu.whoAmI(), fb.displayOk(), wasLocked);
  fb.buzzer.play(sounds::boot, 2);
}

void loop() {
  const uint32_t now = millis();
  esp_task_wdt_reset();

  pollButton(now);
  pollImu(now);
  pollAlarm(now);
  pollNetwork();
  publishLive(now);

  latch.update(now);
  fb.buzzer.update(now);
  updateUi(now);
  delay(2);
}
