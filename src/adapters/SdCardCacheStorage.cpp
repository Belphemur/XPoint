#include "SdCardCacheStorage.h"

#include <Logging.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace freeink {
namespace book {

SdCardCacheStorage::SdCardCacheStorage(const char* dirPath) {
  if (dirPath == nullptr || dirPath[0] == '\0') {
    LOG_ERR("TTFB", "CacheStorage: empty dir path");
    return;
  }
  const size_t len = strlen(dirPath);
  if (len >= kDirMax) {
    LOG_ERR("TTFB", "CacheStorage: dir path too long (%u)", (unsigned)len);
    return;
  }
  memcpy(dir_, dirPath, len + 1);
  // Normalize: no trailing slash; buildPath() always joins with '/'.
  while (strlen(dir_) > 1 && dir_[1] != '\0' && dir_[strlen(dir_) - 1] == '/') {
    dir_[strlen(dir_) - 1] = '\0';
  }
  if (!Storage.ensureDirectoryExists(dir_)) LOG_ERR("TTFB", "CacheStorage: mkdir failed: %s", dir_);

  pathBuf_ = makeUniqueNoThrow<char[]>(kPathMax);
  if (!pathBuf_) {
    LOG_ERR("TTFB", "CacheStorage: OOM: %u bytes path buffer", (unsigned)kPathMax);
  }
}

bool SdCardCacheStorage::validName(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;
  if (strchr(name, '/') != nullptr || strchr(name, '\\') != nullptr) return false;  // flat names only
  // Reject path traversal: "." and ".." would build <dir>/.. and escape the cache dir.
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
  return strlen(name) <= kNameMax;
}

bool SdCardCacheStorage::buildPath(const char* name, const char* suffix, char* out, size_t cap) const {
  const int written = snprintf(out, cap, "%s/%s%s", dir_, name, suffix);
  return written > 0 && static_cast<size_t>(written) < cap;
}

bool SdCardCacheStorage::exists(const char* name) {
  if (dir_[0] == '\0' || !pathBuf_ || !validName(name)) return false;
  if (!buildPath(name, "", pathBuf_.get(), kPathMax)) return false;
  HalFile f;
  if (!Storage.openFileForRead("TTFB", pathBuf_.get(), f)) return false;
  return f.isOpen();
}

bool SdCardCacheStorage::remove(const char* name) {
  if (dir_[0] == '\0' || !pathBuf_ || !validName(name)) return false;
  if (!buildPath(name, "", pathBuf_.get(), kPathMax)) return false;
  const bool matchesActiveFinal = writeFinalPath_[0] != '\0' && strcmp(pathBuf_.get(), writeFinalPath_) == 0;
  if (matchesActiveFinal && (writeHandle_.isOpen() || writeFailed_ || endWriteFailed_)) {
    // PageCacheWriter uses remove() as failure cleanup. Keep the last good
    // final cache when a .tmp write failed; the next beginWrite reuses/truncates it.
    if (writeHandle_.isOpen() && !writeHandle_.close()) {
      LOG_ERR("TTFB", "remove: close failed: %s", writeTmpPath_);
      endWriteFailed_ = true;
      return false;
    }
    writeFailed_ = false;
    endWriteFailed_ = false;
    return true;
  }
  return Storage.remove(pathBuf_.get());
}

int64_t SdCardCacheStorage::fileSize(const char* name) {
  if (dir_[0] == '\0' || !pathBuf_ || !validName(name)) return -1;
  if (!buildPath(name, "", pathBuf_.get(), kPathMax)) return -1;
  HalFile f;
  if (!Storage.openFileForRead("TTFB", pathBuf_.get(), f)) return -1;  // absent
  return static_cast<int64_t>(f.fileSize64());
}

int32_t SdCardCacheStorage::readAt(const char* name, uint32_t offset, void* dst, uint32_t len) {
  if (dir_[0] == '\0' || !pathBuf_ || !validName(name) || dst == nullptr) return -1;
  if (!buildPath(name, "", pathBuf_.get(), kPathMax)) return -1;
  HalFile f;
  if (!Storage.openFileForRead("TTFB", pathBuf_.get(), f)) return -1;
  if (!f.seek64(offset)) return -1;
  return f.read(dst, len);
}

bool SdCardCacheStorage::beginWrite(const char* name) {
  if (dir_[0] == '\0' || !validName(name)) return false;
  if (writeHandle_.isOpen()) {
    if (!endWriteFailed_) {
      LOG_ERR("TTFB", "beginWrite: a write is already active");
      return false;
    }
    // A failed close may have left the .tmp handle open. Retry the close
    // before truncating the retained temporary file.
    if (!writeHandle_.close()) {
      LOG_ERR("TTFB", "beginWrite: failed-write handle still open: %s", writeTmpPath_);
      return false;
    }
  }
  if (!buildPath(name, ".tmp", writeTmpPath_, sizeof(writeTmpPath_))) return false;
  if (!buildPath(name, "", writeFinalPath_, sizeof(writeFinalPath_))) return false;
  // Recover a stale "<final>.old" left by a previous endWrite() that crashed
  // between the rotate-rename and the final rename. If no final exists, the
  // .old IS the last good cache — restore it. If a final exists, the .old is
  // garbage from an already-completed publish and can be removed.
  if (pathBuf_ && buildPath(name, ".old", pathBuf_.get(), kPathMax)) {
    if (!Storage.exists(writeFinalPath_) && Storage.exists(pathBuf_.get())) {
      if (!Storage.rename(pathBuf_.get(), writeFinalPath_)) {
        LOG_ERR("TTFB", "beginWrite: restore .old failed: %s", writeFinalPath_);
        return false;
      }
    } else {
      Storage.remove(pathBuf_.get());
    }
  }
  writeFailed_ = false;
  endWriteFailed_ = false;
  // openFileForWrite truncates, so a stale .tmp from a previous crash is reused.
  if (!Storage.openFileForWrite("TTFB", writeTmpPath_, writeHandle_)) {
    LOG_ERR("TTFB", "beginWrite: open failed: %s", writeTmpPath_);
    return false;
  }
  return true;
}

bool SdCardCacheStorage::write(const void* data, uint32_t len) {
  if (!writeHandle_.isOpen()) return false;
  if (len == 0) return true;
  if (writeHandle_.write(data, len) != len) {
    writeFailed_ = true;
    return false;
  }
  return true;
}

bool SdCardCacheStorage::endWrite() {
  if (!writeHandle_.isOpen()) {
    LOG_ERR("TTFB", "endWrite: no active write");
    return false;
  }
  if (writeFailed_) {
    LOG_ERR("TTFB", "endWrite: skipped publish after write failure");
    if (!writeHandle_.close()) {
      LOG_ERR("TTFB", "endWrite: close failed: %s", writeTmpPath_);
      endWriteFailed_ = true;
    }
    return false;
  }
  if (!writeHandle_.sync()) {
    LOG_ERR("TTFB", "endWrite: sync failed");
    if (!writeHandle_.close()) {
      LOG_ERR("TTFB", "endWrite: close failed: %s", writeTmpPath_);
      endWriteFailed_ = true;
    }
    return false;
  }
  // close() flushes the last sector; a failure here means the .tmp is not a
  // faithful copy — treat it as a failed write and leave it for retry rather
  // than deleting a good cache we then cannot replace.
  if (!writeHandle_.close()) {
    LOG_ERR("TTFB", "endWrite: close failed: %s", writeTmpPath_);
    endWriteFailed_ = true;
    return false;
  }
  // Rotate the previous final aside before publishing: SdFat's rename does
  // not overwrite an existing destination, so the old file must move first.
  // Renaming (instead of removing) preserves the last good cache across a
  // power loss between the two renames — the doc contract at
  // DESIGN_NATIVE_TTF_SUPPORT.md:503 ("mid-build failure retains previous
  // final"). A rotate-rename failure is non-fatal for the .tmp: it is left
  // in place and the whole publish is retried later.
  const char* oldPath = nullptr;
  if (Storage.exists(writeFinalPath_)) {
    if (!pathBuf_) {
      LOG_ERR("TTFB", "endWrite: no path buffer: %s", writeFinalPath_);
      endWriteFailed_ = true;
      return false;
    }
    const int n = snprintf(pathBuf_.get(), kPathMax, "%s.old", writeFinalPath_);
    if (n <= 0 || static_cast<size_t>(n) >= kPathMax) {
      LOG_ERR("TTFB", "endWrite: rotate path overflow: %s", writeFinalPath_);
      endWriteFailed_ = true;
      return false;
    }
    if (!Storage.rename(writeFinalPath_, pathBuf_.get())) {
      LOG_ERR("TTFB", "endWrite: rotate failed (final kept): %s", writeFinalPath_);
      endWriteFailed_ = true;
      return false;
    }
    oldPath = pathBuf_.get();
  }
  if (!Storage.rename(writeTmpPath_, writeFinalPath_)) {
    LOG_ERR("TTFB", "endWrite: rename failed: %s", writeFinalPath_);
    // Best-effort restore: the rotate already moved the previous final, put
    // it back so a cache always exists for readers.
    if (oldPath != nullptr) Storage.rename(oldPath, writeFinalPath_);
    endWriteFailed_ = true;
    return false;
  }
  if (oldPath != nullptr) Storage.remove(oldPath);
  writeFailed_ = false;
  endWriteFailed_ = false;
  return true;
}

int32_t SdCardCacheStorage::readBackAt(uint32_t offset, void* dst, uint32_t len) {
  if (!writeHandle_.isOpen() || dst == nullptr) return -1;
  const size_t cursor = writeHandle_.position();
  if (!writeHandle_.seek64(offset)) return -1;
  const int bytesRead = writeHandle_.read(dst, len);
  if (!writeHandle_.seek(cursor)) {
    // The write cursor is lost — fail the write instead of appending at the
    // wrong position into what would become the final file.
    LOG_ERR("TTFB", "readBackAt: cursor restore failed");
    writeHandle_.close();
    writeFailed_ = true;
    return -1;
  }
  return bytesRead;
}

}  // namespace book
}  // namespace freeink
