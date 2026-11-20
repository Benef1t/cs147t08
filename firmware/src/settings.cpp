#include "settings.h"

#include <Preferences.h>

#include "config.h"

static Preferences prefs;

FlipConfig DeviceSettings::flipConfig() const {
  FlipConfig c;
  c.upAxis = IMU_UP_AXIS;
  c.invert = IMU_UP_INVERT;
  c.requiredMs = (uint32_t)(requiredS < 10 ? 10 : requiredS) * 1000;  // never below 10 s
  c.pauseMs = pauseMs;
  c.resetMs = pauseMs + 3500;
  c.minFlips = minFlips;
  c.faceCos = faceCosPct / 100.f;
  return c;
}

namespace store {

void begin() { prefs.begin("rq", false); }

uint32_t nextBootId() {
  uint32_t id = prefs.getUInt("boot", 0) + 1;
  prefs.putUInt("boot", id);
  return id;
}

bool loadLocked() { return prefs.getBool("locked", false); }
void saveLocked(bool locked) { prefs.putBool("locked", locked); }

uint32_t loadLatchCycles() { return prefs.getUInt("cycles", 0); }
void saveLatchCycles(uint32_t n) { prefs.putUInt("cycles", n); }

bool loadSettings(DeviceSettings &s) {
  // isKey() first: getBytesLength() logs an error on a fresh board with no settings yet.
  if (!prefs.isKey("settings") || prefs.getBytesLength("settings") != sizeof(DeviceSettings)) return false;
  DeviceSettings tmp;
  prefs.getBytes("settings", &tmp, sizeof(tmp));
  tmp.tz[sizeof(tmp.tz) - 1] = 0;
  s = tmp;
  return true;
}

void saveSettings(const DeviceSettings &s) { prefs.putBytes("settings", &s, sizeof(s)); }

}  // namespace store
