# Development / CI image for the PC build, tests, Valgrind and formatting.
# Firmware uses the official espressif/idf:v6.1.1 image instead.
FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates python3 \
        clang-format-18 valgrind curl libssl-dev \
    && rm -rf /var/lib/apt/lists/*
ENV CLANG_FORMAT=clang-format-18
