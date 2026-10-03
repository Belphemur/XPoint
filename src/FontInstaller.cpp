#include "FontInstaller.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"

FontInstaller::FontInstaller(SdCardFontRegistry& registry) : registry_(registry) {}

bool FontInstaller::isValidFamilyName(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;

  // Reject path traversal
  if (strstr(name, "..") != nullptr) return false;
  if (strchr(name, '/') != nullptr) return false;
  if (strchr(name, '\\') != nullptr) return false;

  for (const char* p = name; *p != '\0'; ++p) {
    char c = *p;
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

bool FontInstaller::isValidCpfontFilename(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;

  // Reject path separators / traversal up front. Anything that could escape
  // the family directory or refer to a different one is a hard reject.
  if (strstr(name, "..") != nullptr) return false;
  if (strchr(name, '/') != nullptr) return false;
  if (strchr(name, '\\') != nullptr) return false;

  // Must end with ".cpfont" exactly.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  size_t nameLen = strlen(name);
  if (nameLen <= kExtLen) return false;
  if (strcmp(name + nameLen - kExtLen, kExt) != 0) return false;

  // Basename (before .cpfont) must be alphanumeric + hyphen + underscore only.
  // No additional dots — keeps stray "Foo.cpfont.tmp"-style names out.
  size_t baseLen = nameLen - kExtLen;
  for (size_t i = 0; i < baseLen; ++i) {
    char c = name[i];
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

bool FontInstaller::isValidTtfFilename(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;

  if (strstr(name, "..") != nullptr) return false;
  if (strchr(name, '/') != nullptr) return false;
  if (strchr(name, '\\') != nullptr) return false;

  const size_t nameLen = strlen(name);
  bool hasFontExt = false;
  for (const char* ext : {".ttf", ".otf", ".ttc"}) {
    const size_t extLen = strlen(ext);
    if (nameLen > extLen && strcmp(name + nameLen - extLen, ext) == 0) {
      hasFontExt = true;
      break;
    }
  }
  if (!hasFontExt) return false;

  // Everything before the 4-character extension must be alphanumeric + hyphen
  // + underscore, so no stray dots can smuggle a second extension through.
  constexpr size_t kFontExtLen = 4;
  for (size_t i = 0; i < nameLen - kFontExtLen; ++i) {
    const char c = name[i];
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

bool FontInstaller::isValidTtfFamilyName(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;

  if (strstr(name, "..") != nullptr) return false;
  if (strchr(name, '/') != nullptr) return false;
  if (strchr(name, '\\') != nullptr) return false;

  for (const char* p = name; *p != '\0'; ++p) {
    const char c = *p;
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != ' ') {
      return false;
    }
  }
  return true;
}

bool FontInstaller::ensureFamilyDir(const char* familyName) {
  // Reuse the family's existing root if installed; otherwise pick the
  // default-write root (hidden if no roots exist yet).
  const char* root = SdCardFontRegistry::findFamilyRoot(familyName);
  if (!root) root = SdCardFontRegistry::defaultWriteRoot();

  if (!Storage.exists(root)) {
    if (!Storage.mkdir(root)) {
      LOG_ERR("FONT", "Failed to create fonts dir: %s", root);
      return false;
    }
  }

  char dirPath[160];
  snprintf(dirPath, sizeof(dirPath), "%s/%s", root, familyName);

  if (!Storage.exists(dirPath)) {
    if (!Storage.mkdir(dirPath)) {
      LOG_ERR("FONT", "Failed to create family dir: %s", dirPath);
      return false;
    }
  }
  return true;
}

bool FontInstaller::validateCpfontFile(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("FONT", path, file)) {
    LOG_ERR("FONT", "Cannot open for validation: %s", path);
    return false;
  }

  uint8_t magic[CPFONT_MAGIC_LEN];
  size_t bytesRead = file.read(magic, CPFONT_MAGIC_LEN);
  file.close();

  if (bytesRead < CPFONT_MAGIC_LEN) {
    LOG_ERR("FONT", "File too small: %s (%zu bytes)", path, bytesRead);
    return false;
  }

  if (memcmp(magic, "CPFONT\0\0", CPFONT_MAGIC_LEN) != 0) {
    LOG_ERR("FONT", "Bad magic in: %s", path);
    return false;
  }

  return true;
}

bool FontInstaller::validateTtfFile(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("FONT", path, file)) {
    LOG_ERR("FONT", "Cannot open for validation: %s", path);
    return false;
  }

  uint8_t sig[4];
  const size_t bytesRead = file.read(sig, sizeof(sig));
  file.close();

  if (bytesRead < sizeof(sig)) {
    LOG_ERR("FONT", "File too small: %s (%zu bytes)", path, bytesRead);
    return false;
  }

  // sfnt version tags FreeType accepts: TrueType outlines, CFF ("OTTO"),
  // Apple's "true", the legacy "typ1", and a collection container.
  const uint32_t tag = (static_cast<uint32_t>(sig[0]) << 24) | (static_cast<uint32_t>(sig[1]) << 16) |
                       (static_cast<uint32_t>(sig[2]) << 8) | static_cast<uint32_t>(sig[3]);
  static constexpr uint32_t kSfntVersion = 0x00010000u;
  static constexpr uint32_t kOtto = 0x4F54544Fu;  // 'O''T''T''O'
  static constexpr uint32_t kTrue = 0x74727565u;  // 't''r''u''e'
  static constexpr uint32_t kTyp1 = 0x74797031u;  // 't''y''p''1'
  static constexpr uint32_t kTtcf = 0x74746366u;  // 't''t''c''f'
  if (tag != kSfntVersion && tag != kOtto && tag != kTrue && tag != kTyp1 && tag != kTtcf) {
    LOG_ERR("FONT", "Bad sfnt signature %08x in: %s", tag, path);
    return false;
  }

  return true;
}

void FontInstaller::buildFontPath(const char* family, const char* filename, char* outBuf, size_t outBufSize) {
  // Use the same root selection as ensureFamilyDir: existing install dir wins,
  // otherwise the default-write root.
  const char* root = SdCardFontRegistry::findFamilyRoot(family);
  if (!root) root = SdCardFontRegistry::defaultWriteRoot();
  snprintf(outBuf, outBufSize, "%s/%s/%s", root, family, filename);
}

FontInstaller::Error FontInstaller::deleteTtfFamily(const char* familyName) {
  return deleteFamilyInternal(familyName, /*allowSpaceInName=*/true);
}

FontInstaller::Error FontInstaller::deleteFamily(const char* familyName) {
  return deleteFamilyInternal(familyName, /*allowSpaceInName=*/false);
}

FontInstaller::Error FontInstaller::deleteFamilyInternal(const char* familyName, const bool allowSpaceInName) {
  const bool validName = allowSpaceInName ? isValidTtfFamilyName(familyName) : isValidFamilyName(familyName);
  if (!validName) {
    return Error::INVALID_FAMILY_NAME;
  }

  // A family may exist in either root (or, edge case, both). Remove from both.
  const char* roots[] = {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE};
  bool removedAny = false;
  bool sawAny = false;
  for (const char* root : roots) {
    char dirPath[160];
    snprintf(dirPath, sizeof(dirPath), "%s/%s", root, familyName);
    if (!Storage.exists(dirPath)) continue;
    sawAny = true;
    if (!Storage.removeDir(dirPath)) {
      LOG_ERR("FONT", "Failed to remove family dir: %s", dirPath);
      return Error::SD_WRITE_ERROR;
    }
    removedAny = true;
  }

  if (!sawAny) {
    LOG_DBG("FONT", "Family not found in any fonts root: %s", familyName);
    return Error::OK;  // Already gone
  }
  (void)removedAny;

  // If this was the active font, clear the setting
  if (strcmp(SETTINGS.sdFontFamilyName, familyName) == 0) {
    SETTINGS.clearSdFontFamily();
    LOG_DBG("FONT", "Cleared active SD font (deleted family: %s)", familyName);
  }

  return Error::OK;
}

void FontInstaller::refreshRegistry() { registry_.discover(); }

bool FontInstaller::isFamilyInstalled(const char* familyName) const {
  return registry_.findFamily(familyName) != nullptr;
}
