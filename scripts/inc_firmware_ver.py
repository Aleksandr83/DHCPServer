#!/usr/bin/env python3
"""
Increment firmware subrelease number (cc) in all config files.

Usage:
    python scripts/inc_firmware_ver.py             # increment subrelease (cc +1)
    python scripts/inc_firmware_ver.py --sub       # increment subrelease (default)
    python scripts/inc_firmware_ver.py --rel       # increment release (xxx +1)
    python scripts/inc_firmware_ver.py --min-only  # only refresh the build date

Every run also sets ``CONFIG_FW_MIN_DATETIME`` (the lowest date/time the web
page accepts for a manual clock setting) to the current date and time — the
build moment — so the clock can never be set to a meaningless value (1970).

Separate file: kconfig_tools.py (shared helpers for both version scripts).

The script updates:
  - sdkconfig.defaults
  - src/core/Version.cpp (fallback define)
  - platformio.ini (both [env:esp32dev] and [env:esp32dev-debug])
  - the live root sdkconfig and any generated sdkconfig.* files that already
    contain the key (the ESP32-P4 build reads the live root sdkconfig)
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kconfig_tools as kcfg  # noqa: E402  (own helper module, next to this file)

PROJECT_DIR = Path(__file__).resolve().parent.parent

FILES = [
    PROJECT_DIR / "sdkconfig.defaults",
    PROJECT_DIR / "src" / "core" / "Version.cpp",
    PROJECT_DIR / "platformio.ini",
]


def update_key_in_file(path, key, new_val):
    """Replace ``^KEY=<digits>`` in *path* if present.

    Returns True when the file existed and was actually changed.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return False
    new_text = re.sub(rf"^{re.escape(key)}=\d+", f"{key}={new_val}",
                      text, flags=re.MULTILINE)
    if new_text == text:
        return False
    path.write_text(new_text, encoding="utf-8")
    return True


def increment(increment_sub=True, min_only=False):
    if min_only:
        print("Refreshing the minimum manual date/time:")
        kcfg.apply_min_datetime(kcfg.current_min_datetime())
        print("Done. Rebuild to embed the new value (idf.py build / pio run).")
        return

    # Read sdkconfig.defaults
    sdkconfig_path = PROJECT_DIR / "sdkconfig.defaults"
    text = sdkconfig_path.read_text(encoding="utf-8")

    if increment_sub:
        key_old = "CONFIG_FW_VER_SUBRELEASE"
        key_new = key_old
    else:
        key_old = "CONFIG_FW_VER_RELEASE"
        key_new = key_old

    # Find current value
    m = re.search(rf"^{key_old}=(\d+)", text, re.MULTILINE)
    if not m:
        print(f"Error: {key_old} not found in sdkconfig.defaults")
        sys.exit(1)

    old_val = int(m.group(1))
    new_val = old_val + 1

    # Update sdkconfig.defaults
    text = re.sub(rf"^{key_old}=\d+", f"{key_new}={new_val}", text, flags=re.MULTILINE)
    sdkconfig_path.write_text(text, encoding="utf-8")

    # Update src/core/Version.cpp fallback
    vcpp_path = PROJECT_DIR / "src" / "core" / "Version.cpp"
    vcpp_text = vcpp_path.read_text(encoding="utf-8")
    vcpp_text = re.sub(
        rf"(#ifndef {key_old}\s*#define {key_old})\s*\d+",
        rf"\g<1> {new_val}",
        vcpp_text,
    )
    vcpp_path.write_text(vcpp_text, encoding="utf-8")

    # Update platformio.ini (all environments)
    pio_path = PROJECT_DIR / "platformio.ini"
    pio_text = pio_path.read_text(encoding="utf-8")

    key_pio = key_old.replace("CONFIG_", "")
    pio_text = re.sub(
        rf"(board_build\.menuconfig\.{re.escape(key_pio)}\s*=\s*)\d+",
        rf"\g<1>{new_val}",
        pio_text,
    )
    pio_path.write_text(pio_text, encoding="utf-8")

    # Propagate the new value to every OTHER sdkconfig file that already
    # contains the key, instead of deleting them:
    #   * the LIVE root `sdkconfig` — the ESP32-P4 build (idf.py) merges this
    #     file over sdkconfig.defaults, so leaving it stale produced a firmware
    #     that still reported the OLD version;
    #   * generated per-env caches (sdkconfig.esp32dev, sdkconfig.esp32dev-debug,
    #     sdkconfig.old, ...) — several are tracked in git, so deleting them
    #     left the repo dirty (deleted files) after every bump.
    updated = []
    for f in sorted(PROJECT_DIR.glob("sdkconfig*")):
        if not f.is_file() or f == sdkconfig_path:
            continue  # sdkconfig.defaults was already updated above
        if update_key_in_file(f, key_old, new_val):
            updated.append(f.name)
    for name in updated:
        print(f"  Updated: {name}")
    if not updated:
        print("  (no other sdkconfig* files contained the key)")

    name = "subrelease" if increment_sub else "release"
    print(f"Firmware {name} incremented: {old_val} → {new_val}")
    print(f"Version file: {sdkconfig_path}")

    # Keep the minimum manual clock setting at the build moment (the page
    # refuses earlier values — see CONFIG_FW_MIN_DATETIME in Kconfig.projbuild).
    print("Minimum manual date/time (CONFIG_FW_MIN_DATETIME):")
    kcfg.apply_min_datetime(kcfg.current_min_datetime())

    print("Run 'pio run' and 'pio run --target upload' to rebuild and flash.")


if __name__ == "__main__":
    inc_sub = True
    if len(sys.argv) > 1:
        if sys.argv[1] in ("--rel", "--release"):
            inc_sub = False
        elif sys.argv[1] in ("--sub", "--subrelease"):
            inc_sub = True
        elif sys.argv[1] in ("--min-only", "--min"):
            increment(min_only=True)
            sys.exit(0)
        else:
            print(f"Usage: {sys.argv[0]} [--sub|--rel|--min-only]")
            sys.exit(1)

    increment(increment_sub=inc_sub)
