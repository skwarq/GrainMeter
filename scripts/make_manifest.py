#!/usr/bin/env python3
"""Creates the remote-update manifest for a GrainMeter OTA image.

Usage: make_manifest.py <ota.bin> <url> <out.json> [--expect-version X.Y.Z]

Reads the version and project from the image's app descriptor, checks the
image header (magic 0xE9, chip ESP32-C6, project "grainmeter") and writes
{"version", "url", "sha256"} - the format parsed by core/ota/remote_update.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

CHIP_ESP32C6 = 0x000D
APP_DESC_MAGIC = 0xABCD5432


def cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("url")
    ap.add_argument("out")
    ap.add_argument("--expect-version")
    args = ap.parse_args()

    data = Path(args.image).read_bytes()
    if len(data) < 288 or data[0] != 0xE9:
        print("ERROR: not an ESP-IDF application image", file=sys.stderr)
        return 1
    chip = struct.unpack_from("<H", data, 12)[0]
    magic = struct.unpack_from("<I", data, 32)[0]
    version = cstr(data[48:80])
    project = cstr(data[80:112])
    if chip != CHIP_ESP32C6 or magic != APP_DESC_MAGIC or project != "grainmeter":
        print(f"ERROR: unexpected image (chip {chip:#x}, project {project!r})", file=sys.stderr)
        return 1
    if args.expect_version and version != args.expect_version:
        print(f"ERROR: image version {version} != expected {args.expect_version}", file=sys.stderr)
        return 1
    if not args.url.startswith("https://"):
        print("ERROR: the device only downloads over https://", file=sys.stderr)
        return 1
    manifest = {
        "version": version,
        "url": args.url,
        "sha256": hashlib.sha256(data).hexdigest(),
        "size": len(data),
    }
    Path(args.out).write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
