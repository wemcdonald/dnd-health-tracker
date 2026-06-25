#!/usr/bin/env bash
# Provision a Raspberry Pi Zero 2 W as a D&D Beyond health bar.
# Run on the Pi as root (e.g. `sudo ./provision.sh`) from the repo root.
#
# It: builds+installs the rpi_ws281x C library, builds the healthbar binary
# with the hardware backend (or uses a prebuilt one you copied in), installs the
# config + systemd service, and starts it. Re-runnable (idempotent-ish).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX=/usr/local
CONFIG_DIR=/etc/healthbar
BIN=/usr/local/bin/healthbar

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

if [[ $EUID -ne 0 ]]; then
  echo "Please run as root (sudo $0)" >&2
  exit 1
fi

log "Installing build dependencies"
apt-get update -y
apt-get install -y git build-essential cmake

# --- rpi_ws281x C library (required by the Go hardware backend) ---
if [[ ! -f "$PREFIX/lib/libws2811.a" ]]; then
  log "Building rpi_ws281x C library"
  tmp="$(mktemp -d)"
  git clone --depth 1 https://github.com/jgarff/rpi_ws281x "$tmp/rpi_ws281x"
  cmake -S "$tmp/rpi_ws281x" -B "$tmp/build" -DBUILD_SHARED=OFF -DBUILD_TEST=OFF
  cmake --build "$tmp/build"
  cmake --install "$tmp/build" --prefix "$PREFIX"
  ldconfig
  rm -rf "$tmp"
else
  log "rpi_ws281x already installed, skipping"
fi

# --- healthbar binary ---
if command -v go >/dev/null 2>&1; then
  log "Building healthbar (hardware backend) with Go"
  ( cd "$REPO_DIR" && CGO_ENABLED=1 go build -tags hw -o "$BIN" ./cmd/healthbar )
elif [[ -f "$REPO_DIR/healthbar" ]]; then
  log "Installing prebuilt healthbar binary"
  install -m 0755 "$REPO_DIR/healthbar" "$BIN"
else
  echo "No Go toolchain and no prebuilt ./healthbar binary found." >&2
  echo "Either install Go, or cross-compile (deploy/build-arm.sh) and copy the binary to the repo root." >&2
  exit 1
fi

# --- configuration ---
log "Installing configuration into $CONFIG_DIR"
mkdir -p "$CONFIG_DIR"
for f in device.toml theme.toml; do
  if [[ ! -f "$CONFIG_DIR/$f" ]]; then
    install -m 0644 "$REPO_DIR/deploy/config/$f" "$CONFIG_DIR/$f"
    log "  installed default $f (edit it!)"
  else
    log "  keeping existing $f"
  fi
done

# --- systemd service ---
log "Installing systemd service"
install -m 0644 "$REPO_DIR/deploy/healthbar.service" /etc/systemd/system/healthbar.service
systemctl daemon-reload
systemctl enable healthbar.service
systemctl restart healthbar.service

log "Done. Status:"
systemctl --no-pager --full status healthbar.service || true
echo
log "Edit $CONFIG_DIR/device.toml (player + character_id), then: systemctl restart healthbar"
