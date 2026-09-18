#pragma once

// MemSentinel — FT bring-up diagnostics (PR #146). The on-device crash is a
// silent DRAM corruption whose faulting frames point at IDLE0/scheduler state
// (only IDLE tasks carry a canary watchpoint, so the overflowing task is never
// the one that faults). Phase-scoped heap-integrity checks bracket the suspect
// operations (BW buffer chunk store/restore, TTF page paint, progress flush,
// font reload) so the failing heap block is caught with the culprit still on
// the log.
//
// Gate: TTF device class + FT backend + explicit dev-build opt-in. The checks
// walk the whole heap and would add per-render latency, so release/rc envs do
// not define CROSSPOINT_MEM_SENTINEL — only [env:x4pro] and [env:x4pro_profile]
// carry it while the crash hunt is open. Drop this file with the root-cause
// fix.

#include <Logging.h>

#if defined(CROSSPOINT_TTF_READER) && defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT && \
    defined(CROSSPOINT_MEM_SENTINEL) && CROSSPOINT_MEM_SENTINEL
#include <esp_heap_caps.h>

inline void memSentinelCheck(const char* phase) {
  // print_errors=true: the whole point is naming the smashed block; without it
  // ESP-IDF only returns false and the "block dump above" hint would be wrong.
  if (heap_caps_check_integrity_all(true)) return;
  LOG_ERR("SENT", "Heap integrity FAILED after %s (block dump above)", phase);
}
#else
inline void memSentinelCheck(const char*) {}
#endif
