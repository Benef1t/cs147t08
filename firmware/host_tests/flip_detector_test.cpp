// Laptop-side tests for the flip detector (orientation thresholds,
// debouncing, pause handling, shake rejection and the ten-second rule).
//   c++ -std=c++17 -I../src flip_detector_test.cpp ../src/flip_detector.cpp -o t && ./t
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "flip_detector.h"

static int failures = 0;
#define CHECK(cond, msg)                                   \
  do {                                                     \
    if (!(cond)) {                                         \
      printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      failures++;                                          \
    }                                                      \
  } while (0)

static const float G = 9.80665f;

struct Sim {
  FlipDetector det;
  uint32_t t = 0;
  int starts = 0, completes = 0, abandons = 0, flips = 0;
  uint32_t completeAt = 0;
  void step(float ax, float ay, float az, uint32_t ms) {
    for (uint32_t e = 0; e < ms; e += 20) {
      const FlipStatus &s = det.update(ax, ay, az, t);
      starts += s.evStart;
      flips += s.evFlip;
      abandons += s.evAbandon;
      if (s.evComplete) {
        completes++;
        completeAt = t;
      }
      t += 20;
    }
  }
  void up(uint32_t ms) { step(0, 0, G, ms); }
  void down(uint32_t ms) { step(0, 0, -G, ms); }
  void edge(uint32_t ms) { step(G, 0, 0, ms); }
  // Flip every `period` ms for `total` ms, starting from face-up.
  void flipFor(uint32_t period, uint32_t total) {
    bool u = det.status().face != Face::Up;
    for (uint32_t e = 0; e < total; e += period) {
      edge(60);  // brief edge-on transition, like a real flip
      if (u) up(period - 60); else down(period - 60);
      u = !u;
    }
  }
};

static void testHoldDoesNotCount() {
  printf("hold one orientation\n");
  Sim s;
  s.up(20000);
  CHECK(s.starts == 0, "no attempt from holding face-up");
  s.down(20000);
  CHECK(s.flips == 1, "one flip counted");
  CHECK(!s.det.status().complete, "holding face-down after one flip does not complete");
  CHECK(s.abandons == 1, "single flip then hold is abandoned");
}

static void testTenSecondRule() {
  printf("steady flipping completes at ~10 s\n");
  Sim s;
  s.up(1000);
  uint32_t start = s.t;
  s.flipFor(800, 15000);
  CHECK(s.completes == 1, "completes exactly once");
  uint32_t took = s.completeAt - start;
  printf("  completed after %.2f s of flipping\n", took / 1000.0);
  CHECK(took >= 10000 && took <= 11500, "completes between 10 and 11.5 s");
}

static void testNotBeforeTen() {
  printf("9 s of flipping is not enough\n");
  Sim s;
  s.up(1000);
  s.flipFor(700, 9000);
  CHECK(s.completes == 0, "not complete before 10 s");
  CHECK(s.det.status().attempt, "attempt still in progress");
}

static void testPauseAndResume() {
  printf("pause then resume keeps progress\n");
  Sim s;
  s.up(1000);
  s.flipFor(800, 6400);
  uint32_t before = s.det.status().activeMs;
  s.up(4500);  // pause (longer than pauseMs, shorter than resetMs)
  CHECK(s.det.status().paused || s.det.status().face == Face::Up, "paused while holding");
  uint32_t during = s.det.status().activeMs;
  CHECK(during - before <= 2600, "at most pauseMs counted while holding");
  CHECK(s.det.status().attempt, "attempt kept through short pause");
  s.flipFor(800, 8000);
  CHECK(s.completes == 1, "completes after resuming");
}

static void testLongPauseResets() {
  printf("long pause abandons attempt\n");
  Sim s;
  s.up(1000);
  s.flipFor(800, 6400);
  s.down(8000);
  s.up(8000);
  CHECK(s.abandons >= 1, "abandoned");
  CHECK(s.completes == 0, "never completed");
}

static void testShakeRejected() {
  printf("violent shaking is not counted\n");
  Sim s;
  s.up(1000);
  srand(42);
  for (int i = 0; i < 750; i++) {  // 15 s of shaking at 50 Hz
    float k = (i % 2 ? 1 : -1) * (1.6f + (rand() % 100) / 100.f);
    s.step(0.3f * G, 0.2f * G, k * G, 20);
  }
  CHECK(s.flips == 0, "no flips counted from shaking");
  CHECK(s.completes == 0, "shaking never completes");
}

static void testJitterDebounced() {
  printf("short wobbles are debounced\n");
  Sim s;
  s.up(1000);
  for (int i = 0; i < 50; i++) {
    s.down(100);  // shorter than settleMs
    s.up(300);
  }
  CHECK(s.flips == 0, "100 ms wobbles never count");
}

static void testSlowFlipsNeedMinFlips() {
  printf("slow flips still need the minimum flip count\n");
  Sim s;
  s.up(1000);
  s.flipFor(2400, 30000);
  CHECK(s.completes == 1, "slow but continuous flipping completes");
  CHECK(s.det.status().flips >= s.det.config().minFlips, "min flips reached");
}

static void testResetClears() {
  printf("reset clears a completed attempt\n");
  Sim s;
  s.up(1000);
  s.flipFor(800, 12000);
  CHECK(s.det.status().complete, "complete");
  s.det.reset();
  CHECK(!s.det.status().complete && s.det.status().percent == 0, "cleared");
}

int main() {
  testHoldDoesNotCount();
  testTenSecondRule();
  testNotBeforeTen();
  testPauseAndResume();
  testLongPauseResets();
  testShakeRejected();
  testJitterDebounced();
  testSlowFlipsNeedMinFlips();
  testResetClears();
  if (failures) {
    printf("\n%d check(s) failed\n", failures);
    return 1;
  }
  printf("\nall flip detector tests passed\n");
  return 0;
}
