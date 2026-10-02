#pragma once

// Direct TTF downloader for the native-TTF (PSRAM) device class. Sibling of
// FontDownloadActivity, not a subclass: the two share the Wi-Fi → manifest →
// family list → download flow but nothing else — the catalog ships raw sfnt
// files with a path-shaped layout, where the .cpfont flow ships opaque
// binaries keyed by family + point size. Only one of the two is ever compiled:
// FontDownloadActivity for the C3, this one where CROSSPOINT_TTF_READER is set.

#if defined(CROSSPOINT_TTF_READER)

#include <memory>
#include <string>
#include <vector>

#include "FontInstaller.h"
#include "SdCardFont.h"
#include "activities/UiListActivity.h"

// JSON schema version of the TTF catalog manifest. Kept equal to the .cpfont
// manifest's FONTS_MANIFEST_VERSION: both describe the same document shape for
// different payloads, and the tooling bumps them together. Bump this whenever
// the catalog schema changes.
#define FONTS_MANIFEST_VERSION 1

#ifndef TTF_FONTS_MANIFEST_URL
// Catalog manifest served from the public R2 bucket that also hosts the raw
// .ttf files. The .cpfont catalog keeps its own FONT_MANIFEST_URL (GitHub
// release assets), which is why the constant is not shared.
#define TTF_FONTS_MANIFEST_URL "https://pub-794e4fbb87c8444b952f6a2dd026c7b1.r2.dev/manifest/fonts.json"
#endif

class TtfFontDownloadActivity final : public UiListActivity {
 public:
  explicit TtfFontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == LOADING_MANIFEST || state_ == DOWNLOADING ||
           // The download is synchronous and blocks the main loop until it
           // completes, so activityManager.preventAutoSleep() is never polled
           // during downloading.
           state_ == COMPLETE || state_ == ERROR;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  // Bit per style in a manifest file's "style" token.
  enum StyleFlag : uint8_t {
    STYLE_REGULAR = 1 << 0,
    STYLE_BOLD = 1 << 1,
    STYLE_ITALIC = 1 << 2,
    STYLE_BOLD_ITALIC = 1 << 3,
  };

  // Byte offset into stringArena_; 0 is the empty string.
  using StrRef = uint32_t;

  struct TtfManifestFile {
    StrRef name = 0;
    // Manifest-relative path ("<dir>/<name>"): the base-URL suffix to fetch
    // from and the on-SD suffix to write under the fonts root.
    StrRef path = 0;
    uint32_t size = 0;
    uint32_t crc32 = 0;
    uint8_t styleFlags = 0;
  };

  struct TtfManifestFamily {
    StrRef name = 0;
    StrRef description = 0;
    // Directory component of every file's path; the folder on SD. Kept
    // separate from name because the catalog's display names carry spaces
    // ("Atkinson Hyperlegible") while its paths do not ("AtkinsonHyperlegible").
    StrRef dirName = 0;
    // Range into files_, which holds every family's files back to back.
    uint32_t fileStart = 0;
    uint32_t fileCount = 0;
    uint32_t totalSize = 0;
    // Union of its files' style bits, for the row subtitle.
    uint8_t styleFlags = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;

  // Manifest data
  std::string baseUrl_;
  // Reused for every file of every family: downloadToFile takes a std::string,
  // so a char buffer would just build a temporary per call.
  std::string downloadUrl_;
  // Manifest strings, null-terminated and packed back to back.
  std::unique_ptr<char[]> stringArena_;
  uint32_t arenaUsed_ = 0;
  uint32_t arenaCapacity_ = 0;
  std::vector<TtfManifestFamily> families_;
  // Every family's files back to back; sized once from the manifest, so it is
  // allocated nothrow like the arena rather than through vector::reserve.
  std::unique_ptr<TtfManifestFile[]> files_;
  uint32_t fileEntryCount_ = 0;

  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  std::string errorMessage_;
  bool cancelRequested_ = false;
  // Set when the cancel came from the home gesture (consumed by the download
  // callback's own input pump); exit to home after the abort unwinds.
  bool goHomeRequested_ = false;

  // Row cache. Rebuilt only when the visible list changes, never for cursor
  // movement or tap flash repaints.
  std::vector<std::string> rowLabels_;
  std::vector<freeink::ui::ListItem> rowItems_;
  bool rowsDirty_ = true;
  void rebuildRowItems();
  void rebuildFamilyRowItems();

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  freeink::ui::ListNav& activeNav() override;
  void onBackButton() override;
  // Non-list states (loading, downloading, complete, error) consume the loop
  // pass here; the family list uses the base list protocol.
  bool handleCustomInput() override;

  void activateSelected();

  // Maps a manifest "style" token to its StyleFlag bits. Unknown tokens yield
  // 0: a catalog that grows a style this firmware does not know still
  // installs, because BookFontLoader infers the style from the filename.
  static uint8_t parseStyleToken(const char* token);
  // Human-readable style list for a family's row subtitle, e.g.
  // "Regular, Bold, Italic".
  static std::string formatStyles(uint8_t styleFlags);
  // Splits "<dir>/<name>.ttf" and validates both halves; see the definition for
  // the exact rules.
  static bool splitFilePath(const char* path, char* dirBuf, size_t dirBufSize, const char*& baseName);
  // Removes a family's folder after a failed or aborted download.
  void abandonFamily(TtfManifestFamily& family);

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  // cppcheck-suppress arithOperationsOnVoidPointer // unique_ptr<char[]>::get() is char*, not void*
  const char* str(StrRef ref) const { return stringArena_ ? stringArena_.get() + ref : ""; }
  // Returns false if the string does not fit the arena reserved for the manifest.
  bool internString(const char* text, StrRef& outRef);
  void clearManifest();
  void downloadFamily(TtfManifestFamily& family);
  void downloadAll();
  void updateAll();
  static bool computeFileCrc32(const char* path, uint32_t& outCrc);
  // Reads a family's files from disk and sets installed/hasUpdate from what is
  // there: every file present at the manifest's size means installed, any file
  // present but short of that means an update is due.
  void refreshInstalledState(TtfManifestFamily& family);
  bool showDownloadAllRow() const;
  bool showUpdateAllRow() const;
  int specialRowCount() const;
  bool isDownloadAllRow(int index) const;
  bool isUpdateAllRow(int index) const;
  bool isSelectedFamilyDeletable() const;
  void promptDeleteSelectedFamily();
  void onDeleteConfirmationResult(const ActivityResult& result);
  int familyIndexFromList(int listIndex) const;
  int listItemCount() const;
  size_t totalDownloadSize() const;
  size_t totalUpdateSize() const;
  static std::string formatSize(size_t bytes);
};

#endif  // CROSSPOINT_TTF_READER