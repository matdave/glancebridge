#!/usr/bin/env bash
# Reinstall the toolchain after a VM rebuild (see NOTES.md section 3).
set -e

export PATH="$HOME/.local/bin:$PATH"

echo "[setup] installing PlatformIO Core + nanopb codegen deps"
pip3 install --user --break-system-packages platformio nanopb grpcio-tools

echo "[setup] creating protoc wrapper (nanopb generator needs a protoc binary)"
mkdir -p "$HOME/.local/bin"
printf '#!/bin/sh\nexec python3 -m grpc_tools.protoc "$@"\n' > "$HOME/.local/bin/protoc"
chmod +x "$HOME/.local/bin/protoc"

echo "[setup] first full build downloads the pioarduino platform + xtensa toolchain (~4.5GB)"
pio run -e adafruit_feather_esp32_v2 \
  || echo "[setup] WARNING: build failed (disk space? see NOTES.md section 3)"

# reclaim package cache immediately (7.9GB VM disk fills fast)
rm -rf "$HOME/.platformio/.cache" || true

echo "[setup] running native protocol tests"
pio test -e native || echo "[setup] WARNING: native tests failed - investigate before building firmware"

echo "[setup] done. Build firmware with: pio run -e adafruit_feather_esp32_v2"
