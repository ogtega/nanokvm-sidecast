#!/usr/bin/env bash
# Assemble the on-device app in build/sidecast from app/sidecast: the shim, the installer disk the KVM serves to the
# host, and app.json's version and nanokvm_go from version.toml. Anything but the clean commit tagged v<sidecast>
# gets the commit added (0.3.0+1a2b3c4, .dirty with uncommitted changes), so a dev build never claims a release.
# Usage: ./build.sh [deploy <kvm-host>]
# The disk, clients.iso, holds one installer per host platform, Sidecast-v<version>-<platform>.<ext>: every one in
# release client-v<client> from version.toml (macos-arm64.dmg, windows-arm64.exe, windows-x64.exe), or every one
# in SIDECAST_CLIENTS=<dir> for local client builds (the client repository's build/). Needs xorriso, or hdiutil
# on a Mac. Build the shim first (see README).
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
if [ -n "${SIDECAST_CLIENTS:-}" ]; then
  find "$SIDECAST_CLIENTS" -maxdepth 1 -type f -name 'Sidecast-v*-*' -exec cp {} "$T/disk/" \;   # client builds already follow the naming
else
  # The client release is published whole (its CI uploads every platform at once), so take all of its installers.
  curl -fsSL ${GH_TOKEN:+-H "Authorization: Bearer $GH_TOKEN"} "https://api.github.com/repos/$REPO/releases/tags/client-v$CLIENT" | python3 -c \
    'import json, sys; [print(a["browser_download_url"]) for a in json.load(sys.stdin)["assets"] if a["name"].startswith(sys.argv[1])]' \
    "Sidecast-v$CLIENT-" | (cd "$T/disk" && xargs -n 1 curl -fsSLO)
fi
[ -n "$(ls "$T/disk")" ] || { echo "no client installers (release client-v$CLIENT, or SIDECAST_CLIENTS)"; exit 1; }
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
