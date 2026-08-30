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
     "repository": "Belphemur/crosspoint-x-reader",
     "boards": [
       { "board": "x4pro", "asset": "firmware-x4pro.bin",
         "url": "https://github.com/Belphemur/crosspoint-x-reader/releases/download/v1.2.3/firmware-x4pro.bin",
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

1. Generate a new Ed25519 keypair (raw 32-byte seed):

   ```bash
   python3 - <<'PY'
   from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
   import base64
   sk = Ed25519PrivateKey.generate()
   print(base64.b64encode(sk.private_bytes_raw()).decode())
   PY
   ```

2. Update `lib/OtaSignature/ota_pubkey.h` with the new 32-byte public key.
3. Set the new private key as the `OTA_SIGNING_KEY` secret:

   ```bash
   gh secret set OTA_SIGNING_KEY -b'<new-base64-seed>' --repo Belphemur/crosspoint-x-reader
   ```

4. Commit the public-key change and cut a new release. Devices on the new
   firmware will accept the new key; devices on old firmware keep the old key
   (so stagger the rollout — publish a release carrying the new key *after* the
   fleet has updated to firmware that trusts it, or ship a transitional build
   that trusts both).
