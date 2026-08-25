#pragma once

#include <sqlite3.h>

namespace sqlite_vfs_hal {

// Returns the "hal" sqlite3_vfs. The VFS is registered once on first call.
//
// Every I/O operation is routed through HalStorage/HalFile on the device
// (so all SD access stays under the storage mutex) and through a POSIX
// backend on the host (so the same code path is exercised by the native
// CMake test suite). The two backends are selected at compile time with
// #ifdef ARDUINO.
//
// Single-connection invariant: SQLite is only ever used from the main
// reader task. xLock/xUnlock are therefore EXCLUSIVE-style no-ops and the
// storage mutex already serializes the underlying SD card. Do not open a
// second connection to the same database.
sqlite3_vfs* vfs();

}  // namespace sqlite_vfs_hal
