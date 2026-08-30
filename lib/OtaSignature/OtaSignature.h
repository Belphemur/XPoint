#pragma once

#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "ota_pubkey.h"

// OTA authenticity + integrity helpers for the crosspoint-x-reader fork.
//
// The release workflow signs a small per-release manifest (manifest.json) with
// an Ed25519 private key whose matching PUBLIC key is baked into this lib
// (ota_pubkey.h). The firmware is NOT signed/locked; we only verify the
// manifest's signature and then stream-verify the firmware's SHA-256 against
// the signed hash. This stops tampered/mismatched firmware from flashing
// without coupling the device to a single signing ceremony.
namespace ota_signature {

// Verify a base64 Ed25519 signature over `manifestJson` using the baked-in
// public key. Returns true only when the signature is present, 64 bytes, and
// verifies. Safe to call on untrusted input.
bool verifyManifest(const std::string& manifestJson, const std::string& sigB64);

// Incremental SHA-256 over the streamed firmware bytes. init() in ctor,
// update() per chunk, finish() yields the 32-byte digest. No buffering of the
// whole image required (critical on the 380 KB RAM C3 / constrained S3).
class Sha256Stream {
 public:
  Sha256Stream();
  ~Sha256Stream();

  Sha256Stream(const Sha256Stream&) = delete;
  Sha256Stream& operator=(const Sha256Stream&) = delete;

  void update(const uint8_t* data, size_t len);
  void finish(uint8_t out[32]);

 private:
  struct wc_Sha256 ctx_;
};

}  // namespace ota_signature
