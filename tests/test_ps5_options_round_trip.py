"""(ps5, lot 69) The console's hookshot_options.ini through the in-game menu's first save, which the launch screen's first
change does: nothing the player has is lost or changed, and the game reads the same settings from the file saved.

The console's file (878 bytes, options_version=3: the menu never saved it there; a copy in the user's notes, REPRISE.md
22/11 ~15 h 55) is read by the menu's real reading (menu_init's), its real rows (vk_menu.cpp rows[], with their real keys,
ends and the Aspect ratio and Output resolution's real code), and saved by its real save() as the console's build has it
(__PROSPERO__: in place). The pad_ and key_ lines go through the host's real reading of them (host/nn_hid.c binds_load and
hos_binding), which save() asks. Then:

- every line of the console's file is in the saved one, word for word (options_version 3 becomes 6: the file's format);
- what the host reads at boot from the file is the same before and after: the handheld profile, the Output resolution it
  takes (host/nn_applet.c hos_output_parse, cut out of the source), the frame rate cap (host/nn_vi.c), the Aspect ratio's
  on/off (host/hos_wide_size.h hos_aspect_on), the button prompts (host/prompts.c), the speakers (host/nn_audio.c), and
  each control's pad and keys;
- the menu reading the saved file has every value it had from the console's (the whole Options struct), and saving again
  writes the same bytes;
- the launch screen's changes go where they should: frame_rate_cap=-1 (30) with the rest as it was, and launcher=1 kept;
- (lot 74) the console's menu, HOOKSHOT_PS5_MENU_CONSOLE=1: the file as the in-game menu left it on 22/11 (the Profile row
  set to docked, which made the Quality preset Custom) and other hidden rows changed: menu_init's console_pin puts each
  console's value back, and the one save that follows writes the console's file again byte for byte; a second start has
  nothing to put back; the host's handheld() (host/nn_applet.c) takes the console's profile from such a file with the
  switch, the file's without it, and the PC's never.

The lines save() adds (every row at its value, the controls' other lines) are printed: the audit's list. No game, GPU or
console.

    python -I -B tests/test_ps5_options_round_trip.py [--cxx g++]
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
RUN_ALL_SKIP = {"windows": "the console's save is POSIX's in-place write: compiled as the console builds it"}

ROOT = Path(__file__).resolve().parents[1]

# (the console's file, 878 bytes, as read on 22/11 at ~15 h 55: dist/next/43-fusion-six-mihawkgen/hookshot_options.compteur-fps.ini)
CONSOLE = """quality_preset=1
fullscreen=1
gpu_device=0
frame_generation=2
frame_pacing=1
resolution=1
output_res=0
upscaling=1
sharpness=0
handheld_profile=1
ultrawide=0
robust_buffers=0
fps_counter=1
game_speed=1
text_speed=0
controller_in_background=0
mouse_camera=0
mouse_sensitivity=100
invert_mouse_vertical=0
invert_camera_x=0
invert_camera_y=0
layout_preset=0
options_version=3
pad_a=1
key_a=69,0
pad_b=0
key_b=16,0
pad_x=3
key_x=32,0
pad_y=2
key_y=1,0
pad_ls=7
key_ls=17,0
pad_rs=8
key_rs=67,0
pad_l=9
key_l=81,0
pad_r=10
key_r=82,0
pad_zl=15
key_zl=90,2
pad_zr=16
key_zr=88,70
pad_plus=6
key_plus=9,13
pad_minus=4
key_minus=77,0
pad_left=13
key_left=49,0
pad_up=11
key_up=50,0
pad_right=14
key_right=51,0
pad_down=12
key_down=52,0
key_move_up=87,0
key_move_down=83,0
key_move_left=65,0
key_move_right=68,0
key_cam_up=38,73
key_cam_down=40,75
key_cam_left=37,74
key_cam_right=39,76
"""

FIXTURE = r'''#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include "longshot_env.h"
#include "hos_wide_size.h"
#include "display_modes.h"
#include "upscaler_rows.h"
#include "fps_stats.h"
#include "gpu_choice.h"
#define _Static_assert static_assert   /* (vk_api.h's NVN types are C's) */
#include "vk_api.h"
#include "launcher_logic.h"   /* (lot 74: kConsolePinned) */
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

std::string ini_file;
// ---- the host's side: its reading of the controls (host/nn_hid.c), as a C file compiled here ----
namespace host_side {
typedef long LONG;
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
inline void AcquireSRWLockShared(SRWLOCK*) {}
inline void ReleaseSRWLockShared(SRWLOCK*) {}
inline void AcquireSRWLockExclusive(SRWLOCK*) {}
inline void ReleaseSRWLockExclusive(SRWLOCK*) {}
inline LONG InterlockedExchange(volatile LONG* p, LONG v) { const LONG o = *p; *p = v; return o; }
inline LONG InterlockedCompareExchange(volatile LONG* p, LONG v, LONG c) { const LONG o = *p; if (o == c) *p = v; return o; }
inline void Sleep(int) {}
inline const char* hos_options_file() { return ini_file.c_str(); }
inline void hos_log(const char* fmt, ...) { va_list a; va_start(a, fmt); std::vfprintf(stderr, fmt, a); va_end(a); std::fputc('\n', stderr); }
inline void hos_input_clear_toggles() {}
enum { VK_LBUTTON = 1, VK_RBUTTON = 2, VK_MBUTTON = 4, VK_TAB = 9, VK_RETURN = 13, VK_SHIFT = 16, VK_CONTROL = 17,
       VK_CAPITAL = 20, VK_SPACE = 32, VK_LEFT = 37, VK_UP = 38, VK_RIGHT = 39, VK_DOWN = 40 };
/* HID */
}  // namespace host_side
// (host/nn_applet.c: the Output resolution as the host reads it at boot)
inline void hos_log(const char*, ...) {}
inline const char* hos_options_file() { return ini_file.c_str(); }
/* OUTPUT_PARSE */
/* INI_TEXT */
/* HANDHELD */

namespace mkwvk {
void logf(const char*, ...) {}
/* OPTIONS */
Options opts;
/* AUTO_PRESET */
/* TABS */
/* ROW */
namespace ur = upscaler_rows;
namespace i18n { int game_lang_count() { return @GAME_LANGS@; } int lang_count() { return @MENU_LANGS@; } }
const char* tx(int) { return ""; }
const char* hn(int, int) { return nullptr; }
constexpr int kMaxMics = 64;
int output_pick = 0;
// (the console's host: the main screen as hos_sdl_desktop_size answers, 3840 x 2160; the aspect ratio host/hos_main.c started
// with, 16:9; the controls through the host's own reading)
struct Host {
    int (*screen_size)(int*, int*);
    float (*aspect_ratio)();
    int (*binding)(int, int, int);
    void (*audio_output_select)(const char*);
};
int ps5_screen(int* w, int* h) { *w = 3840, *h = 2160; return 1; }
float ps5_aspect() { return 16.0f / 9.0f; }
int ps5_binding(int set, int button, int slot) { return host_side::hos_binding(set, button, slot); }
Host host_value{ps5_screen, ps5_aspect, ps5_binding, nullptr}, *host = &host_value;
bool can_bind() { return true; }
/* SCREEN */
/* OUTPUTS */
/* ROWS */
constexpr int kRows = sizeof rows / sizeof rows[0];
/* INI_KEY */
/* BIND_NAMES */
int ultrawide_read = 1;
std::string output_res_read;
int launcher_mode = -1;
/* OUTPUTS_SYNC */
std::string ini_path() { return ini_file; }
/* HELPERS */
std::string mic_want, output_want, output_saved_name, gpu_saved_card;
uint64_t auto_for = 0, auto_tuned = 0;
struct { int steps = 0; bool went_down = false; } auto_tuning;
std::vector<gpu_choice::Card> gpu_device_cards;
bool gpu_card_missing = false;
std::string mic_key(const char* t) { return t; }
std::string output_id_key(const char* t) { return t; }
std::string output_name_key(const char* t) { return t; }
void refresh_outputs() {}
bool can_outputs() { return false; }
void refresh_mics() {}
void mic_tell_host() {}
int rumble_step(int v) { return (std::min(std::max(v, 0), 150) + 12) / 25 * 25; }
#define __PROSPERO__ 1   /* (the console's save: in place) */
/* SAVE */
#undef __PROSPERO__
/* CONSOLE_PIN */
/// menu_init's reading of the options file, as it is in vk_menu.cpp, then the Aspect ratio and Output resolution's ends and
/// values (outputs_sync), as menu_init does after it
void load() {
    opts = Options{};
    output_res_read.clear(), launcher_mode = -1, ultrawide_read = 1;
    custom_output = {0, 0};
    bool has_options = false, has_output = false, has_memory_budget = false;
/* LOAD */
/* BUDGET */
    (void)has_options, (void)has_output, (void)has_memory_budget;
    outputs_sync();
}
}  // namespace mkwvk

std::string slurp(const std::string& p) {
    std::string s;
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
        char b[4096];
        for (size_t n; (n = std::fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
        std::fclose(f);
    }
    return s;
}
std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> v;
    size_t at = 0;
    while (at < s.size()) {
        const size_t e = s.find('\n', at);
        v.push_back(s.substr(at, e == std::string::npos ? std::string::npos : e - at));
        if (e == std::string::npos) break;
        at = e + 1;
    }
    return v;
}
/// What the host reads from the options file at boot, in words
std::string host_view() {
    char text[80] = "";
    const int handheld = ini_option("handheld_profile", 0) != 0;
    const int chosen = ini_text("output_res", text, sizeof text);
    uint32_t w = 0, h = 0;
    if (!handheld && chosen) hos_output_parse(text, &w, &h);
    int saved = -1, version = 0, prompts = 0, speakers = 0;
    for (const std::string& l : lines_of(slurp(ini_file))) {
        if (!l.compare(0, 10, "ultrawide=")) saved = atoi(l.c_str() + 10) != 0;
        if (!l.compare(0, 16, "options_version=")) version = atoi(l.c_str() + 16);
        if (!l.compare(0, 15, "button_prompts=")) prompts = atoi(l.c_str() + 15);
        if (!l.compare(0, 13, "audio_output=")) speakers = atoi(l.c_str() + 13);
    }
    speakers = speakers < 1 ? 0 : speakers > 3 ? 3 : speakers;
    char b[512];
    std::snprintf(b, sizeof b, "handheld %d; output %ux%u (0 x 0: the game's own); frame_rate_cap %d; aspect on 4K screen %d, "
                  "on no screen %d; button_prompts %d; speakers %d", handheld, w, h, ini_option("frame_rate_cap", 0),
                  hos_aspect_on(saved, version, 3840, 2160), hos_aspect_on(saved, version, 0, 0), prompts, speakers);
    std::string v = b;
    // (each control's pad inputs and keys, as host/nn_hid.c reads them back)
    host_side::binds_state() = 0;
    for (int i = 0; i < HOS_KEY_N; i++) {
        std::snprintf(b, sizeof b, "; %d: pad %d,%d keys %d,%d", i, host_side::hos_binding(HOS_BIND_PAD, i, 0),
                      host_side::hos_binding(HOS_BIND_PAD, i, 1), host_side::hos_binding(HOS_BIND_KEY, i, 0),
                      host_side::hos_binding(HOS_BIND_KEY, i, 1));
        v += b;
    }
    return v;
}

int main(int argc, char** argv) {
    using namespace mkwvk;
    CHECK(argc == 3);
    ini_file = argv[1];
    const std::string console = slurp(argv[2]);
    CHECK(console.size() == 878);
    if (FILE* f = std::fopen(ini_file.c_str(), "wb")) std::fwrite(console.data(), 1, console.size(), f), std::fclose(f);
    const std::string host_before = host_view();
    host_side::binds_state() = 0;   // (the game's start: read once, from the console's file)
    load();
    const Options loaded = opts;
    CHECK(opts.preset == 1 && opts.framegen == 2 && opts.pace == 1 && opts.resolution == 1 && opts.upscale == 1);
    CHECK(opts.sharpness == 0 && opts.handheld == 1 && opts.show_fps == 1 && opts.realtime == 1 && opts.pad_layout == 0);
    CHECK(opts.budget == 1);   // (Minimum's mip trimming, as the console runs today: the save writes memory_budget=1)
    // (the first save: the launch screen's first change, or the in-game menu's)
    save();
    const std::string saved = slurp(ini_file);
    std::map<std::string, std::string> after;
    for (const std::string& l : lines_of(saved))
        if (l.find('=') != std::string::npos) after[l.substr(0, l.find('='))] = l;
    int kept = 0;
    for (const std::string& l : lines_of(console)) {
        if (l.empty()) continue;
        const std::string key = l.substr(0, l.find('='));
        if (key == "options_version") {
            CHECK(after[key] == "options_version=6");
            continue;
        }
        if (after.count(key) == 0 || after[key] != l) {
            std::fprintf(stderr, "FAIL: the console's line %s became %s\n", l.c_str(), after.count(key) ? after[key].c_str() : "(gone)");
            return 1;
        }
        kept++;
    }
    std::string added;
    std::map<std::string, std::string> before;
    for (const std::string& l : lines_of(console))
        if (l.find('=') != std::string::npos) before[l.substr(0, l.find('='))] = l;
    int n_added = 0;
    for (const auto& [key, l] : after)
        if (!before.count(key)) added += (added.empty() ? "" : " ") + l, n_added++;
    const std::string host_after = host_view();
    if (host_after != host_before) {
        std::fprintf(stderr, "FAIL: what the host reads at boot changed:\nbefore %s\nafter  %s\n", host_before.c_str(), host_after.c_str());
        return 1;
    }
    // (the next start: the menu reads the saved file to every value it had; a second save, the same bytes)
    host_side::binds_state() = 0;
    load();
    CHECK(std::memcmp(&opts, &loaded, sizeof opts) == 0);
    save();
    CHECK(slurp(ini_file) == saved);
    // (the launch screen's changes: frame_rate_cap -1 (30), launcher=1; the rest as it was)
    opts.fps_cap = -1;
    launcher_mode = 1;
    save();
    const std::string changed = slurp(ini_file);
    CHECK(changed.find("frame_rate_cap=-1\n") != std::string::npos && changed.find("launcher=1\n") != std::string::npos);
    for (const std::string& l : lines_of(console))
        if (!l.empty() && l.compare(0, 16, "options_version=") && changed.find(l + "\n") == std::string::npos) {
            std::fprintf(stderr, "FAIL: after a change, the console's line %s is gone\n", l.c_str());
            return 1;
        }
    CHECK(ini_option("frame_rate_cap", 0) == -1);
    host_side::binds_state() = 0;
    load();
    CHECK(opts.fps_cap == -1 && launcher_mode == 1);
    opts.fps_cap = 0;
    CHECK(std::memcmp(&opts, &loaded, sizeof opts) == 0);
    // ---- (lot 74) the console's menu: its values put back over a file the PC's menu changed, the file written again ----
    {
        std::string tonight = saved;   // (build/lot71/console-apres-71-B: the two lines the menu changed on 22/11, then more)
        const auto set_line = [&](const char* from, const char* to) {   // (a whole line, the first one too)
            const size_t at = ("\n" + tonight).find(std::string("\n") + from + "\n");
            CHECK(at != std::string::npos);
            tonight.replace(at, std::strlen(from), to);
        };
        set_line("quality_preset=1", "quality_preset=0");
        set_line("handheld_profile=1", "handheld_profile=0");
        set_line("sharpness=0", "sharpness=30");
        set_line("debug_tools=0", "debug_tools=1");
        set_line("shadow_resolution=0", "shadow_resolution=3");
        set_line("audio_output=0", "audio_output=2");
        set_line("memory_budget=1", "memory_budget=3");
        if (FILE* f = std::fopen(ini_file.c_str(), "wb")) std::fwrite(tonight.data(), 1, tonight.size(), f), std::fclose(f);
        // (the host at boot, before the menu: the PC's menu's docked profile without the switch, the console's with it)
        CHECK(::handheld_pc() == 0 && ::handheld_console_off() == 0);
        setenv("HOOKSHOT_PS5_MENU_CONSOLE", "1", 1);
        CHECK(::handheld_console_on() == 1);
        setenv("HOS_HANDHELD", "0", 1);   // (the development switch still over it)
        CHECK(::handheld_console_env() == 0);
        unsetenv("HOS_HANDHELD");
        unsetenv("HOOKSHOT_PS5_MENU_CONSOLE");
        host_side::binds_state() = 0;
        load();
        CHECK(opts.handheld == 0 && opts.preset == 0 && opts.sharpness == 30 && opts.debug_tools == 1 && opts.shadow_res == 3);
        CHECK(opts.audio_output == 2 && opts.budget == 3);
        const int put_back = console_pin();
        CHECK(put_back == 7);
        CHECK(opts.handheld == 1 && opts.preset == 1 && opts.sharpness == 0 && opts.debug_tools == 0 && opts.shadow_res == 0);
        CHECK(opts.audio_output == 0 && opts.budget == 1);
        save();
        CHECK(slurp(ini_file) == saved);   // (the console's file again, byte for byte)
        host_side::binds_state() = 0;
        load();
        CHECK(console_pin() == 0 && std::memcmp(&opts, &loaded, sizeof opts) == 0);
        std::printf("the console's menu: %d values put back over a file the PC's menu changed; the file written again as the "
                    "console's, byte for byte\n", put_back);
    }
    std::printf("kept %d of the console's lines word for word (options_version 3 -> 6); %d lines added: %s\n", kept, n_added,
                added.c_str());
    std::printf("the host at boot, before and after: %s\n", host_before.substr(0, host_before.find("; 0:")).c_str());
    std::puts("PASS: the console's options file keeps every value through the menu's first save; the host reads the same");
}
'''


def definition(text, signature):
    """The definition of a function by the start of its signature (srccut's block: one match only)"""
    return block(text, signature)


def fixture():
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    header = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    hid = (ROOT / "host/nn_hid.c").read_text(encoding="utf-8")
    applet = (ROOT / "host/nn_applet.c").read_text(encoding="utf-8")
    i18n_cpp = (ROOT / "gpu/vk/vk_menu_i18n.cpp").read_text(encoding="utf-8")
    i18n_h = (ROOT / "gpu/vk/menu_i18n.h").read_text(encoding="utf-8")
    options = re.search(r"struct Options \{.*?\n\};", header, re.S).group(0)
    tabs = re.search(r"enum MenuTab \{.*?\};", menu, re.S).group(0)
    row = re.search(r"struct Row \{.*?\n\};", menu, re.S).group(0)
    rows = between(menu, "Row rows[] = {", "constexpr int kQuitRow")
    # (the rows as they are: their names, keys, values and ends; their texts and hints stubbed)
    rows = re.sub(r"\btx_\w+\b", "tx", rows)
    rows = re.sub(r"\bhn_\w+\b", "hn", rows)
    rows = rows.replace("kFpsDevLineHelp", '""').replace("kMicLevelHelp", '""')
    # (the host's reading of the controls: its tables, the defaults, the file's lines and the binding the menu asks)
    hid_part = "\n".join([
        between(hid, "static const char *const bind_names[HOS_KEY_N]", "static const signed char pad_default[HOS_SW_N]"),
        between(hid, "static const signed char pad_default[HOS_SW_N]", "#ifndef VK_XBUTTON1"),
        between(hid, "static const unsigned char key_default[HOS_KEY_N][2]", "static volatile LONG toggle_reset;"),
        "static SRWLOCK pad_bind_lock = SRWLOCK_INIT;\nstatic int pad_bind[HOS_KEY_N][2];\nstatic volatile LONG key_bind[HOS_KEY_N][2];",
        block(hid, "static void binds_default(void) {"),
        block(hid, "static int pad_code(const char *text) {"),
        block(hid, "static void binds_load(void) {"),
        block(hid, "int hos_binding(int set, int button, int slot) {"),
    ])
    # (binds_load's own once: a counter the check sets back, so each reading of the file is the host's first)
    if hid_part.count("static volatile LONG state;") != 1:
        raise SystemExit("host/nn_hid.c binds_load no longer keeps its once as `static volatile LONG state;`")
    hid_part = hid_part.replace("static volatile LONG state;", "volatile LONG& state = binds_state();")
    hid_part = "inline volatile LONG& binds_state() { static volatile LONG s; return s; }\n" + hid_part
    init = block(menu, "void menu_init() {")
    load = between(init, "    if (FILE* f = fopen(ini_path().c_str(), \"r\")) {", "    first_start = !has_options;")
    # (and the Quality preset's memory budget, which menu_init sets after the reading: a file from before memory_budget=
    # takes its preset's, Minimum's on the console, and the first save writes it)
    budget = between(init, "    if (opts.preset > 0 && !has_memory_budget)", "    menu_debug_rows(")
    screen = "\n".join([block(menu, "bool host_screen_size(uint32_t* w, uint32_t* h) {"),
                        block(menu, "bool screen_size(uint32_t* w, uint32_t* h) {"),
                        block(menu, "std::vector<display_modes::Size> screen_modes() {")])
    outputs = between(menu, "struct Out {", "int twelfths(float scale)")
    helpers = between(menu, "struct EnvOverride {", "std::string ini_path() {")
    bind_names = re.search(r"const char\* const bind_names\[HOS_KEY_N\] = \{.*?\};", menu, re.S).group(0)
    game_langs = len(re.findall(r'\{"[\w-]+", "[^"]*"\}', between(i18n_cpp, "constexpr GameLang kGameLangs[] = {", "};")))
    menu_langs = len(re.findall(r'^    \{"[\w-]+", "[^"]*", \w+, \d+\},$', i18n_h, re.M))
    # (lot 74: host/nn_applet.c handheld() four times: the console's with the switch off, on, and HOS_HANDHELD over it; the PC's)
    hh = block(applet, "static int handheld(void) {")
    handheld = "\n".join(["#define __PROSPERO__ 1", hh.replace("handheld(void)", "handheld_console_off(void)"),
                          hh.replace("handheld(void)", "handheld_console_on(void)"),
                          hh.replace("handheld(void)", "handheld_console_env(void)"), "#undef __PROSPERO__",
                          hh.replace("handheld(void)", "handheld_pc(void)")])
    out = FIXTURE
    for marker, text in (("/* HID */", hid_part), ("/* HANDHELD */", handheld),
                         ("/* CONSOLE_PIN */", block(menu, "static int console_pin() {")), ("/* OUTPUT_PARSE */", block(applet, "int hos_output_parse(const char *text, uint32_t *w, uint32_t *h) {")),
                         ("/* INI_TEXT */", block(applet, "static int ini_text(const char *key, char *out, size_t size) {") +
                          "\nint ini_option(const char *key, int otherwise) {\n    char text[80];\n    return ini_text(key, text, sizeof text) ? atoi(text) : otherwise;\n}"),
                         ("/* OPTIONS */", options), ("/* AUTO_PRESET */", re.search(r"constexpr int kAutoPreset = \d+;", header).group(0)), ("/* TABS */", tabs), ("/* ROW */", row), ("/* SCREEN */", screen),
                         ("/* OUTPUTS */", outputs), ("/* ROWS */", rows), ("/* INI_KEY */", block(menu, "const char* ini_key(const Row& r) {")),
                         ("/* BIND_NAMES */", bind_names), ("/* OUTPUTS_SYNC */", block(menu, "void outputs_sync() {")),
                         ("/* HELPERS */", helpers), ("/* SAVE */", block(menu, "void save() {")), ("/* LOAD */", load), ("/* BUDGET */", budget),
                         ("@GAME_LANGS@", str(game_langs)), ("@MENU_LANGS@", str(menu_langs))):
        out = out.replace(marker, text)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix="ps5-options-") as tmp:
        cpp = Path(tmp) / "round_trip.cpp"
        exe = Path(tmp) / "round_trip"
        cpp.write_text(fixture(), encoding="utf-8")
        console = Path(tmp) / "console.ini"
        if len(CONSOLE.encode("ascii")) != 878:
            raise SystemExit("the console's file here is %d bytes, not its 878" % len(CONSOLE.encode("ascii")))
        console.write_bytes(CONSOLE.encode("ascii"))
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-parameter",
                        "-Wno-missing-field-initializers", "-I", str(ROOT / "host"), "-I", str(ROOT / "gpu/vk/compat"),
                        "-I", str(ROOT / "gpu/vk"), "-I", str(ROOT / "gpu/render"), str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe), str(Path(tmp) / "hookshot_options.ini"), str(console)], check=True)


if __name__ == "__main__":
    main()
