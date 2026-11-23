"""(ps5, lot 69) The launch screen's background, made on the PC: a picture (PNG, 8-bit RGB or RGBA, not interlaced; any other
format through Pillow when it is installed) to hookshot_menu_fond.rgba, which the screen reads from the game folder on the
console (/data/grapnel/hookshot_menu_fond.rgba; gpu/vk/vk_launcher.cpp picture_load) and the eboot never holds:

    16 bytes: "HKBG", then little-endian 32-bit words: version 1, width, height
    (version 2, with --panel: 16 more bytes, the panel's place x0 y0 x1 y1 in thousandths of the picture's width and height)
    width * height * 4 bytes: the pixels, RGBA, row by row from the top

The picture is not resized or filtered here (the screen scales it with the GPU's linear filter, never sharpened). 16 to 4096
pixels a side. Without the file the screen draws its plain dark background, the panel in the same place. A picture made by
someone else (a SteamGridDB hero, say) is for the player's own console: it never goes into a published build.

    python3 -I scripts/ps5/menu_fond.py <picture.png> <hookshot_menu_fond.rgba> [--panel X0,Y0,X1,Y1]

--panel puts the screen's panel on that part of the picture, in percent of its width and height (55,8,92,97: the right
side); without it, the panel keeps its usual place (2.0-27.1% x 8-91%).
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"HKBG"
VERSION = 1


def png_rgba(data):
    """The pixels of an 8-bit RGB or RGBA PNG that is not interlaced, as RGBA bytes: (width, height, bytes); None for any other"""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    at, idat, head = 8, [], None
    while at + 8 <= len(data):
        n, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + n]
        if kind == b"IHDR":
            head = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
        at += 12 + n
    if not head:
        return None
    w, h, depth, colour, _, _, interlace = head
    if depth != 8 or colour not in (2, 6) or interlace:
        return None
    bpp = 3 if colour == 2 else 4
    raw = zlib.decompress(b"".join(idat))
    stride = w * bpp
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            elif f != 0:
                raise SystemExit("PNG: unknown filter %d on row %d" % (f, y))
        row = out[y * w * 4:(y + 1) * w * 4]
        if bpp == 4:
            row[:] = line
        else:
            row[0::4], row[1::4], row[2::4] = line[0::3], line[1::3], line[2::3]
            row[3::4] = b"\xff" * w
        out[y * w * 4:(y + 1) * w * 4] = row
        prev = line
    return w, h, bytes(out)


def read_picture(path):
    data = Path(path).read_bytes()
    got = png_rgba(data)
    if got:
        return got
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("%s: not an 8-bit RGB or RGBA PNG, and Pillow is not installed to read it" % path)
    with Image.open(path) as im:
        im = im.convert("RGBA")
        return im.width, im.height, im.tobytes()


def panel_of(text):
    """--panel "X0,Y0,X1,Y1" in percent to thousandths, checked as the screen checks them (vk_launcher.cpp picture_load)"""
    try:
        x0, y0, x1, y1 = (int(round(float(v) * 10)) for v in text.split(","))
    except ValueError:
        raise SystemExit("--panel wants four percentages: X0,Y0,X1,Y1")
    if not (0 <= x0 < x1 <= 1000 and 0 <= y0 < y1 <= 1000 and x1 - x0 >= 100 and y1 - y0 >= 100):
        raise SystemExit("--panel %s: inside the picture, left before right, top before bottom, at least 10%% each way" % text)
    return x0, y0, x1, y1


def write(w, h, rgba, out, panel=None):
    if not (16 <= w <= 4096 and 16 <= h <= 4096) or len(rgba) != w * h * 4:
        raise SystemExit("the picture is %dx%d: 16 to 4096 pixels a side" % (w, h))
    head = MAGIC + struct.pack("<III", VERSION, w, h) if panel is None else MAGIC + struct.pack("<IIIIIII", 2, w, h, *panel)
    Path(out).write_bytes(head + rgba)
    return len(head)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("picture")
    p.add_argument("out")
    p.add_argument("--panel", help="the panel's place on the picture, X0,Y0,X1,Y1 in percent")
    a = p.parse_args(argv)
    panel = panel_of(a.panel) if a.panel else None
    w, h, rgba = read_picture(a.picture)
    head = write(w, h, rgba, a.out, panel)
    where = "the panel at %.1f-%.1f%% x %.1f-%.1f%%" % (panel[0] / 10, panel[2] / 10, panel[1] / 10, panel[3] / 10) if panel else "the panel in its usual place"
    print("%s: %dx%d, %d bytes (%.1f MiB in memory while the screen shows), %s" % (a.out, w, h, head + len(rgba), len(rgba) / 1048576, where))


if __name__ == "__main__":
    sys.exit(main())
