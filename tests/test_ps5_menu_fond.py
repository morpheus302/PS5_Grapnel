"""(ps5, lot 76) The launch screen's background file, scripts/ps5/menu_fond.py: version 1 (no --panel: the panel in its usual
place) and version 2 (--panel X0,Y0,X1,Y1 in percent: the panel's place in thousandths after the size), the bad --panel
values refused, and gpu/vk/vk_launcher.cpp's picture_load reading both with the same limits. Game-free: a small PNG made here.

    python -I -B tests/test_ps5_menu_fond.py
"""
import importlib.util
import struct
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "ps5" / "menu_fond.py"
LAUNCHER = ROOT / "gpu" / "vk" / "vk_launcher.cpp"


def png(w, h):
    rows = []
    for y in range(h):
        rows.append(b"\x00" + b"".join(bytes((x % 256, y % 256, (x + y) % 256)) for x in range(w)))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


def main():
    spec = importlib.util.spec_from_file_location("menu_fond", SCRIPT)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    fails = []
    with tempfile.TemporaryDirectory() as d:
        pic = Path(d) / "p.png"
        pic.write_bytes(png(40, 20))
        v1, v2 = Path(d) / "v1.rgba", Path(d) / "v2.rgba"
        m.main([str(pic), str(v1)])
        m.main([str(pic), str(v2), "--panel", "55,8,92,97"])
        b1, b2 = v1.read_bytes(), v2.read_bytes()
        if b1[:16] != b"HKBG" + struct.pack("<III", 1, 40, 20) or len(b1) != 16 + 40 * 20 * 4:
            fails.append("version 1: header or size")
        if b2[:32] != b"HKBG" + struct.pack("<IIIIIII", 2, 40, 20, 550, 80, 920, 970) or len(b2) != 32 + 40 * 20 * 4:
            fails.append("version 2: header or size")
        if b1[16:] != b2[32:]:
            fails.append("the pixels differ between the versions")
        for bad in ("10,10,5,50", "0,0,5,50", "50,50,120,90", "a,b,c,d", "1,2,3"):
            try:
                m.panel_of(bad)
                fails.append("--panel %s accepted" % bad)
            except SystemExit:
                pass
    src = LAUNCHER.read_text(encoding="utf-8")
    for needle in ("(head[1] == 1 || head[1] == 2)", "head[6] - head[4] >= 100 && head[7] - head[5] >= 100",
                   "ok = ftell(f) == (long)(at + bytes)", "const bool own = bg.tex && bg.panel"):
        if needle not in src:
            fails.append("vk_launcher.cpp: %s not found" % needle)
    if fails:
        print("FAIL: " + "; ".join(fails))
        return 1
    print("PASS: the background file in versions 1 and 2, bad panels refused, the screen reading both with the same limits")
    return 0


if __name__ == "__main__":
    sys.exit(main())
