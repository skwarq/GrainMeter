#!/usr/bin/env python3
"""Collects firmware artifacts and checks that the app fits both OTA slots.

Usage: firmware_artifacts.py <idf build dir> <output dir>

Reads the generated partition table binary (independent of the CSV text),
compares the application image with every OTA app slot and fails if it does
not fit (or leaves less than the configured margin).
"""
import json
import shutil
import struct
import sys
from pathlib import Path

MARGIN = 0.05  # warn below 5 % free space in a slot


def partitions(table: bytes):
    for off in range(0, len(table), 32):
        entry = table[off:off + 32]
        magic, ptype, subtype, offset, size = struct.unpack_from("<HBBII", entry)
        if magic != 0x50AA:
            break
        label = entry[12:28].split(b"\0")[0].decode()
        yield {"label": label, "type": ptype, "subtype": subtype, "offset": offset, "size": size}


def main() -> int:
    build = Path(sys.argv[1])
    out = Path(sys.argv[2])
    desc = json.loads((build / "project_description.json").read_text())
    app_bin = build / desc["app_bin"]
    table_bin = build / "partition_table" / "partition-table.bin"
    parts = list(partitions(table_bin.read_bytes()))
    slots = [p for p in parts if p["type"] == 0 and 0x10 <= p["subtype"] <= 0x1F]
    if len(slots) < 2:
        print("ERROR: partition table needs two OTA app slots", file=sys.stderr)
        return 1
    app_size = app_bin.stat().st_size
    ok = True
    for s in slots:
        free = s["size"] - app_size
        pct = 100.0 * free / s["size"]
        print(f"{s['label']}: slot {s['size']:#x} ({s['size']} B), app {app_size} B, free {free} B ({pct:.1f} %)")
        if free < 0:
            print(f"ERROR: application does not fit into {s['label']}", file=sys.stderr)
            ok = False
        elif free < s["size"] * MARGIN:
            print(f"WARNING: less than {MARGIN:.0%} free in {s['label']}")
    if not ok:
        return 1

    version = desc.get("project_version", "dev")
    out.mkdir(parents=True, exist_ok=True)
    flash_dir = out / "flash"
    flash_dir.mkdir(exist_ok=True)
    ota_name = f"grainmeter-{version}-esp32c6-ota.bin"
    shutil.copy(app_bin, out / ota_name)
    args = json.loads((build / "flasher_args.json").read_text())
    files = []
    for offset, rel in sorted(args["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
        src = build / rel
        shutil.copy(src, flash_dir / src.name)
        files.append(f"{offset} {src.name}")
    (flash_dir / "flash_args.txt").write_text(
        " ".join(args["write_flash_args"]) + "\n" + "\n".join(files) + "\n")
    (flash_dir / "README.txt").write_text(
        "First USB flash (erases nothing else):\n"
        "  esptool.py --chip esp32c6 -b 460800 write_flash $(cat flash_args.txt | tr '\\n' ' ')\n"
        "or: python -m esptool --chip esp32c6 write_flash @flash_args.txt\n"
        f"Later updates over the service portal: ../{ota_name}\n")
    print(f"artifacts in {out}: {ota_name} + flash/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
