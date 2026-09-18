#pragma once

// MemSentinel — FT bring-up diagnostics (PR #146). The on-device crash is a
// silent DRAM corruption whose faulting frames point at IDLE0/scheduler state
// (only IDLE tasks carry a canary watchpoint, so the overflowing task is never
// the one that faults). Phase-scoped heap-integrity checks bracket the suspect
// operations (BW buffer chunk store/restore, TTF page paint, progress flush,
// font reload) so the failing heap block is caught with the culprit still on
// the log. Gate to the FT backend flag; strip before wide release.

#include <Logging.h>

#if defined(CROSSPOINT_TTF_READER) && defined(CROSSPOINT_FONT_BACKEND_FT)
#include <esp_heap_caps.h>

inline void memSentinelCheck(const char* phase) {
  if (heap_caps_check_integrity_all(false)) return;
  // check_integrity_all(true) already printed the broken block details.
  LOG_ERR("SENT", "Heap integrity FAILED after %s (block dump above)", phase);
}
#else
inline void memSentinelCheck(const char*) {}
#endif
