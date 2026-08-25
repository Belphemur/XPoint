#include "SQLiteVfsHal.h"

#include <cstdio>
#include <cstring>
#include <new>

#ifdef ARDUINO
#include <esp_random.h>
#include <HalStorage.h>
#include <common/FsApiConstants.h>  // oflag_t, O_*
#else
#include <cstdlib>
#include <ctime>
#include <unistd.h>
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
#else
  FILE* fp = nullptr;
#endif
};

extern sqlite3_io_methods gHalIoMethods;

int halClose(sqlite3_file* pFile) {
  HalVfsFile* f = reinterpret_cast<HalVfsFile*>(pFile);
  const bool deleteOnClose = (f->openFlags & SQLITE_OPEN_DELETEONCLOSE) != 0;
#ifdef ARDUINO
  f->file.close();
#else
  if (f->fp) {
    fclose(f->fp);
    f->fp = nullptr;
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
  return SQLITE_OK;
}

int halRead(sqlite3_file* pFile, void* zBuf, int iAmt, sqlite3_int64 iOfst) {
  HalVfsFile* f = reinterpret_cast<HalVfsFile*>(pFile);
  if (iAmt <= 0) return SQLITE_OK;
  std::memset(zBuf, 0, static_cast<size_t>(iAmt));
#ifdef ARDUINO
  if (!f->file.seek64(static_cast<uint64_t>(iOfst))) return SQLITE_IOERR_SEEK;
  const int n = f->file.read(zBuf, static_cast<size_t>(iAmt));
  if (n < 0) return SQLITE_IOERR_READ;
  return SQLITE_OK;
#else
  if (fseeko(f->fp, static_cast<off_t>(iOfst), SEEK_SET) != 0) return SQLITE_IOERR_SEEK;
  const size_t n = fread(zBuf, 1, static_cast<size_t>(iAmt), f->fp);
  if (ferror(f->fp)) return SQLITE_IOERR_READ;
  (void)n;
  return SQLITE_OK;
#endif
}

int halWrite(sqlite3_file* pFile, const void* zBuf, int iAmt, sqlite3_int64 iOfst) {
  HalVfsFile* f = reinterpret_cast<HalVfsFile*>(pFile);
#ifdef ARDUINO
  if (!f->file.seek64(static_cast<uint64_t>(iOfst))) return SQLITE_IOERR_SEEK;
  const size_t n = f->file.write(static_cast<const uint8_t*>(zBuf), static_cast<size_t>(iAmt));
  return (n == static_cast<size_t>(iAmt)) ? SQLITE_OK : SQLITE_IOERR_WRITE;
#else
  if (fseeko(f->fp, static_cast<off_t>(iOfst), SEEK_SET) != 0) return SQLITE_IOERR_SEEK;
  const size_t n = fwrite(zBuf, 1, static_cast<size_t>(iAmt), f->fp);
  return (n == static_cast<size_t>(iAmt)) ? SQLITE_OK : SQLITE_IOERR_WRITE;
#endif
}

int halTruncate(sqlite3_file* pFile, sqlite3_int64 size) {
  HalVfsFile* f = reinterpret_cast<HalVfsFile*>(pFile);
#ifdef ARDUINO
  return f->file.truncate(static_cast<uint64_t>(size)) ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
#else
  return (ftruncate(fileno(f->fp), static_cast<off_t>(size)) == 0) ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
#endif
}

int halSync(sqlite3_file* pFile, int /*flags*/) {
  HalVfsFile* f = reinterpret_cast<HalVfsFile*>(pFile);
#ifdef ARDUINO
  return f->file.sync() ? SQLITE_OK : SQLITE_IOERR_FSYNC;
#else
  if (fflush(f->fp) != 0) return SQLITE_IOERR_FSYNC;
  return (fsync(fileno(f->fp)) == 0) ? SQLITE_OK : SQLITE_IOERR_FSYNC;
#endif
}

int halFileSize(sqlite3_file* pFile, sqlite3_int64* pSize) {
  HalVfsFile* f = reinterpret_cast<HalVfsFile*>(pFile);
#ifdef ARDUINO
  *pSize = static_cast<sqlite3_int64>(f->file.fileSize64());
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
  return Storage.remove(zName) ? SQLITE_OK : SQLITE_IOERR_DELETE;
#else
  return (::remove(zName) == 0) ? SQLITE_OK : SQLITE_IOERR_DELETE;
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
  *piNow = static_cast<sqlite3_int64>(time(nullptr));
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
    3,                    /* iVersion */
    sizeof(HalVfsFile),   /* szOsFile */
    HAL_MAX_PATHNAME,     /* mxPathname */
    nullptr,              /* pNext */
    "hal",                /* zName */
    nullptr,              /* pAppData */
    halOpen,              /* xOpen */
    halDelete,            /* xDelete */
    halAccess,            /* xAccess */
    halFullPathname,      /* xFullPathname */
    nullptr,              /* xDlOpen */
    nullptr,              /* xDlError */
    nullptr,              /* xDlSym */
    nullptr,              /* xDlClose */
    halRandomness,        /* xRandomness */
    halSleep,             /* xSleep */
    halCurrentTime,       /* xCurrentTime */
    nullptr,              /* xGetLastError */
    halCurrentTimeInt64,  /* xCurrentTimeInt64 */
    nullptr,              /* xSetSystemCall */
    nullptr,              /* xGetSystemCall */
    nullptr,              /* xNextSystemCall */
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
