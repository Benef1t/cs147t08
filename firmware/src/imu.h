#pragma once
// Minimal MPU-6050 driver using Wire only (accelerometer, +/-4 g).
#include <Arduino.h>
#include <Wire.h>

struct ImuSample {
  float ax = 0, ay = 0, az = 0;  // m/s^2
  float tiltDeg = 0;             // angle of the up-axis from vertical (0 = face up, 180 = face down)
  float pitchDeg = 0, rollDeg = 0;
};

class Imu {
 public:
  bool begin(TwoWire &wire, uint8_t addr);
  bool read(ImuSample &out, uint8_t upAxis, bool invert);
  bool ok() const { return ok_; }
  uint8_t whoAmI() const { return who_; }

 private:
  bool writeReg(uint8_t reg, uint8_t val);
  TwoWire *wire_ = nullptr;
  uint8_t addr_ = 0x68;
  uint8_t who_ = 0;
  bool ok_ = false;
  uint8_t errors_ = 0;
};
