#include "FinishedBooksIndex.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <utility>

#include "FsHelpers.h"

#ifndef READING_STATS_TEST
#include <Epub.h>
#include <Xtc.h>

#include "RecentBooksStore.h"
#endif

namespace {
constexpr char INDEX_PATH[] = "/.crosspoint/finished_books.bin";
constexpr char INDEX_TMP_PATH[] = "/.crosspoint/finished_books.bin.tmp";
constexpr char INDEX_BACKUP_PATH[] = "/.crosspoint/finished_books.bin.bak";
constexpr uint8_t INDEX_VERSION = 3;
constexpr uint8_t START_DATE_INDEX_VERSION = 2;
constexpr uint8_t LEGACY_INDEX_VERSION = 1;
constexpr uint16_t MAX_TITLE_BYTES = 160;
constexpr uint16_t MAX_AUTHOR_BYTES = 120;
constexpr char MAGIC[] = {'C', 'P', 'F', 'B'};

template <typename T>
bool writePod(HalFile& file, const T& value) {
  return file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

template <typename T>
bool readPod(HalFile& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == static_cast<int>(sizeof(T));
}

uint64_t pathKey(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char c : path) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

bool isNewer(const FinishedBookEntry& a, const FinishedBookEntry& b) {
  if (a.finishedDate.isValid() != b.finishedDate.isValid()) {
    return a.finishedDate.isValid();
  }
  if (a.finishedDate.isValid()) {
    const int dateOrder = compareReadingStatsDate(a.finishedDate, b.finishedDate);
    if (dateOrder != 0) {
      return dateOrder > 0;
    }
  }
  return a.title < b.title;
}

size_t encodedEntrySize(const FinishedBookEntry& entry) {
  const size_t titleLength = std::min(entry.title.size(), static_cast<size_t>(MAX_TITLE_BYTES));
  const size_t authorLength = std::min(entry.author.size(), static_cast<size_t>(MAX_AUTHOR_BYTES));
  return sizeof(uint64_t) + sizeof(uint32_t) + (sizeof(uint16_t) + sizeof(uint8_t) * 2) * 2 + sizeof(uint16_t) +
         titleLength + sizeof(uint16_t) + authorLength;
}

size_t encodedIndexSize(const std::vector<FinishedBookEntry>& entries) {
  const size_t count = std::min(entries.size(), FinishedBooksIndex::MAX_ENTRIES);
  size_t size = sizeof(MAGIC) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t);
  for (size_t i = 0; i < count; ++i) {
    size += encodedEntrySize(entries[i]);
  }
  return size;
}

bool loadPath(const char* path, std::vector<FinishedBookEntry>& entries);

bool writeIndex(const std::vector<FinishedBookEntry>& entries) {
  if (Storage.exists(INDEX_TMP_PATH) && !Storage.remove(INDEX_TMP_PATH)) {
    LOG_ERR("FBI", "Could not remove stale finished-books temp file");
    return false;
  }

  HalFile file;
  if (!Storage.openFileForWrite("FBI", INDEX_TMP_PATH, file)) {
    LOG_ERR("FBI", "Could not open finished-books temp file");
    return false;
  }

  const uint8_t count = static_cast<uint8_t>(std::min(entries.size(), FinishedBooksIndex::MAX_ENTRIES));
  const uint16_t reserved = 0;
  bool ok = file.write(reinterpret_cast<const uint8_t*>(MAGIC), sizeof(MAGIC)) == sizeof(MAGIC) &&
            writePod(file, INDEX_VERSION) && writePod(file, count) && writePod(file, reserved);
  for (size_t i = 0; ok && i < count; ++i) {
    const auto& entry = entries[i];
    const uint16_t titleLength =
        static_cast<uint16_t>(std::min(entry.title.size(), static_cast<size_t>(MAX_TITLE_BYTES)));
    const uint16_t authorLength =
        static_cast<uint16_t>(std::min(entry.author.size(), static_cast<size_t>(MAX_AUTHOR_BYTES)));
    ok = writePod(file, entry.pathKey) && writePod(file, entry.totalReadingSeconds) &&
         writePod(file, entry.startDate.year) && writePod(file, entry.startDate.month) &&
         writePod(file, entry.startDate.day) && writePod(file, entry.finishedDate.year) &&
         writePod(file, entry.finishedDate.month) && writePod(file, entry.finishedDate.day) &&
         writePod(file, titleLength) &&
         (titleLength == 0 ||
          file.write(reinterpret_cast<const uint8_t*>(entry.title.data()), titleLength) == titleLength) &&
         writePod(file, authorLength) &&
         (authorLength == 0 ||
          file.write(reinterpret_cast<const uint8_t*>(entry.author.data()), authorLength) == authorLength);
  }

  file.flush();
  const bool synced = file.sync();
  const bool closed = file.close();
  ok = ok && synced && closed;
  if (!ok) {
    LOG_ERR("FBI", "Could not finish finished-books temp file");
    Storage.remove(INDEX_TMP_PATH);
    return false;
  }

  std::vector<FinishedBookEntry> verifiedEntries;
  const bool verified = loadPath(INDEX_TMP_PATH, verifiedEntries) && verifiedEntries.size() == count &&
                        Storage.exists(INDEX_TMP_PATH) && [&]() {
                          HalFile verifyFile;
                          return Storage.openFileForRead("FBI", INDEX_TMP_PATH, verifyFile) &&
                                 verifyFile.fileSize() == encodedIndexSize(entries);
                        }();
  if (!verified) {
    LOG_ERR("FBI", "Finished-books temp verification failed");
    Storage.remove(INDEX_TMP_PATH);
    return false;
  }

  if (Storage.exists(INDEX_BACKUP_PATH) && !Storage.remove(INDEX_BACKUP_PATH)) {
    LOG_ERR("FBI", "Could not replace finished-books backup");
    Storage.remove(INDEX_TMP_PATH);
    return false;
  }
  if (Storage.exists(INDEX_PATH) && !Storage.rename(INDEX_PATH, INDEX_BACKUP_PATH)) {
    LOG_ERR("FBI", "Could not rotate finished-books index");
    Storage.remove(INDEX_TMP_PATH);
    return false;
  }
  if (!Storage.rename(INDEX_TMP_PATH, INDEX_PATH)) {
    LOG_ERR("FBI", "Could not install finished-books index");
    if (Storage.exists(INDEX_BACKUP_PATH) && !Storage.exists(INDEX_PATH)) {
      Storage.rename(INDEX_BACKUP_PATH, INDEX_PATH);
    }
    Storage.remove(INDEX_TMP_PATH);
    return false;
  }
  return true;
}

bool loadPath(const char* path, std::vector<FinishedBookEntry>& entries) {
  HalFile file;
  if (!Storage.openFileForRead("FBI", path, file)) {
    return false;
  }

  char magic[sizeof(MAGIC)] = {};
  uint8_t version = 0;
  uint8_t count = 0;
  uint16_t reserved = 0;
  bool ok = file.read(reinterpret_cast<uint8_t*>(magic), sizeof(magic)) == static_cast<int>(sizeof(MAGIC)) &&
            readPod(file, version) && readPod(file, count) && readPod(file, reserved) &&
            std::memcmp(magic, MAGIC, sizeof(MAGIC)) == 0 &&
            (version == INDEX_VERSION || version == START_DATE_INDEX_VERSION || version == LEGACY_INDEX_VERSION) &&
            count <= FinishedBooksIndex::MAX_ENTRIES;

  if (ok) {
    entries.reserve(count);
  }
  for (uint8_t i = 0; ok && i < count; ++i) {
    FinishedBookEntry entry;
    uint16_t titleLength = 0;
    ok = readPod(file, entry.pathKey) && readPod(file, entry.totalReadingSeconds);
    if (ok && version >= START_DATE_INDEX_VERSION) {
      ok = readPod(file, entry.startDate.year) && readPod(file, entry.startDate.month) &&
           readPod(file, entry.startDate.day);
    }
    ok = ok && readPod(file, entry.finishedDate.year) && readPod(file, entry.finishedDate.month) &&
         readPod(file, entry.finishedDate.day) && readPod(file, titleLength) && titleLength <= MAX_TITLE_BYTES;
    if (!ok) {
      break;
    }
    entry.title.resize(titleLength);
    ok = titleLength == 0 || file.read(&entry.title[0], titleLength) == static_cast<int>(titleLength);
    if (ok && version >= INDEX_VERSION) {
      uint16_t authorLength = 0;
      ok = readPod(file, authorLength) && authorLength <= MAX_AUTHOR_BYTES;
      if (ok) {
        entry.author.resize(authorLength);
        ok = authorLength == 0 || file.read(&entry.author[0], authorLength) == static_cast<int>(authorLength);
      }
    }
    if (ok && !entry.title.empty()) {
      entries.push_back(std::move(entry));
    }
  }
  ok = ok && file.position() == file.fileSize();
  file.close();
  if (!ok) {
    entries.clear();
  }
  return ok;
}

std::string cachePathForBookPath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(std::string_view{path})) {
    return std::string("/.crosspoint/epub_") + std::to_string(std::hash<std::string>{}(path));
  }
  if (FsHelpers::hasXtcExtension(std::string_view{path})) {
    return std::string("/.crosspoint/xtc_") + std::to_string(std::hash<std::string>{}(path));
  }
  return {};
}

#ifdef READING_STATS_TEST
std::vector<FinishedBookRecoveryBook> testRecentBooks;

std::vector<FinishedBookRecoveryBook> recoveryBooks() { return testRecentBooks; }
#else
std::vector<FinishedBookRecoveryBook> recoveryBooks() {
  std::vector<FinishedBookRecoveryBook> books;
  const auto& recentBooks = RECENT_BOOKS.getBooks();
  books.reserve(recentBooks.size());
  for (const auto& book : recentBooks) {
    books.push_back({book.path, book.title, book.author});
  }
  return books;
}
#endif

void seedFromRecentBooks(std::vector<FinishedBookEntry>& entries) {
  const auto recentBooks = recoveryBooks();
  entries.reserve(std::min(recentBooks.size(), FinishedBooksIndex::MAX_ENTRIES));
  for (const auto& book : recentBooks) {
    const std::string cachePath = cachePathForBookPath(book.path);
    if (cachePath.empty()) {
      continue;
    }
    const BookReadingStats stats = BookReadingStats::load(cachePath);
    if (stats.isCompleted) {
      entries.push_back({pathKey(book.path), book.title, book.author, stats.totalReadingSeconds, stats.startDate,
                         stats.finishedDate});
    }
    if (entries.size() >= FinishedBooksIndex::MAX_ENTRIES) {
      break;
    }
  }
  std::sort(entries.begin(), entries.end(), isNewer);
}
}  // namespace

std::vector<FinishedBookEntry> FinishedBooksIndex::load() {
  std::vector<FinishedBookEntry> entries;
  const bool primaryLoaded = loadPath(INDEX_PATH, entries);
  bool loaded = primaryLoaded;
  if (!primaryLoaded && Storage.exists(INDEX_BACKUP_PATH)) {
    LOG_ERR("FBI", "Primary finished-books index invalid, trying backup");
    loaded = loadPath(INDEX_BACKUP_PATH, entries);
  }
  if (!loaded && !Storage.exists(INDEX_PATH) && !Storage.exists(INDEX_BACKUP_PATH)) {
    seedFromRecentBooks(entries);
    if (!writeIndex(entries)) {
      LOG_ERR("FBI", "Could not persist one-time recent-books migration");
    }
  }
  std::sort(entries.begin(), entries.end(), isNewer);
  return entries;
}

bool FinishedBooksIndex::record(const std::string& path, const std::string& title, const std::string& author,
                                const BookReadingStats& stats) {
  return recordCanonical(path, std::string{}, title, author, stats);
}

bool FinishedBooksIndex::recordCanonical(const std::string& bookPath, const std::string& legacyCachePath,
                                         const std::string& title, const std::string& author,
                                         const BookReadingStats& stats) {
  auto entries = load();
  const uint64_t key = pathKey(bookPath);
  bool changed = false;

  if (!legacyCachePath.empty() && legacyCachePath != bookPath) {
    const uint64_t legacyKey = pathKey(legacyCachePath);
    const size_t previousSize = entries.size();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [legacyKey](const FinishedBookEntry& entry) { return entry.pathKey == legacyKey; }),
                  entries.end());
    changed = entries.size() != previousSize;
  }

  auto it = std::find_if(entries.begin(), entries.end(),
                         [key](const FinishedBookEntry& entry) { return entry.pathKey == key; });

  if (!stats.isCompleted) {
    if (it == entries.end()) {
      return changed ? writeIndex(entries) : true;
    }
    entries.erase(it);
    return writeIndex(entries);
  }

  const FinishedBookEntry updated{key, title, author, stats.totalReadingSeconds, stats.startDate, stats.finishedDate};
  if (it != entries.end()) {
    if (it->pathKey == updated.pathKey && it->title == updated.title && it->author == updated.author &&
        it->totalReadingSeconds == updated.totalReadingSeconds &&
        compareReadingStatsDate(it->startDate, updated.startDate) == 0 &&
        compareReadingStatsDate(it->finishedDate, updated.finishedDate) == 0) {
      return changed ? writeIndex(entries) : true;
    }
    *it = updated;
  } else {
    entries.push_back(updated);
  }

  std::sort(entries.begin(), entries.end(), isNewer);
  if (entries.size() > MAX_ENTRIES) {
    entries.resize(MAX_ENTRIES);
  }
  return writeIndex(entries);
}

bool FinishedBooksIndex::migratePath(const std::string& oldPath, const std::string& newPath) {
  if (oldPath == newPath) {
    return true;
  }

  auto entries = load();
  const uint64_t oldKey = pathKey(oldPath);
  const uint64_t newKey = pathKey(newPath);
  auto oldEntry = std::find_if(entries.begin(), entries.end(),
                               [oldKey](const FinishedBookEntry& entry) { return entry.pathKey == oldKey; });
  if (oldEntry == entries.end()) {
    return true;
  }

  const auto duplicate = std::find_if(entries.begin(), entries.end(),
                                      [newKey](const FinishedBookEntry& entry) { return entry.pathKey == newKey; });
  if (duplicate != entries.end() && duplicate != oldEntry) {
    entries.erase(duplicate);
    oldEntry = std::find_if(entries.begin(), entries.end(),
                            [oldKey](const FinishedBookEntry& entry) { return entry.pathKey == oldKey; });
  }

  oldEntry->pathKey = newKey;
  return writeIndex(entries);
}

#ifdef READING_STATS_TEST
void FinishedBooksIndex::setRecentBooksForTest(const std::vector<FinishedBookRecoveryBook>& books) {
  testRecentBooks = books;
}

void FinishedBooksIndex::clearRecentBooksForTest() { testRecentBooks.clear(); }
#endif
