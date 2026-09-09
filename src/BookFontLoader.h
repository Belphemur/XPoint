#ifndef BOOK_FONT_LOADER_H
#define BOOK_FONT_LOADER_H

#include <cstdint>
#include <string_view>
#include <vector>
#include <memory>

namespace freeink::book
{

using FontFingerprint = uint64_t;

struct FontFaceInfo
{
    std::string_view familyName;
    std::string_view styleName;
    uint16_t faceIndex;
    FontFingerprint fingerprint;
    bool loaded;
};

class BookFontLoader
{
public:
    BookFontLoader() = default;
    ~BookFontLoader() = default;

    void scanFonts(const char* fontPath);
    const std::vector<FontFaceInfo>& getDiscoveredFaces() const;

    FontFingerprint computeFingerprint(const uint8_t* data, size_t size) const;
    FontFingerprint computeFingerprint(const std::string_view& data) const;

    static BookFontLoader& getInstance()
    {
        static BookFontLoader instance;
        return instance;
    }

private:
    std::vector<FontFaceInfo> discoveredFaces_;
    uint32_t fnvOffsetBias_;

    void updateFnvOffsetBias();
};

} // namespace freeink::book

#endif // BOOK_FONT_LOADER_H