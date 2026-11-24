"""(ps5, lot 88) The game file on another drive (HOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup in hookshot_env.txt, which the Grapnel
builder's step 6 writes when it installs on the extended storage). No game, GPU or console:

- host/posix/ps5_game_file.h ps5_game_file_choose, run with a file list of its own: no setting (unset or empty): game.nup in the
  data folder, as before; the setting's file there: that file; not there but game.nup in the data folder: that one, the line says
  why; neither: the setting's file, whose open fails and says why; and the drive a path is on, for the log;
- gpu/vk/launcher_logic.h with no game file: the screen shows whatever the mode (but with the development switch at 0), QUIT is
  chosen, neither Options nor PLAY starts the game, QUIT still closes it; with a game file, as before;
- by reading: the RomFS is opened after stage 4 (the settings in the environment), not at stage 3; the setting is read in a
  function; stage 4 says the game file (path, size, drive) and whether the driver's shader cache folder can be written; the launch
  screen asks the host and says so in both languages.

    python -I -B tests/test_ps5_game_file.py [--cc gcc] [--cxx g++]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cc", "gcc", "--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

CHOOSE = r'''
#include <stdlib.h>
#include "ps5_game_file.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static const char *there[4];
static int exists(const char *p) {
    for (int i = 0; i < 4; i++) if (there[i] && !strcmp(there[i], p)) return 1;
    return 0;
}
int main(void) {
    const char *beside = "/data/grapnel/game.nup", *ext = "/mnt/ext1/grapnel/game.nup";
    char line[900];
    int from = -1;
    /* no setting: game.nup in the data folder, as before (whether it is there or not: rom_from_package says) */
    there[0] = beside;
    CHECK(ps5_game_file_choose(NULL, beside, exists, &from, line, sizeof line) == beside && !from);
    CHECK(ps5_game_file_choose("", beside, exists, &from, line, sizeof line) == beside && !from && strstr(line, "data folder"));
    /* the setting's file there: that one, over game.nup */
    there[1] = ext;
    CHECK(ps5_game_file_choose(ext, beside, exists, &from, line, sizeof line) == ext && from && !strcmp(line, "HOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup"));
    there[0] = NULL;
    CHECK(ps5_game_file_choose(ext, beside, exists, &from, line, sizeof line) == ext && from);
    /* not there, game.nup there: game.nup, and why */
    there[0] = beside, there[1] = NULL;
    CHECK(ps5_game_file_choose(ext, beside, exists, &from, line, sizeof line) == beside && !from);
    CHECK(strstr(line, "HOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup is NOT THERE") && strstr(line, "instead"));
    /* neither: the setting's, which will not open; the line says the game cannot start */
    there[0] = NULL;
    CHECK(ps5_game_file_choose(ext, beside, exists, &from, line, sizeof line) == ext && from && strstr(line, "cannot start"));
    /* the drives */
    CHECK(!strcmp(ps5_game_file_volume(ext), "the extended storage") && !strcmp(ps5_game_file_volume("/mnt/usb0/g.nup"), "a USB drive"));
    CHECK(strstr(ps5_game_file_volume(beside), "/data") && strstr(ps5_game_file_volume("/app0/game.nup"), "/app0"));
    CHECK(!strcmp(ps5_game_file_volume("/somewhere/x.nup"), "a drive not known here"));
    puts("the game file: no setting, there, not there with game.nup, neither");
    return 0;
}
'''

LAUNCHER = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "launcher_logic.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::launcher;
int main() {
    const char* why = "";
    // (shown without the game file whatever the mode, R1, the verdict; not with the development switch at 0)
    CHECK(show(nullptr, true, 1, V_OK, false, false, false, true, &why) && std::strstr(why, "game file"));
    CHECK(show(nullptr, true, 0, V_OK, false, false, false, true, &why));
    CHECK(!show("0", true, 1, V_OK, false, false, false, true, &why));
    CHECK(!show(nullptr, true, 1, V_OK, false, false, false, false, &why));   // (as before: nothing to say)
    CHECK(!show(nullptr, true, 1, V_OK, false, false, false, &why) && show(nullptr, true, 1, V_FAIL, false, false, false, &why));
    // (no game file: QUIT chosen; Options and PLAY start nothing; QUIT, confirmed twice, closes)
    State s;
    begin(s, 0, V_OK, true);
    CHECK(s.no_game && s.home == H_QUIT && s.outcome == NONE);
    Input start;
    start.start = true;
    step(s, start, 1);
    CHECK(s.outcome == NONE);
    s.home = H_PLAY;
    Input confirm;
    confirm.confirm = true;
    step(s, confirm, 2);
    CHECK(s.outcome == NONE);
    s.home = H_QUIT;
    step(s, confirm, 3);
    CHECK(s.outcome == NONE && s.quit_armed);
    step(s, confirm, 4);
    CHECK(s.outcome == QUIT);
    // (with it: as before)
    begin(s, 0, V_OK);
    CHECK(!s.no_game && s.home == H_PLAY);
    step(s, start, 1);
    CHECK(s.outcome == PLAY);
    begin(s, 0, V_FAIL, false);
    CHECK(s.home == H_SHADERS);
    begin(s, 0, V_FAIL, true);
    CHECK(s.home == H_QUIT);
    // (its words, in both languages, the reason in each)
    CHECK(std::strstr(text(T_NO_GAME, EN), "%s") && std::strstr(text(T_NO_GAME, FR), "%s"));
    CHECK(std::strstr(text(T_NO_GAME, EN), "HOOKSHOT_PS5_NUP") && std::strstr(text(T_NO_GAME, FR), "HOOKSHOT_PS5_NUP"));
    std::puts("the launch screen: shown without the game file, nothing starts the game, QUIT closes; with it, as before");
    return 0;
}
'''


def need(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def check_sources():
    main_c = (ROOT / "host/hos_main.c").read_text(encoding="utf-8")
    launcher = (ROOT / "gpu/vk/vk_launcher.cpp").read_text(encoding="utf-8")
    logic = (ROOT / "gpu/vk/launcher_logic.h").read_text(encoding="utf-8")
    # (the RomFS after stage 4: the settings are in the environment then; stage 3 no longer opens it)
    ps5_main = main_c[main_c.index("ps5: stage 3 data root"):]
    need("rom_from_package(" not in ps5_main[:ps5_main.index("load_longshot_env()")],
         "the RomFS is still opened before stage 4's settings")
    at = ps5_main.index("load_longshot_env()")
    need(at < ps5_main.index("ps5_game_file_stage(argc, argv);") < ps5_main.index("static const char elf[] = \"/app0/hookshot-broker.elf\";"),
         "the game file is not opened after stage 4's settings, before the broker")
    need("ps5_cache_folder_probe();" in ps5_main[at:at + 6000], "stage 4 does not say whether the driver's cache folder can be written")
    # (the setting read in a function, at the call: never in a static before main; with the chooser)
    rom = block(main_c, "static int rom_from_package(const char *folder, const char *nup, char *line, size_t cap) {")
    need('getenv("HOOKSHOT_PS5_NUP")' in rom and "ps5_game_file_choose(setting, beside, ps5_file_there" in rom,
         "rom_from_package does not choose the game file by HOOKSHOT_PS5_NUP")
    need(main_c.count('"HOOKSHOT_PS5_NUP"') == 1, "HOOKSHOT_PS5_NUP is read elsewhere")
    stage = block(main_c, "static void ps5_game_file_stage(int argc, char **argv) {")
    need("ps5: stage 4 game file: %s, %.2f GiB, on %s (%s)" in stage and "ps5_game_missing_line" in stage,
         "stage 4 does not say the game file (path, size, drive) or what is missing")
    probe = block(main_c, "static void ps5_cache_folder_probe(void) {")
    need('"/app0/radv-shader-cache"' in probe and 'getenv("MESA_SHADER_CACHE_DIR")' in probe and "unlink(probe)" in probe,
         "the cache folder probe does not look where the driver keeps its cache, or leaves its file")
    # (the launch screen: asks the host, shows, starts nothing, and says why)
    run = block(launcher, "void launcher_run() {")
    need("no_game = hos_ps5_game_missing(no_game_line, sizeof no_game_line) != 0;" in run and
         "CP::asks_for_screen(prep_offer), no_game, &why)" in run and "L::begin(st, now_ns(), check.verdict, no_game);" in run,
         "the launch screen does not ask for the game file and show it")
    need("st.outcome = no_game ? L::QUIT : L::PLAY;" in run, "a screen that does not draw starts the game without its file")
    need("T(L::T_NO_GAME), no_game_line" in block(launcher, "void draw_home(ui::Ctx& c, const Layout& l) {"),
         "the home page does not say the game file is missing")
    need(re.search(r'\{T_NO_GAME, "[^"]*%s', logic) is not None, "T_NO_GAME has no reason")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cc", default="gcc")
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    check_sources()
    with tempfile.TemporaryDirectory(prefix="ps5-game-file-") as tmp:
        t = Path(tmp)
        (t / "choose.c").write_text(CHOOSE, encoding="utf-8")
        (t / "launcher.cpp").write_text(LAUNCHER, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-format-truncation", "-I", str(ROOT / "host/posix"),
                        str(t / "choose.c"), "-o", str(t / "choose")], check=True)
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), "-I", str(ROOT / "gpu/vk/compat"),
                        str(t / "launcher.cpp"), "-o", str(t / "launcher")], check=True)
        for exe in ("choose", "launcher"):
            r = subprocess.run([str(t / exe)], capture_output=True, text=True)
            if r.returncode:
                sys.stderr.write(r.stdout + r.stderr)
                raise SystemExit("FAIL: " + exe)
            print(r.stdout.strip())
    print("PASS: HOOKSHOT_PS5_NUP: the game file chosen, game.nup in the data folder when it is not there, the launch screen's "
          "word when neither is; read after stage 4")


if __name__ == "__main__":
    main()
