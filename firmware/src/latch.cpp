#include "latch.h"

#include "config.h"
#include "settings.h"

static const uint32_t kPeriodUs = 20000;  // 50 Hz
static const uint32_t kMaxDuty = (1 << 16) - 1;
static const uint32_t kWiggleStepMs = 180;

void Latch::begin(bool locked) {
  ledcSetup(LEDC_SERVO_CH, 50, 16);
  ledcAttachPin(PIN_SERVO, LEDC_SERVO_CH);
  ledcWrite(LEDC_SERVO_CH, 0);
  cycles_ = store::loadLatchCycles();
  locked_ = locked;
  // The SG90 holds its position while unpowered, so it is still where it was
  // before the reboot. Don't drive it here: a servo start-up current spike at
  // boot on weak USB power can reset the board again and cause a boot loop.
  curDeg_ = locked ? SERVO_LOCK_DEG : SERVO_UNLOCK_DEG;
}

void Latch::writeDeg(int deg) {
  deg = constrain(deg, 0, 180);
  uint32_t us = map(deg, 0, 180, SERVO_MIN_US, SERVO_MAX_US);
  ledcWrite(LEDC_SERVO_CH, (uint32_t)((uint64_t)us * kMaxDuty / kPeriodUs));
}

void Latch::startSequence(const int *degs, int n) {
  stepCount_ = min(n, 6);
  for (int i = 0; i < stepCount_; i++) steps_[i] = degs[i];
  stepIdx_ = 0;
  lastRampMs_ = 0;
  reached_ = false;
}

void Latch::lock() {
  locked_ = true;
  store::saveLocked(true);
  int deg = SERVO_LOCK_DEG;
  startSequence(&deg, 1);
}

void Latch::unlock() {
  locked_ = false;
  store::saveLocked(false);
  cycles_++;
  store::saveLatchCycles(cycles_);
  // Direction from "open" back toward "locked" (always inside 0..180).
  const int back = SERVO_UNLOCK_DEG < SERVO_LOCK_DEG ? 1 : -1;
  const int u = SERVO_UNLOCK_DEG;
  // Push open, ease off, push again: re-tries a latch stuck by friction.
  int seq[] = {u, u + back * SERVO_WIGGLE_DEG, u, u + back * SERVO_WIGGLE_DEG, u};
  startSequence(seq, 5);
}

void Latch::update(uint32_t now) {
  if (stepCount_ == 0) return;
  const int target = steps_[stepIdx_];

  // Ramp toward the target a few degrees at a time. Small steps draw much less
  // peak current than one big jump, so USB power doesn't sag and reset the ESP32.
  if (!reached_) {
    if (now - lastRampMs_ < SERVO_RAMP_MS) return;
    lastRampMs_ = now;
    const int diff = target - curDeg_;
    const int step = min(abs(diff), SERVO_RAMP_DEG);
    curDeg_ += diff > 0 ? step : -step;
    writeDeg(curDeg_);
    if (curDeg_ == target) {
      reached_ = true;
      stepStart_ = now;
    }
    return;
  }

  const bool last = stepIdx_ == stepCount_ - 1;
  const uint32_t hold = last ? SERVO_MOVE_MS : kWiggleStepMs;
  if (now - stepStart_ < hold) return;
  if (last) {
    ledcWrite(LEDC_SERVO_CH, 0);  // stop pulses: no jitter, no holding current
    stepCount_ = 0;
    return;
  }
  stepIdx_++;
  reached_ = false;
}
