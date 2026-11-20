# RiseQuest: a motion-unlocked phone storage box

CS147 Fall 2026, Group 8 (Liu, Suwandy, Wu).

Store your phone in the box, close the lid and press the button: the SG90 servo
locks the latch. To get the phone back, flip the external MPU-6050 face-up /
face-down repeatedly for **10 seconds**. Holding one orientation, wobbling or
shaking does not count. The servo then releases and you open the lid by hand.
The OLED, three LEDs and the buzzer give feedback, and an optional scheduled
alarm rings until you complete the task. Everything works locally. The laptop
server only logs data and serves the dashboard.

```
MPU-6050 ──I2C──┐                       Wi-Fi (HMAC-signed HTTP)
SSD1306  ──I2C──┤                     ┌──────────────────────────┐
button, LEDs,   ├── ESP32 ────────────┤ server/app.py (laptop)    │── web dashboard
buzzer, SG90 ───┘   core 1: sensing,  │ Flask + SQLite            │   (login required)
                    latch, UI, alarm  └──────────────────────────┘
                    core 0: Wi-Fi, NTP, queue, uploads, config, signed OTA
```

## Layout

| Path | What |
|---|---|
| `firmware/` | PlatformIO project (esp32dev, Arduino, espressif32@6.9.0) |
| `firmware/src/flip_detector.*` | Orientation + flip counting + 10 s rule (pure C++, unit-tested) |
| `firmware/src/main.cpp` | State machine: OPEN → (button) → LOCKED → (flips) → UNLOCKED |
| `firmware/src/net.*` | Wi-Fi supervision, NTP, offline ring buffer, uploads with backoff, config, remote release |
| `firmware/src/ota.*` | ECDSA-signed firmware updates |
| `firmware/src/feedback.*` | OLED screens/animations, LEDs, buzzer |
| `firmware/src/latch.*`, `imu.*`, `settings.*` | Servo, MPU-6050 driver, NVS persistence |
| `firmware/host_tests/` | Laptop tests for the flip detector |
| `server/` | Flask server, SQLite database, dashboard |
| `tools/` | Simulated device, OTA key generation and signing, battery-life calculator |

## Wiring (matches Lab 2 where possible; change in `firmware/include/config.h`)

| Part | ESP32 pin |
|---|---|
| MPU-6050 SDA / SCL (shared with OLED) | 21 / 22, 3V3, GND |
| SSD1306 OLED (0x3C) | 21 / 22 |
| Red LED (locked) | 2 |
| Yellow LED (attempt in progress, blinks when paused) | 4 |
| Green LED (unlocked) | 15 |
| Button (to GND, internal pull-up) | 32 |
| Passive buzzer | 27 |
| SG90 signal | 13 (power the servo from 5V/VIN, common GND) |

The flip axis defaults to the MPU's Z axis (`IMU_UP_AXIS`). If your sensor is
mounted differently, change `IMU_UP_AXIS` / `IMU_UP_INVERT`. Calibrate
`SERVO_LOCK_DEG` / `SERVO_UNLOCK_DEG` to your cardboard latch.

## Setup

### 1. Server (team laptop)

```bash
cd server
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
cp .env.example .env
```

Fill in `server/.env`:
- `RQ_DEVICE_SECRET`: `python3 -c "import secrets; print(secrets.token_hex(32))"`
- `RQ_ADMIN_PASSWORD_HASH`: `.venv/bin/python app.py hash-password`
- `RQ_SECRET_KEY`: another random string

Then run it and open `http://<laptop-ip>:8080`:

```bash
.venv/bin/python app.py
```

### 2. Firmware

```bash
cd firmware
cp include/secrets.example.h include/secrets.h   # Wi-Fi, server URL, same device secret
pio run -t upload && pio device monitor
```

### 3. Signed OTA updates (optional)

```bash
python tools/gen_ota_keys.py          # once: keys/ota_private.pem + firmware/include/ota_pubkey.h
# flash once over USB so the device has the public key, then for each update:
# bump FW_VERSION in firmware/include/config.h
cd firmware && pio run && cd ..
python tools/sign_firmware.py         # publishes server/firmware/{firmware.bin,manifest.json}
```

The device checks for updates only while it is **unlocked and idle**. It
verifies the ECDSA P-256 signature (which covers both the version and the
image's SHA-256), streams the image into the spare slot, and only makes that
slot bootable if the hash matches. It never downgrades.

## How the proposal maps to the code

| Proposal | Where |
|---|---|
| Face up/down from gravity; alternating flips for ≥10 s; holding doesn't count | `flip_detector.cpp`: settled orientation, `pauseMs`, `requiredMs`, `minFlips` |
| False counts from shaking | Samples with \|a\| far from 1 g are rejected; orientation must hold 150 ms; flips < 250 ms apart rejected |
| Pause handling | Progress freezes after 2.5 s without a flip; the attempt is abandoned after 6 s |
| Latch friction | Release does push / ease off / push; unlock cycles counted in NVS and shown on the dashboard |
| Button confirms storage | `onButton()` in `main.cpp` |
| OLED animation, 3 LEDs, buzzer | `feedback.cpp` |
| Scheduled alarm | `pollAlarm()`; set from the dashboard; stops on unlock, button, or after 10 min |
| Live acceleration, tilt, progress, commanded latch | `/api/live` → dashboard top row + 60 s chart |
| Historical attempts, completions, heartbeats | `/api/history`, `/api/events`, heartbeats every 60 s |
| Credentials outside Git | `secrets.h`, `.env`, `keys/` are git-ignored |
| Uploads and settings need authentication | Device: HMAC-SHA256 per request. Dashboard: password login, session cookie, CSRF token |
| Firmware updates need signature verification | `ota.cpp`, `tools/sign_firmware.py` |
| Keep sensing through outages, queue, retry | Networking runs on core 0. 150-record RAM queue, exponential backoff up to 30 s, idempotent `(boot_id, seq)` ack |
| Retain settings, resync time after reboot | NVS (settings, lock state, boot counter). NTP, falling back to server time; queued timestamps back-filled |
| Recover without manual intervention | Task watchdog (30 s) reboots a hung loop. Wi-Fi re-joined every 15 s. Lock state restored on boot |
| Logs explain resets, gaps, overflows | Dashboard "Reliability by boot": reset reason, expected vs received records, missing seq ranges, queue overflow, upload outages, heartbeat gaps |
| Current draw & battery life | `tools/battery_life.py` (fill in your measured mA) |

**Added beyond the proposal:** an emergency "Release latch" button on the
dashboard (login required, logged as `remote_release`), so a failed sensor
can't trap the phone.

## Testing

```bash
# flip detector (thresholds, debouncing, pause, shake, ten-second rule)
cd firmware/host_tests
c++ -std=c++17 -I../src flip_detector_test.cpp ../src/flip_detector.cpp -o /tmp/flip_test && /tmp/flip_test

# server + dashboard without hardware (simulates a 12 s outage)
server/.venv/bin/python tools/fake_device.py --secret <RQ_DEVICE_SECRET> --seconds 60 --outage 18:30 --heartbeat 5
```

### 72-hour run checklist
1. Power from USB, lock the box, and leave it running with the dashboard open.
2. During the run, turn the laptop's server off for 10+ minutes and toggle the router's Wi-Fi a few times.
3. Unplug the ESP32 once to force a reboot. It should come back locked, with settings kept and the clock resynced.
4. Afterwards, check "Reliability by boot". Missing should be 0 unless queue overflow is shown (outage longer than ~150 records ≈ 2.5 h of heartbeats). Every boot should list its reset reason, and every outage should appear as an upload gap.
5. Measure current in each mode, put the numbers in `tools/battery_life.py`, and compare against the budget.

## Assumptions to check
- Pins follow your Lab 2 wiring plus servo on 13 and button on 32. Change them in `config.h` if your board differs.
- Default timezone is Pacific (`PST8PDT,M3.2.0,M11.1.0`), editable on the dashboard.
- Device-to-server traffic is plain HTTP on the LAN. Requests are signed (tamper-proof) but not encrypted.
