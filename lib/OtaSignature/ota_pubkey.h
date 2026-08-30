#pragma once

// OTA Ed25519 verify public key (raw 32-byte Ed25519 public key).
// Generated for the crosspoint-x-reader fork. Keep this file PUBLIC; the
// matching private key lives ONLY in the GitHub repo secret OTA_SIGNING_KEY
// and is used by .github/workflows/release.yml to sign per-board manifests.
// Rotate by regenerating the keypair, updating this array, and the secret.

#include <cstddef>
#include <cstdint>

namespace ota_signature {
inline constexpr size_t PUBKEY_LEN = 32;
inline const uint8_t PUBKEY[PUBKEY_LEN] = {
  0x77, 0xD4, 0x15, 0x8D, 0x5F, 0x8F, 0x12, 0xDA, 0xEC, 0x56, 0x6D, 0x3E, 0x55, 0x8B, 0x2D, 0xB8, 0x9D, 0x02, 0x97, 0x65, 0xA5, 0xAC, 0x82, 0x1D, 0x3B, 0x71, 0xCC, 0x4F, 0xDC, 0x88, 0x95, 0x2D
};
}  // namespace ota_signature
