#include "QuickPageCapture.h"

#include <cstring>

bool QuickPageCapture::capture(const freeink::book::Page& page) {
  valid_ = false;
  stringUsed_ = 0;
  if (buffer_ == nullptr) return false;
  if (page.runCount > kMaxRuns || page.linkCount > kMaxLinks || page.imageCount > kMaxImages ||
      page.ruleCount > kMaxRules || page.rubyCount > kMaxRubies) {
    return false;
  }

  // Validate the string budget up front so a failed capture cannot leave a
  // partially overwritten bump region behind.
  size_t stringNeed = 0;
  for (uint16_t i = 0; i < page.runCount; ++i) stringNeed += page.runs[i].len;
  for (uint16_t i = 0; i < page.linkCount; ++i) {
    stringNeed += strlen(page.links[i].target) + 1;
    if (page.links[i].fragment != nullptr) stringNeed += strlen(page.links[i].fragment) + 1;
  }
  for (uint16_t i = 0; i < page.imageCount; ++i) stringNeed += strlen(page.images[i].href) + 1;
  for (uint16_t i = 0; i < page.rubyCount; ++i) stringNeed += strlen(page.rubies[i].text) + 1;
  if (kStringsOff + stringNeed > cap_) return false;

  auto* runs = reinterpret_cast<freeink::book::PageTextRun*>(buffer_ + kRunsOff);
  auto* links = reinterpret_cast<freeink::book::PageLink*>(buffer_ + kLinksOff);
  auto* images = reinterpret_cast<freeink::book::PageImage*>(buffer_ + kImagesOff);
  auto* rules = reinterpret_cast<freeink::book::PageRule*>(buffer_ + kRulesOff);
  auto* rubies = reinterpret_cast<freeink::book::PageRuby*>(buffer_ + kRubiesOff);

  for (uint16_t i = 0; i < page.runCount; ++i) {
    runs[i] = page.runs[i];
    char* dst = reinterpret_cast<char*>(buffer_ + kStringsOff + stringUsed_);
    memcpy(dst, page.runs[i].text, page.runs[i].len);
    stringUsed_ += page.runs[i].len;
    runs[i].text = dst;
  }
  for (uint16_t i = 0; i < page.linkCount; ++i) {
    links[i] = page.links[i];
    links[i].target = copyString(page.links[i].target);
    links[i].fragment = page.links[i].fragment != nullptr ? copyString(page.links[i].fragment) : nullptr;
  }
  for (uint16_t i = 0; i < page.imageCount; ++i) {
    images[i] = page.images[i];
    images[i].href = copyString(page.images[i].href);
  }
  // Rules carry no strings — geometry only.
  for (uint16_t i = 0; i < page.ruleCount; ++i) rules[i] = page.rules[i];
  for (uint16_t i = 0; i < page.rubyCount; ++i) {
    rubies[i] = page.rubies[i];
    rubies[i].text = copyString(page.rubies[i].text);
  }

  page_ = freeink::book::Page{runs,  page.runCount,  images, page.imageCount, links,          page.linkCount,
                              rules, page.ruleCount, rubies, page.rubyCount,  page.pageIndex, page.charStart};
  valid_ = true;
  return true;
}

const char* QuickPageCapture::copyString(const char* src) {
  if (src == nullptr) return nullptr;
  const size_t len = strlen(src) + 1;
  char* dst = reinterpret_cast<char*>(buffer_ + kStringsOff + stringUsed_);
  memcpy(dst, src, len);
  stringUsed_ += len;
  return dst;
}
