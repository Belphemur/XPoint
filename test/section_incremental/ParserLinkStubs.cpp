// Link stubs for the real Epub/Section/parsers translation units compiled into
// this test. Mirrors test/chapter_html_slim_parser/ParserLinkStubs.cpp, with
// two differences required by the section build path:
//   1. Page/PageLine/PageImage/PageHorizontalRule::serialize() actually write
//      a marker and return true — Section::commitBuildFile() rejects a build
//      whose page LUT contains a zero file offset, which is what the chapter
//      parser test's `return false` stubs would produce.
//   2. Hyphenator::setPreferredLanguage() (called from Section::startBuild).

#include <Epub/Page.h>
#include <Epub/TokenBoundary.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <ImageConverters.h>

const char* lookupHtmlEntity(const char*, size_t) { return nullptr; }

#include <BidiUtils.h>

bool isExplicitHyphen(uint32_t) { return false; }
bool isSoftHyphen(uint32_t) { return false; }

std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string&, bool) { return {}; }
void Hyphenator::setPreferredLanguage(const std::string&) {}

namespace BidiUtils {
bool startsWithRtl(const char*, int) { return false; }
bool computeVisualWordOrder(const std::vector<std::string>& words, bool, std::vector<uint16_t>& order) {
  order.resize(words.size());
  for (size_t index = 0; index < words.size(); ++index) order[index] = static_cast<uint16_t>(index);
  return true;
}
}  // namespace BidiUtils

TextBlock::TextBlock(const std::vector<std::string>&, const std::vector<int16_t>&,
                     const std::vector<EpdFontFamily::Style>&, const std::vector<uint8_t>&,
                     const std::vector<uint16_t>&, const BlockStyle& blockStyle, std::vector<std::string> rubyTexts,
                     std::vector<LinkSpan> linkSpans)
    : blockStyle(blockStyle), rubyTexts(std::move(rubyTexts)), linkSpans(std::move(linkSpans)) {}

bool TextBlock::hasRuby() const { return false; }

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

bool ImageDecoderFactory::isFormatSupported(const std::string&) { return false; }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
bool ImageToFramebufferDecoder::validateAndStoreDimensions(int64_t, int64_t, ImageDimensions&, const char*) {
  return false;
}

namespace {

// Serialized-page marker. Non-zero bytes are what matters: the section file
// must advance so each page LUT entry records a non-zero fileOffset, and the
// next page's entries differ from the previous ones.
constexpr uint32_t kPageMarker = 0x50414745u;  // "PAGE"

bool writePageMarker(HalFile& file) { return file.write(&kPageMarker, sizeof(kPageMarker)) == sizeof(kPageMarker); }

}  // namespace

bool Page::serialize(HalFile& file) const { return writePageMarker(file); }
std::unique_ptr<Page> Page::deserialize(HalFile&) { return nullptr; }

bool PageLine::serialize(HalFile& file) { return writePageMarker(file); }
std::unique_ptr<PageLine> PageLine::deserialize(HalFile&) { return nullptr; }

bool PageImage::serialize(HalFile& file) { return writePageMarker(file); }
std::unique_ptr<PageImage> PageImage::deserialize(HalFile&) { return nullptr; }

bool PageHorizontalRule::serialize(HalFile& file) { return writePageMarker(file); }
std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(HalFile&) { return nullptr; }

void PageLine::render(GfxRenderer&, int, int, int) {}
void PageImage::render(GfxRenderer&, int, int, int) {}
void PageImage::renderPlaceholder(GfxRenderer&, int, int) const {}
void PageHorizontalRule::render(GfxRenderer&, int, int, int) {}

// Host stubs for the cover-image converters (declared in the JpegToBmpConverter.h /
// PngToBmpConverter.h stub headers); Epub.cpp's generateCoverBmp/generateThumbBmp
// are never called by this test.
bool JpegToBmpConverter::jpegFileToBmpStream(HalFile&, HalFile&, bool) { return false; }
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(HalFile&, HalFile&, int, int) { return false; }
bool PngToBmpConverter::pngFileToBmpStream(HalFile&, HalFile&, bool) { return false; }
bool PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(HalFile&, HalFile&, int, int) { return false; }
