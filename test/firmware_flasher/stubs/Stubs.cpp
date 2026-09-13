// Host-test stub definitions for FirmwareFlasher's ESP-IDF, HAL, and mbedTLS
// dependencies. The validator's no-SHA image fixture never invokes the SHA
// finish path, so the SHA calls are inert.
#include <cstring>

#include "HalStorage.h"
#include "OtaBootSwitch.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"

namespace {
constexpr uint16_t kHostChipId = 0x1234;
}  // namespace

HalStorage HalStorage::instance;

const esp_partition_t* esp_ota_get_running_partition() {
  static const esp_partition_t running{"ota_0", 0x10000, 2 * 1024 * 1024};
  return &running;
}

const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* running) {
  (void)running;
  static const esp_partition_t next{"ota_1", 0x210000, 2 * 1024 * 1024};
  return &next;
}

esp_err_t esp_partition_read(const esp_partition_t* partition, size_t src_offset, void* dst, size_t size) {
  if (partition != esp_ota_get_running_partition() || src_offset != 12 || size != sizeof(uint16_t)) return -1;
  uint16_t chip = kHostChipId;
  std::memcpy(dst, &chip, sizeof(chip));
  return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t*, uint32_t, uint32_t) { return ESP_OK; }
esp_err_t esp_partition_write(const esp_partition_t*, uint32_t, const void*, size_t) { return ESP_OK; }

namespace ota_boot {
bool switchTo(const esp_partition_t* dest) {
  (void)dest;
  return true;
}
}  // namespace ota_boot

void mbedtls_sha256_init(mbedtls_sha256_context*) {}
void mbedtls_sha256_free(mbedtls_sha256_context*) {}
int mbedtls_sha256_starts(mbedtls_sha256_context*, int) { return 0; }
int mbedtls_sha256_update(mbedtls_sha256_context*, const uint8_t*, size_t) { return 0; }
int mbedtls_sha256_finish(mbedtls_sha256_context*, uint8_t*) { return 0; }
