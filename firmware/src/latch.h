#pragma once
// SG90 servo latch driven directly by LEDC (50 Hz). Moves are non-blocking;
// the pulse is cut after each move so the servo does not jitter or draw
// holding current.
#include <Arduino.h>

class Latch {
 public:
  void begin(bool locked);
  void lock();
  void unlock();  // includes a small wiggle to free a sticky cardboard latch
  void update(uint32_t now);
  bool locked() const { return locked_; }
  bool moving() const { return stepCount_ > 0; }
  uint32_t cycles() const { return cycles_; }

 private:
  void writeDeg(int deg);
  void startSequence(const int *degs, int n);
  bool locked_ = false;
  uint32_t cycles_ = 0;
  int steps_[6];
  int stepCount_ = 0, stepIdx_ = 0;
  uint32_t stepStart_ = 0, lastRampMs_ = 0;
  int curDeg_ = 0;       // last commanded angle
  bool reached_ = false; // current step's target reached (now holding)
};
