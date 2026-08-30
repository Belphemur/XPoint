#pragma once

#include <string>

#include "ManifestJsonParser.h"

class OtaUpdater {
 public:
  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    WRONG_DEVICE_ERROR,
    SIGNATURE_ERROR,  // manifest signature or firmware SHA-256 mismatch
  };

  using ProgressCallback = void (*)(void* ctx);

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr);

 private:
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  std::string manifestUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;

  // Trusted SHA-256 pinned from the signed manifest, plus a parser instance
  // reused across checkForUpdate()/installUpdate().
  uint8_t expectedSha[32];
  bool haveExpectedSha = false;
  ManifestJsonParser manifestParser;

  // Fetch the release's manifest.json + .sig, verify the Ed25519 signature
  // against the baked-in public key, and pin the running board's entry.
  OtaUpdaterError fetchAndVerifyManifest(const std::string& url);
};
