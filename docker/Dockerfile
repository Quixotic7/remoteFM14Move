# SPDX-License-Identifier: MPL-2.0
# RemoteFM14Move's build and test image: Move Remote's (debian bookworm, the Move's aarch64 toolchain, as Schwung
# builds) plus wabt 1.0.32 (wasm2c: .rfm1 -> C) and what the tests use. On an Apple-silicon Mac Docker runs it as
# linux/arm64 natively, so gcc here builds and runs Move binaries directly. wget and busybox: the catalog's downloads in
# tests/dl_e2e.sh (the Move has BusyBox wget; Debian's GNU wget is checked too) and its server (busybox httpd).
FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc-aarch64-linux-gnu libc6-dev-arm64-cross binutils-aarch64-linux-gnu make file \
    gcc libc6-dev wabt xxd curl ca-certificates wget busybox \
    && rm -rf /var/lib/apt/lists/*
