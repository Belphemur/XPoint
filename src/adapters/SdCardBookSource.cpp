#include "SdCardBookSource.h"

#include <Logging.h>

#include <cstdint>

namespace freeink {
namespace book {

SdCardBookSource::SdCardBookSource(const char* path) {
  if (path == nullptr || path[0] == '\0') {
    LOG_ERR("TTFB", "BookSource: empty path");
    return;
  }
  if (!Storage.openFileForRead("BOOKSRC", path, file_)) {
    LOG_ERR("TTFB", "BookSource open failed: %s", path);
    return;
  }
  sizeBytes_ = file_.fileSize64();
}

int32_t SdCardBookSource::readAt(uint64_t offset, void* dst, uint32_t len) {
  if (!isValid() || dst == nullptr) return -1;
  if (len > static_cast<uint32_t>(INT32_MAX)) len = INT32_MAX;
  if (!file_.seek64(offset)) return -1;
  // Short read at EOF is fine per the engine contract.
  return file_.read(dst, len);
}

}  // namespace book
}  // namespace freeink
