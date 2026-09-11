#!/usr/bin/env python3
"""Shared helpers for the firmware version scripts.

Used by ``inc_firmware_ver.py`` and ``set_firmware_date.py`` so the
kconfig-style key updates, the ``Version.cpp`` fallback defines, the
``platformio.ini`` menuconfig entries and the ``CONFIG_FW_MIN_DATETIME``
propagation live in ONE place.

`CONFIG_FW_MIN_DATETIME` is a Kconfig **string** (``"YYYY-MM-DD HH:00:00"``,
see ``src/Kconfig.projbuild``) holding the lowest date/time the web page
accepts for a manual clock setting — hour granularity: the minutes and seconds
are always zero. The version scripts keep it at the current build date, so the
clock can never be set to a meaningless value (1970).
"""

import re
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent

SDKCONFIG_DEFAULTS = PROJECT_DIR / "sdkconfig.defaults"
VERSION_CPP = PROJECT_DIR / "src" / "core" / "Version.cpp"
PLATFORMIO_INI = PROJECT_DIR / "platformio.ini"

MIN_DATETIME_KEY = "CONFIG_FW_MIN_DATETIME"


def read_int_key(path, key):
    """Return the integer value of ``^KEY=<digits>`` in *path*, or None."""
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return None
    m = re.search(rf"^{re.escape(key)}=(\d+)", text, re.MULTILINE)
    return int(m.group(1)) if m else None


def update_key_in_file(path, key, new_val, quoted=False, append_if_missing=False):
    """Replace ``KEY=<value>`` in *path*.

    ``quoted=True`` matches (and creates) a quoted string value, which is how
    Kconfig stores strings. With ``append_if_missing`` a key the file does not
    contain yet is appended — needed for the live root ``sdkconfig``, where a
    brand-new key only appears after a build regenerates it.

    Returns True when the file was actually changed.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return False

    if quoted:
        pattern = rf'^{re.escape(key)}="[^"]*"'
        line = f'{key}="{new_val}"'
    else:
        pattern = rf"^{re.escape(key)}=\d+"
        line = f"{key}={new_val}"

    if re.search(pattern, text, re.MULTILINE):
        new_text = re.sub(pattern, lambda _m: line, text, flags=re.MULTILINE)
    elif append_if_missing:
        new_text = text.rstrip("\n") + "\n" + line + "\n"
    else:
        return False

    if new_text == text:
        return False

    path.write_text(new_text, encoding="utf-8")
    return True


def update_version_cpp_string(key, new_val):
    """Update a ``#define KEY "value"`` fallback in src/core/Version.cpp."""
    text = VERSION_CPP.read_text(encoding="utf-8")
    new_text = re.sub(rf'(#define {re.escape(key)})\s*"[^"]*"',
                      lambda m: f'{m.group(1)} "{new_val}"', text)
    if new_text == text:
        return False
    VERSION_CPP.write_text(new_text, encoding="utf-8")
    return True


def update_platformio_string(key, new_val):
    """Update a quoted ``board_build.menuconfig.<KEY>`` entry (both envs)."""
    text = PLATFORMIO_INI.read_text(encoding="utf-8")
    pio_key = key.replace("CONFIG_", "")
    pattern = rf'(board_build\.menuconfig\.{re.escape(pio_key)}\s*=\s*)"[^"]*"'
    new_text = re.sub(pattern, lambda m: f'{m.group(1)}"{new_val}"', text)
    if new_text == text:
        return False
    PLATFORMIO_INI.write_text(new_text, encoding="utf-8")
    return True


def apply_min_datetime(value, quiet=False):
    """Store the minimum manual clock setting in every file that carries it.

    Writes ``CONFIG_FW_MIN_DATETIME`` to ``sdkconfig.defaults``, to every other
    ``sdkconfig*`` file (the live root ``sdkconfig`` the ESP32-P4 build merges,
    plus the generated per-env caches — appended if a build has not created the
    key yet), to the ``Version.cpp`` fallback and to ``platformio.ini``.

    Returns the number of files that changed.
    """
    changed = 0
    if update_key_in_file(SDKCONFIG_DEFAULTS, MIN_DATETIME_KEY, value,
                          quoted=True, append_if_missing=True):
        changed += 1
        if not quiet:
            print(f"  sdkconfig.defaults: {MIN_DATETIME_KEY}=\"{value}\"")

    for f in sorted(PROJECT_DIR.glob("sdkconfig*")):
        if not f.is_file() or f == SDKCONFIG_DEFAULTS:
            continue
        if update_key_in_file(f, MIN_DATETIME_KEY, value,
                              quoted=True, append_if_missing=True):
            changed += 1
            if not quiet:
                print(f"  {f.name}: {MIN_DATETIME_KEY} updated")

    if update_version_cpp_string(MIN_DATETIME_KEY, value):
        changed += 1
        if not quiet:
            print(f"  Version.cpp: {MIN_DATETIME_KEY} fallback → \"{value}\"")

    if update_platformio_string(MIN_DATETIME_KEY, value):
        changed += 1
        if not quiet:
            print(f"  platformio.ini: FW_MIN_DATETIME → \"{value}\"")

    return changed


def min_datetime_from_ymd(year2, month, day=1, hour=0):
    """``(YY, MM, DD[, HH])`` → ``"20YY-MM-DD HH:00:00"``.

    The minimum has **hour** granularity: minutes and seconds are always zero.
    """
    return f"20{int(year2):02d}-{int(month):02d}-{int(day):02d} {int(hour):02d}:00:00"


def current_min_datetime():
    """Build-moment minimum as ``"YYYY-MM-DD HH:00:00"`` (hour granularity)."""
    from datetime import datetime
    return datetime.now().strftime("%Y-%m-%d %H:00:00")
