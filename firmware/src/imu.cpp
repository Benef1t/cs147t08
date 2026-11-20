#include "imu.h"

#include <math.h>

static const float kLsbPerG = 8192.f;  // +/-4 g range
static const float kG = 9.80665f;

bool Imu::writeReg(uint8_t reg, uint8_t val) {
  wire_->beginTransmission(addr_);
  wire_->write(reg);
  wire_->write(val);
  return wire_->endTransmission() == 0;
}

bool Imu::begin(TwoWire &wire, uint8_t addr) {
  wire_ = &wire;
  addr_ = addr;
  errors_ = 0;
  ok_ = false;

  wire_->beginTransmission(addr_);
  wire_->write(0x75);  // WHO_AM_I
  if (wire_->endTransmission(false) != 0 || wire_->requestFrom(addr_, (uint8_t)1) != 1) return false;
  who_ = wire_->read();  // 0x68 on genuine parts; clones report other values but work

  ok_ = writeReg(0x6B, 0x01)     // wake up, PLL with X gyro clock
        && writeReg(0x1A, 0x03)  // DLPF ~44 Hz: smooths vibration
        && writeReg(0x19, 0x13)  // sample rate 50 Hz
        && writeReg(0x1C, 0x08); // accel +/-4 g (headroom to detect shaking)
  delay(20);
  return ok_;
}

bool Imu::read(ImuSample &o, uint8_t upAxis, bool invert) {
  if (!wire_) return false;
  wire_->beginTransmission(addr_);
  wire_->write(0x3B);  // ACCEL_XOUT_H
  if (wire_->endTransmission(false) != 0 || wire_->requestFrom(addr_, (uint8_t)6) != 6) {
    if (++errors_ >= 5) ok_ = false;  // a few glitches are tolerated; persistent failure is reported
    return false;
  }
  int16_t raw[3];
  for (int i = 0; i < 3; i++) raw[i] = (int16_t)((wire_->read() << 8) | wire_->read());
  errors_ = 0;
  ok_ = true;

  o.ax = raw[0] / kLsbPerG * kG;
  o.ay = raw[1] / kLsbPerG * kG;
  o.az = raw[2] / kLsbPerG * kG;

  const float a[3] = {o.ax, o.ay, o.az};
  const float mag = sqrtf(o.ax * o.ax + o.ay * o.ay + o.az * o.az);
  float up = a[upAxis < 3 ? upAxis : 2] * (invert ? -1.f : 1.f);
  float c = mag > 0.1f ? up / mag : 1.f;
  if (c > 1) c = 1;
  if (c < -1) c = -1;
  o.tiltDeg = acosf(c) * 180.f / PI;
  o.pitchDeg = atan2f(-o.ax, sqrtf(o.ay * o.ay + o.az * o.az)) * 180.f / PI;
  o.rollDeg = atan2f(o.ay, o.az) * 180.f / PI;
  return true;
}
