"""(ps5, lot 69) The launch screen's logic (gpu/vk/launcher_logic.h), compiled alone: its texts in both languages, the
settings it offers and their values on the console, whether it shows (lot 73: also when the in-game menu sent the player
back to it), the in-game menu's rows on the console and the console's values of the others (lot 74), its pages and buttons,
no countdown, the shader
check's verdict, and the buttons that confirm and go back with each layout (vk_menu_keys.h). Then, by reading the sources:
each setting is a row of the in-game menu (its key in hookshot_options.ini), the menu's language places and the game's
language order are the ones the screen assumes. No game, GPU or console.

    python -I -B tests/test_launcher_logic.py [--cxx g++]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import between   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''#include "launcher_logic.h"
#include "vk_menu_keys.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::launcher;
namespace mk = mkwvk::menu_keys;
constexpr int64_t S = kSecond;
Input press(const char* which) {
    Input in;
    const std::string w = which;
    in.up = w == "up", in.down = w == "down", in.left = w == "left", in.right = w == "right", in.confirm = w == "confirm";
    in.back = w == "back", in.start = w == "start", in.l1 = w == "l1", in.r1 = w == "r1", in.triangle = w == "triangle";
    return in;
}
/// The printf conversions of a text, in order (not ' ': "100 % :" is a sign, as tests/test_menu_i18n.py reads them)
std::string conversions(const char* t) {
    std::string out;
    for (const char* p = strchr(t, '%'); p; p = strchr(p + 1, '%')) {
        const char* q = p + 1;
        while (*q && strchr("-+#0123456789.lhz", *q)) q++;
        if (*q == '%') { p = q; continue; }
        if (*q && strchr("sducxgf", *q)) out += std::string(p, q + 1) + " ";
    }
    return out;
}
bool utf8(const char* s) {
    for (const unsigned char* p = (const unsigned char*)s; *p;) {
        int n = *p < 0x80 ? 0 : (*p >> 5) == 6 ? 1 : (*p >> 4) == 14 ? 2 : (*p >> 3) == 30 ? 3 : -1;
        if (n < 0) return false;
        for (p++; n > 0; n--, p++)
            if ((*p & 0xC0) != 0x80) return false;
    }
    return true;
}
int main() {
    // ---- the texts: each id in its place, both languages there, the same conversions, valid UTF-8, no button symbol ----
    for (int i = 0; i < T_COUNT; i++) {
        CHECK(kTexts[i].id == i);
        CHECK(kTexts[i].en && *kTexts[i].en && kTexts[i].fr && *kTexts[i].fr);
        CHECK(conversions(kTexts[i].en) == conversions(kTexts[i].fr));
        CHECK(utf8(kTexts[i].en) && utf8(kTexts[i].fr));
        for (const char* sym : {"\xE2\x9C\x95", "\xE2\x97\x8B", "\xE2\x96\xB3", "\xE2\x96\xA1"})   // (cross, circle, triangle, square)
            CHECK(!strstr(kTexts[i].en, sym) && !strstr(kTexts[i].fr, sym));
    }
    CHECK(!strcmp(text(T_PLAY, FR), "JOUER") && !strcmp(text(T_CONTINUE, FR), "CONTINUER") && !strcmp(text(T_SETTINGS, FR), "RÉGLAGES"));
    CHECK(!strcmp(text(T_QUIT, FR), "QUITTER") && !strcmp(text(T_PLAY, EN), "PLAY"));
    // (the frame rate's help says the TV's 60 Hz and V-Sync: nothing above 60 shown)
    CHECK(strstr(text(T_H_FPS_CAP, FR), "60 Hz") && strstr(text(T_H_FPS_CAP, EN), "60 Hz"));
    for (int k = 0; k < kSettingCount; k++)
        for (int v = -2; v < 20; v++) {
            char b[96];
            CHECK(utf8(value_text(kSettings[k], v, FR, b, sizeof b)) && utf8(value_text(kSettings[k], v, EN, b, sizeof b)));
        }
    // ---- the settings: what each offers, the defaults the player chose, nothing that touches the picture ----
    std::set<std::string> keys;
    for (int k = 0; k < kSettingCount; k++) {
        const Setting& s = kSettings[k];
        CHECK(keys.insert(s.key).second);
        CHECK(s.n_values > 0 || (s.step > 0 && s.lo < s.hi));
        if (s.fallback != kNoDefault) CHECK(offered(s, s.fallback));
        for (const char* never : kNeverOffered) CHECK(strcmp(s.key, never));
    }
    // ---- (lot 74) the in-game menu on the console: the screen's settings, the rows of its own, the console's values of the rest ----
    CHECK(!strcmp(kConsoleMenuSwitch, "HOOKSHOT_PS5_MENU_CONSOLE"));
    for (int k = 0; k < kSettingCount; k++) CHECK(console_menu_key(kSettings[k].key));   // (every setting of the screen's)
    for (const char* own : kConsoleMenuOnly) {
        CHECK(console_menu_key(own) && setting_of(own) < 0 && !console_pinned(own));
        for (const char* never : kNeverOffered) CHECK(strcmp(own, never));   // (nothing of the picture, the memory, the driver)
    }
    std::set<std::string> pinned;
    for (const Pinned& p : kConsolePinned) {
        CHECK(pinned.insert(p.key).second && !console_menu_key(p.key) && console_pinned(p.key) == &p);   // (hidden, once)
    }
    for (const char* k : {"handheld_profile", "quality_preset", "dlss", "resolution", "sharpness", "upscaling", "skip_title_screen",
                          "debug_tools", "anisotropic_filtering", "shadow_resolution", "field_of_view", "crt_filter"})
        CHECK(console_pinned(k));
    CHECK(console_pinned("handheld_profile")->value == 1 && console_pinned("quality_preset")->value == 1 &&
          console_pinned("sharpness")->value == 0 && console_pinned("anisotropic_filtering")->value == 4 && kConsoleBudget == 1);
    CHECK(!console_pinned("frame_rate_cap") && !console_pinned("output_res") && !console_pinned("ultrawide"));
    CHECK(console_menu_page("Motion") && console_menu_page("Quit") && console_menu_page("Back to the Grapnel screen"));
    CHECK(!console_menu_page("Restart") && !console_menu_page("Controls") && !console_menu_page("Bug report") &&
          !console_menu_page("Set up for a weak laptop") && !console_menu_page("FPS counter details"));
    CHECK(!console_menu_key("handheld_profile") && !console_menu_key("dlss") && !console_menu_key("button_prompts") &&
          !console_menu_key("discord_status"));
    const Setting& cap = kSettings[setting_of("frame_rate_cap")];
    CHECK(cap.n_values == 2 && cap.values[0] == -1 && cap.values[1] == 0 && cap.fallback == 0);   // (30, or the console's 60: default)
    char b[96];
    CHECK(!strcmp(value_text(cap, -1, FR, b, sizeof b), "30 images/s") && !strncmp(value_text(cap, 0, FR, b, sizeof b), "Débloquée", 10));
    const Setting& fg = kSettings[setting_of("frame_generation")];
    CHECK(fg.n_values == 2 && fg.values[0] == 0 && fg.values[1] == 2 && fg.fallback == 2);   // (off or x2; x2 as the file has it)
    const Setting& fps = kSettings[setting_of("fps_counter")];
    CHECK(!offered(fps, 5) && offered(fps, 1));   // (no Dev level)
    for (const char* k : {"text_speed", "text_size", "skip_opening", "game_language", "menu_language", "menu_size", "rumble_strength",
                          "motion_controls", "motion_sensitivity_x", "motion_sensitivity_y", "invert_motion_x", "invert_motion_y",
                          "invert_camera_x", "invert_camera_y", "camera_speed", "stick_dead_zone", "layout_preset", "volume", "launcher"})
        CHECK(setting_of(k) >= 0);
    const Setting& layout = kSettings[setting_of("layout_preset")];
    CHECK(layout.fallback == 0 && !strcmp(value_text(layout, 0, FR, b, sizeof b), "Nintendo") &&
          !strcmp(value_text(layout, 1, FR, b, sizeof b), "PlayStation"));
    int tabs[TAB_N] = {};
    for (const Setting& s : kSettings) tabs[s.tab]++;
    for (int t = 0; t < TAB_N; t++) CHECK(tabs[t] > 0 && tab_count(t) == tabs[t]);
    // (the arrows: a list goes round, a range stops at its ends and snaps to its steps)
    CHECK(next_value(cap, 0, 1) == -1 && next_value(cap, -1, 1) == 0 && next_value(cap, 3, 1) == -1);
    const Setting& vol = kSettings[setting_of("volume")];
    CHECK(next_value(vol, 100, 1) == 100 && next_value(vol, 0, -1) == 0 && next_value(vol, 50, 1) == 55 && next_value(vol, 52, 1) == 55 &&
          next_value(vol, 52, -1) == 50);
    // (a switch of hookshot_env.txt holds its row for the run)
    std::map<std::string, std::string> env;
    const auto get = [&](const char* n) -> const char* { return env.count(n) ? env[n].c_str() : nullptr; };
    CHECK(!held_by(cap, get));
    env["HOS_FPS_MAX"] = "30";
    CHECK(held_by(cap, get) && !strcmp(held_by(cap, get), "HOS_FPS_MAX"));
    env["RX_VK_FRAMEGEN"] = "1";
    CHECK(!held_by(fg, get));
    env["RX_VK_FRAMEGEN"] = "0";
    CHECK(held_by(fg, get));
    // ---- the screen's language: French for the menu's French, or its Auto with the game in French ----
    CHECK(lang_for(kMenuLangAuto, "fr") == FR && lang_for(kMenuLangAuto, "fr-CA") == FR && lang_for(kMenuLangAuto, "en-US") == EN);
    CHECK(lang_for(kMenuLangFrench, "en-US") == FR && lang_for(kMenuLangEnglish, "fr") == EN && lang_for(9, "fr") == EN);
    // ---- whether it shows ----
    const char* why = nullptr;
    CHECK(!show("0", true, 0, V_OK, true, &why));            // (HOOKSHOT_PS5_LAUNCHER=0: never, the measurement runs)
    CHECK(!show("0", true, 1, V_FAIL, true, &why));
    CHECK(show("1", true, 1, V_OK, false, &why));            // (=1: always)
    CHECK(show(nullptr, true, 0, V_OK, false, &why));        // (the mode Always: every launch)
    CHECK(!show(nullptr, true, 1, V_OK, false, &why));       // (only when needed, nothing to say)
    CHECK(!show(nullptr, true, 1, V_WARN, false, &why));
    CHECK(show(nullptr, true, 1, V_FAIL, false, &why));      // (a problem)
    CHECK(show(nullptr, true, 1, V_OK, true, &why));         // (R1 held)
    CHECK(!show(nullptr, false, 0, V_OK, true, &why));       // (off the console: only when asked)
    CHECK(show("1", false, 0, V_OK, false, &why));
    // (lot 73: back from the game, the in-game menu's file found: shown whatever the mode and the check; the switch that turns
    // the screen off, and the PC, before it; without the file, as before)
    CHECK(show(nullptr, true, 1, V_OK, false, true, &why) && strstr(why, "Back to the Grapnel screen"));
    CHECK(show(nullptr, true, 0, V_OK, false, true, &why) && strstr(why, "Back to the Grapnel screen"));
    CHECK(show(nullptr, true, 1, V_WARN, false, true, &why));
    CHECK(!show("0", true, 1, V_OK, false, true, &why));
    CHECK(!show(nullptr, false, 1, V_OK, false, true, &why));
    CHECK(!show(nullptr, true, 1, V_OK, false, false, &why) && !strstr(why, "Back"));
    for (int mode = 0; mode < 2; mode++)   // (no file: the same answer and reason as the six-argument call)
        for (Verdict v : {V_OK, V_WARN, V_FAIL})
            for (bool r1 : {false, true})
                for (const char* env : {(const char*)nullptr, "0", "1"}) {
                    const char *a = nullptr, *b = nullptr;
                    CHECK(show(env, true, mode, v, r1, false, &a) == show(env, true, mode, v, r1, &b) && !strcmp(a, b));
                }
    // ---- no countdown (lot 71): nothing starts the game but the player, whatever the check said ----
    State st;
    begin(st, 100 * S, V_OK);
    CHECK(!st.counting && countdown_left(st, 100 * S) == -1 && countdown_left(st, 104 * S + S / 10) == -1);
    CHECK(step(st, Input{}, 104 * S).kind == A_NONE && st.outcome == NONE);
    step(st, Input{}, 105 * S);
    CHECK(st.outcome == NONE);   // (5 s gone: nothing starts by itself)
    step(st, Input{}, 3600 * S);
    CHECK(st.outcome == NONE);   // (an hour gone: still the player's choice)
    begin(st, 0, V_WARN);
    CHECK(!st.counting);
    step(st, press("down"), S);
    CHECK(!st.counting && st.home == H_SETTINGS && countdown_left(st, 99 * S) == -1);
    step(st, Input{}, 60 * S);
    CHECK(st.outcome == NONE);
    begin(st, 0, V_FAIL);
    CHECK(!st.counting && st.home == H_SHADERS);
    step(st, Input{}, 60 * S);
    CHECK(st.outcome == NONE);
    // ---- the home page ----
    begin(st, 0, V_OK);
    step(st, press("confirm"), 1);
    CHECK(st.outcome == PLAY);
    begin(st, 0, V_OK);
    step(st, press("down"), 1), step(st, press("confirm"), 2);
    CHECK(st.outcome == NONE && st.page == SETTINGS);   // (no CONTINUE since lot 71: under PLAY, SETTINGS)
    CHECK(H_N == 4);
    for (int i = 0; i < H_N; i++) CHECK(kHomeNames[i] != T_CONTINUE);
    begin(st, 0, V_OK);
    step(st, press("up"), 1);
    CHECK(st.home == H_QUIT);
    step(st, press("confirm"), 2);
    CHECK(st.outcome == NONE && st.quit_armed);   // (QUIT asks twice)
    step(st, press("back"), 3);
    CHECK(!st.quit_armed);
    step(st, press("confirm"), 4), step(st, press("confirm"), 5);
    CHECK(st.outcome == QUIT);
    begin(st, 0, V_OK);
    step(st, press("triangle"), 1);
    CHECK(st.page == SETTINGS);
    step(st, press("back"), 2);
    CHECK(st.page == HOME);
    begin(st, 0, V_OK);
    step(st, press("down"), 1), step(st, press("down"), 2), step(st, press("confirm"), 4);
    CHECK(st.page == SHADERS);
    CHECK(step(st, press("confirm"), 5).kind == A_DRIVER_CHECK);   // (the driver asked only when the player asks)
    step(st, press("start"), 6);
    CHECK(st.outcome == PLAY);   // (Options: play now, from any page)
    // ---- the settings page ----
    begin(st, 0, V_OK);
    step(st, press("triangle"), 1);
    CHECK(st.tab == TAB_PICTURE && selected_setting(st) == setting_of("frame_rate_cap"));
    Action a = step(st, press("right"), 2);
    CHECK(a.kind == A_STEP && a.setting == setting_of("frame_rate_cap") && a.dir == 1);
    a = step(st, press("left"), 3);
    CHECK(a.kind == A_STEP && a.dir == -1);
    a = step(st, press("triangle"), 4);
    CHECK(a.kind == A_DEFAULT && a.setting == setting_of("frame_rate_cap"));
    a = step(st, press("right"), 5, true);
    CHECK(a.kind == A_NONE);   // (held by hookshot_env.txt: left alone)
    step(st, press("l1"), 6);
    CHECK(st.tab == TAB_LAUNCH);
    step(st, press("r1"), 7), step(st, press("r1"), 8);
    CHECK(st.tab == TAB_GAME);
    step(st, press("up"), 9);
    CHECK(st.row[TAB_GAME] == tab_count(TAB_GAME) - 1);
    // ---- the shader check's verdict ----
    const uint64_t stamp = 0xc1f75256405efa1eull;
    ShaderFacts f;
    f.programs_present = f.programs_magic = f.index_present = f.index_ok = true;
    f.programs_stamp = stamp, f.programs = 4566, f.notes_present = f.notes_ok = true, f.notes = 12340, f.notes_covered = 12340;
    f.driver_known = true, f.driver_bytes = 1009ull << 20;
    Check c = judge(f, stamp);
    CHECK(c.verdict == V_OK && c.coverage == 100 && why_text(c.why) == T_SH_WHY_OK);
    ShaderFacts g = f;
    g.programs_stamp = 1;
    c = judge(g, stamp);
    CHECK(c.verdict == V_FAIL && c.why == WHY_VERSION && c.version == V_FAIL);
    g = f, g.programs_present = false, g.index_present = false, g.programs = 0;
    c = judge(g, stamp);
    CHECK(c.verdict == V_FAIL && c.why == WHY_PROGRAMS);
    g = f, g.driver_bytes = 0;
    c = judge(g, stamp);
    CHECK(c.verdict == V_FAIL && c.why == WHY_DRIVER && c.driver == V_FAIL);
    g = f, g.notes_covered = 6000;
    c = judge(g, stamp);
    CHECK(c.verdict == V_WARN && c.why == WHY_COVERAGE && c.coverage == 48);
    g = f, g.notes_present = false;
    c = judge(g, stamp);
    CHECK(c.verdict == V_WARN && c.why == WHY_NOTES);
    // ---- the buttons that confirm and go back: those the game's A and B are on (the Button layout) ----
    int confirm = -1, back = -1;
    mk::confirm_back(mk::kEast, mk::kSouth, &confirm, &back);   // (Nintendo: A on the right face button, circle)
    CHECK(confirm == mk::kEast && back == mk::kSouth);
    mk::confirm_back(mk::kSouth, mk::kEast, &confirm, &back);   // (PlayStation: A at the bottom, cross)
    CHECK(confirm == mk::kSouth && back == mk::kEast);
    std::puts("PASS: the launch screen's texts, settings, showing, pages, countdown, verdicts and buttons");
}
'''


def ini_key_of(name):
    """vk_menu.cpp ini_key's rule for a row with no key of its own: lower case, spaces as underscores"""
    return "".join("_" if c == " " else c.lower() for c in name)


def check_sources():
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    logic = (ROOT / "gpu/vk/launcher_logic.h").read_text(encoding="utf-8")
    rows = between(menu, "Row rows[] = {", "constexpr int kQuitRow")
    keys = {ini_key_of(n) for n in re.findall(r'\{TAB_\w+, "([^"]+)", &', rows)}
    special = between(menu, "const char* ini_key(const Row& r) {", "static char k[48];")
    keys |= set(re.findall(r'return "(\w+)";', special))
    offered = re.findall(r'\{TAB_\w+, "(\w+)", T_S_', logic) + (["launcher"] if "kLaunchModeKey, T_S_LAUNCH_MODE" in logic else [])
    missing = [k for k in offered if k != "launcher" and k not in keys]
    if missing:
        raise SystemExit("launcher_logic.h offers settings that are no row of the in-game menu: %s" % ", ".join(missing))
    # (lot 74: the in-game menu's rows on the console are rows of the menu; the console's values are rows' and within their ends;
    # the host's guard reads the same switch)
    row_keys = {}
    for name, value, lo, hi in re.findall(r'\{TAB_\w+, "([^"]+)", &([\w.\[\]]+), (-?\w+), ([\w:() -]+?), ', rows):
        key = ini_key_of(name)
        for k in re.findall(r'return "(\w+)";', special):
            if re.search(r'&%s\) return "%s"' % (re.escape(value), k), special):
                key = k
        row_keys[key] = (lo, hi)
    pages = set(re.findall(r'\{TAB_\w+, "([^"]+)", nullptr, 0, 0, 0, nullptr,', rows))   # (and the titles: no page is one)
    own = re.findall(r'"([^"]+)"', between(logic, "kConsoleMenuOnly[] = {", "};"))
    console_pages = re.findall(r'"([^"]+)"', between(logic, "kConsoleMenuPages[] = {", "};"))
    pins = re.findall(r'\{"(\w+)", (-?\d+)\}', between(logic, "inline constexpr Pinned kConsolePinned[] = {", "};"))
    if [k for k in own if k not in keys] or [n for n in console_pages if n not in pages]:
        raise SystemExit("launcher_logic.h lists for the console's menu rows the menu does not have: %s"
                         % ([k for k in own if k not in keys] + [n for n in console_pages if n not in pages]))
    if not pins:
        raise SystemExit("launcher_logic.h kConsolePinned read as empty")
    for key, value in pins:
        if key not in row_keys:
            raise SystemExit("kConsolePinned: %s is no row of the in-game menu" % key)
        lo, hi = row_keys[key]
        if lo.lstrip("-").isdigit() and hi.lstrip("-").isdigit() and not int(lo) <= int(value) <= int(hi):
            raise SystemExit("kConsolePinned: %s=%s is outside its row's %s..%s" % (key, value, lo, hi))
    applet = (ROOT / "host/nn_applet.c").read_text(encoding="utf-8")
    if 'env_on("HOOKSHOT_PS5_MENU_CONSOLE")' not in applet or 'kConsoleMenuSwitch = "HOOKSHOT_PS5_MENU_CONSOLE"' not in logic:
        raise SystemExit("host/nn_applet.c's guard and launcher_logic.h name the console menu's switch differently")
    if '"launcher"' not in menu or 'mine |= key == "launcher"' not in menu:
        raise SystemExit("vk_menu.cpp no longer reads and writes the launch screen's launcher= line")
    # (the menu's languages: en first, fr fifth, as kMenuLangEnglish and kMenuLangFrench say)
    langs = re.findall(r'^    \{"([\w-]+)", "[^"]*", \w+, \d+\},$', (ROOT / "gpu/vk/menu_i18n.h").read_text(encoding="utf-8"), re.M)
    en = int(re.search(r"kMenuLangEnglish = (\d+)", logic).group(1))
    fr = int(re.search(r"kMenuLangFrench = (\d+)", logic).group(1))
    if langs[en - 1] != "en" or langs[fr - 1] != "fr":
        raise SystemExit("menu_i18n.h kLangs moved: en is %d, fr %d (launcher_logic.h says %d and %d)" % (langs.index("en") + 1,
                                                                                                       langs.index("fr") + 1, en, fr))
    # (the game's languages, in vk_menu_i18n.cpp's order)
    game = re.findall(r'\{"([\w-]+)", "[^"]*"\}', between((ROOT / "gpu/vk/vk_menu_i18n.cpp").read_text(encoding="utf-8"),
                                                          "constexpr GameLang kGameLangs[] = {", "};"))
    mine = re.findall(r'"([\w-]+)"', between(logic, "kGameLangCodes[16] = {", "};"))
    if game != mine:
        raise SystemExit("the game's languages are %s in vk_menu_i18n.cpp, %s in launcher_logic.h" % (game, mine))
    return len(offered)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    n = check_sources()
    with tempfile.TemporaryDirectory(prefix="launcher-logic-") as tmp:
        cpp = Path(tmp) / "logic.cpp"
        exe = Path(tmp) / "logic"
        cpp.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), str(cpp), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)
    print("the %d settings offered are rows of the in-game menu; the languages' places hold" % n)


if __name__ == "__main__":
    main()
