#!/bin/bash
# Build both KiTTY MSI installers with wixl (msitools + wixl packages).
#   prereq:  sudo apt install -y wixl uuid-runtime
#   sources: the packaged binaries in release-084/ (build them first via
#            wsl_release.sh + wsl_package.sh + wsl_package_portable.sh)
set -e
cd "$(dirname "$0")"
OUT=/mnt/c/build/release-084

echo "=== system-wide (per-machine, needs admin) ==="
wixl -v -a x64 -o "$OUT/KiTTY-0.84.0.2-beta-x64-system.msi" kitty-system.wxs 2>&1 | tail -2

echo "=== per-user (no UAC) ==="
wixl -v -a x64 -o "$OUT/KiTTY-0.84.0.2-beta-x64-peruser.msi" kitty-peruser.wxs 2>&1 | tail -2

echo "=== built ==="
ls -la "$OUT"/KiTTY-0.84.0.2-beta-x64-*.msi
