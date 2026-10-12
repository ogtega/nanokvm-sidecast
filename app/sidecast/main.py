#!/usr/bin/env python3
"""Sidecast: stream a host into this KVM. One toggle (the whole card), one status line, and a
secondary chip only when there is something to do about the host client."""
import math
import time

import actions
from appbase import Rect, app, rgb565

# Panel notes: dark tones render lighter than on a monitor, and RGB565 has only 32 blue steps, so
# near-black tints come out blue. Greys are multiples of 8 so all three channels quantise evenly.
BG = rgb565(0, 0, 0)
CARD = rgb565(24, 24, 24)
CHIP = rgb565(32, 32, 32)
CHIP_EDGE = rgb565(72, 72, 72)
TRACK_OFF = rgb565(56, 56, 56)
KNOB_OFF = rgb565(200, 200, 200)
GREEN = rgb565(40, 152, 88)
GREEN_TEXT = rgb565(104, 208, 136)
AMBER = rgb565(240, 160, 48)
WHITE = rgb565(248, 248, 248)
TEXT = rgb565(200, 200, 200)
MUTED = rgb565(136, 136, 136)
CAPTION = rgb565(176, 176, 176)

INSET = 24   # keeps taps out of the launcher's 40px edge zones (its exit gesture)


def visible(fb):
    """Visible canvas: the panel is landscape (rotate 90), the bottom 14 rows sit under the bezel."""
    return Rect(0, 0, fb.width, fb.height - 14)


def layout(fb):
    """Card (toggle target) and chip (secondary action) rects for whatever canvas we were given."""
    v = visible(fb)
    w = v.w - 2 * INSET
    card = Rect(v.x + INSET, v.y + 28, w, 56)
    chip = Rect(v.x + INSET, v.y + 144, w, 44)
    return v, card, chip


def rrect(fb, x, y, w, h, r, color):
    """Rounded rectangle from row spans."""
    for row in range(h):
        dy = min(row, h - 1 - row)
        inset = 0
        if dy < r:
            inset = r - int(math.sqrt(max(0.0, r * r - (r - dy - 0.5) ** 2)))
        fb.fill_rect(x + inset, y + row, w - 2 * inset, 1, color)


def circle(fb, cx, cy, d, color):
    r = d / 2.0
    for row in range(d):
        dy = row + 0.5 - r
        half = math.sqrt(max(0.0, r * r - dy * dy))
        fb.fill_rect(int(cx - half), cy + row, int(2 * half + 0.5), 1, color)


def tracked_text(fb, cx, cy, text, color, gap=3):
    w = len(text) * 8 + (len(text) - 1) * gap
    x = cx - w // 2
    for ch in text:
        fb.draw_text(x, cy, ch, color, 1)
        x += 8 + gap


def draw(ctx, st, busy=None):
    fb = ctx.fb
    v, card, chip = layout(fb)
    cx = v.x + v.w // 2
    fb.clear(BG)
    tracked_text(fb, cx, v.y + 10, "SIDECAST", CAPTION)

    on = st["shim"]
    locked = not on and not st["supported"]   # the shim may only be loaded into the KVM version it was built for; off is always allowed
    rrect(fb, card.x, card.y, card.w, card.h, 18, CARD)
    fb.draw_text(card.x + 16, card.y + 12, "STREAM", MUTED if locked else WHITE, 2)
    fb.draw_text(card.x + 16, card.y + 36, "on" if on else "off", GREEN_TEXT if on else MUTED, 1)
    tx = card.x + card.w - 16 - 60
    rrect(fb, tx, card.y + 12, 60, 32, 16, GREEN if on else TRACK_OFF)
    circle(fb, tx + (44 if on else 16), card.y + 16, 24, WHITE if on else (TRACK_OFF if locked else KNOB_OFF))

    link, disk, host = st["link"], st["disk"], st["host"]
    if locked:
        dot, text, hint = AMBER, "KVM %s unsupported" % st["kvm"], "Shim built for KVM %s" % actions.KVM_VERSION
    elif not on:
        dot, text, hint = MUTED, "Showing USB-C input", "Turn on to stream host"
    elif link:
        s = st["settings"]
        dot, text = GREEN, "Host connected"
        hint = "%s  %d kbps  %s" % (s["mode"].upper().replace("H", "H."), s["bitrate"], ("%dfps" % s["fps"]) if s["fps"] else "auto")
    else:
        dot, text = AMBER, "Waiting for the host"
        hint = "Install from the disk" if disk else ("Install the client:" if host else "Connect the host by USB")
    sy = card.y + card.h + 22   # status row centre line
    circle(fb, card.x + 22, sy - 6, 12, dot)           # 12 px dot centred on sy (circle() takes the top edge)
    fb.draw_text(card.x + 36, sy - 4, text, TEXT, 1)   # 8 px glyphs centred on sy
    fb.draw_text(card.x + 16, sy + 14, busy or hint, AMBER if busy else MUTED, 1)

    show_chip = disk or (on and host and not link)   # offer the client only when a host is plugged in and not streaming
    if show_chip:
        rrect(fb, chip.x, chip.y, chip.w, chip.h, 18, AMBER if disk else CHIP_EDGE)
        rrect(fb, chip.x + 1, chip.y + 1, chip.w - 2, chip.h - 2, 17, CHIP)
        fb.text_center(cx, chip.y + 14, "EJECT DISK" if disk else "GET CLIENT", WHITE, 2)
    fb.flush()
    return card, chip if show_chip else None


@app()
def main(ctx):
    st = actions.status()
    card, chip = draw(ctx, st)
    last = time.monotonic()
    while True:
        for px, py in ctx.taps():
            if card.contains(px, py):
                if not st["shim"] and not st["supported"]: continue   # locked: nothing to do
                draw(ctx, st, "Restarting the KVM...")
                actions.shim_off() if st["shim"] else actions.shim_on()
            elif chip and chip.contains(px, py):
                draw(ctx, st, "Resetting USB...")
                actions.disk_detach() if st["disk"] else actions.disk_attach()
            else:
                continue
            st = actions.status(); card, chip = draw(ctx, st); last = time.monotonic()
        if time.monotonic() - last > 2:
            new = actions.status()
            if new != st:
                st = new; card, chip = draw(ctx, st)
            last = time.monotonic()
        time.sleep(0.05)


if __name__ == "__main__":
    main()
