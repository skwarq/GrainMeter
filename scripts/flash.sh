#!/bin/bash
# First USB flash of a FireBeetle 2 ESP32-C6 (esptool in the pinned ESP-IDF
# container, only Docker needed on the host).
#
#   scripts/flash.sh release [vX.Y.Z]   download a GitHub release, verify
#                                       SHA256SUMS and the image signature
#                                       (firmware/signing_pubkey.pem), flash
#   scripts/flash.sh local [DIR]        flash a local build (default
#                                       firmware/dist/flash)
#
# Environment: PORT (default /dev/ttyACM0), ERASE=0 to keep the flash
# contents (default: erase everything first, configuration included),
# REPO (default skwarq/GrainMeter), BAUD (default 460800).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v6.1.1@sha256:2b4f966c37eaedc19542b0637a5f850b2eff9478d8e858192aa0073d92c2dc91}"
PORT="${PORT:-/dev/ttyACM0}"
ERASE="${ERASE:-1}"
REPO="${REPO:-skwarq/GrainMeter}"
BAUD="${BAUD:-460800}"

die() { echo "error: $*" >&2; exit 1; }

idf() {  # idf <workdir> <command...>
    local dir="$1"
    shift
    docker run --rm -v "$dir:/w" -w /w -u "$(id -u):$(id -g)" -e HOME=/tmp \
        $DEVICE_ARGS "$IDF_IMAGE" "$@"
}

mode="${1:-}"
DEVICE_ARGS=""
case "$mode" in
    release)
        tag="${2:-v$(head -n1 "$ROOT/VERSION")}"
        dir="$ROOT/build/flash/$tag"
        mkdir -p "$dir"
        base="https://github.com/$REPO/releases/download/$tag"
        zip="grainmeter-${tag#v}-esp32c6-flash.zip"
        ota="grainmeter-${tag#v}-esp32c6-ota.bin"
        echo "== downloading $tag from $REPO"
        for f in "$zip" "$ota" SHA256SUMS; do
            curl -fsSL -o "$dir/$f" "$base/$f" || die "cannot download $base/$f"
        done
        (cd "$dir" && sha256sum -c --ignore-missing SHA256SUMS) || die "checksum mismatch"
        rm -rf "$dir/flash"
        mkdir -p "$dir/flash"
        (cd "$dir/flash" && unzip -q "../$zip")
        cmp -s "$dir/$ota" "$dir/flash/grainmeter.bin" || die "flash bundle and OTA image differ"
        echo "== verifying signature with firmware/signing_pubkey.pem"
        cp "$ROOT/firmware/signing_pubkey.pem" "$dir/signing_pubkey.pem"
        idf "$dir" espsecure verify-signature --version 2 --keyfile signing_pubkey.pem \
            flash/grainmeter.bin | grep -q "verification successful" \
            || die "signature does not match the project key - not flashing"
        echo "   signature OK"
        flashdir="$dir/flash"
        ;;
    local)
        flashdir="${2:-$ROOT/firmware/dist/flash}"
        [ -f "$flashdir/flash_args.txt" ] || die "no build in $flashdir (run: make firmware)"
        flashdir="$(cd "$flashdir" && pwd)"
        ;;
    *)
        die "usage: $0 release [vX.Y.Z] | local [DIR]"
        ;;
esac

[ -e "$PORT" ] || die "$PORT not found - connect the board (USB-C) or set DEVICE_PORT=... (make) / PORT=... (script)"
DEVICE_ARGS="--device $PORT"
args="$(tr '\n' ' ' < "$flashdir/flash_args.txt")"

if [ "$ERASE" = "1" ]; then
    echo "== erasing flash on $PORT (ERASE=0 to keep configuration)"
    idf "$flashdir" esptool --chip esp32c6 -p "$PORT" erase-flash
    # The USB Serial/JTAG port re-enumerates after the reset.
    for _ in $(seq 1 20); do [ -e "$PORT" ] && break; sleep 0.5; done
    [ -e "$PORT" ] || die "$PORT did not come back after erase"
    sleep 1
fi

echo "== flashing $flashdir"
# shellcheck disable=SC2086
idf "$flashdir" esptool --chip esp32c6 -p "$PORT" -b "$BAUD" write-flash $args
echo "== done. Console: make monitor PORT=$PORT"
