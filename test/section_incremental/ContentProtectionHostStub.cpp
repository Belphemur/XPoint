// Host stand-in for the firmware-side SD binding (lib/Epub/ContentProtection.cpp):
// host tests never open protected books, so the open always reports plain content.

#include <ContentProtection.h>

namespace freeink {
namespace content {

std::unique_ptr<ContentDecryptor> openProtectedBook(const std::string&, std::string&) { return nullptr; }

}  // namespace content
}  // namespace freeink