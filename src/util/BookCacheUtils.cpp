#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    Epub book(path, "/.crosspoint");
    book.clearCache();
    // FIBP page caches (TTF reader) live in <book cache>/ficache. The
    // recursive removeDir above takes the whole book dir, but a leftover
    // ficache tree (e.g. a file still held open by a late-cancelled worker)
    // would silently survive — remove it explicitly and log the outcome.
    const std::string fibpDir = book.getCachePath() + "/ficache";
    if (Storage.exists(fibpDir.c_str())) {
      if (!Storage.removeDir(fibpDir.c_str())) {
        LOG_ERR("BookCache", "Failed to remove FIBP cache dir: %s", fibpDir.c_str());
      }
    }
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasTxtExtension(path)) {
    Txt(path, "/.crosspoint").clearCache();
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}
