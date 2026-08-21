#!/usr/bin/env python3
"""
Increment firmware subrelease number (cc) in all config files.

Usage:
    python scripts/inc_firmware_ver.py       # increment subrelease (cc +1)
    python scripts/inc_firmware_ver.py --sub  # increment subrelease (default)
    python scripts/inc_firmware_ver.py --rel  # increment release (xxx +1)

The script updates:
  - sdkconfig.defaults
  - src/core/Version.cpp (fallback define)
  - platformio.ini (both [env:esp32dev] and [env:esp32dev-debug])
"""

import re
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent

FILES = [
    PROJECT_DIR / "sdkconfig.defaults",
    PROJECT_DIR / "src" / "core" / "Version.cpp",
    PROJECT_DIR / "platformio.ini",
]

def increment(increment_sub=True):
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

    # Delete all cached sdkconfig.* files so rebuild picks new version
    for f in PROJECT_DIR.glob("sdkconfig.*"):
        if f.name != "sdkconfig.defaults":
            f.unlink()
            print(f"  Deleted cached: {f.name}")

    name = "subrelease" if increment_sub else "release"
    print(f"Firmware {name} incremented: {old_val} → {new_val}")
    print(f"Version file: {sdkconfig_path}")
    print("Run 'pio run' and 'pio run --target upload' to rebuild and flash.")


if __name__ == "__main__":
    inc_sub = True
    if len(sys.argv) > 1:
        if sys.argv[1] in ("--rel", "--release"):
            inc_sub = False
        elif sys.argv[1] in ("--sub", "--subrelease"):
            inc_sub = True
        else:
            print(f"Usage: {sys.argv[0]} [--sub|--rel]")
            sys.exit(1)

    increment(increment_sub=inc_sub)
