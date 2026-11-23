#!/bin/bash
# Builds the ESP32-C6 firmware in the pinned ESP-IDF container (same image
# and commands as CI) and collects flash/OTA artifacts in firmware/dist/.
# Usage: scripts/build-firmware.sh [idf.py args...]   (default: build)
#   GM_SIGNED=1  signed-images profile (firmware/sdkconfig.signed), key in
#                firmware/keys/signing_key.pem, output in firmware/dist-signed/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Pinned by digest (v6.1.1): the release job hands the signing key to this
# image, so the tag must not be able to change underneath us.
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v6.1.1@sha256:2b4f966c37eaedc19542b0637a5f850b2eff9478d8e858192aa0073d92c2dc91}"

if [ -n "${IDF_PATH:-}" ] && [ "${GM_NO_DOCKER:-0}" = "1" ]; then
    RUN=(bash -c)
else
    RUN=(docker run --rm -v "$ROOT:/project" -w /project -u "$(id -u):$(id -g)" -e HOME=/tmp
         -e IDF_GIT_SAFE_DIR=/project "$IDF_IMAGE" bash -c)
fi

ARGS="${*:-build}"
if [ "${GM_SIGNED:-0}" = "1" ]; then
    if [ ! -f "$ROOT/firmware/keys/signing_key.pem" ]; then
        echo "missing firmware/keys/signing_key.pem (see README: signed images)" >&2
        exit 1
    fi
    BUILD=firmware/build-signed
    DIST=firmware/dist-signed
    EXTRA="-D SDKCONFIG=build-signed/sdkconfig -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.signed'"
else
    BUILD=firmware/build
    DIST=firmware/dist
    EXTRA=""
fi
"${RUN[@]}" "idf.py -C firmware -B $BUILD $EXTRA $ARGS && python3 scripts/firmware_artifacts.py $BUILD $DIST"
