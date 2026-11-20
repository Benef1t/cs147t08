#include "flip_detector.h"

#include <math.h>

static const float kG = 9.80665f;
static const uint32_t kMaxDtMs = 200;  // cap so a stalled loop can't add a big jump

const char *faceName(Face f) {
  switch (f) {
    case Face::Up: return "up";
    case Face::Down: return "down";
    case Face::Edge: return "edge";
    default: return "unknown";
  }
}

void FlipDetector::clearAttempt() {
  st_.attempt = false;
  st_.paused = false;
  st_.complete = false;
  st_.activeMs = 0;
  st_.wallMs = 0;
  st_.flips = 0;
  st_.rejected = 0;
  st_.percent = 0;
}

void FlipDetector::reset() { clearAttempt(); }

const FlipStatus &FlipDetector::update(float ax, float ay, float az, uint32_t now) {
  st_.evFlip = st_.evStart = st_.evComplete = st_.evAbandon = st_.evReject = false;

  uint32_t dt = haveUpdate_ ? now - lastUpdateMs_ : 0;
  if (dt > kMaxDtMs) dt = kMaxDtMs;
  lastUpdateMs_ = now;
  haveUpdate_ = true;

  // ---- 1. Classify this sample ----
  const float a[3] = {ax, ay, az};
  const float mag = sqrtf(ax * ax + ay * ay + az * az);
  const float up = a[cfg_.upAxis < 3 ? cfg_.upAxis : 2] * (cfg_.invert ? -1.f : 1.f);

  st_.shaking = mag < 0.1f || fabsf(mag / kG - 1.f) > cfg_.shakeTolG;
  Face raw;
  if (st_.shaking) {
    raw = Face::Unknown;  // shaking resets the settle timer, so it can never count as a flip
  } else {
    const float c = up / mag;
    raw = c > cfg_.faceCos ? Face::Up : (c < -cfg_.faceCos ? Face::Down : Face::Edge);
  }

  // ---- 2. Debounce: orientation must hold for settleMs ----
  if (raw != candidate_) {
    candidate_ = raw;
    candidateSince_ = now;
  }
  const bool settled = now - candidateSince_ >= cfg_.settleMs;
  if (settled && candidate_ != st_.face) {
    st_.face = candidate_;
    const bool solid = candidate_ == Face::Up || candidate_ == Face::Down;
    if (solid) {
      if (lastSolid_ != Face::Unknown && candidate_ != lastSolid_ && !st_.complete) {
        if (st_.flips > 0 && now - lastFlipMs_ < cfg_.minFlipGapMs) {
          st_.rejected++;
          st_.evReject = true;
        } else {
          if (!st_.attempt) {
            clearAttempt();
            st_.attempt = true;
            st_.evStart = true;
            attemptStartMs_ = now;
          }
          st_.flips++;
          st_.evFlip = true;
          lastFlipMs_ = now;
        }
      }
      lastSolid_ = candidate_;
    }
  }

  // ---- 3. Time the attempt ----
  if (st_.attempt && !st_.complete) {
    const uint32_t sinceFlip = now - lastFlipMs_;
    st_.wallMs = now - attemptStartMs_;
    if (sinceFlip > cfg_.resetMs) {
      st_.evAbandon = true;
      st_.abandonActiveMs = st_.activeMs;
      st_.abandonWallMs = st_.wallMs;
      st_.abandonFlips = st_.flips;
      clearAttempt();
    } else if (sinceFlip <= cfg_.pauseMs) {
      st_.paused = false;
      if (!st_.evStart) st_.activeMs += dt;  // the starting flip itself adds no time
    } else {
      st_.paused = true;  // holding one orientation does not count
    }

    if (st_.attempt && st_.activeMs >= cfg_.requiredMs && st_.flips >= cfg_.minFlips) {
      st_.complete = true;
      st_.paused = false;
      st_.evComplete = true;
    }
  }

  // ---- 4. Progress for display ----
  if (st_.complete) {
    st_.percent = 100;
  } else if (st_.attempt) {
    uint32_t p = cfg_.requiredMs ? (uint64_t)st_.activeMs * 100 / cfg_.requiredMs : 100;
    if (p > 99) p = 99;  // 100% only once the flip count is also met
    st_.percent = (uint8_t)p;
  } else {
    st_.percent = 0;
  }
  return st_;
}
