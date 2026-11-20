#pragma once
// Signed over-the-air updates.
//
// The server publishes a manifest {version, size, sha256, signature}. The
// signature is ECDSA P-256 over SHA-256("riseq-fw:v<version>:<sha256hex>"),
// made offline with tools/sign_firmware.py. The device checks the signature
// with the public key in include/ota_pubkey.h, streams the image into the
// spare OTA slot while hashing it, and only marks the slot bootable if the
// hash matches. Unsigned, tampered or older images are never booted.
#include <Arduino.h>

namespace ota {
bool verifyManifest(int version, const char *sha256Hex, const char *signatureB64, String &err);
void checkAndInstall();  // called from the net task; restarts on success
}  // namespace ota
