# Working in this repository

Guidance for coding agents. Read it before changing anything; most of it was learned the hard way on
real hardware.

## Layout

| Path | What |
|---|---|
| `app/shim/libkvm_shim.c` | proxy `libkvm.so` for the NanoKVM Go+ server; C, no dependencies beyond libc |
| `app/shim/test_shim.c` | native self-check; must pass before any shim change ships |
| `docs/protocol.md` | the client protocol: transport, control lines, stream requirements, ffmpeg recipe |
| `app/sidecast/` | the Go+ touchscreen app (Python, SDK `appbase`); `actions.py` is ssh-testable |
| `app/render-states.py` | renders the device app offscreen through the real SDK for preview |
| `build.sh` | assembles the device app with the client's install disk; `deploy <host>` copies to the KVM |

The Mac client is built in a separate repository and arrives here only as `mac-client.iso`; see `build.sh`.

## Invariants

- The shim must behave like the stock library whenever no client is attached. The stock
  `libkvm.so.0.1.0` is never modified; the shim is a sibling file and a symlink flip.
- `kvmv_read_img` serves whatever codec is flowing to every caller. Never filter by the requested type:
  the server runs stale stream loops during a mode switch and they would starve the live one.
- The shim is built against one `nanokvm-go` package version, recorded as `KVM_VERSION` in `actions.py`
  and read on the device with `dpkg-query`. The device app refuses to turn the shim on under any other
  version; a `dlopen` of a library the shim was not compiled against can fail in ways the stock server
  does not survive. Bump the constant only after rebuilding and re-testing the shim.
- Codec and encoder settings come from `/etc/kvm/kvm_vin.toml`, which the server rewrites on every UI
  change. Do not infer them from call arguments.
- Keyframe samples are self-contained: parameter sets prepended to the IDR. The browser's direct-mode
  decoder discards everything before the first keyframe message.
- Per-caller state is keyed by requested type (1 = H.264, 5 = H.265), never by thread id. Go runs a
  goroutine's cgo calls on varying OS threads.
- H.264 SPS detection is by NAL type, not header byte: VideoToolbox emits `0x27`, x264 `0x67`.
- Frames end with an access-unit delimiter from the client. The idle flush is a 200 ms fallback only; a
  short idle timeout splits large keyframes on the USB 2.0 link.
- Never SIGPIPE the server: all socket writes use `MSG_NOSIGNAL`. A newer client replaces the current
  one, and TCP keepalive is on, because a USB re-enumeration kills the host side without a FIN.
- The stock `kvmv_close_video_stream` blocks about three seconds; skip it while a client streams and
  replay it on disconnect.

## Device app

- The canvas is landscape: 284 by 240 logical, bottom 14 rows under the bezel. Derive every position
  from `fb.width`, `fb.height` and the `visible()` helper. Do not trust the SDK README's 240 by 270 table.
- Pure black background and greys in multiples of 8. The panel lifts dark tones and 16-bit colour
  turns near-black tints blue.
- The SDK font is an 8 px monospace at integer scale: 24 characters per 192 px lane at scale 1, 12 at
  scale 2. Copy must fit; the whole STREAM card is the toggle target.
- Copy says "host", never "Mac". The device side is client-agnostic.
- Preview with `app/render-states.py` before asking for a photo of the panel.

## Verifying a change

- Shim: build `test_shim`, run it, then cross-build in Docker, deploy with `./build.sh deploy <host>` and switch with `python3 /kvmcomm/apps/sidecast/actions.py shim-on` over ssh.
- Shim performance: `bench_shim.c` reports the feed thread's CPU per stream; run it natively and cross-built on the
  KVM (static, `-static`) before and after a change to `feed()` or `on_nal()`. At 8 Mbit/s the parse path costs about
  4 ms of the KVM's core per second of stream; the server log is `/var/log/nanokvm-go/NanoKVM-Server.log`.
- End to end: the KVM server log shows `libkvm_shim:` lines; the client logs to `/tmp/sidecast.log`;
  bytes arriving at the KVM are visible in the usb0 interface counters.
- The device app is driven by `actions.py` over ssh for everything except taps.
