#include <Arduino.h>
#include <BoardConfig.h>
#include <BookFontLoader.h>
#include <BookTypes.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <WiFi.h>
#include <XteinkDetect.h>
#include <builtinFonts/all.h>
#include <esp_heap_caps.h>
#include <freertos/task.h>
#if FREEINK_CAP_TOUCH
#include <esp_sntp.h>
#endif
#include <esp_sleep.h>
#include <esp_system.h>

#include <atomic>
#include <cstring>

#include "BoardFeatures.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "ProgressManager.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "platform/UsbSerialJtagHandoff.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;

// Profiling counters for Phase 1 memory instrumentation. g_psram_free_at_boot
// is the boot-time PSRAM baseline (defined here, extern'd in Logging.h). The
// central-directory and file-open counters live in ZipFile.cpp; do not duplicate
// them as static (internal-linkage) here or they will report 0 forever.
size_t g_psram_free_at_boot = 0;

// Defined in the book namespace so it matches the header's
// `extern BookFontLoader fontLoader;` declaration (design §3.2).
namespace freeink {
namespace book {
BookFontLoader fontLoader;
}  // namespace book
}  // namespace freeink

// TTF Phase 0 wiring proof: a link-time reference to the FreeInkBook engine.
// File scope + gnu::used defeats the optimizer on every build variant (valid
// for a namespace-scope variable on both the RISC-V and Xtensa GCC targets).
// gnu::used alone does not guarantee retention under --gc-sections; gnu::retain
// would, but the S3 framework's Xtensa GCC rejects it with -Wattributes at
// namespace scope, so the LOG_DBG reference in setup() anchors the section
// instead.
using BookStatusProbe = const char* (*)(freeink::book::BookStatus);
[[gnu::used]] static const BookStatusProbe bookStatusProbe = &freeink::book::bookStatusName;

// A wake hold must never become an in-app power-button action.  Boot may continue
// while the button is held; swallow the one release that ends that wake gesture.
static bool wakePowerReleasePending = false;

// Fonts
EpdFont notoserif14RegularFont(&notoserif_14_regular);
EpdFont notoserif14BoldFont(&notoserif_14_bold);
EpdFont notoserif14ItalicFont(&notoserif_14_italic);
EpdFont notoserif14BoldItalicFont(&notoserif_14_bolditalic);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont, &notoserif14ItalicFont,
                                    &notoserif14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notoserif_12_regular);
EpdFont notoserif12BoldFont(&notoserif_12_bold);
EpdFont notoserif12ItalicFont(&notoserif_12_italic);
EpdFont notoserif12BoldItalicFont(&notoserif_12_bolditalic);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont, &notoserif12ItalicFont,
                                    &notoserif12BoldItalicFont);
EpdFont notoserif16RegularFont(&notoserif_16_regular);
EpdFont notoserif16BoldFont(&notoserif_16_bold);
EpdFont notoserif16ItalicFont(&notoserif_16_italic);
EpdFont notoserif16BoldItalicFont(&notoserif_16_bolditalic);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont, &notoserif16ItalicFont,
                                    &notoserif16BoldItalicFont);
EpdFont notoserif18RegularFont(&notoserif_18_regular);
EpdFont notoserif18BoldFont(&notoserif_18_bold);
EpdFont notoserif18ItalicFont(&notoserif_18_italic);
EpdFont notoserif18BoldItalicFont(&notoserif_18_bolditalic);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont, &notoserif18ItalicFont,
                                    &notoserif18BoldItalicFont);

EpdFont atkinson_hn12RegularFont(&atkinson_hn_12_regular);
EpdFont atkinson_hn12BoldFont(&atkinson_hn_12_bold);
EpdFont atkinson_hn12ItalicFont(&atkinson_hn_12_italic);
EpdFont atkinson_hn12BoldItalicFont(&atkinson_hn_12_bolditalic);
EpdFontFamily atkinson_hn12FontFamily(&atkinson_hn12RegularFont, &atkinson_hn12BoldFont, &atkinson_hn12ItalicFont,
                                      &atkinson_hn12BoldItalicFont);
EpdFont atkinson_hn14RegularFont(&atkinson_hn_14_regular);
EpdFont atkinson_hn14BoldFont(&atkinson_hn_14_bold);
EpdFont atkinson_hn14ItalicFont(&atkinson_hn_14_italic);
EpdFont atkinson_hn14BoldItalicFont(&atkinson_hn_14_bolditalic);
EpdFontFamily atkinson_hn14FontFamily(&atkinson_hn14RegularFont, &atkinson_hn14BoldFont, &atkinson_hn14ItalicFont,
                                      &atkinson_hn14BoldItalicFont);
EpdFont atkinson_hn16RegularFont(&atkinson_hn_16_regular);
EpdFont atkinson_hn16BoldFont(&atkinson_hn_16_bold);
EpdFont atkinson_hn16ItalicFont(&atkinson_hn_16_italic);
EpdFont atkinson_hn16BoldItalicFont(&atkinson_hn_16_bolditalic);
EpdFontFamily atkinson_hn16FontFamily(&atkinson_hn16RegularFont, &atkinson_hn16BoldFont, &atkinson_hn16ItalicFont,
                                      &atkinson_hn16BoldItalicFont);
EpdFont atkinson_hn18RegularFont(&atkinson_hn_18_regular);
EpdFont atkinson_hn18BoldFont(&atkinson_hn_18_bold);
EpdFont atkinson_hn18ItalicFont(&atkinson_hn_18_italic);
EpdFont atkinson_hn18BoldItalicFont(&atkinson_hn_18_bolditalic);
EpdFontFamily atkinson_hn18FontFamily(&atkinson_hn18RegularFont, &atkinson_hn18BoldFont, &atkinson_hn18ItalicFont,
                                      &atkinson_hn18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&atkinson_hn_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10MediumFont(&ubuntu_10_medium);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10MediumFont, &ui10BoldFont);

EpdFont ui12MediumFont(&ubuntu_12_medium);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12MediumFont, &ui12BoldFont);

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
RTC_NOINIT_ATTR uint32_t silentRebootPayload;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
constexpr uint32_t SILENT_REBOOT_TARGET_SETTINGS = 2;
constexpr uint32_t SILENT_REBOOT_TARGET_MAX = SILENT_REBOOT_TARGET_SETTINGS;
constexpr uint32_t SILENT_REBOOT_LIGHT_ON = 1U << 0;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,          // cold boot, flash, panic, or plain reboot
  Silent,          // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  SplashlessWake,  // wake from deep sleep with the splash suppressed by the SD flag
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

// Latched true before enterPowerOff() tears down the current activity: WiFi
// activities call silentRestart() in onExit() to clear heap fragmentation,
// but a power-off is a full rail cut — rebooting here would power the device
// back up against the user's power gesture. Also makes main-loop sleep
// checks inert while the shutdown teardown runs.
static bool powerOffInProgress = false;

#if FREEINK_CAP_TOUCH
static bool finishWifiSessionWithoutRestart() {
  if (!BoardConfig::hasTouch()) return false;

  // A software reset does not cycle externally powered touch/frontlight rails.
  // Shut down the network stack in place so those peripherals retain state.
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.mode(WIFI_OFF);
  delay(100);
  LOG_DBG("MAIN", "WiFi stopped without restart on touch device");
  return true;
}
#endif

// A silent restart is internal maintenance, so the light must come back exactly
// as the user left it. SETTINGS.frontlightOn is the saved preference and
// legitimately diverges from the live state (a wake with Restore Light on Wake
// off leaves the light off while the saved "was on" preference is kept), so
// carry the live state across the reboot instead of re-deriving it from
// settings. Cleared with the magic in setup().
static void armSilentReboot(const uint32_t target) {
  silentRebootTarget = target;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
}

// Returns instead of rebooting when sleep or power-off supersedes the reboot;
// callers keep running in that case.
static void silentRestartTo(const uint32_t target, const char* targetName) {
  if (deepSleepInProgress || powerOffInProgress) return;  // sleeping/powering off supersedes the heap-defrag reboot
#if FREEINK_CAP_TOUCH
  if (finishWifiSessionWithoutRestart()) return;
#endif
  armSilentReboot(target);
  LOG_DBG("MAIN", "Silent restart (target=%s)", targetName);
  // E-ink retains the previous frame until the target's first paint lands
  // (~2-3s). Without an overlay, users don't see the reboot and fire input
  // through to the new activity. On Home, Select on the default
  // selectorIndex=0 opens the most-recent book, looking like a trampoline back
  // to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestart() { silentRestartTo(SILENT_REBOOT_TARGET_HOME, "home"); }

void silentRestartToReader() { silentRestartTo(SILENT_REBOOT_TARGET_READER, "reader"); }

void silentRestartToSettings() { silentRestartTo(SILENT_REBOOT_TARGET_SETTINGS, "settings"); }

void restartToHomeAfterStorageHandoff() {
  if (deepSleepInProgress || powerOffInProgress) return;  // sleeping/powering off supersedes the storage handoff reboot
  armSilentReboot(SILENT_REBOOT_TARGET_HOME);
  LOG_DBG("MAIN", "Restart after storage handoff (target=home)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  handoffUsbOtgToSerialJtag();
  ESP.restart();
}

void toggleFrontlight() {
#if FREEINK_CAP_FRONTLIGHT
  if (!Frontlight.present()) return;
  const bool lightOn = !Frontlight.isOn();
  Frontlight.setOn(lightOn);
  SETTINGS.frontlightOn = lightOn ? 1 : 0;
  SETTINGS.saveToFile();
  LOG_INF("LIGHT", "Frontlight toggled %s", lightOn ? "on" : "off");
#endif
}

// Run a configured capacitive Home-key action. Returns true when something ran.
// Reader-only actions (Reader Menu) no-op outside the reader; Sleep and
// Screenshot are global. GO_HOME uses goHome() so the home screen re-renders.
void enterDeepSleep(bool fromTimeout);
void enterPowerOff();

// Global Home-key actions run before activity input. Reader-scoped actions are
// consumed by EpubReaderActivity; a single tap is inert on the device home
// screen, while double-tap and hold deliveries remain available there.
bool dispatchGlobalHomeButtonAction() {
  const auto action = mappedInputManager.homeButtonAction();
  if (action == HomeButtonAction::Ignore) return false;
  if (mappedInputManager.homeButtonGesture() == HomeButtonGesture::Tap && activityManager.isOnHomeScreen()) {
    return true;  // consume the frame without delivering the tap action
  }

  switch (action) {
    case HomeButtonAction::ToggleFrontlight:
      toggleFrontlight();
      return false;  // the frame may still carry ordinary input
    case HomeButtonAction::Refresh:
      LOG_DBG("MAIN", "Manual screen refresh triggered");
      if (!activityManager.handleForcedRefresh()) {
        RenderLock lock;
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      return false;
    case HomeButtonAction::Sleep:
      LOG_INF("MAIN", "Sleep triggered by Home-key shortcut");
      enterDeepSleep(false);
      return true;
    case HomeButtonAction::Screenshot: {
      RenderLock lock;
      ScreenshotUtil::takeScreenshot(renderer);
      return true;
    }
    case HomeButtonAction::GoBack:
      // The current activity gets first claim: in the reader this closes an
      // open popup/panel/toolbar sheet instead of leaving the book. At the top
      // of the stack it falls back to the home screen (mirroring the X4's
      // left-edge back swipe).
      if (activityManager.handleBackOnCurrent()) return true;
      activityManager.popActivity();
      return true;
    default:
      return false;
  }
}

bool handleX4ProFrontlightDoubleClick() {
  // Window ownership moved into MappedInputManager (soak-fix7 JFhK): the
  // manager holds the first release out of the served mask until the window
  // resolves, so Sleep/Force-Refresh handlers cannot fire on an ambiguous
  // click. This consumes only the resolved double-click verdict.
  if (mappedInputManager.consumePowerDoubleClick()) {
    toggleFrontlight();
    return true;
  }
  return false;
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Stage the shutdown cover for the power off screen while the book is still
// available; at wake it is only read back and painted. requireTimerEnabled
// gates staging on the auto power off setting for deep-sleep entry; manual
// power off passes false. Only modes whose shutdown screen shows the staged
// cover (COVER, or COVER_CUSTOM from the reader) pay for cover generation;
// renderShutdownScreen falls back per sleepScreen otherwise.
static void stageAutoPowerOffCover(bool requireTimerEnabled) {
  APP_STATE.autoPowerOffCoverBmpPath.clear();
  if (requireTimerEnabled && SETTINGS.getAutoPowerOffMs() == 0) return;
  const auto sleepMode = static_cast<CrossPointSettings::SLEEP_SCREEN_MODE>(SETTINGS.sleepScreen);
  const bool coverShown =
      sleepMode == CrossPointSettings::SLEEP_SCREEN_MODE::COVER ||
      (sleepMode == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM && APP_STATE.lastSleepFromReader);
  if (!coverShown) return;
  if (APP_STATE.openEpubPath.empty()) return;
  std::string coverPath;
  if (SleepActivity::resolveCoverBmpPath(renderer, APP_STATE.openEpubPath, coverPath)) {
    APP_STATE.autoPowerOffCoverBmpPath = std::move(coverPath);
  }
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  // Every sleep mode leaves a complete retained frame on the e-ink panel. Keep
  // it visible until the first useful reader or home paint replaces it.
  APP_STATE.showBootScreen = false;

  stageAutoPowerOffCover(true);

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  } else if (Storage.exists(SLEEP_FRAME_FILE)) {
    // A stale Quick Resume frame must not replace the selected sleep screen during wake.
    Storage.remove(SLEEP_FRAME_FILE);
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  halTiltSensor.deepSleep();
  display.deepSleep();
  Storage.prepareForDeepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  uint64_t autoPowerOffUs = 0;
  if (const uint32_t apOffMs = SETTINGS.getAutoPowerOffMs(); apOffMs > 0) {
    autoPowerOffUs = static_cast<uint64_t>(apOffMs) * 1000ULL;
    // Stage the stock-parity shutdown marker at sleep entry so it survives
    // even if the timer wake crashes before the setup()-side re-stage.
    // Non-timer wakes suppress it below before takeLastShutdownKind().
    powerManager.stageAutoPowerOff();
  }
  powerManager.startDeepSleep(gpio, autoPowerOffUs);
}

// Manual power off: render the shutdown screen, then cut power. Unlike deep
// sleep there is no wake timer — the next power-button press is a normal cold
// boot (same next-boot state as the auto power off shutdown).
void enterPowerOff() {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for shutdown preparation
  // Snapshot the from-reader context BEFORE teardown: shutdown() empties the
  // activity stack, so isReaderActivity() would always be false afterwards
  // and COVER_CUSTOM / resume context would be lost (Copilot+CodeRabbit,
  // PR #107). Also latch the shutdown FIRST so a WiFi activity's onExit()
  // cannot silentRestart() its way into a reboot instead of a power-off.
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
  powerOffInProgress = true;
  // Run the outgoing activity's onExit() — enterPowerOff() is reachable from
  // ANY activity, and this is what commits the reader's session state
  // (progress flush, stats commit) exactly like goToSleep() does before
  // replaceActivity() (review B2 / user directive, PR #107). The reader's
  // own exit path flushes progress via the saver.
  activityManager.shutdown();
  // Belt-and-braces: any capture that raced the exit above still lands.
  progressManager.flushNow();
  // COVER_CUSTOM branch reads this; the timer-wake path reads the value
  // persisted at deep-sleep entry instead.
  stageAutoPowerOffCover(false);
  {
    RenderLock lock;
    SleepActivity::renderShutdownScreen(renderer);  // Clears APP_STATE.autoPowerOffCoverBmpPath after paint
  }
  // Persist the post-off state: splashless wake on the next power-button
  // press, and no stale staged cover path in the settings file.
  APP_STATE.showBootScreen = false;
  APP_STATE.saveToFile();
  // A stale Quick Resume frame must not replace the shutdown screen on the
  // next boot.
  Storage.remove(SLEEP_FRAME_FILE);

  // Same teardown as enterDeepSleep(): the modem power domain must not be
  // held alive across the rail cut.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  // Park the panel BEFORE the sink cuts its rail: the SSD1677 must receive its
  // deep-sleep command while the rail that powers it is still up (same ordering
  // rule as enterDeepSleep(); CodeRabbit finding).
  display.deepSleep();
  LOG_INF("MAIN", "Powering off");
  // Stock-parity shutdown marker (ghidra_poweroff_report.md): record WHY we are
  // powering off in RTC slow RAM; the next boot reports + clears it.
  powerManager.stageUserPowerOff();
  powerManager.enterPowerOffSleep(gpio);  // [[noreturn]]
}

void setupDisplayAndFonts(bool seamless = false) {
#if !FREEINK_MCU_C3
  // C3 resolves its controller in HalGPIO::begin() before SPI claims the
  // display pins. X4 Pro skips that C3-only path, so probe here before
  // display.begin() selects and initializes its panel driver.
  static bool controllerResolved = false;
  if (!controllerResolved) {
    controllerResolved = true;
    if (freeink::applyXteinkDisplayController()) {
      LOG_DBG("MAIN", "Panel controller: UltraChip UC81xx variant detected");
    }
  }
#endif

  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

  renderer.insertFont(ATKINSON_HN_12_FONT_ID, atkinson_hn12FontFamily);
  renderer.insertFont(ATKINSON_HN_14_FONT_ID, atkinson_hn14FontFamily);
  renderer.insertFont(ATKINSON_HN_16_FONT_ID, atkinson_hn16FontFamily);
  renderer.insertFont(ATKINSON_HN_18_FONT_ID, atkinson_hn18FontFamily);
  // NOTOSANS_* are macro aliases of the ATKINSON_HN_* IDs (implicit migration
  // for existing settings), so the registrations above already cover them.
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  // Native-TTF reader font discovery (design §14.1): gated so PSRAM-less
  // builds never scan or allocate for the native-TTF path.
#if defined(CROSSPOINT_TTF_READER)
  freeink::book::fontLoader.begin();
#endif

  LOG_DBG("MAIN", "Fonts setup");
}

// loopTask override: ALL rendering runs on the loop task since the Adobe CFF
// engine (faster; chosen over the old "freetype" CFF engine, SDK PR #29/#30)
// interprets charstrings with an unbounded, multi-KB caller stack. 40KB is
// the EPub-InkPlate-proven size for the Adobe engine on ESP32; 48KB adds
// margin for the reader's ChapterLayout rebuild + paint on top of it.
// Replaces the ActivityManagerRender task (16KB) — net task-stack budget
// unchanged, one fewer task, no cross-task FreeType calls. Rebuilds are
// UX-modal anyway ("Indexing" popup), so blocking the loop is accepted.
SET_LOOP_TASK_STACK_SIZE(49152)

void setup() {
  BoardConfig::holdPowerRails();

#ifdef ENABLE_SERIAL_LOG
#ifdef CROSSPOINT_WAIT_FOR_USB_SERIAL
  // Development builds preserve reliable early CDC logs; release builds let
  // enumeration proceed asynchronously so users do not pay this startup cost.
  delay(250);
#endif
  Serial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
#if defined(BOARD_HAS_PSRAM)
  // The ESP32-S3 Arduino framework's early system-init (priority 99) calls
  // psramInit() during boot, but on some X4 Pro boards it fails silently
  // (esp_psram_init returns an error), leaving spiramDetected=false and
  // psramFound()=false. The result is heap_caps_malloc(MALLOC_CAP_SPIRAM)
  // returns nullptr, which silently disables the ZipFileCache and other
  // PSRAM-backed buffers -- turning a 380 KB DRAM ceiling into a death by
  // a thousand SD re-reads during section building.
  // Retry psramInit() here, after the SDMMC peripheral clocks are up but
  // before any PSRAM allocation is attempted.
  if (!psramFound()) {
    const bool ok = psramInit();
    if (ok) {
      // psramInit() only sets the spiramDetected flag; psramAddToHeap()
      // actually registers PSRAM with the heap allocator so heap_caps_malloc
      // with MALLOC_CAP_SPIRAM can use it.
      psramAddToHeap();
    }
    LOG_INF("SYS", "PSRAM init: %s (free=%u bytes)", ok ? "OK" : "FAILED", ESP.getFreePsram());
  }
#endif
  g_psram_free_at_boot = ESP.getFreePsram();
  logMemAt("boot");
  // checkPanic() clears the watchdog capture marker after a successful SD
  // dump, so retain the boot classification for the later activity route.
  const bool rebootedFromPanic = HalSystem::isRebootFromPanic();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_MAX) ? silentRebootTarget : 0;
  const bool silentRebootLightOn = isSilentReboot && (silentRebootPayload & SILENT_REBOOT_LIGHT_ON) != 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  silentRebootPayload = 0;

  gpio.begin();
  powerManager.begin();
  progressManager.begin();

  // Determine the wake cause BEFORE consuming the shutdown marker: if the
  // previous session staged an auto-off marker at sleep entry but the user
  // interrupted the dwell (button wake), suppress the marker so the boot log
  // and downstream routing don't treat a user-interrupted sleep as a clean
  // power-off.
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason != HalGPIO::WakeupReason::Timer && SETTINGS.getAutoPowerOffMs() > 0) {
    // Non-timer wake with auto-off configured: the staged auto-off marker is a
    // false positive — suppress it before takeLastShutdownKind() reads it.
    powerManager.clearShutdownMarker();
  }

  // Stock-parity shutdown marker (ghidra_poweroff_report.md): if the previous
  // session ended in a power off (manual or auto), RTC slow RAM carries the
  // magic + reason byte. Read + clear exactly once per boot and log it.
  const auto lastShutdown = HalPowerManager::takeLastShutdownKind();
  if (lastShutdown != HalPowerManager::ShutdownKind::None) {
    LOG_INF("MAIN", "Previous session ended in a clean power off (%s)",
            lastShutdown == HalPowerManager::ShutdownKind::AutoOff ? "auto-power-off" : "user power-off");
  }
  // Sample the wake hold now — a click wake is released within milliseconds of
  // boot — but defer the sleep-or-boot decision until SETTINGS is loaded below:
  // click-to-wake is a setting, and an X4 battery power-off cuts all power, so
  // only SD state survives to the next boot.
  //
  // Deep-sleep wakes skip verification: the EXT1/GPIO wake source armed at sleep
  // entry only fires on a real power-button press, but the press is already
  // released by the time verifyPowerButtonWakeup() samples (chip reset +
  // begin() work delay) — so a held-stability check would eat every short click
  // wake. Only the post-power-off (cold-boot) path actually has a held button to
  // verify, and that's the ghost-wake debounce the SETTINGS check covers.
  const bool wakeHoldVerified =
      wakeupReason != HalGPIO::WakeupReason::PowerButton || gpio.wokeFromDeepSleep() || gpio.verifyPowerButtonWakeup();

  // X4 Pro and X4 Classic both map BTN_UP to GPIO0 — an ESP32-S3 boot strap — so
  // gate recovery on the non-strap Down key (GPIO7) to avoid a stuck-in-recovery loop.
  const auto recoveryButton = (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? MappedInputManager::Button::Down
                                                                                     : MappedInputManager::Button::Up;
  const bool recoveryFirmwareMode = wakeupReason == HalGPIO::WakeupReason::PowerButton && !BoardConfig::isPaperMono() &&
                                    mappedInputManager.isPressed(recoveryButton);

  halTiltSensor.begin();
  halClock.begin();

#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
#else
  LOG_INF("MAIN", "Device: %s", BoardConfig::ACTIVE.name);
#endif

  // Surface RTC availability, the system epoch the zone resolver will use, and
  // the IANA id / cached offset driving the first render. The epoch here comes
  // straight from HalClock::begin()'s RTC seed, so it is wall-clock time on
  // boards with an RTC and a seconds-since-boot counter on boards without.
  {
    const time_t bootEpoch = time(nullptr);
    char rtcBuf[16] = "--:--";
    uint8_t rh = 0, rm = 0;
    if (halClock.getTime(rh, rm)) {
      snprintf(rtcBuf, sizeof(rtcBuf), "%02u:%02u", rh, rm);
    }
    LOG_INF("CLK", "boot: rtcAvailable=%d rtcLocal=%s sysEpoch=%lld tzId='%s' tzOffsetMin=%d hasBeenSynced=%u",
            halClock.isAvailable() ? 1 : 0, rtcBuf, static_cast<long long>(bootEpoch), SETTINGS.clockTimeZoneId,
            static_cast<int>(SETTINGS.clockTzOffsetMin), static_cast<unsigned>(SETTINGS.clockHasBeenSynced));
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  APP_STATE.loadFromFile();
  const bool isSleepWake = wakeupReason == HalGPIO::WakeupReason::PowerButton;
  const bool isPersistedSleepWake = isSleepWake && !APP_STATE.showBootScreen;

  if (recoveryFirmwareMode) {
    LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)",
            (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? "DOWN" : "UP");
  }

  // Touch boards default the reader menu to the toolbar overlay instead of the
  // full-screen list. Seeded before the load: fromJson() falls back to the
  // in-memory value only when the file carries no readerMenuStyle key, so a
  // user's saved choice (either style) still wins.
  if (gpio.hasTouch()) {
    SETTINGS.readerMenuStyle = CrossPointSettings::READER_MENU_TOOLBAR;
  }
  SETTINGS.loadFromFile();

  // Auto power off: the dwell timer woke us, so the sleep entry's staged marker
  // was consumed above as a genuine AutoOff shutdown. Re-stage it for the next
  // boot so the downstream timer-wake intercept (render shutdown screen + sink)
  // has the marker to report; the only re-stage that matters is on THIS wake
  // where the auto-off dwell actually elapsed.
  if (wakeupReason == HalGPIO::WakeupReason::Timer && SETTINGS.getAutoPowerOffMs() > 0) {
    powerManager.stageAutoPowerOff();
  }

  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  // TTF Phase 0 wiring proof: the FreeInkBook engine is linked but unused.
  // The reference lives at file scope (bookStatusProbe above); the log line
  // is the visible smoke trace only where logging is compiled in.
  LOG_DBG("MAIN", "Book engine linked: bookStatusName(Ok)=%s, vendor=%s",
          freeink::book::bookStatusName(freeink::book::BookStatus::Ok), freeink::book::vendorVersions());
  // Unconditional live reference: LOG_DBG compiles out below LOG_LEVEL 2, so
  // this volatile read is what anchors the probe section against linker GC
  // ([[gnu::used]] does not protect a section from garbage collection).
  [[maybe_unused]] const volatile BookStatusProbe keepAlive = bookStatusProbe;

  // Brightness and warmth are always restored. A normal wake starts with the
  // light off unless Restore Light on Wake is enabled; silent maintenance
  // reboots replay the live state captured at restart, so they neither go dark
  // nor light up against the user's wake preference.
  const bool restoreLightOn =
      isSilentReboot ? silentRebootLightOn : (SETTINGS.frontlightOn != 0 && SETTINGS.frontlightRestoreOnWake != 0);
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      // With Short Power Button Press = Sleep, a single click wakes on any
      // device; otherwise the button must still be held (ghost-wake debounce).
      if (!wakeHoldVerified && SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::SLEEP) {
        LOG_DBG("MAIN", "Power-button wake not held through verification, sleeping");
        Storage.prepareForDeepSleep();
        powerManager.startDeepSleep(gpio);
      }
      wakePowerReleasePending = true;
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // Most devices return to sleep after a USB-powered cold boot.
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
#if FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_EEGO_A4
      // X4 Pro must stay awake so USB Serial/JTAG remains available after leaving
      // USB Drive and reconnecting the cable. Paper Mono has no armable GPIO wake
      // (its button is behind the PMIC). EEGO A4's post-flash reset reads as
      // POWERON (native-USB), so a flash would otherwise be misclassified as a
      // USB-power cold boot and sleep. Sleeping any of these here would strand
      // the device in a USB-replug boot loop (or sleep right after a flash).
      break;
#else
      Storage.prepareForDeepSleep();
      powerManager.startDeepSleep(gpio);
      break;
#endif
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  // Only a verified deep-sleep wake may use the one-shot persisted flag.
  // Otherwise a stale flag could suppress the splash on a cold boot.
  const BootResume resume = isSilentReboot         ? BootResume::Silent
                            : isPersistedSleepWake ? BootResume::SplashlessWake
                                                   : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;
  bool needsWakeRefresh = false;

  setupDisplayAndFonts(resume != BootResume::Splash);

  // Auto power off: the dwell timer elapsed while in deep sleep. Render the
  // shutdown screen and cut power; the next power-button press is a normal
  // cold boot. Checked before any other wake routing; classification goes
  // through the HAL API (WakeupReason::Timer), not raw IDF sleep calls.
  if (gpio.getWakeupReason() == HalGPIO::WakeupReason::Timer && SETTINGS.getAutoPowerOffMs() > 0) {
    SleepActivity::renderShutdownScreen(renderer);
    // A stale Quick Resume frame must not replace the shutdown screen on the
    // next boot.
    Storage.remove(SLEEP_FRAME_FILE);
    // Park the panel before the sink cuts its rail (same ordering rule as
    // enterDeepSleep(); CodeRabbit finding).
    display.deepSleep();
    // The stock-parity marker was already staged earlier in setup()
    // (stageAutoPowerOff), so only the sink remains.
    powerManager.enterPowerOffSleep(gpio);
  }

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::SplashlessWake:
      // One-shot flag: re-arm the splash for the next ordinary boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a splashless-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (Storage.exists(SLEEP_FRAME_FILE) && loadSleepFrameBuffer()) {
        const bool useDifferentialRefresh = gpio.deviceIsX3();
        if (useDifferentialRefresh) {
          // begin() clears the X3 controller RAM, so restore the saved frame as
          // the baseline before replacing the moon with the loading icon.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }

        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        if (useDifferentialRefresh) {
          renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
          allowFastInitialReaderRefresh = true;
        } else {
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }
      } else {
        // The first Home/Reader paint is followed by an explicit clean refresh
        // because the panel still physically shows the sleep image.
        needsWakeRefresh = true;
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  // Output polarity is resolved per render by ActivityManager (night mode
  // inverts only the reading surfaces), so nothing to restore here.

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (rebootedFromPanic) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_SETTINGS) {
    // Back out of the WiFi rows and the user is where they left off, not on Home.
    activityManager.goToSettings();
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome(HomeMenuItem::NONE, needsWakeRefresh);
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  mappedInputManager.update();

  if (activityManager.requiresExclusiveStorageLoop()) {
    // USB Drive handed the raw SD card to the host. Do not run screenshots,
    // sleep, shortcuts, or normal navigation while its filesystem is detached.
    activityManager.loop();
    if (activityManager.preventAutoSleep()) {
      powerManager.setPowerSaving(false);
      delay(10);
    } else {
      // No host is active, so a slower loop is safe. The activity itself times
      // out the raw-storage handoff rather than entering deep sleep detached.
      powerManager.setPowerSaving(true);
      delay(50);
    }
    return;
  }

  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT && defined(CROSSPOINT_MEM_SENTINEL) && \
    CROSSPOINT_MEM_SENTINEL
  // Memory sentinel (FT bring-up, PR #146): the on-device crash (IDLE0 stack
  // canary + corrupted TWDT entry, LoadProhibited at 0x10c/0x1c9) is a silent
  // DRAM corruption whose faulting frames point at IDLE0, not the culprit.
  // Census task stacks (a near-overflow task names itself) and walk the heap
  // block headers so the corrupting allocation is caught while still alive.
  static uint32_t lastMemSentinel = 0;
  if (millis() - lastMemSentinel >= 2000) {
    lastMemSentinel = millis();
    const UBaseType_t taskCount = uxTaskGetNumberOfTasks();
    auto snapshot = makeUniqueNoThrow<TaskStatus_t[]>(taskCount);
    if (snapshot) {
      const UBaseType_t got = uxTaskGetSystemState(snapshot.get(), taskCount, nullptr);
      for (UBaseType_t i = 0; i < got; ++i) {
        const UBaseType_t hwm = snapshot[i].usStackHighWaterMark;
        const char* name = snapshot[i].pcTaskName;
        if (hwm < 256) {
          LOG_ERR("SENT", "Task %s stack HWM=%u (overflow suspect)", name, static_cast<unsigned>(hwm));
        }
        // Victim-stack time series: IDLE0 (core 0) and the render task have
        // both carried wild-write scars during quick-font repros. Log their
        // HWM every tick so the write can be dated against the log phases.
        // FreeRTOS truncates names to configMAX_TASK_NAME_LEN (16), so match
        // the shared prefix instead of the full "ActivityManagerRender".
        if (name != nullptr && (strcmp(name, "IDLE0") == 0 || strncmp(name, "ActivityManager", 15) == 0)) {
          LOG_DBG("SENT", "%s stack HWM=%u", name, static_cast<unsigned>(hwm));
        }
      }
    }
    // print_errors=true: the whole point is naming the smashed block. The
    // full-heap walk is expensive — keep it on a slower cadence than the
    // cheap per-task census so renders aren't starved.
    static uint32_t lastHeapWalk = 0;
    if (millis() - lastHeapWalk >= 10000) {
      lastHeapWalk = millis();
      // print_errors=true: the whole point is naming the smashed block.
      if (!heap_caps_check_integrity_all(true)) {
        LOG_ERR("SENT", "Heap integrity check FAILED (see dump above)");
      }
    }
  }
#endif

  if (Serial && millis() - lastMemPrint >= 10000) {
    const auto heap = HalMemory::getInternalHeap();
    LOG_INF("MEM", "Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes", heap.freeBytes,
            heap.totalBytes, heap.minFreeBytes, heap.largestBlockBytes);
#ifdef BOARD_HAS_PSRAM
    const auto psram = HalMemory::getPsramHeap();
    LOG_INF("MEM", "PSRAM: Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes",
            psram.freeBytes, psram.totalBytes, psram.minFreeBytes, psram.largestBlockBytes);
#endif
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  // Snapshot masks (soak-fix7): gpio.wasAny* reports only edges NOT yet
  // consumed by the manager's snapshot — after update() that's always
  // nothing, and the inactivity timer would never reset on buttons.
  if (mappedInputManager.wasAnyPressed() || mappedInputManager.wasAnyReleased() || gpio.wasTouchActivity() ||
      halTiltSensor.hadActivity() || activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // Let wake continue as soon as its hold has been verified. The release can
  // arrive after setup, so consume that one input frame rather than making it
  // a page turn, refresh, or other short power-button action.
  // Wake release swallow and the screenshot combo read LEVELS through the
  // manager (logical buttons map 1:1 for Power/Down); no raw BTN reads here.
  if (wakePowerReleasePending && !mappedInputManager.isPressed(MappedInputManager::Button::Power)) {
    wakePowerReleasePending = false;
    return;
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (mappedInputManager.isPressed(MappedInputManager::Button::Power) &&
      mappedInputManager.isPressed(MappedInputManager::Button::Down)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (mappedInputManager.isPressed(MappedInputManager::Button::Power)) return;
    if (mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    // The combo's tail Power release was swallowed by the click window
    // (manager armed + stripped it this tick): wasReleased(Power) above is
    // false, so this branch ends the combo. Cancel the window so the armed
    // release cannot resolve as a short-power click at expiry (audit F2).
    mappedInputManager.cancelPowerClickWindow();
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  // Consume the second X4 Pro power-button release so it does not also run a
  // configured short-power action after toggling the frontlight. The window
  // itself (arming, expiry → powerConfirmClickFrame) lives in the manager.
  if (handleX4ProFrontlightDoubleClick()) {
    return;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // A hold that woke the device must be released before it can count as a new
  // in-app long press. Otherwise a user who keeps holding after wake would put
  // the device straight back to sleep once allowSleepAt expires.
  static bool powerReleasedSinceWake = false;
  if (!mappedInputManager.isPressed(MappedInputManager::Button::Power)) powerReleasedSinceWake = true;

  // On X4 Pro with SLEEP, a press still within the click window is a
  // double-click candidate — let it be released and evaluated by the click
  // tracking below instead of powering off on button-down.
  const bool x4ProAwaitingClickWindow =
      mappedInputManager.isPowerClickHoldCandidate() && SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP;

  if (!x4ProAwaitingClickWindow && powerReleasedSinceWake && millis() >= allowSleepAt &&
      mappedInputManager.isPressed(MappedInputManager::Button::Power) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't power off
    if (mappedInputManager.isPressed(MappedInputManager::Button::Down)) {
      return;
    }
    LOG_INF("MAIN", "Power button held %lums, powering off", gpio.getPowerButtonHeldTime());
    enterPowerOff();
    // This should never be hit as `enterPowerOff` never returns
    return;
  }

  // Short power click with the Sleep binding: sleep on release. This is the
  // only sleep trigger on Paper Mono (its PMIC reports the button as a
  // one-tick click, so the hold path above cannot fire); on the other boards
  // it complements the hold path, which is now exclusively the power off
  // gesture. allowSleepAt also covers the release of the press that woke the
  // device when wakePowerReleasePending missed it.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP && millis() >= allowSleepAt &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_INF("MAIN", "Sleep triggered by power-button click");
    enterDeepSleep();
    return;
  }

#if FREEINK_DEVICE_PAPERMONO
  // Paper Mono reports the PMIC power button as a one-tick click, so the held
  // path above cannot fire. With the Ignore binding, keep sleeping on the
  // click: the GPIO boards' Ignore short-click no-op has no Paper Mono
  // equivalent because a click is the only power-button event it can report.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::IGNORE && millis() >= allowSleepAt &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    enterDeepSleep();
    return;
  }
#endif

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    if (!activityManager.handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  // Not while reading: there a repaint is a full page re-render (visible
  // flash, the AA pass re-running, and a frontlight dip under the refresh
  // load); the reader's status bar picks the charging state up on the next
  // page turn instead.
  if (gpio.wasUsbStateChanged() && !activityManager.isReaderActivity()) {
    activityManager.requestUpdate();
  }

  if (dispatchGlobalHomeButtonAction()) {
    return;
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      // Sleep in short slices and wake the poll as soon as a button contact closes.
      // InputManager commits a press only when two consecutive polls agree, so a
      // press shorter than one 50 ms sleep could land in a single sample and be lost.
      const unsigned long idleStart = millis();
      while (millis() - idleStart < 50) {
        delay(10);
        if (gpio.rawInputActive()) break;
      }
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
