#!/usr/bin/env python3
"""Device-side actions for Sidecast. No framebuffer dependency so it can be driven from ssh:
   python3 actions.py status|shim-on|shim-off|attach|detach"""
import os, shutil, subprocess, sys
from pathlib import Path

APP = os.path.dirname(os.path.abspath(__file__))
DL = "/kvmapp/server/dl_lib"
SHIM_SRC, SHIM = os.path.join(APP, "libkvm.so.shim"), os.path.join(DL, "libkvm.so.shim")
LINK = os.path.join(DL, "libkvm.so")
ISO = os.path.join(APP, "mac-client.iso")
DISK_FLAG = "/boot/usb.disk0"
LUN = "/sys/kernel/config/usb_gadget/g0/functions/mass_storage.disk0/lun.0"
USBDEV = "/opt/scripts/usbdev.sh"
KVM_VERSION = "0.2.2"   # nanokvm-go package the shim was built and tested against; it dlopens that package's libkvm.so.0.1.0

def sh(*cmd, timeout=60):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).returncode == 0

# ---- shim
def shim_on_state():
    try: return os.readlink(LINK) == "libkvm.so.shim"
    except OSError: return False

def kvm_version():
    out = subprocess.run(["dpkg-query", "-W", "-f=${Version}", "nanokvm-go"], capture_output=True, text=True)
    return out.stdout.strip() or "unknown"

def shim_on():
    if kvm_version() != KVM_VERSION: return False   # never load the shim into a server it was not built for
    shutil.copy2(SHIM_SRC, SHIM)   # 72 KB, every time: no staleness check to get wrong
    os.symlink("libkvm.so.shim", LINK + ".tmp"); os.replace(LINK + ".tmp", LINK)
    return sh("systemctl", "restart", "nanokvm-go")

def shim_off():
    os.symlink("libkvm.so.0", LINK + ".tmp"); os.replace(LINK + ".tmp", LINK)
    ok = sh("systemctl", "restart", "nanokvm-go")
    try: os.remove(SHIM)  # leave the device as shipped
    except OSError: pass
    return ok

# ---- installer disk (same sequence the server uses for ISO mounts)
def disk_state():
    try: return Path(LUN, "file").read_text().strip() != ""
    except OSError: return False

def disk_attach():
    open(DISK_FLAG, "a").close()
    if not sh(USBDEV, "restart", timeout=90): return False
    Path(LUN, "cdrom").write_text("1")
    Path(LUN, "ro").write_text("1")
    Path(LUN, "file").write_text(ISO)
    return True

def disk_detach():
    try: Path(LUN, "file").write_text("\n")
    except OSError: pass
    try: os.remove(DISK_FLAG)
    except OSError: pass
    return sh(USBDEV, "restart", timeout=90)

# ---- USB host
UDC = "/sys/class/udc"

def host_state():
    """A USB host has enumerated the gadget: something is plugged in and awake on the USB-C port."""
    try: return any(Path(UDC, u, "state").read_text().strip() == "configured" for u in os.listdir(UDC))
    except OSError: return False

# ---- Mac client link
def link_state():
    out = subprocess.run(["ss", "-tn", "state", "established", "( sport = :5555 )"], capture_output=True, text=True).stdout
    return len(out.strip().splitlines()) > 1

def host_settings():
    """Codec, bitrate and fps the server currently asks for (what the host client is encoding)."""
    out = {"mode": "h264", "bitrate": 0, "fps": 0}
    try:
        for line in open("/etc/kvm/kvm_vin.toml"):
            k, _, v = line.partition("=")
            k, v = k.strip(), v.strip().strip("'\"")
            if k in out: out[k] = v if k == "mode" else int(v)
    except (OSError, ValueError): pass
    return out

def status():
    v = kvm_version()
    return {"shim": shim_on_state(), "link": link_state(), "disk": disk_state(), "host": host_state(), "settings": host_settings(),   # settings included so a codec or bitrate change redraws
            "kvm": v, "supported": v == KVM_VERSION}

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    fn = {"status": status, "shim-on": shim_on, "shim-off": shim_off, "attach": disk_attach, "detach": disk_detach}[cmd]
    print(fn())
