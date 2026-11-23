"""(ps5, lot 79) The in-game menu's shortcut on the console (the Grapnel menu since lot 80): the touchpad's click (BACK) and
R1 together, in the host (the chord kept from the game, host/nn_hid.c) and in the menu (the chord that opens it,
gpu/vk/vk_menu.cpp), never Options (START: the game's own pause), and the R1 of the chord not also the next tab; Back and
Start as before off the console. The texts that name the shortcut say the same. Game-free: the sources read.

    python -I -B tests/test_ps5_menu_chord.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def branches(src, marker):
    """The #ifdef __PROSPERO__ / #else lines that follow the first line holding marker's context: (console, elsewhere)"""
    m = re.search(r"#ifdef __PROSPERO__[^\n]*\n(.*?)#else\n(.*?)#endif", src[src.index(marker) - 600:], re.S)
    return (m.group(1), m.group(2)) if m else (None, None)


def main():
    fails = []
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    logic = (ROOT / "gpu" / "vk" / "launcher_logic.h").read_text(encoding="utf-8")
    for name, src, marker, shift in (("host/nn_hid.c", hid, "if ((raw & chord) == chord) raw &= ~chord;", "1ULL"),
                                     ("vk_menu.cpp", menu, "const bool chord_now =", "1u")):
        console, other = branches(src, marker)
        if console is None:
            fails.append("%s: no console branch for the chord" % name)
            continue
        if "HOS_PAD_BACK" not in console or "HOS_PAD_RSHOULDER" not in console or "HOS_PAD_START" in console:
            fails.append("%s: the console's chord is not the touchpad and R1 alone" % name)
        if "HOS_PAD_BACK" not in other or "HOS_PAD_START" not in other:
            fails.append("%s: off the console the chord is no longer Back and Start" % name)
    if "(pad(HOS_PAD_RSHOULDER) && !chord_now)" not in menu:
        fails.append("vk_menu.cpp: the chord's R1 is also the next tab")
    if "touchpad and Options" in logic or "pavé tactile et Options" in logic:
        fails.append("launcher_logic.h: a text still names touchpad and Options")
    if "touchpad and R1 together" not in logic or "pavé tactile et R1 ensemble" not in logic:
        fails.append("launcher_logic.h: the texts do not name touchpad and R1")
    # (the launch screen's home page names the shortcut, above its prompts)
    launcher = (ROOT / "gpu" / "vk" / "vk_launcher.cpp").read_text(encoding="utf-8")
    if "En jeu : pavé tactile + R1 ouvre le menu Grapnel." not in logic or "T(L::T_MENU_SHORTCUT)" not in launcher:
        fails.append("the home page does not name the in-game menu's shortcut")
    # (the left stick as the D-pad for the menus only: hos_pad_raw gives the menus' copy, the game keeps pad_raw_now)
    if "uint32_t hos_pad_raw(void) { return (uint32_t)pad_menu_now; }" not in hid:
        fails.append("host/nn_hid.c: the menus do not read their own copy of the pads")
    menu_part = hid[hid.index("uint64_t menu = raw;"):hid.index("InterlockedExchange(&pad_menu_now, (LONG)menu);")] \
        if "uint64_t menu = raw;" in hid and "InterlockedExchange(&pad_menu_now, (LONG)menu);" in hid else ""
    for need in ("#ifdef __PROSPERO__", "s[0] < -19660", "s[0] > 19660", "s[1] > 19660", "s[1] < -19660", "HOS_PAD_UP",
                 "HOS_PAD_DOWN", "HOS_PAD_LEFT", "HOS_PAD_RIGHT"):
        if need not in menu_part:
            fails.append("host/nn_hid.c: the stick as the D-pad lacks %s" % need)
    if "InterlockedExchange(&pad_raw_now, (LONG)raw);" not in hid:
        fails.append("host/nn_hid.c: the game's pad_raw_now changed")
    if fails:
        print("FAIL: " + "; ".join(fails))
        return 1
    print("PASS: on the console the touchpad and R1 open the menu (host and menu), not Options; R1 of the chord is not a tab; "
          "Back and Start elsewhere; the texts say so")
    return 0


if __name__ == "__main__":
    sys.exit(main())
