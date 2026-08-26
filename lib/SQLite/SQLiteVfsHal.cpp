#include "SQLiteVfsHal.h"

#include <cstdio>
#include <cstring>
#include <new>

#ifdef ARDUINO
#include <HalStorage.h>
#include <Logging.h>
#include <common/FsApiConstants.h>  // oflag_t, O_*
#include <esp_random.h>
#else
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <ctime>

#define LOG_INF(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(module, fmt, ...) fprintf(stderr, "[%s] " fmt "\n", module, ##__VA_ARGS__)
#endif

namespace sqlite_vfs_hal {

namespace {

constexpr int HAL_SECTOR_SIZE = 512;
constexpr int HAL_MAX_PATHNAME = 512;

struct HalVfsFile {
  sqlite3_file base;
  char path[HAL_MAX_PATHNAME];
  int openFlags;
#ifdef ARDUINO
  HalFile file;
  // Rollback-journal write buffer (mirrors Sqlite3Esp32's esp32.cpp VFS):
  // journal traffic is many small writes at alternating offsets (header @0,
  // page records, nRec rewrite @0), the access pattern SdFat's cache handles
  // worst over the SDMMC bounce-buffer path. Buffering coalesces it into one
  // contiguous write per flush (sync/read/size/close). ~8 KiB internal RAM,
  // allocated only while a journal file is open (i.e. inside a transaction).
  uint8_t* journalBuf = nullptr;
  int journalBufLen = 0;
  sqlite3_int64 journalBufOfst = 0;
#else
  FILE* fp = nullptr;
#endif
};

extern sqlite3_io_methods gHalIoMethods;

// Failure-point logging: every IOERR below carries its exact operation in the
// log, so an on-device "disk I/O error" can be attributed to a layer (VFS op
// + path) instead of surfacing as SQLite's opaque generic message. Host builds
// keep the stderr shim above; error paths only, so no steady-state noise.
#define HAL_LOG_IOERR(op, f) LOG_ERR("STATS-VFS", "%s failed on %s", (op), (f)->path)

#ifdef ARDUINO
constexpr int kJournalBufSize = 8192;  // matches SQLITE_ESP32VFS_BUFFERSZ

bool isJournalFile(const HalVfsFile* f) { return (f->openFlags & SQLITE_OPEN_MAIN_JOURNAL) != 0; }

// Writes buffered journal data through to the SD card. No-op when empty.
int halFlushJournalBuffer(HalVfsFile* f) {
  if (f->journalBufLen == 0) return SQLITE_OK;
  if (!f->file.seek64(static_cast<uint64_t>(f->journalBufOfst))) {
    HAL_LOG_IOERR("journal write(seek)", f);
    f->journalBufLen = 0;
    return SQLITE_IOERR_SEEK;
  }
  const size_t n = f->file.write(f->journalBuf, static_cast<size_t>(f->journalBufLen));
  const int expected = f->journalBufLen;
  f->journalBufLen = 0;
  if (n != static_cast<size_t>(expected)) {
    HAL_LOG_IOERR("journal write", f);
    return SQLITE_IOERR_WRITE;
  }
  return SQLITE_OK;
}

// Appends data to the journal buffer, flushing whenever the new data is not
// the exact continuation of the buffered region (the reference VFS does the
// same: contiguous appends coalesce, anything else flushes first).
int halBufferedJournalWrite(HalVfsFile* f, const uint8_t*& zBuf, int& iAmt, sqlite3_int64& iOfst) {
  while (iAmt > 0) {
    if (f->journalBufLen == kJournalBufSize || f->journalBufOfst + f->journalBufLen != iOfst) {
      const int rc = halFlushJournalBuffer(f);
      if (rc != SQLITE_OK) return rc;
      // After a non-contiguous flush the buffer is empty; restart the region.
      f->journalBufOfst = iOfst;
    }
    const int nCopy = kJournalBufSize - f->journalBufLen;
    const int chunk = nCopy < iAmt ? nCopy : iAmt;
    std::memcpy(f->journalBuf + f->journalBufLen, zBuf, static_cast<size_t>(chunk));
    f->journalBufLen += chunk;
    zBuf += chunk;
    iOfst += chunk;
    iAmt -= chunk;
  }
  return SQLITE_OK;
}
#endif

int halClose(sqlite3_file* pFile) {
  HalVfsFile* f = reinterpret_cast<HalVfsFile*>(pFile);
  const bool deleteOnClose = (f->openFlags & SQLITE_OPEN_DELETEONCLOSE) != 0;
  int rc = SQLITE_OK;
#ifdef ARDUINO
  // Flush any buffered journal data before the handle goes away. A failed
  // flush is reported (os_unix does the same) even though the OS handle is
  // released regardless.
  rc = halFlushJournalBuffer(f);
#endif
#ifdef ARDUINO
  if (!f->file.close() && rc == SQLITE_OK) {
    HAL_LOG_IOERR("close", f);
    rc = SQLITE_IOERR_CLOSE;
  }
#else
  if (f->fp) {
    fclose(f->fp);
    f->fp = nullptr;
  }
#endif
#ifdef ARDUINO
  if (f->journalBuf) {
    delete[] f->journalBuf;
    f->journalBuf = nullptr;
    f->journalBufLen = 0;
  }
#endif
  if (deleteOnClose) {
#ifdef ARDUINO
    Storage.remove(f->path);
#else
    ::remove(f->path);
#endif
  }
  f->~HalVfsFile();
  return rc;
}

int halRead(sqlite3_file* pFile, void* zBuf, int iAmt, sqlite3_int64 iOfst) {
  HalVfsFile* f = reinterpret_cast<HalVfsFile*>(pFile);
  if (iAmt <= 0) return SQLITE_OK;
  std::memset(zBuf, 0, static_cast<size_t>(iAmt));
#ifdef ARDUINO
  // Buffered journal data must reach the file before it can be read back
  // (SQLite reads journal headers/records during rollback and commit).
  int rc = halFlushJournalBuffer(f);
  if (rc != SQLITE_OK) return rc;
  if (!f->file.seek64(static_cast<uint64_t>(iOfst))) {
    HAL_LOG_IOERR("read(seek)", f);
    return SQLITE_IOERR_SEEK;
  }
  const int n = f->file.read(zBuf, static_cast<size_t>(iAmt));
  if (n < 0) {
    HAL_LOG_IOERR("read", f);
    return SQLITE_IOERR_READ;
  }
  if (n < iAmt) return SQLITE_IOERR_SHORT_READ;
  return SQLITE_OK;
#else
  if (fseeko(f->fp, static_cast<off_t>(iOfst), SEEK_SET) != 0) return SQLITE_IOERR_SEEK;
  const size_t n = fread(zBuf, 1, static_cast<size_t>(iAmt), f->fp);
  if (ferror(f->fp)) return SQLITE_IOERR_READ;
  if (n < static_cast<size_t>(iAmt)) return SQLITE_IOERR_SHORT_READ;
  return SQLITE_OK;
#endif
}

int halWrite(sqlite3_file* pFile, const void* zBuf, int iAmt, sqlite3_int64 iOfst) {
  HalVfsFile* f = reinterpret_cast<HalVfsFile*>(pFile);
#ifdef ARDUINO
  // Journal files take the buffered path (coalesces SQLite's small writes at
  // alternating offsets into contiguous SD transfers); everything else is
  // written straight through as before.
  if (isJournalFile(f)) {
    const uint8_t* cursor = static_cast<const uint8_t*>(zBuf);
    return halBufferedJournalWrite(f, cursor, iAmt, iOfst);
  }
  if (!f->file.seek64(static_cast<uint64_t>(iOfst))) {
    HAL_LOG_IOERR("write(seek)", f);
    return SQLITE_IOERR_SEEK;
  }
  const size_t n = f->file.write(static_cast<const uint8_t*>(zBuf), static_cast<size_t>(iAmt));
  if (n != static_cast<size_t>(iAmt)) {
    HAL_LOG_IOERR("write", f);
    return SQLITE_IOERR_WRITE;
  }
  return SQLITE_OK;
#else
  if (fseeko(f->fp, static_cast<off_t>(iOfst), SEEK_SET) != 0) return SQLITE_IOERR_SEEK;
  const size_t n = fwrite(zBuf, 1, static_cast<size_t>(iAmt), f->fp);
  return (n == static_cast<size_t>(iAmt)) ? SQLITE_OK : SQLITE_IOERR_WRITE;
#endif
}

int halTruncate(sqlite3_file* pFile, sqlite3_int64 size) {
  HalVfsFile* f = reinterpret_cast<HalVfsFile*>(pFile);
#ifdef ARDUINO
  if (!f->file.truncate(static_cast<uint64_t>(size))) {
    HAL_LOG_IOERR("truncate", f);
    return SQLITE_IOERR_TRUNCATE;
  }
  return SQLITE_OK;
#else
  return (ftruncate(fileno(f->fp), static_cast<off_t>(size)) == 0) ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
#endif
}

int halSync(sqlite3_file* pFile, int /*flags*/) {
  HalVfsFile* f = reinterpret_cast<HalVfsFile*>(pFile);
#ifdef ARDUINO
  // Sync is the durability point: buffered journal data must hit the card.
  int rc = halFlushJournalBuffer(f);
  if (rc != SQLITE_OK) return rc;
  if (!f->file.sync()) {
    HAL_LOG_IOERR("sync", f);
    return SQLITE_IOERR_FSYNC;
  }
  return SQLITE_OK;
#else
  if (fflush(f->fp) != 0) return SQLITE_IOERR_FSYNC;
  return (fsync(fileno(f->fp)) == 0) ? SQLITE_OK : SQLITE_IOERR_FSYNC;
#endif
}

int halFileSize(sqlite3_file* pFile, sqlite3_int64* pSize) {
  HalVfsFile* f = reinterpret_cast<HalVfsFile*>(pFile);
#ifdef ARDUINO
  // The reported size must include data still sitting in the journal buffer.
  const sqlite3_int64 buffered = (f->journalBufLen > 0) ? f->journalBufOfst + f->journalBufLen : 0;
  const sqlite3_int64 onDisk = static_cast<sqlite3_int64>(f->file.fileSize64());
  *pSize = buffered > onDisk ? buffered : onDisk;
  return SQLITE_OK;
#else
  const off_t cur = ftello(f->fp);
  if (fseeko(f->fp, 0, SEEK_END) != 0) return SQLITE_IOERR_SEEK;
  const off_t end = ftello(f->fp);
  fseeko(f->fp, cur, SEEK_SET);
  *pSize = static_cast<sqlite3_int64>(end);
  return SQLITE_OK;
#endif
}

// The storage mutex already serializes all SD access, so SQLite's file
// locking is redundant. These are EXCLUSIVE-style no-ops.
int halLock(sqlite3_file*, int) { return SQLITE_OK; }
int halUnlock(sqlite3_file*, int) { return SQLITE_OK; }
int halCheckReservedLock(sqlite3_file*, int* pResOut) {
  *pResOut = 0;
  return SQLITE_OK;
}

int halFileControl(sqlite3_file*, int /*op*/, void* /*pArg*/) { return SQLITE_NOTFOUND; }

int halSectorSize(sqlite3_file*) { return HAL_SECTOR_SIZE; }

// No ATOMIC/POWERSAFE_OVERWRITE: journal_mode=DELETE + synchronous=NORMAL is
// used instead (see design doc D3).
int halDeviceCharacteristics(sqlite3_file*) { return 0; }

// Shared-memory (WAL) support is intentionally absent (journal_mode=DELETE).
int halShmMap(sqlite3_file*, int, int, int, void volatile**) { return SQLITE_IOERR_SHMMAP; }
int halShmLock(sqlite3_file*, int, int, int) { return SQLITE_IOERR_SHMLOCK; }
void halShmBarrier(sqlite3_file*) {}
int halShmUnmap(sqlite3_file*, int) { return SQLITE_OK; }

int halOpen(sqlite3_vfs*, const char* zName, sqlite3_file* pFile, int flags, int* pOutFlags) {
  HalVfsFile* f = new (pFile) HalVfsFile();
  f->base.pMethods = nullptr;
  if (!zName) {
    f->~HalVfsFile();
    return SQLITE_CANTOPEN;
  }
  std::strncpy(f->path, zName, HAL_MAX_PATHNAME - 1);
  f->path[HAL_MAX_PATHNAME - 1] = '\0';
  f->openFlags = flags;

  const bool readOnly = (flags & SQLITE_OPEN_READONLY) != 0;
  const bool create = (flags & SQLITE_OPEN_CREATE) != 0;

#ifdef ARDUINO
  // Rollback journals get the write buffer (see HalVfsFile). new is not
  // nothrow on ESP32 (-fno-exceptions): use the nothrow form and fail open.
  if ((flags & SQLITE_OPEN_MAIN_JOURNAL) != 0) {
    f->journalBuf = new (std::nothrow) uint8_t[kJournalBufSize];
    if (!f->journalBuf) {
      f->~HalVfsFile();
      return SQLITE_NOMEM;
    }
  }
#endif

#ifdef ARDUINO
  oflag_t oflag = O_RDONLY;
  if (readOnly) {
    oflag = O_RDONLY;
  } else if (create) {
    oflag = O_RDWR | O_CREAT;  // no O_TRUNC: SQLite rewrites in place
  } else {
    oflag = O_RDWR;
  }
  f->file = Storage.open(f->path, oflag);
  if (!f->file.isOpen()) {
    f->~HalVfsFile();
    return SQLITE_CANTOPEN;
  }
#else
  if (readOnly) {
    f->fp = fopen(f->path, "rb");
  } else if (create) {
    f->fp = fopen(f->path, "rb+");
    if (!f->fp) f->fp = fopen(f->path, "wb+");
  } else {
    f->fp = fopen(f->path, "rb+");
  }
  if (!f->fp) {
    f->~HalVfsFile();
    return SQLITE_CANTOPEN;
  }
#endif

  f->base.pMethods = &gHalIoMethods;
  if (pOutFlags) *pOutFlags = flags;
  return SQLITE_OK;
}

int halDelete(sqlite3_vfs*, const char* zName, int /*syncDir*/) {
#ifdef ARDUINO
  if (!Storage.remove(zName)) {
    // Match os_unix: deleting a file that is already gone is reported
    // distinctly (callers tolerate it), anything else is a hard IOERR.
    if (Storage.exists(zName)) {
      LOG_ERR("STATS-VFS", "delete failed on %s", zName);
      return SQLITE_IOERR_DELETE;
    }
    return SQLITE_IOERR_DELETE_NOENT;
  }
  return SQLITE_OK;
#else
  if (::remove(zName) != 0) {
    // errno must be read before any intervening call can clobber it; only a
    // genuine ENOENT maps to the tolerated NOENT code.
    const int err = errno;
    if (err == ENOENT) return SQLITE_IOERR_DELETE_NOENT;
    LOG_ERR("STATS-VFS", "delete failed on %s", zName);
    return SQLITE_IOERR_DELETE;
  }
  return SQLITE_OK;
#endif
}

int halAccess(sqlite3_vfs*, const char* zName, int /*flags*/, int* pResOut) {
#ifdef ARDUINO
  *pResOut = Storage.exists(zName) ? 1 : 0;
#else
  *pResOut = (::access(zName, F_OK) == 0) ? 1 : 0;
#endif
  return SQLITE_OK;
}

int halFullPathname(sqlite3_vfs*, const char* zName, int nOut, char* zOut) {
  if (nOut <= 0) return SQLITE_ERROR;
  std::strncpy(zOut, zName, static_cast<size_t>(nOut) - 1);
  zOut[nOut - 1] = '\0';
  return SQLITE_OK;
}

int halRandomness(sqlite3_vfs*, int nByte, char* zOut) {
  int written = 0;
#ifdef ARDUINO
  while (written < nByte) {
    const uint32_t r = esp_random();
    const int n = (nByte - written) < 4 ? (nByte - written) : 4;
    std::memcpy(zOut + written, &r, static_cast<size_t>(n));
    written += n;
  }
#else
  for (int i = 0; i < nByte; ++i) zOut[i] = static_cast<char>(rand() & 0xFF);
  written = nByte;
#endif
  return written;
}

int halSleep(sqlite3_vfs*, int microseconds) {
#ifdef ARDUINO
  delay((microseconds + 999) / 1000);
#else
  usleep(static_cast<useconds_t>(microseconds));
#endif
  return microseconds;
}

int halCurrentTime(sqlite3_vfs*, double* prNow) {
  *prNow = static_cast<double>(time(nullptr)) / 86400.0 + 2440587.5;
  return SQLITE_OK;
}

int halCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64* piNow) {
  double julianDay = 0.0;
  halCurrentTime(nullptr, &julianDay);
  *piNow = static_cast<sqlite3_int64>(julianDay * 86400000.0);
  return SQLITE_OK;
}

sqlite3_io_methods gHalIoMethods = {
    2,                        /* iVersion */
    halClose,                 /* xClose */
    halRead,                  /* xRead */
    halWrite,                 /* xWrite */
    halTruncate,              /* xTruncate */
    halSync,                  /* xSync */
    halFileSize,              /* xFileSize */
    halLock,                  /* xLock */
    halUnlock,                /* xUnlock */
    halCheckReservedLock,     /* xCheckReservedLock */
    halFileControl,           /* xFileControl */
    halSectorSize,            /* xSectorSize */
    halDeviceCharacteristics, /* xDeviceCharacteristics */
    halShmMap,                /* xShmMap */
    halShmLock,               /* xShmLock */
    halShmBarrier,            /* xShmBarrier */
    halShmUnmap,              /* xShmUnmap */
    nullptr,                  /* xFetch */
    nullptr,                  /* xUnfetch */
};

sqlite3_vfs gHalVfs = {
    3,                   /* iVersion */
    sizeof(HalVfsFile),  /* szOsFile */
    HAL_MAX_PATHNAME,    /* mxPathname */
    nullptr,             /* pNext */
    "hal",               /* zName */
    nullptr,             /* pAppData */
    halOpen,             /* xOpen */
    halDelete,           /* xDelete */
    halAccess,           /* xAccess */
    halFullPathname,     /* xFullPathname */
    nullptr,             /* xDlOpen */
    nullptr,             /* xDlError */
    nullptr,             /* xDlSym */
    nullptr,             /* xDlClose */
    halRandomness,       /* xRandomness */
    halSleep,            /* xSleep */
    halCurrentTime,      /* xCurrentTime */
    nullptr,             /* xGetLastError */
    halCurrentTimeInt64, /* xCurrentTimeInt64 */
    nullptr,             /* xSetSystemCall */
    nullptr,             /* xGetSystemCall */
    nullptr,             /* xNextSystemCall */
};

bool gRegistered = false;

}  // namespace

sqlite3_vfs* vfs() {
  if (!gRegistered) {
    sqlite3_vfs_register(&gHalVfs, 0);
    gRegistered = true;
  }
  return &gHalVfs;
}

}  // namespace sqlite_vfs_hal
