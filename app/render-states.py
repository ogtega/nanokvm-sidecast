"""Render each of Sidecast's states offscreen through the real SDK drawing code and write PNGs."""
import json, os, re, shutil, struct, sys, types, zlib
sdk, appsrc, out = sys.argv[1], sys.argv[2], sys.argv[3]
# Render a copy: the SDK needs app.json's version, actions.py its nanokvm_go (only builds add them, from
# version.toml), the SDK a folder named after app_id, and the imports write __pycache__.
appdir = os.path.join(out, os.path.basename(os.path.normpath(appsrc)))
shutil.rmtree(appdir, ignore_errors=True); shutil.copytree(appsrc, appdir)
pins = dict(re.findall(r'^(\w+) = "([^"]*)"', open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "version.toml")).read(), re.M))
meta = json.load(open(os.path.join(appdir, "app.json")))
meta.setdefault("version", pins["sidecast"] + "+preview"); meta.setdefault("nanokvm_go", pins["nanokvm_go"])   # as build.sh writes them
json.dump(meta, open(os.path.join(appdir, "app.json"), "w"))
sys.path[:0] = [sdk, appdir]
os.environ["APPBASE_FB_ROTATE"] = "90"
fbfile = os.path.join(out, "fb.bin")
open(fbfile, "wb").write(bytes(480 * 284))
os.environ["APPBASE_FB_DEVICE"] = fbfile
import appbase
appbase._probe_screeninfo = lambda fd: (240, 284, 16, 480)  # this unit: 240x284 physical, rotate 90 -> 284x240 logical
import actions
import main as app


def png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

def snapshot(name):
    """Read the visible logical canvas back using the SDK's own logical-to-physical mapping."""
    fb = ctx.fb
    data = open(fbfile, "rb").read()
    w, h = fb.width, fb.height - 14  # rotate 90: the hidden physical columns are the bottom logical rows
    rgb = bytearray()
    for ly in range(h):
        for lx in range(w):
            px, py = fb._phys(lx, ly)
            v = struct.unpack_from("<H", data, py * fb.stride + px * 2)[0]
            rgb += bytes(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
    png(os.path.join(out, name + ".png"), w, h, bytes(rgb))

states = {   # journey order; names match the frames in the Sketch file
    "A-off": ({"shim": False, "link": False, "disk": False, "host": False}, None),
    "B-nohost": ({"shim": True, "link": False, "disk": False, "host": False}, None),
    "C-waiting": ({"shim": True, "link": False, "disk": False, "host": True}, None),
    "D-attached": ({"shim": True, "link": False, "disk": True, "host": True}, None),
    "E-connected": ({"shim": True, "link": True, "disk": False, "host": True, "settings": {"mode": "h265", "bitrate": 8000, "fps": 30}}, None),
    "F-unsupported": ({"shim": False, "link": False, "disk": False, "host": True, "kvm": "0.3.0", "supported": False}, None),
    "G-busy": ({"shim": False, "link": False, "disk": False, "host": False}, "Restarting the KVM..."),
}
ctx = types.SimpleNamespace(fb=appbase.FrameBuffer())   # draw() only needs fb
print("canvas", ctx.fb.width, "x", ctx.fb.height, "rotate", ctx.fb.rotate)
for name, (st, busy) in states.items():
    st.setdefault("settings", {"mode": "h264", "bitrate": 0, "fps": 0}); st.setdefault("kvm", "0.2.2"); st.setdefault("supported", True)
    app.draw(ctx, st, busy)
    snapshot(name)
print("rendered", list(states))
