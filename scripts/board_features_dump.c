// Host-side board capability dump. Compiles BoardConfig.h's real board
// profiles (the single source of truth) and prints, per FREEINK_DEVICE_*
// device name, the OR of its boards' long-press-relevant hardware features:
//
//   home_key    — BoardConfig TouchConfig::hasHomeKey
//   menu_button — InputPins::confirm != PIN_UNASSIGNED OR TouchConfig::synthesizeConfirm
//
// stdout is a JSON object keyed by device name (consumed by
// scripts/gen_board_features.py); stderr carries one "# ..." comment line per
// board for the generator's audit log.
//
// Host build (C++, not the ESP toolchain — BoardConfig.h uses enum class /
// constexpr):
//
//   g++ -std=gnu++20 -x c++ -I scripts/hoststubs \
//       -I freeink-sdk/libs/hardware/BoardConfig/include \
//       scripts/board_features_dump.c -o build/board_features_dump
//
// scripts/hoststubs/ contains minimal stand-ins for the ESP-only includes
// (<Arduino.h>, <driver/gpio.h>, <esp_rom_sys.h>); the dump never executes
// them.
//
// BoardConfig.h's build-composition check requires a coherent single-MCU
// device set, but its board profiles are ungated constexpr data — so the dump
// selects the C3 pair (X3+X4) and reads every board's profile constant
// directly rather than through selectDevice().

#define FREEINK_DEVICE_X4 1
#define FREEINK_DEVICE_X3 1
#include <BoardConfig.h>

#include <cstdio>
#include <cstring>

namespace {

struct BoardEntry {
  BoardConfig::Board board;
  const char* device;   // FREEINK_DEVICE_* macro suffix (JSON key)
  const char* profile;  // profile constant name, for the audit log
  const BoardConfig::BoardProfile* profileRef;
};

// Board → device mapping. The SDK's Board enum has 16 values but 15 device
// flags: XteinkX3 and XteinkX3Uc8279 are two profiles of the FREEINK_DEVICE_X3
// binary (mirroring BoardConfig.h's selectDevice(), which selects both under
// FREEINK_DEVICE_X3). Device flags for a shared key are OR'd.
constexpr BoardEntry ENTRIES[] = {
    {BoardConfig::Board::XteinkX4, "X4", "XTEINK_X4", &BoardConfig::XTEINK_X4},
    {BoardConfig::Board::XteinkX3, "X3", "XTEINK_X3", &BoardConfig::XTEINK_X3},
    {BoardConfig::Board::XteinkX3Uc8279, "X3", "XTEINK_X3_UC8279", &BoardConfig::XTEINK_X3_UC8279},
    {BoardConfig::Board::XteinkX4Pro, "X4PRO", "XTEINK_X4_PRO", &BoardConfig::XTEINK_X4_PRO},
    {BoardConfig::Board::XteinkX4Classic, "X4CLASSIC", "XTEINK_X4_CLASSIC", &BoardConfig::XTEINK_X4_CLASSIC},
    {BoardConfig::Board::M5StackPaperColor, "M5", "M5STACK_PAPER_COLOR", &BoardConfig::M5STACK_PAPER_COLOR},
    {BoardConfig::Board::MurphyM3, "MURPHY", "MURPHY_M3", &BoardConfig::MURPHY_M3},
    {BoardConfig::Board::MurphyM4, "MURPHY_M4", "MURPHY_M4", &BoardConfig::MURPHY_M4},
    {BoardConfig::Board::DeLink, "DELINK", "DE_LINK", &BoardConfig::DE_LINK},
    {BoardConfig::Board::LilyGoT5S3, "LILYGO", "LILYGO_T5S3", &BoardConfig::LILYGO_T5S3},
    {BoardConfig::Board::M5PaperV11, "M5PAPER", "M5PAPER_V11", &BoardConfig::M5PAPER_V11},
    {BoardConfig::Board::Sticky, "STICKY", "STICKY", &BoardConfig::STICKY},
    {BoardConfig::Board::PaperMono, "PAPERMONO", "PAPER_MONO", &BoardConfig::PAPER_MONO},
    {BoardConfig::Board::M5PaperS3, "PAPERS3", "M5PAPER_S3", &BoardConfig::M5PAPER_S3},
    {BoardConfig::Board::EegoA4, "EEGO_A4", "EEGO_A4", &BoardConfig::EEGO_A4},
    {BoardConfig::Board::OnePage, "ONEPAGE", "ONEPAGE", &BoardConfig::ONEPAGE},
};

struct DeviceFlags {
  const char* device;
  bool homeKey;
  bool menuButton;
};

constexpr size_t ENTRY_COUNT = sizeof(ENTRIES) / sizeof(ENTRIES[0]);
constexpr size_t DEVICE_COUNT = 15;  // 16 boards, X3's two profiles share a device key

bool menuButton(const BoardConfig::BoardProfile& profile) {
  return profile.input.confirm != BoardConfig::PIN_UNASSIGNED || profile.touch.synthesizeConfirm;
}

}  // namespace

int main() {
  DeviceFlags flags[DEVICE_COUNT] = {};
  size_t deviceCount = 0;

  for (size_t i = 0; i < ENTRY_COUNT; ++i) {
    const BoardEntry& entry = ENTRIES[i];
    const bool homeKey = entry.profileRef->touch.hasHomeKey;
    const bool menuBtn = menuButton(*entry.profileRef);

    std::fprintf(stderr, "# board=%s profile=%s device=%s home_key=%s menu_button=%s\n", entry.device, entry.profile,
                 entry.device, homeKey ? "true" : "false", menuBtn ? "true" : "false");

    size_t slot = deviceCount;
    for (size_t d = 0; d < deviceCount; ++d) {
      if (std::strcmp(flags[d].device, entry.device) == 0) {
        slot = d;
        break;
      }
    }
    if (slot == deviceCount) {
      flags[slot].device = entry.device;
      ++deviceCount;
    }
    flags[slot].homeKey = flags[slot].homeKey || homeKey;
    flags[slot].menuButton = flags[slot].menuButton || menuBtn;
  }

  std::printf("{\n");
  for (size_t d = 0; d < deviceCount; ++d) {
    std::printf("  \"%s\": {\"home_key\": %s, \"menu_button\": %s}%s\n", flags[d].device,
                flags[d].homeKey ? "true" : "false", flags[d].menuButton ? "true" : "false",
                d + 1 < deviceCount ? "," : "");
  }
  std::printf("}\n");
  return 0;
}
