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

.PHONY: all pc test run valgrind format format-check firmware frontend check ci clean dev-image help

all: test

help:
	@sed -n '1,13p' Makefile

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

check: format-check test valgrind firmware

ci: check

clean:
	rm -rf build firmware/build firmware/dist
