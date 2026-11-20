#pragma once
// Detects repeated face-up / face-down flips and times the 10-second task.
// Pure C++ (no Arduino headers) so it can be unit-tested on a laptop:
//   see firmware/host_tests/flip_detector_test.cpp
#include <stdint.h>

enum class Face : uint8_t { Unknown = 0, Up, Down, Edge };

const char *faceName(Face f);

struct FlipConfig {
  uint8_t upAxis = 2;          // 0=x 1=y 2=z
  bool invert = false;
  float faceCos = 0.80f;       // up-axis must be within ~37 deg of vertical
  float shakeTolG = 0.35f;     // sample rejected if | |a| - 1 g | > this
  uint32_t settleMs = 150;     // orientation must be stable this long to count
  uint32_t minFlipGapMs = 250; // flips closer together than this are rejected
  uint32_t pauseMs = 2500;     // no flip for this long -> progress pauses
  uint32_t resetMs = 6000;     // no flip for this long -> attempt abandoned
  uint32_t requiredMs = 10000; // active flipping time needed
  uint16_t minFlips = 6;       // and at least this many flips
};

struct FlipStatus {
  Face face = Face::Unknown;   // debounced orientation
  bool shaking = false;        // current sample rejected as shake
  bool attempt = false;        // an attempt is in progress
  bool paused = false;         // attempt in progress but user stopped flipping
  bool complete = false;       // task done (sticky until reset())
  uint32_t activeMs = 0;       // time counted toward requiredMs
  uint32_t wallMs = 0;         // time since the attempt's first flip
  uint16_t flips = 0;
  uint16_t rejected = 0;       // flips rejected as too fast
  uint8_t percent = 0;
  // One-shot events, true only on the update where they happen.
  bool evFlip = false, evStart = false, evComplete = false, evAbandon = false, evReject = false;
  // Snapshot of the attempt that was just abandoned (valid when evAbandon).
  uint32_t abandonActiveMs = 0, abandonWallMs = 0;
  uint16_t abandonFlips = 0;
};

class FlipDetector {
 public:
  explicit FlipDetector(const FlipConfig &cfg = FlipConfig()) : cfg_(cfg) {}
  void setConfig(const FlipConfig &cfg) { cfg_ = cfg; }
  const FlipConfig &config() const { return cfg_; }

  // Clear the attempt (keeps the current orientation).
  void reset();
  // Accelerations in m/s^2, nowMs = monotonic milliseconds.
  const FlipStatus &update(float ax, float ay, float az, uint32_t nowMs);
  const FlipStatus &status() const { return st_; }

 private:
  void clearAttempt();

  FlipConfig cfg_;
  FlipStatus st_;
  Face candidate_ = Face::Unknown;
  uint32_t candidateSince_ = 0;
  Face lastSolid_ = Face::Unknown;  // last settled Up/Down
  uint32_t lastFlipMs_ = 0;
  uint32_t attemptStartMs_ = 0;
  uint32_t lastUpdateMs_ = 0;
  bool haveUpdate_ = false;
};
