#include "ota.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "net.h"
#include "ota_pubkey.h"
#include "secrets.h"

namespace ota {

static uint32_t lastFailMs = 0;
static bool everFailed = false;

static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), data, len, out);
}

static void toHex(const uint8_t *b, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

bool verifyManifest(int version, const char *shaHex, const char *sigB64, String &err) {
  if (sizeof(OTA_PUBLIC_KEY_PEM) <= 1) {
    err = "no public key compiled in";
    return false;
  }
  if (strlen(shaHex) != 64) {
    err = "bad sha256";
    return false;
  }
  String msg = String("riseq-fw:v") + version + ":" + shaHex;
  uint8_t hash[32];
  sha256((const uint8_t *)msg.c_str(), msg.length(), hash);

  uint8_t sig[128];
  size_t sigLen = 0;
  if (mbedtls_base64_decode(sig, sizeof sig, &sigLen, (const uint8_t *)sigB64, strlen(sigB64)) != 0) {
    err = "bad signature encoding";
    return false;
  }

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int rc = mbedtls_pk_parse_public_key(&pk, (const uint8_t *)OTA_PUBLIC_KEY_PEM, sizeof(OTA_PUBLIC_KEY_PEM));
  if (rc == 0) rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash, sig, sigLen);
  mbedtls_pk_free(&pk);
  if (rc != 0) {
    err = "signature check failed";
    return false;
  }
  return true;
}

static void fail(const String &why) {
  everFailed = true;
  lastFailMs = millis();
  Serial.printf("[ota] failed: %s\n", why.c_str());
  String d = "{\"reason\":\"" + why + "\"}";
  net::event("ota_fail", d.c_str());
}

void checkAndInstall() {
  if (everFailed && millis() - lastFailMs < 10UL * 60UL * 1000UL) return;  // retry at most every 10 min

  // 1. Manifest
  HTTPClient http;
  String path = "/api/device/firmware/manifest";
  http.begin(String(SERVER_URL) + path);
  http.setTimeout(5000);
  net::signRequest(http, "GET", path, "");
  int code = http.GET();
  if (code != 200) {
    http.end();
    return fail("manifest http " + String(code));
  }
  JsonDocument m;
  if (deserializeJson(m, http.getString())) {
    http.end();
    return fail("manifest json");
  }
  http.end();
  const int version = m["version"] | 0;
  const uint32_t size = m["size"] | 0;
  String shaHex = m["sha256"] | "";
  String sigB64 = m["signature"] | "";
  shaHex.toLowerCase();
  if (version <= FW_VERSION) return;  // never downgrade

  // 2. Signature (before downloading anything)
  String err;
  if (!verifyManifest(version, shaHex.c_str(), sigB64.c_str(), err)) return fail(err);

  // 3. Download into the spare slot, hashing as we go
  net::event("ota_start", (String("{\"version\":") + version + "}").c_str());
  path = "/api/device/firmware/image";
  http.begin(String(SERVER_URL) + path);
  http.setTimeout(10000);
  net::signRequest(http, "GET", path, "");
  code = http.GET();
  if (code != 200 || (uint32_t)http.getSize() != size) {
    http.end();
    return fail("image http " + String(code));
  }
  if (!Update.begin(size)) {
    http.end();
    return fail("no ota partition");
  }

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  uint32_t got = 0, lastData = millis();
  bool ok = true;
  while (got < size) {
    esp_task_wdt_reset();
    size_t avail = stream->available();
    if (!avail) {
      if (millis() - lastData > 15000) {
        ok = false;
        break;
      }
      delay(2);
      continue;
    }
    size_t n = stream->readBytes(buf, min(avail, sizeof buf));
    mbedtls_md_update(&ctx, buf, n);
    if (Update.write(buf, n) != n) {
      ok = false;
      break;
    }
    got += n;
    lastData = millis();
  }
  uint8_t digest[32];
  mbedtls_md_finish(&ctx, digest);
  mbedtls_md_free(&ctx);
  http.end();

  char hex[65];
  toHex(digest, 32, hex);
  if (!ok || got != size) {
    Update.abort();
    return fail("download incomplete");
  }
  if (shaHex != hex) {
    Update.abort();  // signed manifest does not match the bytes we received
    return fail("hash mismatch");
  }
  if (!Update.end(true)) return fail("update end " + String(Update.errorString()));

  Serial.printf("[ota] installed v%d, restarting\n", version);
  delay(500);
  ESP.restart();
}

}  // namespace ota
