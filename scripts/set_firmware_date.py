#!/usr/bin/env python3
"""
Set the firmware year/month (YY.MM) in all config files.

Usage:
    python scripts/set_firmware_date.py --year 26 --month 7
    python scripts/set_firmware_date.py 26 7
    python scripts/set_firmware_date.py --year 2026 --month 07
    python scripts/set_firmware_date.py --year 26          # keep current month
    python scripts/set_firmware_date.py --month 8          # keep current year
    python scripts/set_firmware_date.py --day 15           # min date only
    python scripts/set_firmware_date.py --hour 12          # min date, 12:00

The script updates:
  - sdkconfig.defaults          (CONFIG_FW_VER_YEAR / CONFIG_FW_VER_MONTH)
  - src/core/Version.cpp        (fallback defines)
  - platformio.ini              (both [env:esp32dev] and [env:esp32dev-debug])
  - the live root sdkconfig and any generated sdkconfig.* files that already
    contain the keys (the ESP32-P4 build reads the live root sdkconfig)
And it keeps CONFIG_FW_MIN_DATETIME — the lowest date/time the web page accepts
for a manual clock setting — at the release date ("20YY-MM-DD HH:00:00", the day
defaulting to 01 and the hour to 00 unless --day/--hour is given; minutes and
seconds are always zero).

Year is stored as 2 digits (last two of the given year), month as 2 digits
(01-12). A missing parameter keeps the current value.
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import kconfig_tools as kcfg  # noqa: E402  (own helper module, next to this file)

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


def parse_day(value):
    if value is None:
        return None
    if not re.fullmatch(r"\d{1,2}", value):
        sys.exit(f"Error: invalid day '{value}' (expected 1-31)")
    n = int(value)
    if not 1 <= n <= 31:
        sys.exit(f"Error: invalid day '{value}' (expected 1-31)")
    return n


def parse_hour(value):
    if value is None:
        return None
    if not re.fullmatch(r"\d{1,2}", value):
        sys.exit(f"Error: invalid hour '{value}' (expected 0-23)")
    n = int(value)
    if not 0 <= n <= 23:
        sys.exit(f"Error: invalid hour '{value}' (expected 0-23)")
    return n


def update_sdkconfig(path, updates, quiet=False):
    """Update ``KEY=<digits>`` values in *path*.

    With ``quiet=True`` files that do not contain the keys are silently
    skipped (used when propagating the value to the extra sdkconfig* files).
    Returns True when the file was actually changed.
    """
    text = path.read_text(encoding="utf-8")
    changed = False
    notes = []
    for key, new_val in updates:
        m = re.search(rf"^{key}=(\d+)", text, re.MULTILINE)
        if not m:
            if not quiet:
                print(f"  Warning: {key} not found in {path.name} — skipped")
            continue
        old_val = m.group(1)
        text = re.sub(rf"^{key}=\d+", f"{key}={new_val}", text, flags=re.MULTILINE)
        changed = True
        notes.append(f"  {path.name}: {key} {old_val} → {new_val}")
    if changed:
        path.write_text(text, encoding="utf-8")
        for n in notes:
            print(n)
    return changed


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
    parser.add_argument("--day", help="day for the minimum date (1-31, default 1)")
    parser.add_argument("--hour", help="hour for the minimum time (0-23, default 0)")
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

    if year is None and month is None and args.day is None and args.hour is None:
        print(__doc__)
        sys.exit("Error: specify --year and/or --month (or --day/--hour)")

    new_year = parse_year(year)
    new_month = parse_month(month)
    new_day = parse_day(args.day)
    new_hour = parse_hour(args.hour)

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

    # Propagate the new values to every OTHER sdkconfig file that already
    # contains the keys, instead of deleting them:
    #   * the LIVE root `sdkconfig` — the ESP32-P4 build (idf.py) merges this
    #     file over sdkconfig.defaults, so leaving it stale produced a firmware
    #     that still reported the OLD date;
    #   * generated per-env caches (sdkconfig.esp32dev, sdkconfig.esp32dev-debug,
    #     ...) — several are tracked in git, so deleting them dirtied the repo.
    for f in sorted(PROJECT_DIR.glob("sdkconfig*")):
        if not f.is_file() or f.name == "sdkconfig.defaults":
            continue  # sdkconfig.defaults was already updated above
        update_sdkconfig(f, updates, quiet=True)

    # Keep the minimum manual clock setting on the release date. The effective
    # YY/MM are read back from sdkconfig.defaults, so a partial call
    # (--month only) still composes the full value; the day defaults to 01.
    sdk_defaults = PROJECT_DIR / "sdkconfig.defaults"
    eff_year = new_year if new_year is not None else kcfg.read_int_key(
        sdk_defaults, "CONFIG_FW_VER_YEAR")
    eff_month = new_month if new_month is not None else kcfg.read_int_key(
        sdk_defaults, "CONFIG_FW_VER_MONTH")
    if eff_year is None or eff_month is None:
        sys.exit("Error: could not read CONFIG_FW_VER_YEAR/MONTH from sdkconfig.defaults")

    min_dt = kcfg.min_datetime_from_ymd(eff_year, eff_month, new_day or 1,
                                        new_hour or 0)
    print("Minimum manual date/time (CONFIG_FW_MIN_DATETIME):")
    kcfg.apply_min_datetime(min_dt)

    print("Done. Run 'pio run' and then 'pio run --target upload' to rebuild and flash.")


if __name__ == "__main__":
    main()
