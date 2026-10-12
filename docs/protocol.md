# Sidecast client protocol

How a host feeds its screen to a NanoKVM Go or Go+ through the Sidecast shim. This is enough to write a client
for any platform; the Sidecast Mac client is one implementation. The shim in `app/shim/libkvm_shim.c` is the
authority when this document and the code disagree.

## Transport

- One TCP connection from the host to the KVM, port 5555. Over the USB cable the KVM is `10.136.177.1`
  on the USB network interface; any other route to the KVM works too.
- The client sends only the video byte stream. The KVM sends only text control lines. Both directions
  share the one socket.
- A newer connection replaces the current one: the shim accepts it and drops the old stream. Use this to
  recover from a half-open socket after a USB re-enumeration.
- The KVM enables TCP keepalive (5 s idle, 5 s interval, 3 probes, so a vanished host is noticed in about
  20 s). Set `TCP_NODELAY` on the client side. On any disconnect, reconnect after a second or two, forever.
- No authentication. The link is the USB cable.

## Control lines, KVM to client

ASCII lines terminated by `\n`. Parse them as they arrive; they are not padded or aligned with the video.

| Line | Meaning |
|---|---|
| `ver 1` | protocol version; always the first line after connect |
| `codec h264` or `codec h265` | the codec the KVM's web UI has selected |
| `fps N` | target frame rate, N ≥ 1 (the KVM's "auto" is sent as 30) |
| `bitrate N` | target bitrate in kbit/s |
| `gop N` | keyframe interval in frames |
| `idr` | emit a keyframe as soon as possible (a viewer just connected or switched) |
| `pause` | nobody is watching: stop capturing and sending |
| `resume` | a viewer is back: start capturing and sending again |

On connect the KVM sends `ver 1`, then the full state: `codec`, `fps`, `bitrate`, `gop`, then `pause`
or `resume`, the latter only if a viewer has read a frame within the last 15 s. After that it sends only lines
whose value changed, plus `idr` when requested. Values come from the KVM's own configuration, which the web
UI rewrites on every change, so a client that follows them live behaves exactly as the stock capture pipeline
would. `pause` arrives after about 15 s with no viewer reading
frames; `resume` arrives on the next viewer read.

A client must act on `codec` by restarting its encoder with the new codec. The other settings can be applied
to a running encoder. Unknown lines should be ignored, not treated as errors.

## Video stream, client to KVM

A raw Annex-B elementary stream in the codec last requested: H.264 or H.265, NAL units with 3- or 4-byte
start codes, nothing else on the socket. The shim splits it on start codes and hands each NAL to the KVM
server with the stock library's return codes, so the browser sees exactly what it would see from the KVM's
own capture.

Requirements, in order of how badly things go wrong without them:

1. **Real time, no frame reordering.** No B-frames. The browser decodes in display order as frames arrive.
2. **Self-contained keyframes.** Every IDR must be preceded by its parameter sets: SPS and PPS for H.264,
   VPS, SPS and PPS for H.265. The shim caches the most recent sets and prepends them to each keyframe, so
   a stream that sends them once would work, but sending them with every keyframe is the safe form.
   The browser's decoder discards everything before the first keyframe it sees.
3. **An access-unit delimiter after every frame.** H.264: NAL type 9, bytes `09 F0`. H.265: NAL type 35,
   bytes `46 01 50`. The shim completes a NAL only when the next start code arrives; the delimiter makes
   every frame deliverable immediately. Without it the last frame waits for a 200 ms idle flush, and a
   large keyframe split across USB packets can be flushed mid-frame.
4. **Keyframe interval equal to `gop`**, and a keyframe promptly after `idr`.
5. **One slice per keyframe.** Multi-slice IDRs work but get the parameter sets prepended to each slice.
6. **NAL units under 1 MiB.** Larger ones are dropped.

The shim drops SEI and other non-VCL NALs, so sending them is harmless. It detects the codec from the
parameter sets, not from what it asked for, and serves whatever is flowing: a client that switches codec
late or never does not break the pipeline, it only mismatches what the viewer expects until it catches up.

Resolution, profile and pixel format are the client's choice. H.264 Main or High and H.265 Main at the
display's native size, or scaled to a width of 2560, decode fine in the KVM's browser viewer. The Mac client
captures at native resolution, scales to a target width, and uses 4:2:0 limited range.

## Lifecycle

```
connect 10.136.177.1:5555
read lines; apply codec/fps/bitrate/gop; on resume start capture+encode; on pause stop
for each encoded frame: write [params if keyframe][NALs][AUD] to the socket
on idr: force a keyframe on the next frame
on socket error or EOF: stop capture, close, wait 1-2 s, reconnect
```

The client needs no state across connections: the KVM resends the full state on every connect.

## Quickest implementation on Linux or Windows

ffmpeg already produces a conforming stream, including the access-unit delimiters, and can write straight
to the socket. What it cannot do is react to control lines, so a small wrapper must own the socket, read the
lines, and restart ffmpeg with new arguments when `codec`, `fps`, `bitrate` or `gop` change, stopping it on
`pause` and starting it on `resume`. `idr` cannot be honoured by ffmpeg mid-run; viewers will wait for the next
keyframe, which is at most `gop` frames away.

Encoding arguments that produce a conforming stream, with `-f h264` or `-f hevc` and the encoder of the
platform in place of the VideoToolbox ones:

```
ffmpeg -hide_banner -loglevel error -nostdin \
  <capture input: x11grab, kmsgrab, gdigrab, ddagrab...> -framerate FPS \
  -vf scale=2560:-2 -fps_mode passthrough \
  -c:v h264_<encoder> -profile:v main -bsf:v h264_metadata=aud=insert \
  -b:v BITRATEk -g GOP -bf 0 -flush_packets 1 -f h264 pipe:1
```

For H.265 use `-c:v hevc_<encoder> -bsf:v hevc_metadata=aud=insert -f hevc`. `-bf 0` removes B-frames,
`-flush_packets 1` sends each frame as soon as it is encoded, and the `aud=insert` filters add the
access-unit delimiters. Write ffmpeg's stdout to the socket.

## Versioning

`ver 1` is the protocol version. The shim is built against one KVM application package version, pinned as
`nanokvm_go` in `version.toml`; the device app refuses to load it into any other. The client protocol does not
depend on the KVM version.
