"""(ps5, lot 80) What the player sees on the console says Grapnel, the port's public name, not Hookshot, and the console's
in-game menu has its last English values in French. No game, GPU or console:

- the launch screen's title (gpu/vk/vk_launcher.cpp draw_title) is GRAPNEL; none of the launch screen's texts
  (gpu/vk/launcher_logic.h kTexts, English and French) names Hookshot, and the home page's shortcut line names the Grapnel
  menu; the in-game menu's rows and their help (gpu/vk/vk_menu.cpp rows[]) name no Hookshot, the console's row is
  "Back to the Grapnel screen" and launcher_logic.h lists it by that name; no French text of the menu names Hookshot
  (the game's own item, the Hookshot of the Debug tab's inventory, is no row name: it stays);
- compiled with a C++ compiler: gpu/vk/fps_stats.h's names the console's menu shows (the FPS counter row's levels, Off to
  Breakdown, which the console offers; the FPS counter position row's four corners) each have a French text in
  gpu/vk/menu_i18n.h, the launch screen's words for the same levels (Images/s, Temps d'image, Graphique, Detail);
- by reading the source: the counter on screen measures its "FPS" as it draws it (AddText, no lookup), so the French table's
  "Images/s" for the row's value does not move the number;
- the title under the tile: nx2/toolchain_ps5.py param_json, GRPN00001 "Grapnel", its content id's 16 characters.

    python -I -B tests/test_ps5_grapnel_texts.py [--cxx g++]
"""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block, between   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

NAMES_TEST = r'''#include "fps_stats.h"
#include "menu_i18n.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk;
static const char* fr(const char* en) {
    for (const menu_i18n::Lang& l : menu_i18n::kLangs)
        if (!std::strcmp(l.code, "fr"))
            for (int i = 0; i < l.count; i++)
                if (!std::strcmp(l.entries[i].en, en)) return l.entries[i].text;
    return nullptr;
}
int main() {
    // (the console's FPS counter row: Off to Breakdown, launcher_logic.h kSettings; Dev is not offered there)
    static const char* const want[] = {"D\xC3\xA9sactiv\xC3\xA9", "Images/s", "Temps d'image", "Graphique", "D\xC3\xA9tail"};
    for (int v = fps_stats::kOff; v <= fps_stats::kBreakdown; v++) {
        const char* t = fr(fps_stats::level_name(v));
        if (!t || std::strcmp(t, want[v])) std::fprintf(stderr, "level %d %s: %s\n", v, fps_stats::level_name(v), t ? t : "(none)");
        CHECK(t && !std::strcmp(t, want[v]));
    }
    // (the FPS counter position row's corners)
    static const char* const corners[] = {"En haut \xC3\xA0 droite", "En haut \xC3\xA0 gauche", "En bas \xC3\xA0 droite", "En bas \xC3\xA0 gauche"};
    for (int v = 0; v < 4; v++) {
        const char* t = fr(fps_stats::corner_name(v));
        CHECK(t && !std::strcmp(t, corners[v]));
    }
    std::puts("PASS: the FPS counter's levels and corners in French");
}
'''


def need(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def c_strings(text):
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"\s*\n\s*"', "", text)
    return re.findall(r'"((?:[^"\\\n]|\\.)*)"', text)


def check_sources():
    launcher = (ROOT / "gpu/vk/vk_launcher.cpp").read_text(encoding="utf-8")
    logic = (ROOT / "gpu/vk/launcher_logic.h").read_text(encoding="utf-8")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    i18n = (ROOT / "gpu/vk/menu_i18n.h").read_text(encoding="utf-8")
    # ---- the launch screen ----
    title = block(launcher, "float draw_title(ui::Ctx& c, const Layout& l, const char* page) {")
    need('"GRAPNEL"' in title and "HOOKSHOT" not in title.upper().replace("GRAPNEL", ""), "the launch screen's title is not GRAPNEL")
    texts = between(logic, "inline constexpr TextEntry kTexts[] = {", "\n};")
    for s in c_strings(texts):
        need("hookshot" not in s.lower() or "hookshot_env.txt" in s, "a launch screen text names Hookshot: %r" % s)
    need('{T_MENU_SHORTCUT, "In the game: touchpad + R1 opens the Grapnel menu.", '
         '"En jeu : pavé tactile + R1 ouvre le menu Grapnel."}' in texts, "the home page's shortcut does not name the Grapnel menu")
    need('inline constexpr const char* kConsoleMenuPages[] = {"Motion", "Quit", "Back to the Grapnel screen"};' in logic,
         "launcher_logic.h does not list Back to the Grapnel screen")
    # ---- the in-game menu: its rows and their help ----
    rows = between(menu, "Row rows[] = {", "constexpr int kQuitRow")
    for s in c_strings(rows):
        need("hookshot" not in s.lower(), "a row of the in-game menu names Hookshot: %r" % s[:100])
    need('{TAB_GAMEPLAY, "Back to the Grapnel screen", nullptr,' in rows, "the console's row is not Back to the Grapnel screen")
    need('!strcmp(r.name, "Back to the Grapnel screen")' in block(menu, "bool is_back_row(const Row& r) {"),
         "is_back_row does not find the row by its new name")
    # ---- the French table ----
    fr = between(i18n, "inline constexpr Entry kfr[] = {", "\n};")
    for s in c_strings(fr):
        need("hookshot" not in s.lower(), "a French text names Hookshot: %r" % s[:100])
    need('{"Back to the Grapnel screen", "Revenir à l\'écran Grapnel"},' in fr, "the row's name is not in French")
    need("Ferme le jeu et relance Grapnel sur son écran de lancement" in fr, "the row's help is not in French")
    # ---- the counter on screen: "FPS" measured as drawn, without the menu's lookup ----
    fps = block(menu, "void draw_fps(ui::Ctx& c, bool under_banner) {")
    need('lw = c.f.strong->CalcTextSizeA(small, FLT_MAX, 0.0f, "FPS").x;' in fps, "the counter's FPS is measured through the lookup")
    need('ui::size_of(c.f.strong, small, "FPS")' not in fps, "the counter's FPS is measured through the lookup")
    need(fps.count('"FPS");') == 2 and "AddText(c.f.strong, small," in fps, "the counter no longer draws its FPS with AddText")
    print("ok: the launch screen, the menu's rows and the French table say Grapnel; the counter's FPS measured as drawn")


def check_title():
    spec = importlib.util.spec_from_file_location("toolchain_ps5", ROOT / "nx2/toolchain_ps5.py")
    tool = importlib.util.module_from_spec(spec)
    old = os.environ.get("HOOKSHOT_PS5_FLEXIBLE_MB")
    os.environ["HOOKSHOT_PS5_FLEXIBLE_MB"] = "580"
    try:
        spec.loader.exec_module(tool)
        param = json.loads(tool.param_json())
    finally:
        if old is None:
            os.environ.pop("HOOKSHOT_PS5_FLEXIBLE_MB", None)
        else:
            os.environ["HOOKSHOT_PS5_FLEXIBLE_MB"] = old
    need(param["titleId"] == "GRPN00001" and param["localizedParameters"]["en-US"]["titleName"] == "Grapnel",
         "the title is not GRPN00001 Grapnel")
    need(re.fullmatch(r"[A-Z]{4}\d{5}", param["titleId"]), "the title id is not four letters and five digits")
    need(param["contentId"] == "UP9000-GRPN00001_00-GRAPNELPS5000000" and len(param["contentId"].split("-")[-1]) == 16,
         "the content id is not the title's")
    need(param["kernel"]["flexibleMemorySize"] == 580 << 20, "the flexible memory is not the one asked for")
    print("ok: param.json: GRPN00001 \"Grapnel\", %s" % param["contentId"])


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    check_sources()
    check_title()
    with tempfile.TemporaryDirectory(prefix="ps5-grapnel-texts-") as tmp:
        t = Path(tmp)
        (t / "names.cpp").write_text(NAMES_TEST, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-I", str(ROOT / "gpu/vk"), str(t / "names.cpp"), "-o",
                        str(t / "names")], check=True)
        r = subprocess.run([str(t / "names")], capture_output=True, text=True)
        need(r.returncode == 0, "the FPS counter's names in French: %s%s" % (r.stdout, r.stderr))
        print(r.stdout.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
