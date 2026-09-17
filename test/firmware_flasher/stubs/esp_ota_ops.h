#pragma once

#include "esp_partition.h"

const esp_partition_t* esp_ota_get_running_partition();
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* running);

// Host-test hook: re-points the chip id served by esp_partition_read at
// offset 12 (what runningPartitionChipId() consumes). Call it before
// validating an image whose chip_id differs from the default 0x1234.
void testSetRunningChipId(uint16_t chipId);
