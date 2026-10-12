#!/usr/bin/env bash
# Assemble the on-device app directory: the shim and the Mac client's install disk.
# Usage: ./build.sh [deploy <kvm-host>]
# The disk comes from the client repository's build: SIDECAST_CLIENT_ISO, by default a checkout of it at
# client/ (ignored here) after its own ./build.sh. Build the shim first (see README).
set -euo pipefail
cd "$(dirname "$0")"
DEVICE=app/sidecast
cp "${SIDECAST_CLIENT_ISO:-client/build/mac-client.iso}" "$DEVICE/mac-client.iso"
cp app/shim/libkvm.so "$DEVICE/libkvm.so.shim"
if [ "${1:-}" = deploy ]; then
  scp -rq "$DEVICE" "root@${2:?kvm host}:/kvmcomm/apps/"
  echo "deployed; the launcher rescans within 10 s"
fi
