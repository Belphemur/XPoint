#!/usr/bin/env python3
"""Generate build/board_features.h from the SDK's BoardConfig.h (DRY chain).

Runs scripts/board_features_dump.c (host-compiled against the real board
profiles), parses its JSON, resolves the active env's -DFREEINK_DEVICE_* set
from platformio.ini, and emits FREEINK_CAP_HOME_KEY / FREEINK_CAP_MENU_BUTTON
macros. An audit trail lands in build/board_features.audit.txt.

Used both as a PlatformIO pre: script (writes the header for $PIOENV) and
standalone (--env / --show).
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path.cwd()
DUMP_SRC = REPO_ROOT / "scripts" / "board_features_dump.c"
DUMP_BIN = REPO_ROOT / "build" / "board_features_dump"
BOARD_CONFIG_H = REPO_ROOT / "freeink-sdk" / "libs" / "hardware" / "BoardConfig" / "include" / "BoardConfig.h"
PLATFORMIO_INI = REPO_ROOT / "platformio.ini"
OUT_HEADER = REPO_ROOT / "build" / "board_features.h"
OUT_AUDIT = REPO_ROOT / "build" / "board_features.audit.txt"


def _init_paths(root: Path) -> None:
    """Bind the module paths. PlatformIO's pre-script exec context has no
    __file__, so the root is passed in per execution mode."""
    global REPO_ROOT, DUMP_SRC, DUMP_BIN, BOARD_CONFIG_H, PLATFORMIO_INI, OUT_HEADER, OUT_AUDIT
    REPO_ROOT = root
    DUMP_SRC = REPO_ROOT / "scripts" / "board_features_dump.c"
    DUMP_BIN = REPO_ROOT / "build" / "board_features_dump"
    BOARD_CONFIG_H = REPO_ROOT / "freeink-sdk" / "libs" / "hardware" / "BoardConfig" / "include" / "BoardConfig.h"
    PLATFORMIO_INI = REPO_ROOT / "platformio.ini"
    OUT_HEADER = REPO_ROOT / "build" / "board_features.h"
    OUT_AUDIT = REPO_ROOT / "build" / "board_features.audit.txt"

# BoardConfig.h struct fields the dump derives the flags from, for the audit log.
AUDIT_FIELD_LINES = ("int8_t confirm;", "bool synthesizeConfirm;", "bool hasHomeKey = false;")


class GenerationError(Exception):
    pass


def build_dump() -> None:
    """Compile the C dump if missing or stale (source, stubs, or BoardConfig.h newer)."""
    stale_sources = [DUMP_SRC, BOARD_CONFIG_H] + list((REPO_ROOT / "scripts" / "hoststubs").rglob("*"))
    if DUMP_BIN.exists() and all(s.stat().st_mtime < DUMP_BIN.stat().st_mtime for s in stale_sources):
        return
    DUMP_BIN.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "g++",
        "-std=gnu++20",
        "-x",
        "c++",
        "-I",
        str(REPO_ROOT / "scripts" / "hoststubs"),
        "-I",
        str(BOARD_CONFIG_H.parent),
        str(DUMP_SRC),
        "-o",
        str(DUMP_BIN),
    ]
    try:
        subprocess.run(cmd, check=True, capture_output=True, text=True)
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        detail = getattr(exc, "stderr", "") or str(exc)
        raise GenerationError(f"host build of {DUMP_SRC} failed:\n{detail}") from exc


def run_dump() -> tuple[dict, list[str]]:
    """Run the dump; return (device_keyed JSON, stderr '# board=...' audit lines)."""
    proc = subprocess.run([str(DUMP_BIN)], capture_output=True, text=True, check=True)
    return json.loads(proc.stdout), [line for line in proc.stderr.splitlines() if line.startswith("#")]


def read_ini_env_block(env: str) -> str:
    """Return the raw text of the [env:<name>] block from platformio.ini."""
    text = PLATFORMIO_INI.read_text(encoding="utf-8")
    match = re.search(rf"^\[env:{re.escape(env)}\]\s*$(.*?)(?=^\[|\Z)", text, re.MULTILINE | re.DOTALL)
    if not match:
        raise GenerationError(f"env '{env}' not found in {PLATFORMIO_INI.name}")
    return match.group(1)


def active_devices(env: str) -> list[str]:
    """Extract -DFREEINK_DEVICE_*=1 flags from the env block's build_flags."""
    block = read_ini_env_block(env)
    m = re.search(r"^build_flags\s*=(.*?)(?=^\S|\Z)", block, re.MULTILINE | re.DOTALL)
    if not m:
        raise GenerationError(f"env '{env}' has no build_flags block")
    flags = m.group(1)
    devices = []
    for name, value in re.findall(r"-D(FREEINK_DEVICE_[A-Z0-9_]+)=(\d+)", flags):
        if int(value) == 0:
            continue
        suffix = name.removeprefix("FREEINK_DEVICE_")
        if suffix not in devices:
            devices.append(suffix)
    if not devices:
        raise GenerationError(f"env '{env}' declares no active -DFREEINK_DEVICE_* in its build_flags")
    return devices


def audit_lines(env: str, devices: list[str], board_lines: list[str]) -> list[str]:
    """Truth-check lines: per device, the exact BoardConfig.h lines the dump derived."""
    header_text = BOARD_CONFIG_H.read_text(encoding="utf-8").splitlines()
    field_refs = []
    for pattern in AUDIT_FIELD_LINES:
        for i, line in enumerate(header_text, 1):
            if line.strip().startswith(pattern):
                field_refs.append(f"BoardConfig.h:{i}: {line.strip()}")
                break
    lines = [f"env: {env}", "struct fields read by scripts/board_features_dump.c:"]
    lines += [f"  {ref}" for ref in field_refs]
    for device in devices:
        lines.append(f"{device}:")
        for line in board_lines:
            # stderr detail: "# board=<enum> profile=<NAME> device=<device> home_key=..."
            parts = line.lstrip("#").split()
            if len(parts) < 3 or parts[2].removeprefix("device=") != device:
                continue
            profile = parts[1].removeprefix("profile=")
            for i, text in enumerate(header_text, 1):
                if text.strip().startswith(f"constexpr BoardProfile {profile}"):
                    lines.append(f"  BoardConfig.h:{i}: {text.strip()}")
                    break
            lines.append(f"  {line}")
    return lines


def resolve(devices: list[str], table: dict) -> tuple[int, int]:
    home_key = menu_button = 0
    for device in devices:
        entry = table.get(device)
        if entry is None:
            known = ", ".join(sorted(table))
            raise GenerationError(
                f"device '{device}' (from platformio.ini build_flags) has no entry in the board dump; "
                f"known devices: {known}"
            )
        home_key |= int(bool(entry["home_key"]))
        menu_button |= int(bool(entry["menu_button"]))
    return home_key, menu_button


def generate(env_name: str, show: bool) -> int:
    """Build+run the dump, resolve the env's devices, and print/write the results."""
    build_dump()
    table, board_lines = run_dump()

    env = env_name
    if env is None:
        ini_text = PLATFORMIO_INI.read_text(encoding="utf-8")
        m = re.search(r"^default_envs\s*=\s*(\S+)", ini_text, re.MULTILINE)
        if not m:
            raise GenerationError("no --env given and platformio.ini has no default_envs")
        env = m.group(1)

    devices = active_devices(env)
    home_key, menu_button = resolve(devices, table)

    if show:
        print(json.dumps(table, indent=2))
        print(f"\nenv: {env}")
        print(f"active devices: {', '.join('FREEINK_DEVICE_' + d for d in devices)}")
        print(f"FREEINK_CAP_HOME_KEY={home_key}")
        print(f"FREEINK_CAP_MENU_BUTTON={menu_button}")
        return 0

    audit = audit_lines(env, devices, board_lines)
    audit += [
        f"resolved: FREEINK_CAP_HOME_KEY={home_key}",
        f"resolved: FREEINK_CAP_MENU_BUTTON={menu_button}",
        "",
    ]

    header = "\n".join(
        [
            "#pragma once",
            "// GENERATED by scripts/gen_board_features.py - DO NOT EDIT.",
            f"// Active env:     {env}",
            "// Active devices: " + " ".join(f"FREEINK_DEVICE_{d}=1" for d in devices),
            f"#define FREEINK_CAP_HOME_KEY {home_key}",
            f"#define FREEINK_CAP_MENU_BUTTON {menu_button}",
            "",
        ]
    )

    OUT_HEADER.parent.mkdir(parents=True, exist_ok=True)
    OUT_HEADER.write_text(header, encoding="utf-8")
    OUT_AUDIT.write_text("\n".join(audit), encoding="utf-8")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env", help="PlatformIO env to resolve (default: [platformio] default_envs)")
    parser.add_argument("--show", action="store_true", help="print the dump table + resolved flags; write nothing")
    args = parser.parse_args()
    try:
        return generate(args.env, args.show)
    except GenerationError as exc:
        print(f"gen_board_features: ERROR: {exc}", file=sys.stderr)
        return 1


try:
    Import("env")  # noqa: F821 — SCons builtin, only defined in a PlatformIO pre: script
except NameError:
    _init_paths(Path(__file__).resolve().parent.parent)
    if __name__ == "__main__":
        sys.exit(main())
else:
    # PlatformIO pre-script: emit the header for the env being built. An
    # exception here aborts the build loudly.
    _init_paths(Path(env.subst("$PROJECT_DIR")))  # noqa: F821
    generate(env.subst("$PIOENV"), show=False)  # noqa: F821
