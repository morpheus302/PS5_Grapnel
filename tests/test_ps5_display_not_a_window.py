"""On the console start() is given a MkwVkDisplay (gpu/vk/vk_api.h: 16 bytes, its first word the tag MKWVK_DISPLAY_VERSION),
not the PC host's MkwVkWindow. Code that reads the handle as a MkwVkWindow must tell the two apart first: the tag, read as a
window's version, is past every version, so a callback was taken from beyond the structure and called. The merged eboot's
first run with a renderer ended that way in auto_profile (signal 11 at an address in .rodata, 2026-11-16).

gpu/vk/vk_main.cpp is read as text: after every cast of main_window to a MkwVkWindow, the tag is compared under __PROSPERO__
before the first callback is taken through it. No game, GPU, console or compiler.

    python -I -B tests/test_ps5_display_not_a_window.py
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
CAST = "(const MkwVkWindow*)main_window"
CALLBACK = re.compile(r"\bw->(desktop_size|refresh_mhz|drawable_size|create_surface)\b")


def main():
    text = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    places = [m.start() for m in re.finditer(re.escape(CAST), text)]
    if not places:
        raise SystemExit("FAIL: vk_main.cpp no longer casts main_window to a MkwVkWindow (bring this check up to date)")
    bad = []
    for at in places:
        use = CALLBACK.search(text, at)
        if not use:
            continue
        between = text[at:use.start()]
        guard = between.find("#ifdef __PROSPERO__")
        tag = between.find("w->version == MKWVK_DISPLAY_VERSION")
        if guard < 0 or tag < guard or "w = nullptr;" not in between[tag:]:
            bad.append(text.count("\n", 0, at) + 1)
    if bad:
        raise SystemExit("FAIL: vk_main.cpp line%s %s: main_window is read as a MkwVkWindow and a callback taken through it "
                         "before the console's MkwVkDisplay is told apart (MKWVK_DISPLAY_VERSION under __PROSPERO__)"
                         % ("s" if len(bad) > 1 else "", ", ".join(map(str, bad))))
    print("ok: %d reads of main_window as a MkwVkWindow, each after the console's display is told apart" % len(places))
    return 0


if __name__ == "__main__":
    sys.exit(main())
