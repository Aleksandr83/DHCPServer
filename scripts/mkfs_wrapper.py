#!/usr/bin/env python3
"""Translate the mkspiffs-style CLI used by PlatformIO's FS builder into a call
to the ESP-IDF spiffsgen.py tool.

PlatformIO invokes it as (see the DataToBin builder in the platform main.py):
    <this script> -c <data_dir> -s <size> -p <page> -b <block> <out_file>

We replace the broken mkspiffs (2018, v2.30) which fails with
SPIFFS_ERR_OUT_OF_FILE_DESCS on images with many files. spiffsgen.py handles any
number of files and embeds the SPIFFS magic the firmware requires
(CONFIG_SPIFFS_USE_MAGIC / CONFIG_SPIFFS_USE_MAGIC_LENGTH).
"""
import argparse
import os
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description="SPIFFS image builder (spiffsgen.py)")
    parser.add_argument("-c", dest="data_dir", required=True, help="source data directory")
    parser.add_argument("-s", dest="size", required=True, help="image size (bytes or 0x...)")
    parser.add_argument("-p", dest="page", default="256", help="logical page size")
    parser.add_argument("-b", dest="block", default="4096", help="logical block size")
    parser.add_argument("out", help="output image file")
    args = parser.parse_args()

    size = int(args.size, 16) if args.size.lower().startswith("0x") else int(args.size)

    spiffsgen = os.environ.get("SPIFFSGEN_PATH")
    if not spiffsgen or not os.path.exists(spiffsgen):
        print("mkfs_wrapper: SPIFFSGEN_PATH is not set or missing: %r" % spiffsgen,
              file=sys.stderr)
        return 1

    cmd = [
        sys.executable, spiffsgen,
        "--page-size", args.page,
        "--block-size", args.block,
        "--use-magic",
        "--use-magic-len",
        str(size),
        args.data_dir,
        args.out,
    ]
    print("Building FS image with spiffsgen.py -> %s" % args.out)
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
