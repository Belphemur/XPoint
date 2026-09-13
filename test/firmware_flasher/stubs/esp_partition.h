#pragma once

#include <cstddef>
#include <cstdint>

using esp_err_t = int;

#define ESP_OK 0

struct esp_partition_t {
  const char* label;
  uint32_t address;
  size_t size;
};

esp_err_t esp_partition_read(const esp_partition_t* partition, size_t src_offset, void* dst, size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t* partition, uint32_t start_addr, uint32_t size);
esp_err_t esp_partition_write(const esp_partition_t* partition, uint32_t dst_offset, const void* src, size_t size);
