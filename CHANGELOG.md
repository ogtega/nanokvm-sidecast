# Changelog

Changes to Sidecast for NanoKVM: the shim and the device app. Each version names the `nanokvm-go` version
it supports; Sidecast refuses to switch on under any other. Releases of the Mac and Windows apps (`client-v*`)
list their own changes.

## [Unreleased]

### Security

- Shim: accepts a stream only from the computer on the USB cable. It used to accept one from any network the KVM
  is on, so anyone on the same LAN or VPN could replace the video viewers see.

## [0.4.0] - 2026-10-14

Requires `nanokvm-go` 0.2.2 on the NanoKVM Go or Go+. The installer disk carries Sidecast for Mac and Windows
0.4.0.

### Added

- Installer disk: carries every installer of the client release it names, now the Windows app (x64 and Arm)
  beside the Mac app.

### Changed

- Device app: its status check, every 2 seconds while the app is open, costs about 2 ms of the KVM's CPU
  instead of about 60.
- Device app: switching STREAM on puts the shim in place without rewriting the file the running server has
  loaded.

### Fixed

- Shim: video from encoders that split each picture into slices, such as the hardware encoder on Windows on
  Arm tablets, showed black in the browser. The shim now hands each picture to the viewer whole.

## [0.3.0] - 2026-10-12

Requires `nanokvm-go` 0.2.2 on the NanoKVM Go or Go+.

### Added

- Shim: streams H.264 or H.265 from a client over the USB network link into the KVM's own viewer, and
  follows the codec, bitrate, frame rate and keyframe settings chosen in the web UI. With no client
  connected the KVM behaves exactly as shipped.
- Device app: one STREAM switch to turn the shim on and off, a status line for the host and the stream,
  and GET CLIENT / EJECT DISK to serve the client installers to the host as a USB disk.
- Installer disk with one installer per platform; this release carries the Mac app.
- Viewer tracking: the client is told to pause about 15 seconds after the last viewer leaves and to resume
  when one returns.
- Open client protocol (`ver 1`), documented in `docs/protocol.md`, with an ffmpeg recipe for other
  systems.
