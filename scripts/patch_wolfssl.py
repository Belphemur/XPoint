from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
# Only NO_DH / FFDHE remain here. The Ed25519 OTA-verify + FP_MAX_BITS knobs now
# live in platformio.ini [base] build_flags (they are order-independent and reach
# wolfSSL's own sources), so they no longer depend on this patch landing.
#
# NO_DH is defined by Arduino-wolfSSL's user_settings.h, but command-line -D cannot
# undefine a header macro, so we must strip it from the downloaded file. FFDHE_2048
# lets the TLS client offer/accept standard DH groups.
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
"""


def patch_user_settings(path: Path) -> None:
    text = path.read_text()
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    path.write_text(text + OVERRIDES + "\n")
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


# wolfSSL resolves two ways here, and the installed folder name differs:
#  * registry pin (wolfssl/Arduino-wolfSSL @ 5.7.2) -> .pio/libdeps/<env>/Arduino-wolfSSL/
#  * GitHub tag (https://github.com/wolfSSL/Arduino-wolfSSL.git#5.8.4) -> .pio/libdeps/<env>/wolfssl/
# The library's internal name flipped to "wolfssl" at 5.8.x.
#
# pathlib.Path.glob does NOT support brace expansion ({a,b}), so we enumerate
# both folder names explicitly.
#
# Runs as a `post:` script so it executes AFTER PlatformIO has installed the
# wolfSSL dependency (a `pre:` script would run before install on a clean/CI
# checkout, find no user_settings.h, and silently skip the patch).
WOLFSSL_FOLDERS = ("Arduino-wolfSSL", "wolfssl")
patched = 0
for folder in WOLFSSL_FOLDERS:
    for settings in PROJECT_DIR.glob(f".pio/libdeps/*/{folder}/src/user_settings.h"):
        patch_user_settings(settings)
        patched += 1
if patched == 0:
    print(f"Warning: no wolfSSL user_settings.h found under {PROJECT_DIR}/.pio/libdeps/*/ "
          "(have you run a build yet?)")
