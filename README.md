# Sidecast

Stream a computer into a NanoKVM Go+ over the USB cable it is already plugged into, for machines that
have no spare display output to give the KVM.

The NanoKVM Go+ takes video only through USB-C DisplayPort Alt Mode. A Mac with two external monitors
on an M-series Pro chip has no third display output, so the KVM never sees a picture. Sidecast feeds
the KVM an H.264 or H.265 stream of the host's screen over the USB network link instead. The KVM's web
UI, keyboard, mouse, codec selection, bitrate and GOP controls keep working exactly as before, and the
KVM still captures any other target through its USB-C port when no Sidecast client is connected.

## How it works

```
host screen ──capture+encode──▶ USB network link ──▶ proxy shim inside the KVM server ──▶ browser
   (client)                       (TCP :5555)         (replaces libkvm.so, stock lib underneath)
```

- **Shim** (`app/shim/libkvm_shim.c`): a drop-in `libkvm.so` for the KVM's `NanoKVM-Server`. It loads the stock
  library and forwards every call, so the device behaves as shipped. While a client streams, frame
  reads are served from the network instead. A text control channel on the same socket tells the
  client which codec, bitrate, frame rate and GOP the KVM's UI asked for, when to pause (no viewer)
  and when to send a keyframe.
- **Client** (protocol in `docs/protocol.md`): a menu bar app for macOS. Captures a display, encodes with VideoToolbox,
  follows the shim's settings live, switches display from the menu, pauses when nobody is watching.
  Installs itself as a Login Item on first open. `Sidecast --install --sck` captures through ScreenCaptureKit
  instead: about a fifth of the CPU, at the price of the menu bar recording indicator.
  With more than one display, the KVM's mouse lands on the display the pointer is on, so the client puts the
  pointer on the streamed display when a viewer starts looking or when you pick a display. **Follow Pointer** in the menu (or
  `--install --follow`) turns that around: the stream follows the pointer, and a viewer who holds the pointer
  against the edge of the picture is carried onto the next display. Opening the viewer wakes the Mac's
  displays, and your input keeps them awake, as with any monitor; the lock screen still asks for the password.
- **Device app** (`app/sidecast/`): a touchscreen app for the Go+. One switch turns the shim on or off;
  when no client is connected it offers the client installer as a USB disk to the host.

## Install

On the KVM:

1. Copy `app/sidecast` to `/kvmcomm/apps/` on the Go+ (`./build.sh deploy <kvm-host>` does this),
   or upload it as a zip from the Apps page of the web UI.
2. Open **Sidecast** from the Apps page and tap the STREAM card.

On the host:

3. Tap **GET CLIENT** on the KVM. A disk named "Sidecast" appears on the host; macOS asks once to allow
   the new USB accessory.
4. Drag Sidecast to Applications and open it. Allow Screen Recording when asked. The stream starts
   within a minute and the KVM's web UI shows the host's screen.
5. Tap **EJECT DISK** on the KVM.

Sidecast runs as a Login Item from then on and reconnects whenever the cable is plugged in. The menu
bar glyph shows the status, lets you pick which display is streamed, and quits the app.

Uninstall: `Sidecast.app/Contents/Resources/Uninstall.command` (right-click the app, Show Package
Contents), or `Sidecast --uninstall` and move the app to the Trash. On the KVM, the STREAM switch off
restores the stock library; removing the app directory removes the rest.

## Building

- Shim, from `app/shim/`: `docker run --rm --platform linux/arm/v7 -v "$PWD":/src -w /src debian:trixie-20260918-slim sh -c
  'apt-get -qq update && apt-get -qq install -y gcc libc6-dev && gcc -O2 -shared -fPIC -Wl,-soname,libkvm.so.0 -o libkvm.so libkvm_shim.c -ldl -lpthread'`
- Shim self-check (native), from `app/shim/`: `cc -O1 -o test_shim test_shim.c -lpthread && ./test_shim test.h264 test.h265 test_vt.h264`
- Device app: `./build.sh [deploy <kvm-host>]`. It bundles the Mac client release pinned in `client.env`
  (signed and notarized, built in a separate repository), or a local build from `SIDECAST_CLIENT_ISO`.
- Preview the device app without hardware: `app/render-states.py <appbase dir> app/sidecast <out dir>`.

## Limits

- The host must be booted and logged in with the client running. This is remote desktop through the
  KVM's own pipeline, not out-of-band access; the KVM's USB-C capture still covers other targets.
- The KVM's absolute pointer lands on the host's main display, as macOS maps every absolute pointing
  device there. Streaming another display needs the host's relative mouse mode, or the planned
  pointer-follows-stream option.
- Audio is not carried. The USB link is USB 2.0, so only encoded video crosses it.
- Tested on a NanoKVM Go+ and an Apple silicon Mac. The shim targets the `kvmv_*` library interface
  shared across the NanoKVM family; other models are untested.

## Status

Working end to end on the author's hardware. Not yet published as a release.
