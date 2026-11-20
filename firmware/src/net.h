#pragma once
// Networking runs in its own FreeRTOS task on core 0 so that Wi-Fi drops,
// slow HTTP calls or a dead server never block sensing on core 1.
//
// Records (events + heartbeats) go into a RAM ring buffer and are uploaded
// in order with retries. Each record carries (boot_id, seq) so the server can
// detect gaps; overflow drops the oldest record and is counted.
#include <Arduino.h>
#include <HTTPClient.h>

#include "flip_detector.h"
#include "settings.h"

struct LiveSample {
  float ax = 0, ay = 0, az = 0;  // m/s^2
  float tilt = 0, pitch = 0, roll = 0;  // degrees
  Face face = Face::Unknown;
  uint8_t percent = 0;
  uint32_t activeMs = 0;
  uint16_t flips = 0;
  bool attempt = false, paused = false, shaking = false;
  bool locked = false, alarm = false, imuOk = false;
  uint32_t latchCycles = 0;
};

namespace net {
void begin(uint32_t bootId, const DeviceSettings &s);

// ---- called from the main loop ----
void setLive(const LiveSample &s);
void event(const char *kind, const char *jsonData = "{}");
void applySettings(const DeviceSettings &s);  // tz / heartbeat interval
bool takeNewSettings(DeviceSettings &out);    // settings pushed from the server
bool takeRemoteRelease(int &commandId);      // emergency release from the dashboard
void setOtaAllowed(bool allowed);             // only update while unlocked and idle

bool wifiUp();
bool serverOk();
bool timeValid();
uint16_t queued();

// ---- shared with ota.cpp ----
void signRequest(HTTPClient &http, const char *method, const String &path, const String &body);
}  // namespace net
