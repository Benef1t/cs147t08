#pragma once
// Hardware and behaviour constants. Anything a user may want to change at
// runtime (alarm, thresholds) is in DeviceSettings and comes from the server.

// ---- Firmware version (bump for every OTA build; must increase) ----
#define FW_VERSION 1

// ---- Pins (esp32dev kit, same wiring as Lab 2 where possible) ----
#define PIN_SDA 21          // I2C shared by MPU-6050 and SSD1306
#define PIN_SCL 22
#define PIN_LED_RED 2       // locked
#define PIN_LED_YELLOW 4    // unlock attempt in progress
#define PIN_LED_GREEN 15    // unlocked
#define PIN_BUTTON 32       // to GND, uses internal pull-up; "phone stored" button
#define PIN_BUZZER 27       // passive buzzer
#define PIN_SERVO 13        // SG90 signal

// ---- I2C addresses ----
#define MPU_ADDR 0x68       // AD0 low
#define OLED_ADDR 0x3C

// ---- Sensor mounting: which MPU axis points out of the "face" being flipped ----
#define IMU_UP_AXIS 2       // 0 = X, 1 = Y, 2 = Z
#define IMU_UP_INVERT false // true if the chip is mounted upside down

// ---- Servo latch ----
#define SERVO_LOCK_DEG 0
#define SERVO_UNLOCK_DEG 90
#define SERVO_MIN_US 500
#define SERVO_MAX_US 2400
#define SERVO_MOVE_MS 600   // time to hold the pulse before cutting power
#define SERVO_WIGGLE_DEG 12 // extra back-and-forth on release to beat latch friction
#define SERVO_RAMP_DEG 3    // move this many degrees per ramp step...
#define SERVO_RAMP_MS 15    // ...every this many ms (90 deg in ~0.45 s; limits current spikes)

// ---- LEDC channels (servo and buzzer use different timers) ----
#define LEDC_SERVO_CH 0     // timer 0
#define LEDC_BUZZER_CH 4    // timer 2

// ---- Timing ----
#define IMU_PERIOD_MS 20          // 50 Hz sampling
#define DISPLAY_PERIOD_MS 100     // 10 fps OLED
#define LIVE_PERIOD_MS 250        // how often the live sample is refreshed for upload
#define UPLOAD_PERIOD_MS 1000     // live + queued records upload cadence
#define BUTTON_DEBOUNCE_MS 30
#define ALARM_MAX_MS (10UL * 60UL * 1000UL)   // alarm gives up after 10 minutes
#define WDT_TIMEOUT_S 30

// ---- Offline record queue ----
#define QUEUE_CAPACITY 150
#define UPLOAD_BATCH 15

// ---- NTP ----
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"
