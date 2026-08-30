#include "OtaSignature.h"

#include <Logging.h>

namespace ota_signature {

namespace {

// Minimal standard-base64 decoder (no validation beyond the alphabet; '=' and
// whitespace are skipped). Ed25519 sigs are 64 bytes -> ~88 base64 chars.
int b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool b64Decode(const std::string& in, uint8_t* out, size_t outCap, size_t* outLen) {
  size_t i = 0;
  size_t o = 0;
  int buf = 0;
  int bits = 0;
  for (; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    const int v = b64Val(c);
    if (v < 0) return false;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= outCap) return false;
      out[o++] = static_cast<uint8_t>((buf >> bits) & 0xFF);
    }
  }
  *outLen = o;
  return true;
}

}  // namespace

bool verifyManifest(const std::string& manifestJson, const std::string& sigB64) {
  uint8_t sig[ED25519_SIG_SIZE];
  size_t sigLen = 0;
  if (!b64Decode(sigB64, sig, sizeof(sig), &sigLen)) {
    LOG_ERR("OTA", "manifest signature base64 decode failed");
    return false;
  }
  if (sigLen != ED25519_SIG_SIZE) {
    LOG_ERR("OTA", "manifest signature wrong length: %zu", sigLen);
    return false;
  }

  ed25519_key key;
  if (wc_ed25519_init(&key) != 0) {
    LOG_ERR("OTA", "ed25519 key init failed");
    return false;
  }

  bool ok = false;
  const int rc = wc_ed25519_import_public(ota_signature::PUBKEY, ota_signature::PUBKEY_LEN, &key);
  if (rc == 0) {
    int res = 0;
    const int vrc =
        wc_ed25519_verify_msg(sig, static_cast<word32>(sigLen), reinterpret_cast<const byte*>(manifestJson.data()),
                              static_cast<word32>(manifestJson.size()), &res, &key);
    ok = (vrc == 0 && res == 1);
    if (!ok) LOG_ERR("OTA", "manifest Ed25519 verify failed (rc=%d res=%d)", vrc, res);
  } else {
    LOG_ERR("OTA", "ed25519 import public failed: %d", rc);
  }
  wc_ed25519_free(&key);
  return ok;
}

Sha256Stream::Sha256Stream() { wc_InitSha256(&ctx_); }

Sha256Stream::~Sha256Stream() { wc_Sha256Free(&ctx_); }

void Sha256Stream::update(const uint8_t* data, size_t len) { wc_Sha256Update(&ctx_, data, static_cast<word32>(len)); }

void Sha256Stream::finish(uint8_t out[32]) { wc_Sha256Final(&ctx_, out); }

}  // namespace ota_signature
