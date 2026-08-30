from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
/* Ed25519 support for OTA manifest verification. HAVE_ED25519 alone enables
   key gen/sign scaffolding; the verify + public-key import paths are gated on
   these two separate macros in wolfSSL 5.7.2. Ed25519 hashes the message with
   SHA-512, so that must be enabled too. */
#ifndef HAVE_ED25519
#define HAVE_ED25519
#endif
#ifndef WOLFSSL_SHA512
#define WOLFSSL_SHA512
#endif
#ifndef HAVE_ED25519_VERIFY
#define HAVE_ED25519_VERIFY
#endif
#ifndef HAVE_ED25519_KEY_IMPORT
#define HAVE_ED25519_KEY_IMPORT
#endif
/* MEMFIX-PORT: 8192 handles up to RSA-4096 keys (the public-CA maximum,
   ISRG Root X1 included) with half the per-bignum heap of 16384: with
   WOLFSSL_SMALL_STACK each fast-math temp is FP_MAX_BITS/8 * 2 bytes on the
   heap, and TLS cert verification allocates dozens at once. */
#undef FP_MAX_BITS
#define FP_MAX_BITS 8192
"""


def patch_user_settings(path: Path) -> None:
    text = path.read_text()
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    path.write_text(text + OVERRIDES + "\n")
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)
