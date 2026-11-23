# GrainMeter - convenience targets. By default everything runs in Docker
# (reproducible toolchain, nothing to install but Docker); USE_DOCKER=0 uses
# the host tools with the very same CMake presets / scripts as CI.
#
#   make            PC build + tests (same as `cmake --workflow --preset dev`)
#   make run        start the PC simulator with the portal on :8080
#   make check      everything CI runs for Linux: format, tests, valgrind, firmware
#   make firmware   ESP32-C6 firmware in espressif/idf:v6.1.1 -> firmware/dist/
#   make frontend   portal UI (frontend/) -> portal/index.html(.gz)
#   make format     clang-format (Google style) in place
#   make flash-release [RELEASE=v0.1.0] [DEVICE_PORT=/dev/ttyACM0] [ERASE=0]
#                   first USB flash from a GitHub release (checksums + signature)
#   make flash      first USB flash of the local build (firmware/dist)
#   make monitor    USB console of the board

USE_DOCKER ?= 1
DEV_IMAGE  ?= grainmeter-dev:$(shell sha1sum docker/dev.Dockerfile | cut -c1-12)
NODE_IMAGE ?= node:26-alpine
PORT       ?= 8080
SPEED      ?= 60

ifeq ($(USE_DOCKER),1)
  # Same path inside the container; separate build dirs (build/docker-*).
  RUN = docker run --rm -t -v "$(CURDIR):$(CURDIR)" -w "$(CURDIR)" -u "$$(id -u):$$(id -g)" \
        -e HOME=/tmp -e GM_BUILD_PREFIX=docker- $(DEV_IMAGE)
  IMAGE_DEP = dev-image
else
  RUN =
  IMAGE_DEP =
endif

.PHONY: all pc test test-tls run valgrind format format-check firmware frontend check ci clean dev-image help \
        flash flash-release monitor

all: test

help:
	@sed -n '1,17p' Makefile

dev-image:
	@docker image inspect $(DEV_IMAGE) >/dev/null 2>&1 || \
		docker build -t $(DEV_IMAGE) -f docker/dev.Dockerfile docker

pc: $(IMAGE_DEP)
	$(RUN) cmake --preset dev
	$(RUN) cmake --build --preset dev

test: $(IMAGE_DEP)
	$(RUN) cmake --workflow --preset dev

valgrind: $(IMAGE_DEP)
	$(RUN) cmake --workflow --preset valgrind

# Simulator with HTTPS (OpenSSL): can report to a real ThingSpeak channel.
test-tls: $(IMAGE_DEP)
	$(RUN) cmake --workflow --preset dev-tls

format: $(IMAGE_DEP)
	$(RUN) scripts/clang-format.sh

format-check: $(IMAGE_DEP)
	$(RUN) scripts/clang-format.sh --dry-run

# The simulator is interactive (console on stdin), so it runs on the host
# build; in Docker the portal is published on localhost:$(PORT).
run:
ifeq ($(USE_DOCKER),1)
	$(MAKE) pc
	docker run --rm -it -p 127.0.0.1:$(PORT):8080 -v "$(CURDIR):$(CURDIR)" -w "$(CURDIR)" \
		-u "$$(id -u):$$(id -g)" $(DEV_IMAGE) build/docker-dev/pc/grainmeter_pc --bind 0.0.0.0 \
		--port 8080 --speed $(SPEED) --data-dir pc-data
else
	cmake --preset dev && cmake --build --preset dev
	build/dev/pc/grainmeter_pc --port $(PORT) --speed $(SPEED) --data-dir pc-data
endif

firmware:
ifeq ($(USE_DOCKER),1)
	scripts/build-firmware.sh build
else
	GM_NO_DOCKER=1 scripts/build-firmware.sh build
endif

frontend:
	docker run --rm -v "$(CURDIR):$(CURDIR)" -w "$(CURDIR)/frontend" -u "$$(id -u):$$(id -g)" \
		-e HOME=/tmp -e npm_config_cache=/tmp/.npm $(NODE_IMAGE) sh -c "npm ci && npm run verify"

# --- board (USB) ------------------------------------------------------------
DEVICE_PORT ?= /dev/ttyACM0
RELEASE     ?=
ERASE       ?= 1
IDF_PINNED  := espressif/idf:v6.1.1@sha256:2b4f966c37eaedc19542b0637a5f850b2eff9478d8e858192aa0073d92c2dc91

flash-release:
	PORT=$(DEVICE_PORT) ERASE=$(ERASE) scripts/flash.sh release $(RELEASE)

flash:
	PORT=$(DEVICE_PORT) ERASE=$(ERASE) scripts/flash.sh local

monitor:
	@test -e $(DEVICE_PORT) || { echo "$(DEVICE_PORT) not found"; exit 1; }
	docker run --rm -it --device $(DEVICE_PORT) $(IDF_PINNED) \
		python -m serial.tools.miniterm --eol CR --raw $(DEVICE_PORT) 115200

check: format-check test test-tls valgrind firmware

ci: check

clean:
	rm -rf build firmware/build firmware/dist
