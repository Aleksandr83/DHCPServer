# scripts/fs_image.py
#
# PlatformIO's bundled mkspiffs (2018, v2.30) fails on SPIFFS images that
# contain many files: "SPIFFS_write error(-10010)" = SPIFFS_ERR_OUT_OF_FILE_DESCS.
# This extra script redirects the FS builder to a small wrapper that calls the
# ESP-IDF spiffsgen.py instead, so the image is always built correctly and the
# broken mkspiffs is never invoked.
#
# The DataToBin builder action (defined in the platform main.py) is a shell
# string referencing "$MKFSTOOL", which is substituted at BUILD time — so
# overriding the construction variable here (after the platform script ran)
# still takes effect.
Import("env")

import os

mkfs_bat = os.path.join(env.subst("$PROJECT_DIR"), "scripts", "mkfs.bat")
spiffsgen = os.path.join(
    env.subst("$PROJECT_PACKAGES_DIR"),
    "framework-espidf", "components", "spiffs", "spiffsgen.py",
)

env["MKFSTOOL"] = mkfs_bat
env["ENV"]["SPIFFSGEN_PATH"] = spiffsgen
