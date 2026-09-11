#pragma once

#include <BookStorage.h>
#include <HalStorage.h>
#include <Memory.h>

#include <memory>

namespace freeink {
namespace book {

// CacheStorage over HalStorage: flat cache files inside one per-book
// directory. Writes go to "<name>.tmp" and rename onto the final name in
// endWrite(), so a crash mid-write never leaves a torn final file.
class SdCardCacheStorage : public CacheStorage {
 public:
  static constexpr size_t kDirMax = 160;   // directory string, NUL included
  static constexpr size_t kPathMax = 256;  // joined path, NUL included
  static constexpr size_t kNameMax = 64;   // flat file name length cap

  // Takes the cache DIRECTORY (e.g. "/books/.crosspoint/<hash>/ficache").
  // Creates it if missing. Invalid/oversized paths leave the storage dead:
  // every operation then fails without crashing.
  explicit SdCardCacheStorage(const char* dirPath);
  ~SdCardCacheStorage() override = default;

  bool exists(const char* name) override;
  bool remove(const char* name) override;
  int64_t fileSize(const char* name) override;
  int32_t readAt(const char* name, uint32_t offset, void* dst, uint32_t len) override;
  bool beginWrite(const char* name) override;
  bool write(const void* data, uint32_t len) override;
  bool endWrite() override;
  int32_t readBackAt(uint32_t offset, void* dst, uint32_t len) override;

 private:
  static bool validName(const char* name);
  // Joins "<dir>/<name><suffix>" into out; false when it does not fit.
  bool buildPath(const char* name, const char* suffix, char* out, size_t cap) const;

  char dir_[kDirMax] = {};
  char writeTmpPath_[kPathMax] = {};
  char writeFinalPath_[kPathMax] = {};
  // Shared scratch for joined paths across the read-side methods: a 256B
  // buffer would blow the <256B stack-local budget on every call, and
  // HalStorage serializes SD access so single-buffer reuse is safe. Null
  // (allocation failed) makes every path-based operation fail.
  std::unique_ptr<char[]> pathBuf_;
  HalFile writeHandle_;  // the open .tmp between beginWrite()/endWrite()
};

}  // namespace book
}  // namespace freeink
