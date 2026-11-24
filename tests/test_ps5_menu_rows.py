"""(ps5, lot 74) The in-game menu on the console, HOOKSHOT_PS5_MENU_CONSOLE=1: the rows it shows are the launch screen's settings
and the few launcher_logic.h lists beside them (an allow list: nothing else, a row a merge adds included), their arrows step
through the launch screen's values, Layout preset is on the Controls tab, the Debug tab's tools come back with HOS_OOT_CHEATS=1
only, a section's title shows only over a row it heads. Without the switch the console's menu is the PC's, and the PC's is as it
was. No game, GPU or console:

- (lot 77b) as on the console: the switches put in the environment by main (host/hos_main.c stage 4 applies hookshot_env.txt
  there), after the static initializers, one of which (kTop, the Controls page's rows) already asks the rows; then menu_init's
  reading of the switch. In 77-1 the switch was read once by that initializer, before stage 4, and frozen at off: the PC's menu
  on the console ("C'est l'ancien menu. Je vois FSR 3.1"). The check before set the switch before the process started;

- vk_menu.cpp's real rows[], ini_key, row_shown, the console's functions, tab_row_count, tab_row_to_global and tx_pad_layout,
  compiled three ways (the PC; the console without the switch; the console with it), each run with the switches of a case: the
  rows each tab shows, and the arrows' values;
- by reading the sources: everything the lot adds to vk_menu.cpp and host/nn_applet.c is under __PROSPERO__ and behind the
  switch (restart_ask, the arrows, Quit, menu_init's console values), Quit's second press saves before the title closes, the
  host's handheld profile guard.

    python -I -B tests/test_ps5_menu_rows.py [--cxx g++]
"""
import argparse
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

FIXTURE = r'''#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include "longshot_env.h"
#include "upscaler_rows.h"
#include "fps_stats.h"
#define _Static_assert static_assert   /* (vk_api.h's NVN types are C's) */
#include "vk_api.h"
#include "launcher_logic.h"
#include "launcher_back.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace mkwvk {
void logf(const char*, ...) {}
/* OPTIONS */
Options opts;
/* TABS */
/* ROW */
namespace ur = upscaler_rows;
namespace i18n { int game_lang_count() { return 16; } int lang_count() { return 14; } }
const char* tx(int) { return ""; }
const char* hn(int, int) { return nullptr; }
constexpr int kMaxMics = 64;
int output_pick = 0;
/* ROWS */
constexpr int kRows = sizeof rows / sizeof rows[0];
/* INI_KEY */
bool cheats_env = false;
/* CHEATS_ON */
// (the console's GPU as the menu sees it: no Reflex, no Anti-Lag 2, no Intel compiler row, an integrated GPU (Set up for a weak
// laptop shows, as on the console), the game's own upscaler, FSR 3.1 and Off (the Upscaler row shows), no DLSS)
bool reflex_available() { return false; }
bool antilag_available() { return false; }
bool intel_compiler_row() { return false; }
bool laptop_row() { return true; }
unsigned upscalers_available() { return ur::bit(ur::GAME) | ur::bit(ur::OFF) | ur::bit(ur::FSR3); }
constexpr int DLSS_OK = 0;
int dlss_why() { return 1; }
/* LAPTOP */
/* BACK */
/* CONSOLE */
/* CONSOLE_ROWS */
/* ROW_SHOWN */
/* CONSOLE_SHOWN */
/* TAB_COUNT */
/* TAB_GLOBAL */
/* KTOP */
const char* const bind_names_unused = nullptr;
/* TX_PAD_LAYOUT */
const char* const tab_names[] = {"Graphics", "Audio", "Gameplay", "Controls", "Debug", "Motion", "FPS details", "Controls page"};
Row* row_of(const char* key) {
    for (Row& r : rows)
        if (r.value && !std::strcmp(ini_key(r), key)) return &r;
    std::fprintf(stderr, "FAIL: no row of key %s\n", key);
    std::exit(1);
}
#ifdef __PROSPERO__
/// The arrows on the row of key from cur, by dir: the value set_row is given
int arrow(const char* key, int cur, int dir) {
    Row& r = *row_of(key);
    *r.value = cur;
    return console_next(r, cur + dir * r.step, dir);
}
#endif

#ifdef __PROSPERO__
/// menu_init's own reading of the switch: the lines its console's part runs before it first asks console_menu()
void menu_init_switch() {
    int pinned = 0;
/* MENU_INIT_SWITCH */
    (void)pinned;
}
#endif
}  // namespace mkwvk

int main() {
    using namespace mkwvk;
    // (host/hos_main.c, stage 4: hookshot_env.txt into the environment, in main, after the static initializers: TEST_STAGE4's
    // "NAME=value" pairs, comma separated. Then menu_init, the renderer's first, reads the menu's switches)
    if (const char* e = std::getenv("TEST_STAGE4")) {
        std::string all = e;
        for (size_t at = 0; at < all.size();) {
            const size_t end = all.find(',', at) == std::string::npos ? all.size() : all.find(',', at);
            const std::string pair = all.substr(at, end - at);
            if (pair.find('=') != std::string::npos) setenv(pair.substr(0, pair.find('=')).c_str(), pair.substr(pair.find('=') + 1).c_str(), 1);
            at = end + 1;
        }
    }
#ifdef __PROSPERO__
    menu_init_switch();
#endif
    if (const char* c = std::getenv("TEST_CHEATS")) cheats_env = std::atoi(c) != 0;
    // (the rows each tab shows, in order: one line each, "tab|name")
    for (int t = 0; t <= TAB_PAGE; t++)
        for (int k = 0, n = tab_row_count(t); k < n; k++) {
            const int i = tab_row_to_global(t, k);
            std::printf("%s|%s\n", tab_names[t], rows[i].name);
        }
    std::printf("layout 1|%s\n", tx_pad_layout(1));
#ifdef __PROSPERO__
    if (console_menu()) {
        // (the launch screen's values: launcher_logic.h kSettings, next_value)
        CHECK(arrow("frame_generation", 2, 1) == 0 && arrow("frame_generation", 0, 1) == 2 && arrow("frame_generation", 2, -1) == 0);
        CHECK(arrow("frame_rate_cap", 0, 1) == -1 && arrow("frame_rate_cap", -1, 1) == 0 && arrow("frame_rate_cap", 0, -1) == -1);
        CHECK(arrow("fps_counter", 4, 1) == 0 && arrow("fps_counter", 0, -1) == 4 && arrow("fps_counter", 5, 1) == 0);
        CHECK(arrow("menu_language", 0, 1) == 5 && arrow("menu_language", 5, 1) == 1 && arrow("menu_language", 1, 1) == 0);
        CHECK(arrow("menu_language", 3, 1) == 0);   // (another language from before: to the list's first)
        CHECK(arrow("layout_preset", 0, 1) == 1 && arrow("layout_preset", 1, 1) == 0 && arrow("layout_preset", 3, 1) == 0);
        CHECK(arrow("volume", 100, 1) == 100 && arrow("volume", 100, -1) == 95 && arrow("rumble_strength", 0, -1) == 0);
        CHECK(arrow("text_speed", 3, 1) == 0 && arrow("menu_size", 3, 1) == 1 && arrow("stick_dead_zone", 5, 1) == 0);
        // (a row of the menu's own: the menu's next, as before)
        CHECK(arrow("autosave:", 1, 1) == 2 && arrow("motion_deadzone", 1, -1) == 0);
    } else {
        CHECK(arrow("frame_generation", 2, 1) == 3 && arrow("frame_rate_cap", 0, 1) == 1 && arrow("fps_counter", 4, 1) == 5);
        CHECK(arrow("layout_preset", 1, 1) == 2 && arrow("menu_language", 0, 1) == 1);
    }
#endif
    return 0;
}
'''

# (what the console shows with the switch: the launch screen's settings that are rows, the rows launcher_logic.h lists beside
# them, Quit, Back to the Grapnel screen with its own switch; Layout preset on the Controls tab)
CONSOLE_ROWS = """Graphics|Frame generation and latency
Graphics|Frame generation
Graphics|Game frame rate cap
Audio|Volume
Gameplay|FPS counter
Gameplay|FPS counter position
Gameplay|FPS counter size
Gameplay|Menu size
Gameplay|Text speed
Gameplay|Text size
Gameplay|Game language
Gameplay|Menu language
Gameplay|Skip opening
Gameplay|Back to the Grapnel screen
Gameplay|Quit
Controls|Disable camera centering
Controls|Invert camera X
Controls|Invert camera Y
Controls|Camera speed
Controls|Stick dead zone
Controls|Rumble strength
Controls|Motion
Controls|Layout preset
Debug|Autosave:
Motion|Motion controls
Motion|Motion sensitivity X
Motion|Motion sensitivity Y
Motion|Invert motion X
Motion|Invert motion Y
Motion|Motion deadzone
layout 1|PlayStation"""

# (lot 86: with HOOKSHOT_PS5_MIC=1 the microphone's rows under Volume as well: player 1's DualSense microphone is on)
MIC_ROWS = "Audio|Volume\nAudio|Microphone\nAudio|Microphone gain\nAudio|Test microphone"

# (with HOS_OOT_CHEATS=1, our trials: the Debug tab's switch, its tools and Damage taken, as on the PC with the tools on)
DEBUG_TOOLS = ["Debug tools", "Heart protection", "Autosave:", "Damage taken", "No-clip flight", "Mark position", "Return to mark",
               "Set time", "Inventory item", "Change quantity", "Free camera", "Teleport", "Switch age", "Inventory",
               "Movement speed"]


def need(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def fixture():
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    header = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    rows = between(menu, "Row rows[] = {", "constexpr int kQuitRow")
    rows = re.sub(r"\btx_\w+\b", "tx", rows)
    rows = re.sub(r"\bhn_\w+\b", "hn", rows)
    rows = rows.replace("kFpsDevLineHelp", '""').replace("kMicLevelHelp", '""')
    pieces = {
        "/* OPTIONS */": re.search(r"struct Options \{.*?\n\};", header, re.S).group(0),
        "/* TABS */": re.search(r"enum MenuTab \{.*?\};", menu, re.S).group(0),
        "/* ROW */": re.search(r"struct Row \{.*?\n\};", menu, re.S).group(0),
        "/* ROWS */": rows,
        "/* INI_KEY */": block(menu, "const char* ini_key(const Row& r) {"),
        "/* CHEATS_ON */": block(menu, "bool cheats_on() {"),
        "/* LAPTOP */": block(menu, "bool is_laptop_row(const Row& r) {"),
        "/* BACK */": block(menu, "bool is_back_row(const Row& r) {") + "\n" + block(menu, "bool back_row() {"),
        "/* CONSOLE */": "#ifdef __PROSPERO__\n" + between(menu, "// ---- (ps5, lot 74) The in-game menu on the console",
                                                           "bool row_shown(int i) {"),
        "/* CONSOLE_ROWS */": between(menu, "#ifdef __PROSPERO__\n/// (lot 74) Each row's place in the console's menu, worked out once",
                                      "// (longshot) the controls, kept by the host"),
        "/* ROW_SHOWN */": block(menu, "bool row_shown(int i) {"),
        "/* CONSOLE_SHOWN */": between(menu, "#ifdef __PROSPERO__\n/// (lot 74) Whether the console's menu shows row i",
                                       "int tab_row_count(int t) {"),
        "/* TAB_COUNT */": block(menu, "int tab_row_count(int t) {"),
        "/* TAB_GLOBAL */": block(menu, "int tab_row_to_global(int t, int local_idx) {"),
        "/* TX_PAD_LAYOUT */": block(menu, "const char* tx_pad_layout(int v) {"),
        # (the static initializer that asks the rows before main: the Controls page's count)
        "/* KTOP */": re.search(r"^const int kTop = tab_row_count\(TAB_PAGE\);[^\n]*$", menu, re.M).group(0),
        "/* MENU_INIT_SWITCH */": between(block(menu, "void menu_init() {"), "#ifdef __PROSPERO__\n    int pinned = 0;",
                                          "    if (console_menu()) {")[len("#ifdef __PROSPERO__\n    int pinned = 0;"):],
    }
    out = FIXTURE
    for marker, text in pieces.items():
        out = out.replace(marker, text)
    return out


def check_sources():
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    applet = (ROOT / "host/nn_applet.c").read_text(encoding="utf-8")
    logic = (ROOT / "gpu/vk/launcher_logic.h").read_text(encoding="utf-8")
    # ---- everything the lot adds is the console's: each line naming it under #ifdef __PROSPERO__ (the PC compiles none) ----
    names = ("console_menu", "console_rows", "console_shown", "console_next", "console_pin", "row_tab(", "quit_title",
             "hos_ps5_quit", "HOOKSHOT_PS5_MENU_CONSOLE", "kConsoleMenuSwitch")
    for path, text in (("gpu/vk/vk_menu.cpp", menu), ("host/nn_applet.c", applet)):
        stack = []   # (per #if: whether its branch is the console's)
        for n, line in enumerate(text.split("\n"), 1):
            s = line.strip()
            if s.startswith("#ifdef __PROSPERO__"):
                stack.append(True)
            elif s.startswith("#ifndef __PROSPERO__"):
                stack.append(False)
            elif s.startswith("#if"):
                stack.append(None)
            elif s.startswith("#else") and stack:
                stack[-1] = None if stack[-1] is None else not stack[-1]
            elif s.startswith("#endif") and stack:
                stack.pop()
            elif any(k in line for k in names) and not s.startswith("//") and True not in stack:
                raise SystemExit("FAIL: %s:%d names the console's menu outside #ifdef __PROSPERO__: %s" % (path, n, s))
    # ---- behind the switch, read once ----
    need("kConsoleMenuSwitch" in between(block(menu, "void menu_init() {"), "#ifdef __PROSPERO__\n    int pinned = 0;",
                                         "    if (console_menu()) {"), "menu_init does not read the switch before it asks it")
    need(not re.search(r"static[^;\n]*env_on\(launcher::kConsoleMenuSwitch\)", menu),
         "the switch is kept in a static where it is first asked (a static initializer asks it before main: 77-1)")
    need('inline constexpr const char* kConsoleMenuSwitch = "HOOKSHOT_PS5_MENU_CONSOLE";' in logic, "the switch's name moved")
    # (lot 86: the microphone's switch read there too, before console_rows works the rows out, never in a static)
    init_switch = between(block(menu, "void menu_init() {"), "#ifdef __PROSPERO__\n    int pinned = 0;", "    if (console_menu()) {")
    need("console_mic_on.store(env_on(launcher::kConsoleMicSwitch)" in init_switch,
         "menu_init does not read HOOKSHOT_PS5_MIC before the console's rows are worked out")
    need(not re.search(r"static[^;\n]*env_on\(launcher::kConsoleMicSwitch\)", menu), "the microphone's switch is kept in a static")
    need('inline constexpr const char* kConsoleMicSwitch = "HOOKSHOT_PS5_MIC";' in logic, "the microphone switch's name moved")
    need("launcher::console_menu_key(key, console_mic())" in block(menu, "const ConsoleRows& console_rows() {") and
         "launcher::console_menu_page(r.name, console_mic())" in block(menu, "const ConsoleRows& console_rows() {"),
         "console_rows does not show the microphone's rows with the switch")
    need(block(menu, "bool console_shown(int i) {").split("\n")[1].strip() == "if (!console_menu()) return true;",
         "console_shown hides rows without the switch")
    ask = block(menu, "bool restart_ask() {")
    need(re.search(r"#ifdef __PROSPERO__\s*(//[^\n]*\s*)*if \(console_menu\(\)\) return false;\s*#endif", ask),
         "the console's menu still asks to restart (the console cannot)")
    # ---- the arrows: the launch screen's values in the console's menu, the menu's own elsewhere ----
    tick = block(menu, "void menu_tick() {")
    need(re.search(r"#ifdef __PROSPERO__[^\n]*\n\s*set_row\(r, console_next\(r, \*r\.value \+ \(press\[4\] \? r\.step : -r\.step\), "
                   r"press\[4\] \? 1 : -1\), press\[4\] \? 1 : -1\);\s*#else\s*set_row\(r, \*r\.value \+ \(press\[4\] \? r\.step : "
                   r"-r\.step\), press\[4\] \? 1 : -1\);\s*#endif", tick), "the arrows do not step through the launch screen's values")
    # ---- Quit: twice, the options saved and the pipeline cache kept, then the title closes; the PC's WM_CLOSE as before ----
    second = tick[tick.index("const bool confirm_selected"):]
    second = second[:second.index("} else if (r.cheat >= 0)")]
    need(second.index("save();") < second.index("pipeline_cache_save(true);") < second.index("quit_title();"),
         "Quit does not save before the title closes")
    need(re.search(r"#ifdef __PROSPERO__[^\n]*\n\s*open_now = false, changed = true;\s*quit_title\(\);\s*#else\s*"
                   r"if \(const HWND window = app_window\(\)\) PostMessageA\(window, WM_CLOSE, 0, 0\);\s*#endif", second),
         "Quit is not the console's close there and the window's WM_CLOSE on the PC")
    need("hos_ps5_quit()" in block(menu, "void quit_title() {"), "quit_title does not ask the host to close the title")
    # ---- menu_init: the console's values before the run's switches, the file written once when one was put back ----
    init = block(menu, "void menu_init() {")
    need(init.index("pinned = console_pin();") < init.index('if (const char* e = getenv("RX_VK_FRAMEGEN_MULT"))'),
         "the console's values are put back after the run's switches (they would be saved)")
    need(init.index("first_start = false;   // (no Auto (this PC)") > init.index("first_start = !has_options;"),
         "a console without an options file would still start on Auto (this PC)")
    need(re.search(r"if \(pinned\) \{[^}]*save\(\);", init) and init.index("if (pinned) {") > init.index("restart_init();"),
         "the options file is not written once, at the end, when a value was put back")
    # ---- the host: the handheld profile, whatever the file says, with the switch only ----
    hh = block(applet, "static int handheld(void) {")
    need(re.search(r'#ifdef __PROSPERO__.*if \(!v && !getenv\("HOS_HANDHELD"\) && env_on\("HOOKSHOT_PS5_MENU_CONSOLE"\)\) \{.*v = 1;.*#endif',
                   hh, re.S), "host/nn_applet.c handheld() does not take the console's profile with the switch")
    # ---- the lists ----
    need('inline constexpr const char* kConsoleMenuPages[] = {"Motion", "Quit", "Back to the Grapnel screen"};' in logic,
         "the console's pages changed: Restart and the Controls page are not the console's")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    src = fixture()
    clean = {k: v for k, v in os.environ.items()
             if k not in ("HOOKSHOT_PS5_MENU_CONSOLE", "HOOKSHOT_PS5_BACK_TO_LAUNCHER", "HOOKSHOT_PS5_LAUNCHER", "TEST_CHEATS",
                          "TEST_STAGE4", "HOOKSHOT_PS5_MIC")}
    with tempfile.TemporaryDirectory(prefix="ps5-menu-rows-") as tmp:
        t = Path(tmp)
        (t / "rows.cpp").write_text(src, encoding="utf-8")
        flags = ["-std=c++20", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
                 "-I", str(ROOT / "host"), "-I", str(ROOT / "gpu/vk/compat"), "-I", str(ROOT / "gpu/vk"), "-I", str(ROOT / "gpu/render")]
        subprocess.run([a.cxx] + flags + [str(t / "rows.cpp"), "-o", str(t / "pc")], check=True)
        subprocess.run([a.cxx] + flags + ["-D__PROSPERO__=1", str(t / "rows.cpp"), "-o", str(t / "console")], check=True)

        def run(exe, **env):
            e = dict(clean)
            e.update(env)
            return subprocess.run([str(t / exe)], env=e, check=True, capture_output=True, text=True).stdout.strip()

        pc = run("pc")
        pc_back = run("pc", HOOKSHOT_PS5_BACK_TO_LAUNCHER="1", TEST_CHEATS="1")
        # (the console without the switch: the PC's menu, row for row, with Back to the Grapnel screen when its switch is on)
        off = run("console")
        need(off == pc, "the console's menu without HOOKSHOT_PS5_MENU_CONSOLE is not the PC's:\n%s\n---\n%s" % (off, pc))
        off_back = run("console", TEST_STAGE4="HOOKSHOT_PS5_BACK_TO_LAUNCHER=1", TEST_CHEATS="1")
        need(off_back.replace("Gameplay|Back to the Grapnel screen\n", "") == pc_back,
             "the console's menu without its switch differs from the PC's beyond lot 73's row")
        need("Gameplay|Back to the Grapnel screen" in off_back and "Back to the Grapnel screen" not in pc_back,
             "lot 73's row is not the console's alone")
        need("Graphics|Quality preset" in pc and "Graphics|Profile" in pc and "Graphics|Upscaler" in pc and
             "Debug|Set up for a weak laptop" in pc and "Gameplay|Restart" in pc and "Controls|Controls" in pc and
             "Controls page|Layout preset" in pc and "layout 1|Xbox / PC" in pc, "the PC's menu lost a row it had")
        # (the console with the switch: the list, exactly)
        on = run("console", TEST_STAGE4="HOOKSHOT_PS5_MENU_CONSOLE=1,HOOKSHOT_PS5_BACK_TO_LAUNCHER=1")
        need(on == CONSOLE_ROWS, "the console's menu, its switch set at stage 4 as hookshot_env.txt sets it, shows:\n%s\n--- not:"
                                 "\n%s" % (on, CONSOLE_ROWS))
        need(run("console", HOOKSHOT_PS5_MENU_CONSOLE="1", HOOKSHOT_PS5_BACK_TO_LAUNCHER="1") == CONSOLE_ROWS,
             "the console's menu with its switch set before the start is not the same")
        shown = [l.split("|", 1)[1] for l in on.split("\n") if not l.startswith("layout")]
        print("the console's menu: %d rows (and the title %r): %s" % (len(shown) - 1, shown[0], ", ".join(shown[1:])))
        # (lot 73's row only with its own switch; the screen switched off takes it away too)
        need(run("console", TEST_STAGE4="HOOKSHOT_PS5_MENU_CONSOLE=1") == CONSOLE_ROWS.replace("Gameplay|Back to the Grapnel screen\n", ""),
             "Back to the Grapnel screen shows without its switch")
        need(run("console", TEST_STAGE4="HOOKSHOT_PS5_MENU_CONSOLE=1,HOOKSHOT_PS5_BACK_TO_LAUNCHER=1,HOOKSHOT_PS5_LAUNCHER=0") ==
             CONSOLE_ROWS.replace("Gameplay|Back to the Grapnel screen\n", ""), "Back to the Grapnel screen shows with the screen off")
        # (lot 86: HOOKSHOT_PS5_MIC=1, set at stage 4: the microphone's rows under Volume, nothing else; the PC's and the console's
        # menu without its own switch as before)
        mic = run("console", TEST_STAGE4="HOOKSHOT_PS5_MENU_CONSOLE=1,HOOKSHOT_PS5_BACK_TO_LAUNCHER=1,HOOKSHOT_PS5_MIC=1")
        need(mic == CONSOLE_ROWS.replace("Audio|Volume", MIC_ROWS),
             "the console's menu with HOOKSHOT_PS5_MIC=1 shows:\n%s\n--- not:\n%s" % (mic, CONSOLE_ROWS.replace("Audio|Volume", MIC_ROWS)))
        need(run("console", TEST_STAGE4="HOOKSHOT_PS5_MIC=1") == pc, "HOOKSHOT_PS5_MIC=1 changes the console's menu without its switch")
        need(run("pc", TEST_STAGE4="HOOKSHOT_PS5_MIC=1") == pc, "HOOKSHOT_PS5_MIC=1 changes the PC's menu")
        print("the console's menu with HOOKSHOT_PS5_MIC=1: Volume, Microphone, Microphone gain, Test microphone on the Audio tab")
        # (HOS_OOT_CHEATS=1: the Debug tab's tools back, in the PC's order, the rest as without)
        cheats = run("console", TEST_STAGE4="HOOKSHOT_PS5_MENU_CONSOLE=1,HOOKSHOT_PS5_BACK_TO_LAUNCHER=1", TEST_CHEATS="1")
        debug = [l.split("|", 1)[1] for l in cheats.split("\n") if l.startswith("Debug|")]
        need(debug == DEBUG_TOOLS, "the Debug tab with HOS_OOT_CHEATS=1 is %s" % debug)
        need([l for l in cheats.split("\n") if not l.startswith("Debug|")] ==
             [l for l in on.split("\n") if not l.startswith("Debug|")], "HOS_OOT_CHEATS=1 changes another tab")
    check_sources()
    print("PASS: the console's menu is the lists' (the launch screen's values on its arrows), behind its switch; the PC's as before")


if __name__ == "__main__":
    main()
