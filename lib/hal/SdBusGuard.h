#pragma once

#include <atomic>
#include <cstdint>

// Counts SD-card transactions that are in flight, so the CPU clock can never be
// switched while the host is mid-command.
//
// Why this exists (design doc 2026-10-01 §7.2): the endurance governor drops the
// idle clock as far as 10 MHz, and it does so from a core-0 task and from
// `HalPowerManager::setPowerSaving()` — asynchronously to whichever task is doing
// SD I/O. On the ESP32-S3 the SDMMC bus clock is fed from the fixed 160 MHz PLL
// (`SDMMC_CLK_DEFAULT = SOC_MOD_CLK_PLL_F160M`, soc/esp32s3/include/soc/
// clk_tree_defs.h:481), so the card itself keeps its clock — but the host's
// register interface and its ISR run off APB, and APB follows the CPU frequency.
// Reconfiguring that divider while a multi-block write command is outstanding
// loses the completion event, which is exactly what the field log shows:
//
//   sdmmc_cmd: sdmmc_write_sectors_dma: sdmmc_send_cmd returned 0x107,
//              failed to get status (0x107)     <- components/sdmmc/sdmmc_cmd.c:597
//   sdmmc_req: handle_idle_state_events unhandled: 00001000 00000000
//
// The first line means the write timed out AND the recovery CMD13 also timed
// out, so command sequencing was gone, not merely slow. The second is the driver
// objecting that a non-card-detect interrupt status survived into idle, which
// its own comment says cannot happen (components/esp_driver_sdmmc/src/
// sdmmc_transaction.c:193-207).
//
// ESP-IDF normally defends against this with a power-management lock held for
// the duration of a transfer, but that lock is compiled out unless
// CONFIG_PM_ENABLE is set, and this firmware does not enable PM (it has no light
// sleep path at all). So the defence has to live here.
//
// The counter is recursive-aware: HalStorage's storage mutex is a *recursive*
// mutex and StorageLock nests, so a plain bool would let an inner scope clear
// the busy mark while an outer transaction is still running. Only the 0 -> 1 and
// 1 -> 0 edges matter.
//
// Header-only and free of Arduino/FreeRTOS includes so the accounting is
// host-testable (test/power/PowerTest.cpp).

namespace sd_bus {

class TransactionCounter {
 public:
  // Increment. Returns true when this call took the count from 0 to 1, i.e. the
  // moment the bus went from quiescent to busy — the point at which the clock
  // must already be at a safe frequency, because a transfer is about to start.
  bool enter() {
    uint16_t cur = count_.load(std::memory_order_relaxed);
    if (cur == UINT16_MAX) return false;  // pinned; never overflow to 0
    // The edge is decided inside the CAS, so two tasks can both increment and
    // still only one of them observes 0 -> 1.
    while (!count_.compare_exchange_weak(cur, static_cast<uint16_t>(cur + 1), std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
      if (cur == UINT16_MAX) return false;
    }
    return cur == 0;
  }

  // Decrement. Returns true when this call dropped the count to 0, i.e. the bus
  // is quiescent again and the governor may lower the clock.
  bool leave() {
    uint16_t cur = count_.load(std::memory_order_relaxed);
    for (;;) {
      if (cur == 0) return false;  // unbalanced release; stay busy rather than lie
      if (count_.compare_exchange_weak(cur, static_cast<uint16_t>(cur - 1), std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
        return cur == 1;
      }
    }
  }

  // Acquire pairs with the release in enter(): a reader that sees the bus idle
  // also sees every store the transaction made before dropping the count.
  bool busy() const { return count_.load(std::memory_order_acquire) != 0; }
  uint16_t count() const { return count_.load(std::memory_order_acquire); }

 private:
  // Atomic because HalPowerManager::setPowerSaving() reads the busy state on
  // purpose WITHOUT holding the storage mutex: the governor must be able to
  // refuse an idle entry while a transfer is in flight without blocking on the
  // card. A plain field read against writers that hold the mutex is a data race.
  std::atomic<uint16_t> count_{0};
};

}  // namespace sd_bus
