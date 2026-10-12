#!/usr/bin/env bash
# Assemble the on-device app directory: the shim and the Mac client's install disk.
# Usage: ./build.sh [deploy <kvm-host>]
# The disk is the client release pinned in client.env, fetched with gh and checked against its hash, or
# SIDECAST_CLIENT_ISO for a local client build (client/build/mac-client.iso in a checkout of the client
# repository at client/). Build the shim first (see README).
set -euo pipefail
cd "$(dirname "$0")"
DEVICE=app/sidecast
if [ -n "${SIDECAST_CLIENT_ISO:-}" ]; then
  cp "$SIDECAST_CLIENT_ISO" "$DEVICE/mac-client.iso"
else
  [ -f client.env ] || { echo "no client pinned in client.env; set SIDECAST_CLIENT_ISO"; exit 1; }
  . ./client.env
  # ponytail: gh because the repo is private; plain curl of the release URL once it is public
  gh release download "$CLIENT" -R ogtega/nanokvm-sidecast -p mac-client.iso -O "$DEVICE/mac-client.iso" --clobber
  echo "$CLIENT_SHA256  $DEVICE/mac-client.iso" | shasum -a 256 -c - >/dev/null
fi
cp app/shim/libkvm.so "$DEVICE/libkvm.so.shim"
if [ "${1:-}" = deploy ]; then
  scp -rq "$DEVICE" "root@${2:?kvm host}:/kvmcomm/apps/"
  echo "deployed; the launcher rescans within 10 s"
fi
