#include "net.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <sys/time.h>
#include <time.h>

#include "config.h"
#include "mbedtls/md.h"
#include "ota.h"
#include "secrets.h"

namespace net {

// ---------------- shared state (guarded by mtx) ----------------
struct Record {
  uint32_t seq;
  uint32_t upMs;
  uint32_t epoch;  // 0 = wall time unknown when recorded
  char kind[20];
  char data[100];  // pre-serialised JSON object
};

static SemaphoreHandle_t mtx;
static Record queue[QUEUE_CAPACITY];
static uint16_t qHead = 0, qCount = 0;
static uint32_t nextSeq = 1;
static uint32_t overflowCount = 0;

static LiveSample live;
static bool liveFresh = false;
static DeviceSettings settings;
static bool settingsChanged = false;  // main -> net (tz)
static DeviceSettings pending;
static bool pendingReady = false;     // net -> main
static bool releaseReady = false;
static int releaseCommandId = -1;
static volatile bool otaAllowed = false;

static volatile bool sWifi = false, sServer = false, sTime = false;
static uint32_t bootId = 0;
static uint32_t nonceCounter = 0;

// stats (net task only)
static uint32_t uploadFailures = 0, wifiConnects = 0, uploadsOk = 0;
static int32_t lastCommandId = -1;

struct Lock {
  Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mtx); }
};

static bool clockValid() { return time(nullptr) > 1700000000; }

// ---------------- API for the main loop ----------------
static void pushRecord(const char *kind, const char *data) {
  Lock l;
  if (qCount == QUEUE_CAPACITY) {  // drop oldest, remember that we did
    qHead = (qHead + 1) % QUEUE_CAPACITY;
    qCount--;
    overflowCount++;
  }
  Record &r = queue[(qHead + qCount) % QUEUE_CAPACITY];
  r.seq = nextSeq++;
  r.upMs = millis();
  r.epoch = clockValid() ? (uint32_t)time(nullptr) : 0;
  strlcpy(r.kind, kind, sizeof r.kind);
  strlcpy(r.data, data && *data ? data : "{}", sizeof r.data);
  qCount++;
}

void event(const char *kind, const char *jsonData) {
  pushRecord(kind, jsonData);
  Serial.printf("[event] %s %s\n", kind, jsonData);
}

void setLive(const LiveSample &s) {
  Lock l;
  live = s;
  liveFresh = true;
}

void applySettings(const DeviceSettings &s) {
  Lock l;
  settings = s;
  settingsChanged = true;
}

bool takeNewSettings(DeviceSettings &out) {
  Lock l;
  if (!pendingReady) return false;
  out = pending;
  pendingReady = false;
  return true;
}

bool takeRemoteRelease(int &commandId) {
  Lock l;
  bool r = releaseReady;
  releaseReady = false;
  commandId = releaseCommandId;
  return r;
}

void setOtaAllowed(bool a) { otaAllowed = a; }
bool wifiUp() { return sWifi; }
bool serverOk() { return sServer; }
bool timeValid() { return sTime; }
uint16_t queued() {
  Lock l;
  return qCount;
}

// ---------------- request signing ----------------
static String hmacHex(const String &msg) {
  uint8_t out[32];
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info, (const uint8_t *)DEVICE_SECRET, strlen(DEVICE_SECRET), (const uint8_t *)msg.c_str(),
                  msg.length(), out);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", out[i]);
  return String(hex);
}

void signRequest(HTTPClient &http, const char *method, const String &path, const String &body) {
  String nonce = String(bootId) + "-" + String(++nonceCounter);
  String msg = String(method) + "\n" + path + "\n" + nonce + "\n" + body;
  http.addHeader("X-RQ-Device", DEVICE_ID);
  http.addHeader("X-RQ-Nonce", nonce);
  http.addHeader("X-RQ-Signature", hmacHex(msg));
}

// ---------------- net task internals ----------------
static void heartbeat() {
  LiveSample l;
  uint32_t overflow;
  uint16_t depth;
  {
    Lock k;
    l = live;
    overflow = overflowCount;
    depth = qCount;
  }
  char buf[100];
  snprintf(buf, sizeof buf,
           "{\"up\":%lu,\"heap\":%u,\"rssi\":%d,\"q\":%u,\"ovf\":%lu,\"fail\":%lu,\"wc\":%lu,\"lk\":%d,\"imu\":%d}",
           (unsigned long)(millis() / 1000), ESP.getFreeHeap(), sWifi ? (int)WiFi.RSSI() : 0, depth,
           (unsigned long)overflow, (unsigned long)uploadFailures, (unsigned long)wifiConnects, l.locked, l.imuOk);
  pushRecord("heartbeat", buf);
}

static void fetchConfig() {
  HTTPClient http;
  const String path = "/api/device/config";
  http.begin(String(SERVER_URL) + path);
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  signRequest(http, "GET", path, "");
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      DeviceSettings s;
      {
        Lock k;
        s = settings;
      }
      s.version = doc["version"] | s.version;
      s.alarmEnabled = doc["alarm_enabled"] | s.alarmEnabled;
      s.alarmHour = doc["alarm_hour"] | s.alarmHour;
      s.alarmMinute = doc["alarm_minute"] | s.alarmMinute;
      strlcpy(s.tz, doc["timezone"] | s.tz, sizeof s.tz);
      s.requiredS = doc["required_s"] | s.requiredS;
      s.pauseMs = doc["pause_ms"] | s.pauseMs;
      s.minFlips = doc["min_flips"] | s.minFlips;
      s.faceCosPct = doc["face_cos_pct"] | s.faceCosPct;
      s.heartbeatS = doc["heartbeat_s"] | s.heartbeatS;
      Lock k;
      pending = s;
      pendingReady = true;
    }
  } else {
    Serial.printf("[net] config fetch failed: %d\n", code);
  }
  http.end();
}

// Builds and sends one upload. Returns true if the server accepted it.
static Record batch[UPLOAD_BATCH];

static bool upload() {
  uint16_t n;
  LiveSample l;
  bool haveLive;
  uint32_t cfgVersion;
  {
    Lock k;
    n = min<uint16_t>(qCount, UPLOAD_BATCH);
    for (uint16_t i = 0; i < n; i++) batch[i] = queue[(qHead + i) % QUEUE_CAPACITY];
    l = live;
    haveLive = liveFresh;
    cfgVersion = settings.version;
  }

  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["boot_id"] = bootId;
  doc["fw"] = FW_VERSION;
  doc["time_valid"] = (bool)sTime;
  doc["config_version"] = cfgVersion;
  if (haveLive) {
    JsonObject o = doc["live"].to<JsonObject>();
    o["ax"] = serialized(String(l.ax, 2));
    o["ay"] = serialized(String(l.ay, 2));
    o["az"] = serialized(String(l.az, 2));
    o["tilt"] = serialized(String(l.tilt, 1));
    o["pitch"] = serialized(String(l.pitch, 1));
    o["roll"] = serialized(String(l.roll, 1));
    o["face"] = faceName(l.face);
    o["progress"] = l.percent;
    o["active_ms"] = l.activeMs;
    o["flips"] = l.flips;
    o["attempt"] = l.attempt;
    o["paused"] = l.paused;
    o["shaking"] = l.shaking;
    o["latch"] = l.locked ? "locked" : "unlocked";
    o["alarm"] = l.alarm;
    o["imu_ok"] = l.imuOk;
    o["latch_cycles"] = l.latchCycles;
    o["queued"] = qCount;
    o["rssi"] = WiFi.RSSI();
  }
  JsonArray recs = doc["records"].to<JsonArray>();
  const uint32_t nowEpoch = sTime ? (uint32_t)time(nullptr) : 0;
  const uint32_t nowMs = millis();
  for (uint16_t i = 0; i < n; i++) {
    const Record &r = batch[i];
    JsonObject o = recs.add<JsonObject>();
    o["seq"] = r.seq;
    o["kind"] = r.kind;
    o["up_ms"] = r.upMs;
    // Back-fill wall time for records queued before the clock was synced.
    uint32_t t = r.epoch ? r.epoch : (nowEpoch ? nowEpoch - (nowMs - r.upMs) / 1000 : 0);
    if (t) o["t"] = t;
    else o["t"] = nullptr;
    o["data"] = serialized(r.data);
  }
  String body;
  serializeJson(doc, body);

  HTTPClient http;
  const String path = "/api/device/ingest";
  http.begin(String(SERVER_URL) + path);
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  signRequest(http, "POST", path, body);
  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("[net] upload failed: %d\n", code);
    http.end();
    return false;
  }
  JsonDocument resp;
  DeserializationError err = deserializeJson(resp, http.getString());
  http.end();
  if (err) return false;

  // Drop acknowledged records (the server stores them idempotently).
  uint32_t ack = resp["ack_seq"] | 0;
  {
    Lock k;
    if (haveLive) liveFresh = false;
    while (qCount && queue[qHead].seq <= ack) {
      qHead = (qHead + 1) % QUEUE_CAPACITY;
      qCount--;
    }
  }

  // Time fallback: use the server clock until NTP answers.
  uint32_t serverTime = resp["server_time"] | 0;
  if (!clockValid() && serverTime > 1700000000) {
    timeval tv = {(time_t)serverTime, 0};
    settimeofday(&tv, nullptr);
  }

  if ((uint32_t)(resp["config_version"] | 0) != cfgVersion) fetchConfig();

  // Emergency release requested from the dashboard (each command runs once).
  int cmdId = resp["command_id"] | -1;
  const char *cmd = resp["command"] | "";
  if (cmdId >= 0 && cmdId != lastCommandId && strcmp(cmd, "release") == 0) {
    lastCommandId = cmdId;
    Lock k;
    releaseReady = true;
    releaseCommandId = cmdId;
  }

  int latestFw = resp["fw_latest"] | 0;
  if (latestFw > FW_VERSION && otaAllowed) ota::checkAndInstall();
  return true;
}

static void netTask(void *) {
  esp_task_wdt_add(nullptr);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t lastWifiTry = millis(), lastUpload = 0, lastBeat = 0;
  uint32_t backoffMs = UPLOAD_PERIOD_MS;
  bool wasUp = false, timeAnnounced = false, ntpConfigured = false;
  char tz[48];

  for (;;) {
    esp_task_wdt_reset();
    const uint32_t now = millis();

    uint16_t hbS;
    bool tzChanged;
    {
      Lock k;
      hbS = settings.heartbeatS ? settings.heartbeatS : 60;
      tzChanged = settingsChanged;
      settingsChanged = false;
      strlcpy(tz, settings.tz, sizeof tz);
    }
    if (tzChanged) {
      setenv("TZ", tz, 1);
      tzset();
      ntpConfigured = false;  // re-run configTzTime with the new zone
    }

    // ---- Wi-Fi supervision ----
    const bool up = WiFi.status() == WL_CONNECTED;
    if (up != wasUp) {
      Serial.printf("[net] wifi %s\n", up ? "connected" : "lost");
      if (up) wifiConnects++;
      wasUp = up;
      if (!up) sServer = false;
    }
    sWifi = up;
    if (!up && now - lastWifiTry > 15000) {
      lastWifiTry = now;
      // 1 = network not found (check SSID spelling / 2.4 GHz), 4 = wrong password, 6 = still connecting
      Serial.printf("[net] wifi not connected (status %d), retrying \"%s\"\n", (int)WiFi.status(), WIFI_SSID);
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    if (up && !ntpConfigured) {
      configTzTime(tz, NTP_SERVER_1, NTP_SERVER_2);
      ntpConfigured = true;
    }

    // ---- clock ----
    sTime = clockValid();
    if (sTime && !timeAnnounced) {
      timeAnnounced = true;
      pushRecord("time_sync", "{}");
    }

    // ---- heartbeat (queued even while offline) ----
    if (now - lastBeat >= hbS * 1000UL) {
      lastBeat = now;
      heartbeat();
    }

    // ---- upload with exponential backoff ----
    if (up && now - lastUpload >= backoffMs) {
      lastUpload = now;
      if (upload()) {
        sServer = true;
        uploadsOk++;
        backoffMs = UPLOAD_PERIOD_MS;
        // Drain a backlog quickly after an outage.
        if (queued() > 0) backoffMs = 200;
      } else {
        sServer = false;
        uploadFailures++;
        backoffMs = min<uint32_t>(backoffMs * 2, 30000);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void begin(uint32_t id, const DeviceSettings &s) {
  mtx = xSemaphoreCreateMutex();
  bootId = id;
  settings = s;
  setenv("TZ", s.tz, 1);
  tzset();
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}

}  // namespace net
