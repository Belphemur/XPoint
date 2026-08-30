#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
// clang-format on

#include <algorithm>
#include <cstring>
#include <string>

#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"
#include "ManifestJsonParser.h"
#include "OtaSignature.h"

namespace {
// This fork (crosspoint-x-reader) publishes its firmware as GitHub releases on
// the renamed repo. The updater reads the latest release + a signed manifest
// attached to it. The manifest is what we actually verify; the firmware is
// stream-checked against the manifest's signed SHA-256 (not locked/co-signed).
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/Belphemur/crosspoint-x-reader/releases/latest";
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the verified-https GET, redirects, and
  // User-Agent (see HttpDownloader).
  ReleaseJsonParser releaseParser;
  // Each board updates from its own release asset: plain firmware.bin for the
  // C3 X4/X3 binary (pre-existing releases), firmware-<board>.bin otherwise.
  const bool isX4 = board_tag::boardNameLen() == 2 && memcmp(board_tag::boardName(), "x4", 2) == 0;
  char assetName[48] = "firmware.bin";
  if (!isX4) {
    snprintf(assetName, sizeof(assetName), "firmware-%.*s.bin", static_cast<int>(board_tag::boardNameLen()),
             board_tag::boardName());
  }
  releaseParser.setFirmwareAssetName(assetName);
  releaseParser.setManifestAssetName("manifest.json");
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s manifest=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no", releaseParser.foundManifest() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_INF("OTA", "No %s asset in latest release", assetName);
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());

  // Fetch + verify the signed manifest if the release carries one. We do NOT
  // hard-fail here if it is missing (older/third-party releases) — we simply
  // skip signature verification and rely on the existing chip/board guards.
  manifestUrl.clear();
  if (releaseParser.foundManifest()) {
    manifestUrl = releaseParser.getManifestUrl();
    const auto mres = fetchAndVerifyManifest(manifestUrl);
    if (mres != OK) {
      LOG_ERR("OTA", "Signed manifest check failed (%d)", mres);
      return mres;
    }
  } else {
    LOG_INF("OTA", "Release has no signed manifest; skipping signature verification");
  }

  return OK;
}

OtaUpdater::OtaUpdaterError OtaUpdater::fetchAndVerifyManifest(const std::string& url) {
  // Pull the manifest body into memory (it is tiny — a few hundred bytes) so
  // we can verify its Ed25519 signature. The firmware itself is never buffered.
  std::string manifestJson;
  std::string sigJson;
  const bool mOk = HttpDownloader::fetchUrl(url, manifestJson);
  if (!mOk) {
    LOG_ERR("OTA", "Manifest fetch failed");
    return HTTP_ERROR;
  }
  const std::string sigUrl = url + ".sig";
  const bool sOk = HttpDownloader::fetchUrl(sigUrl, sigJson);
  if (!sOk) {
    LOG_ERR("OTA", "Manifest signature fetch failed");
    return HTTP_ERROR;
  }

  if (!ota_signature::verifyManifest(manifestJson, sigJson)) {
    LOG_ERR("OTA", "Manifest signature verification FAILED");
    return SIGNATURE_ERROR;
  }

  // Parse the now-trusted manifest and pin the running board's entry (URL,
  // size, SHA-256). installUpdate() re-stream-checks the firmware against it.
  // NOTE: we keep the release tag as latestVersion (shown to the user in the UI,
  // e.g. "1.2.3-x4pro"); the manifest's version is only the bare semver.
  manifestParser.reset();
  manifestParser.feed(manifestJson.data(), manifestJson.size());

  const ManifestBoardEntry* entry = manifestParser.findBoard(board_tag::boardName(), board_tag::boardNameLen());
  if (!entry) {
    LOG_INF("OTA", "Manifest has no entry for this board (%.*s)", static_cast<int>(board_tag::boardNameLen()),
            board_tag::boardName());
    return NO_UPDATE;
  }
  if (entry->hasSha) {
    memcpy(expectedSha, entry->sha256, 32);
    haveExpectedSha = true;
  }
  // Prefer the manifest's authoritative URL/size over the release asset entry
  // (the manifest is what was signed).
  if (entry->url[0] != '\0') {
    otaUrl = entry->url;
    otaSize = entry->size;
    totalSize = entry->size;
  }
  LOG_INF("OTA", "Manifest verified; board %.*s url=%s", static_cast<int>(board_tag::boardNameLen()),
          board_tag::boardName(), otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  // esp_https_ota is hardwired to esp-tls/mbedTLS, whose precompiled build on this
  // package can't negotiate TLS 1.3 (see SecureClient.h). Drive the OTA partition
  // ourselves and stream the firmware through HttpDownloader, which runs over
  // wolfSSL when FREEINK_NET_WOLFSSL is set, reusing its redirect handling for the
  // GitHub -> CDN hop.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  esp_err_t esp_err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  // The image streams in chunks; only the first bytes carry the header. Buffer
  // the first 14 bytes so we can read chip_id (esp_image_header_t offset 12)
  // and reject a wrong-MCU image before it overwrites the OTA partition.
  uint8_t hdr[14];
  size_t hdrLen = 0;
  bool wrongChip = false;
  // All S3 boards share a chip_id, so also scan the stream for the embedded
  // board tag (FirmwareBoardTag.h). An untagged image passes; a tag naming a
  // different board aborts the download. The wrong image may partially land in
  // the inactive OTA slot, but esp_ota_abort() below means it never becomes
  // the boot target.
  board_tag::Scanner tagScanner;
  ota_signature::Sha256Stream sha;
  const bool checkSha = haveExpectedSha;
  uint8_t computedSha[32];
  const auto fetchOk = HttpDownloader::fetchUrl(otaUrl, [&](const uint8_t* data, size_t len) {
    if (hdrLen < sizeof(hdr)) {
      const size_t take = std::min(len, sizeof(hdr) - hdrLen);
      std::memcpy(hdr + hdrLen, data, take);
      hdrLen += take;
      if (hdrLen == sizeof(hdr)) {
        uint16_t imageChip;
        std::memcpy(&imageChip, hdr + 12, sizeof(imageChip));
        const uint16_t deviceChip = firmware_flash::runningPartitionChipId();
        if (deviceChip != 0xFFFF && imageChip != deviceChip) {
          LOG_ERR("OTA", "wrong chip: image=0x%04X device=0x%04X", imageChip, deviceChip);
          wrongChip = true;
          return false;  // abort the transfer
        }
      }
    }
    tagScanner.feed(data, len);
    if (tagScanner.mismatch()) {
      LOG_ERR("OTA", "wrong board: image=%s device=%.*s", tagScanner.foundName(),
              static_cast<int>(board_tag::boardNameLen()), board_tag::boardName());
      return false;  // abort the transfer
    }
    if (checkSha) sha.update(data, len);
    if (esp_ota_write(otaHandle, data, len) != ESP_OK) {
      flashOk = false;
      return false;  // abort the transfer
    }
    processedSize += len;
    // Fire the callback only on whole-percent change. Per-chunk updates wake the
    // render task, whose framebuffer work contends with TLS on the internal arena,
    // and e-ink can't repaint faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    return true;
  });

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (wrongChip || tagScanner.mismatch()) {
    LOG_ERR("OTA", "Firmware install aborted: wrong device");
    esp_ota_abort(otaHandle);
    return WRONG_DEVICE_ERROR;
  }

  if (!fetchOk || !flashOk) {
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }

  // The signed manifest gave us a trusted SHA-256; confirm the streamed image
  // matches before we mark the partition bootable. A mismatch means the bytes
  // on the wire (MITM / CDN corruption) differed from what was signed.
  if (checkSha) {
    sha.finish(computedSha);
    if (memcmp(computedSha, expectedSha, 32) != 0) {
      LOG_ERR("OTA", "Firmware SHA-256 does not match signed manifest");
      esp_ota_abort(otaHandle);
      return SIGNATURE_ERROR;
    }
    LOG_INF("OTA", "Firmware SHA-256 matches signed manifest");
  }

  esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
