#pragma once
// Settings pushed from the dashboard, persisted in NVS so they survive reboots.
#include <Arduino.h>

#include "flip_detector.h"

struct DeviceSettings {
  uint32_t version = 0;           // server config_version these came from
  bool alarmEnabled = false;
  uint8_t alarmHour = 7;
  uint8_t alarmMinute = 0;
  char tz[48] = "PST8PDT,M3.2.0,M11.1.0";  // POSIX TZ string (Irvine)
  uint16_t requiredS = 10;
  uint16_t pauseMs = 2500;
  uint16_t minFlips = 6;
  uint8_t faceCosPct = 80;        // 80 -> 0.80
  uint16_t heartbeatS = 60;

  FlipConfig flipConfig() const;
};

// Persistent device state (NVS namespace "rq").
namespace store {
void begin();
uint32_t nextBootId();             // increments and returns the boot counter
bool loadLocked();
void saveLocked(bool locked);
uint32_t loadLatchCycles();
void saveLatchCycles(uint32_t n);
bool loadSettings(DeviceSettings &s);
void saveSettings(const DeviceSettings &s);
}  // namespace store
