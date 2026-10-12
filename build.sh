#!/usr/bin/env bash
# Assemble the on-device app in build/sidecast from app/sidecast: the shim, the installer disk the KVM serves to the
# host, and app.json's version and nanokvm_go from version.toml. Anything but the clean commit tagged v<sidecast>
# gets the commit added (0.3.0+1a2b3c4, .dirty with uncommitted changes), so a dev build never claims a release.
# Usage: ./build.sh [deploy <kvm-host>]
# The disk, clients.iso, holds one installer per host platform, Sidecast-<version>-<platform>.<ext>.
# Today that is the Mac client's DMG: release client-v<client> from version.toml, or
# SIDECAST_CLIENT_MACOS_DMG=<path> for a local client build. Needs xorriso, or hdiutil on a Mac. Build the
# shim first (see README).
set -euo pipefail
cd "$(dirname "$0")"
DEVICE=build/sidecast; REPO=ogtega/nanokvm-sidecast
v() { sed -n "s/^$1 = \"\([^\"]*\)\".*/\1/p" version.toml; }
SIDECAST=$(v sidecast); NANOKVM_GO=$(v nanokvm_go); CLIENT=$(v client)
APP_VERSION=$SIDECAST
if HEAD=$(git rev-parse --short HEAD 2>/dev/null); then
  git tag --points-at HEAD | grep -qx "v$SIDECAST" && git diff --quiet HEAD || APP_VERSION+="+$HEAD"
  git diff --quiet HEAD || APP_VERSION+=".dirty"
fi
rm -rf "$DEVICE" && mkdir -p build && cp -R app/sidecast "$DEVICE" && rm -rf "$DEVICE/__pycache__"
python3 -c 'import json, sys; p = sys.argv[1]; d = json.load(open(p)); d.update(version=sys.argv[2], nanokvm_go=sys.argv[3]); open(p, "w").write(json.dumps(d, indent=2) + "\n")' \
  "$DEVICE/app.json" "$APP_VERSION" "$NANOKVM_GO"
echo "app version: $APP_VERSION (nanokvm-go $NANOKVM_GO)"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir "$T/disk"
if [ -n "${SIDECAST_CLIENT_MACOS_DMG:-}" ]; then
  cp "$SIDECAST_CLIENT_MACOS_DMG" "$T/disk/"   # keeps its name: client builds already follow the naming
else
  A=Sidecast-v$CLIENT-macos-arm64.dmg
  curl -fsSL -o "$T/disk/$A" "https://github.com/$REPO/releases/download/client-v$CLIENT/$A" ||
    { echo "no $A in release client-v$CLIENT"; exit 1; }
fi
echo "installer disk: $(ls "$T/disk" | tr '\n' ' ')"
if command -v xorriso >/dev/null; then
  xorriso -as mkisofs -quiet -V Sidecast -J -R -o "$DEVICE/clients.iso" "$T/disk"
else
  hdiutil makehybrid -quiet -iso -joliet -default-volume-name Sidecast -o "$DEVICE/clients.iso" "$T/disk"
fi
cp app/shim/libkvm.so "$DEVICE/libkvm.so.shim"
if [ "${1:-}" = deploy ]; then
  scp -rq "$DEVICE" "root@${2:?kvm host}:/kvmcomm/apps/"
  echo "deployed; the launcher rescans within 10 s"
fi
