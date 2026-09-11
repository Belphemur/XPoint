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
    LOG_ERR("TTFB", "beginWrite: a write is already active");
    return false;
  }
  if (!buildPath(name, ".tmp", writeTmpPath_, sizeof(writeTmpPath_))) return false;
  if (!buildPath(name, "", writeFinalPath_, sizeof(writeFinalPath_))) return false;
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
  return writeHandle_.write(data, len) == len;
}

bool SdCardCacheStorage::endWrite() {
  if (!writeHandle_.isOpen()) {
    LOG_ERR("TTFB", "endWrite: no active write");
    return false;
  }
  if (!writeHandle_.sync()) {
    LOG_ERR("TTFB", "endWrite: sync failed");
    writeHandle_.close();
    return false;
  }
  writeHandle_.close();
  // SdFat's rename does not overwrite an existing destination, so drop the old
  // file first (pattern from ProgressFile::writeAtomic,
  // src/activities/reader/ProgressFile.h). Unlike writeAtomic, a failed remove
  // is fatal here: the .tmp is left in place for a retry.
  if (Storage.exists(writeFinalPath_) && !Storage.remove(writeFinalPath_)) {
    LOG_ERR("TTFB", "endWrite: remove failed: %s", writeFinalPath_);
    return false;
  }
  if (!Storage.rename(writeTmpPath_, writeFinalPath_)) {
    LOG_ERR("TTFB", "endWrite: rename failed: %s", writeFinalPath_);
    return false;
  }
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
    return -1;
  }
  return bytesRead;
}

}  // namespace book
}  // namespace freeink
