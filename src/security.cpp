#include "security.h"
#include "config.h"
#include "secrets.h"
#include <SparkFun_ATECCX08a_Arduino_Library.h>
#include <mbedtls/sha256.h>
#include <mbedtls/md.h>

static ATECCX08A atecc;

bool PayloadSigner::begin(TwoWire& wire) {
  if (!atecc.begin(Config::ADDR_ATECC608A, wire)) {
    Serial.println("[SEC] ATECC608A not detected at 0x60; using HMAC fallback.");
    hwReady_ = false;
    return false;
  }

  atecc.readConfigZone(false);
  if (atecc.configLockStatus && atecc.dataOTPLockStatus && atecc.slot0LockStatus) {
    Serial.println("[SEC] ATECC608A locked and ready for ECDSA signing.");
    hwReady_ = true;
  } else {
    Serial.println("[SEC] ATECC608A found but Slot 0 unlocked; using HMAC fallback.");
    hwReady_ = false;
  }
  return hwReady_;
}

String PayloadSigner::signMessage(const String& message) {
  uint8_t digest[32];
  mbedtls_sha256(reinterpret_cast<const uint8_t*>(message.c_str()),
                 message.length(), digest, 0);

  if (hwReady_ && atecc.createSignature(digest)) {
    return toHex(atecc.signature, 64);
  }

  // Fallback HMAC-SHA256 if ATECC608A slot 0 is unprovisioned
  uint8_t hmac[32];
  const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(mdInfo,
                  reinterpret_cast<const uint8_t*>(FALLBACK_HMAC_KEY),
                  strlen(FALLBACK_HMAC_KEY),
                  reinterpret_cast<const uint8_t*>(message.c_str()),
                  message.length(),
                  hmac);
  return toHex(hmac, 32);
}

String PayloadSigner::toHex(const uint8_t* bytes, size_t len) {
  char out[129];
  size_t maxBytes = (len > 64) ? 64 : len;
  for (size_t i = 0; i < maxBytes; i++) {
    snprintf(&out[i * 2], 3, "%02x", bytes[i]);
  }
  out[maxBytes * 2] = '\0';
  return String(out);
}