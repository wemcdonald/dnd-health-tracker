#!/usr/bin/env bash
# Cross-compile the healthbar hardware binary for the Raspberry Pi Zero 2 W
# from a dev machine, using the rpi-ws281x-go Docker toolchain (cgo + the C
# library make plain `GOOS=linux GOARCH=arm64 go build` insufficient).
#
# Output: ./healthbar (copy it to the Pi's repo root, then run provision.sh,
# which will `install` it when no Go toolchain is present on the Pi).
#
# Pi Zero 2 W runs a 64-bit SoC; use arm64 with 64-bit Raspberry Pi OS, or
# linux/arm/v7 with 32-bit OS. Override PLATFORM to switch.
set -euo pipefail

PLATFORM="${PLATFORM:-linux/arm64}"
IMAGE=ws2811-builder
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> Building cross-compile toolchain image ($PLATFORM)"
# The Dockerfile ships with the rpi-ws281x-go module; fetch it into a temp dir.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
git clone --depth 1 https://github.com/rpi-ws281x/rpi-ws281x-go "$work/lib"
docker buildx build --platform "$PLATFORM" --tag "$IMAGE" \
  --file "$work/lib/docker/app-builder/Dockerfile" --load "$work/lib"

echo "==> Cross-compiling healthbar"
docker run --rm -v "$REPO_DIR":/src -w /src --platform "$PLATFORM" \
  "$IMAGE":latest go build -tags hw -o healthbar ./cmd/healthbar

echo "==> Built ./healthbar for $PLATFORM"
file "$REPO_DIR/healthbar" || true
