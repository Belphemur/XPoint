# OTA signing for crosspoint-x-reader

This fork verifies OTA updates against an **Ed25519 signature** so a tampered or
mismatched firmware image is rejected before it is flashed. The firmware image
itself is **not** co-signed/locked — only a small per-release **manifest** is
signed, and the streamed firmware is checked against the manifest's signed
SHA-256. This is the standard *sign the digest, stream-verify the bytes* pattern.

## How it works

1. On a new tag, `.github/workflows/release.yml` builds every board's
   `firmware-<board>.bin`, creates a (draft) GitHub release, and generates
   `manifest.json`:

   ```json
   {
     "version": "1.2.3",
     "repository": "Belphemur/XPoint",
     "boards": [
       { "board": "x4pro", "asset": "firmware-x4pro.bin",
         "url": "https://github.com/Belphemur/XPoint/releases/download/v1.2.3/firmware-x4pro.bin",
         "size": 1234567, "sha256": "<64 hex>" }
     ]
   }
   ```

2. The workflow signs the **exact manifest bytes** with the Ed25519 private key
   (base64, stored in the repo secret `OTA_SIGNING_KEY`) and uploads
   `manifest.json.sig` (raw signature, base64) next to it.

3. On device, `OtaUpdater` (in `src/network/OtaUpdater.cpp`):
   - fetches `manifest.json` + `manifest.json.sig` from the release,
   - verifies the signature with the **public key baked into**
     `lib/OtaSignature/ota_pubkey.h` (`ota_signature::PUBKEY`),
   - pins the running board's entry (URL, size, SHA-256),
   - streams the firmware and computes its SHA-256, comparing it to the signed
     hash before marking the OTA partition bootable.
   - The existing chip-id + embedded board-tag guards still run as defense in depth.

If a release has **no** signed manifest, the updater logs and proceeds **without**
signature verification (so older/third-party releases still install) — but any
release produced by this workflow is signed, and a bad signature or SHA mismatch
fails the update with `SIGNATURE_ERROR`.

## Keys

- **Private key** — `OTA_SIGNING_KEY` repo secret (raw 32-byte Ed25519 seed, base64).
  Never committed. Used only by the release workflow.
- **Public key** — `lib/OtaSignature/ota_pubkey.h`, committed. Anyone can read it;
  only someone with the secret can produce a valid signature.

## Triggering a release (easy OTA workflow)

1. Bump `crosspoint.version` in `platformio.ini`.
2. Commit + push to `develop`.
3. Tag and push:

   ```bash
   git tag v1.2.3 -m "Release v1.2.3"
   git push origin v1.2.3
   ```

4. The release is created as a **draft** with all assets + signed manifest.
   Publish it from the GitHub UI (or change `--draft` to `--latest` in
   `release.yml` to auto-publish). Devices on this fork then see and verify it.

## Rotating the signing key

The device trusts exactly one baked-in public key (`ota_signature::PUBKEY` in
`lib/OtaSignature/ota_pubkey.h`). To rotate without bricking OTA for devices
that haven't updated yet, you must ship the new key in a *transitional* firmware
before any release is signed with it:

1. **Generate a new Ed25519 keypair** (raw 32-byte seed) and keep the old one
   available for one more release cycle.

2. **Update the firmware to trust BOTH keys.** If `OtaSignature` verifies
   against a single compiled-in key today, extend `verifyManifest()` to try each
   key in a small built-in list (`PUBKEY_PRIMARY`, `PUBKEY_PREVIOUS`) and accept
   if any verifies. Commit that firmware with both public keys baked in.

3. **Cut and publish a release signed with the OLD key** (so it is still
   authentic to every device in the field — including those running firmware that
   only knows the old key). This propagates the dual-key firmware from step 2
   to the fleet. Wait for broad adoption before continuing.

4. **Switch the signing secret to the NEW key** and cut the next release. The
   dual-key firmware accepts it. Devices still on the old single-key firmware
   that missed the step-3 rollout will now reject the new-key release and keep
   their current firmware (safe, no brick) until they install the transitional
   firmware from step 3.

5. **Later**, once the old single-key fleet is negligible, drop
   `PUBKEY_PREVIOUS` from the firmware in a normal release.

```bash
# Generate a new keypair and print the base64 seed (store the seed as OTA_SIGNING_KEY):
python3 - <<'PY'
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import base64
sk = Ed25519PrivateKey.generate()
print("seed (set as OTA_SIGNING_KEY):", base64.b64encode(sk.private_bytes_raw()).decode())
PY
```

- **Private key** — `OTA_SIGNING_KEY` repo secret (raw 32-byte Ed25519 seed, base64).
  Never committed. Used only by the release workflow.
- **Public key(s)** — `lib/OtaSignature/ota_pubkey.h`, committed. Anyone can read
  them; only someone with the matching secret can produce a valid signature.
