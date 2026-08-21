#!/usr/bin/env python3
"""
Set the firmware year/month (YY.MM) in all config files.

Usage:
    python scripts/set_firmware_date.py --year 26 --month 7
    python scripts/set_firmware_date.py 26 7
    python scripts/set_firmware_date.py --year 2026 --month 07
    python scripts/set_firmware_date.py --year 26          # keep current month
    python scripts/set_firmware_date.py --month 8          # keep current year

The script updates:
  - sdkconfig.defaults          (CONFIG_FW_VER_YEAR / CONFIG_FW_VER_MONTH)
  - src/core/Version.cpp        (fallback defines)
  - platformio.ini              (both [env:esp32dev] and [env:esp32dev-debug])
Year is stored as 2 digits (last two of the given year), month as 2 digits
(01-12). A missing parameter keeps the current value. Cached sdkconfig.* files
are deleted so a rebuild picks up the new version.
"""

import argparse
import re
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent


def parse_year(value):
    if value is None:
        return None
    if not re.fullmatch(r"\d{2,4}", value):
        sys.exit(f"Error: invalid year '{value}' (expected 2-4 digits)")
    return int(value) % 100  # keep the last two digits


def parse_month(value):
    if value is None:
        return None
    if not re.fullmatch(r"\d{1,2}", value):
        sys.exit(f"Error: invalid month '{value}' (expected 1-12)")
    n = int(value)
    if not 1 <= n <= 12:
        sys.exit(f"Error: invalid month '{value}' (expected 1-12)")
    return n


def update_sdkconfig(path, updates):
    text = path.read_text(encoding="utf-8")
    for key, new_val in updates:
        m = re.search(rf"^{key}=(\d+)", text, re.MULTILINE)
        if not m:
            print(f"  Warning: {key} not found in {path.name} — skipped")
            continue
        old_val = m.group(1)
        text = re.sub(rf"^{key}=\d+", f"{key}={new_val}", text, flags=re.MULTILINE)
        print(f"  {path.name}: {key} {old_val} → {new_val}")
    path.write_text(text, encoding="utf-8")


def update_version_cpp(path, updates):
    text = path.read_text(encoding="utf-8")
    for key, new_val in updates:
        pattern = rf"(#ifndef {key}\s*#define {key})\s*\d+"
        if not re.search(pattern, text):
            print(f"  Warning: {key} fallback not found in {path.name} — skipped")
            continue
        text = re.sub(pattern, rf"\g<1> {new_val}", text)
        print(f"  {path.name}: {key} fallback → {new_val}")
    path.write_text(text, encoding="utf-8")


def update_platformio(path, updates):
    text = path.read_text(encoding="utf-8")
    for key, new_val in updates:
        pio_key = key.replace("CONFIG_", "")
        pattern = rf"(board_build\.menuconfig\.{re.escape(pio_key)}\s*=\s*)\d+"
        if not re.search(pattern, text):
            print(f"  Warning: {pio_key} not found in {path.name} — skipped")
            continue
        text = re.sub(pattern, rf"\g<1>{new_val}", text)
        print(f"  {path.name}: {pio_key} → {new_val}")
    path.write_text(text, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(
        description="Set the firmware year/month (YY.MM) in all config files."
    )
    parser.add_argument("--year", help="year (2-4 digits; last two are used)")
    parser.add_argument("--month", help="month (1-12)")
    parser.add_argument(
        "positional", nargs="*", help="alternatively: positional YEAR [MONTH]"
    )
    args = parser.parse_args()

    year = args.year
    month = args.month
    if year is None and len(args.positional) > 0:
        year = args.positional[0]
    if month is None and len(args.positional) > 1:
        month = args.positional[1]

    if year is None and month is None:
        print(__doc__)
        sys.exit("Error: specify --year and/or --month")

    new_year = parse_year(year)
    new_month = parse_month(month)

    updates = []
    if new_year is not None:
        # NOTE: no zero-padding — the values are C integer literals in
        # Version.cpp, and a leading zero would be read as octal ("08" fails
        # with "invalid digit '8' in octal constant"). The version string is
        # zero-padded at runtime by Version::toString() (%02u).
        updates.append(("CONFIG_FW_VER_YEAR", str(new_year)))
    if new_month is not None:
        updates.append(("CONFIG_FW_VER_MONTH", str(new_month)))

    print("Updating firmware date:")
    update_sdkconfig(PROJECT_DIR / "sdkconfig.defaults", updates)
    update_version_cpp(PROJECT_DIR / "src" / "core" / "Version.cpp", updates)
    update_platformio(PROJECT_DIR / "platformio.ini", updates)

    # Delete all cached sdkconfig.* files so a rebuild picks up the new version
    for f in PROJECT_DIR.glob("sdkconfig.*"):
        if f.name != "sdkconfig.defaults":
            f.unlink()
            print(f"  Deleted cached: {f.name}")

    print("Done. Run 'pio run' and then 'pio run --target upload' to rebuild and flash.")


if __name__ == "__main__":
    main()
