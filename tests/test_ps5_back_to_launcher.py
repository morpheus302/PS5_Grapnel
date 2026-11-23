"""(ps5, lot 73) Back to the Grapnel screen from the game: the in-game menu's row (gpu/vk/vk_menu.cpp), the file it leaves for
the launch screen (gpu/vk/launcher_back.h), the title started again by the system (host/posix/ps5_relaunch.h, wired in
host/posix/ps5_relaunch.c) and the screen of the new process (gpu/vk/vk_launcher.cpp). No game, GPU or console:

- launcher_back.h compiled with a C++ compiler: when the row is offered (the console, HOOKSHOT_PS5_BACK_TO_LAUNCHER, the screen not
  switched off), the file written with its line, taken once (read and erased), not taken when it is not there, refused where
  it cannot be written;
- ps5_relaunch.h compiled with a C compiler and fake console calls: the close work first and bounded, then
  sceSystemServiceLoadExec("/app0/eboot.bin"), and the safe way out (the title closed by hos_ps5_exit) when the system refuses or
  accepts without ending the process; each step said in the log;
- by reading the sources: everything new is behind the switch (the row hidden, the file never looked at, the host called only
  from the row's second press), the options saved and the file left before the host is called, the screen told before it
  decides, the French texts there, and the title's usual exit (LoadExec "exit") untouched.

    python -I -B tests/test_ps5_back_to_launcher.py [--cxx g++] [--cc gcc]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block, between   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

BACK_TEST = r'''#include "launcher_back.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk::launcher;
static bool exists(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}
int main(int argc, char** argv) {
    (void)argc;
    const std::string dir = argv[1];
    // (offered: on the console, with the switch, unless the screen is switched off)
    CHECK(back_offered(true, true, nullptr) && back_offered(true, true, "1") && back_offered(true, true, ""));
    CHECK(!back_offered(true, true, "0") && !back_offered(true, false, nullptr) && !back_offered(false, true, nullptr));
    CHECK(!strcmp(kBackSwitch, "HOOKSHOT_PS5_BACK_TO_LAUNCHER") && !strcmp(kBackMarker, "hookshot_back_to_launcher.txt"));
    const std::string path = dir + "/" + kBackMarker;
    std::string why, said;
    // (no file: nothing taken, nothing said)
    CHECK(!back_take(path, &said) && said.empty());
    // (left, then taken once: its line in what is said, the file gone)
    CHECK(back_mark(path, "asked by the in-game menu, time 1795383067", &why) && why == path + ": written" && exists(path));
    CHECK(back_take(path, &said) && !exists(path));
    CHECK(said.find("asked by the in-game menu, time 1795383067") != std::string::npos && said.find("erased") != std::string::npos);
    CHECK(!back_take(path, &said));
    // (an empty file is the file all the same: the screen shows for it being there)
    { FILE* f = std::fopen(path.c_str(), "wb"); std::fclose(f); }
    CHECK(back_take(path, &said) && !exists(path));
    // (a long line: at most 200 bytes of it said, the file still erased)
    CHECK(back_mark(path, std::string(1000, 'x').c_str(), &why));
    CHECK(back_take(path, &said) && !exists(path) && said.size() < 400);
    // (a folder that is not there: not written, and why)
    CHECK(!back_mark(dir + "/missing/" + kBackMarker, "x", &why) && why.find("not written") != std::string::npos);
    std::puts("PASS: the launch screen's file: offered, left, taken once");
}
'''

RELAUNCH_TEST = r'''#include "ps5_relaunch.h"
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static char trail[8192];
static int close_answer, exec_answer;
static unsigned slept;
static void note(const char *what) { strcat(trail, what), strcat(trail, "\n"); }
static void say(const char *line) { char b[400]; snprintf(b, sizeof b, "say %s", line); note(b); }
static int close_work(unsigned ms) { char b[64]; snprintf(b, sizeof b, "close %u", ms); note(b); return close_answer; }
static int load_exec(const char *path) { char b[300]; snprintf(b, sizeof b, "exec %s", path); note(b); return exec_answer; }
static void sleep_ms(unsigned ms) { slept += ms; }
static void exit_title(void) { note("exit"); }
static const HosPs5Relaunch ops = {say, close_work, load_exec, sleep_ms, exit_title};
static void run(int c, int e) {
    trail[0] = 0, slept = 0, close_answer = c, exec_answer = e;
    hos_ps5_relaunch_run(&ops);
}
/* the lines of the trail in order, each found after the last */
static int in_order(const char *const *want, int n) {
    const char *at = trail;
    for (int i = 0; i < n; i++) {
        const char *f = strstr(at, want[i]);
        if (!f) { fprintf(stderr, "missing after %d: %s\n--\n%s", i, want[i], trail); return 0; }
        at = f + strlen(want[i]);
    }
    return 1;
}
int main(void) {
    CHECK(!strcmp(HOS_PS5_RELAUNCH_PATH, "/app0/eboot.bin"));
    /* refused: the close work, the call, the safe way out at once */
    run(1, (int)0x80020001);
    {
        static const char *const want[] = {"say relaunch: back to the Grapnel screen: the close work first", "close 15000",
                                           "say relaunch: close work done", "say relaunch: sceSystemServiceLoadExec(/app0/eboot.bin)",
                                           "exec /app0/eboot.bin", "say relaunch: FALLBACK: refused (0x80020001)", "exit"};
        CHECK(in_order(want, 7) && slept == 0);
        CHECK(strstr(trail, "start it again from the home screen"));
    }
    /* accepted (0) yet still running: 5 s given to the system, then the safe way out */
    run(1, 0);
    {
        static const char *const want[] = {"close 15000", "exec /app0/eboot.bin", "say relaunch: FALLBACK: accepted (0), yet this process "
                                           "still runs 5 s later", "exit"};
        CHECK(in_order(want, 4) && slept == 5000 && !strstr(trail, "refused"));
    }
    /* the close work not done in time: said, and on all the same */
    run(0, -1);
    {
        static const char *const want[] = {"close 15000", "say relaunch: the close work did not end within 15 s: going on without it",
                                           "exec /app0/eboot.bin", "say relaunch: FALLBACK: refused (0xffffffff)", "exit"};
        CHECK(in_order(want, 5) && !strstr(trail, "close work done"));
    }
    /* the call made once, the title closed once */
    {
        int execs = 0, exits = 0;
        for (const char *p = trail; (p = strstr(p, "exec ")); p++) execs++;
        for (const char *p = trail; (p = strstr(p, "\nexit\n")); p++) exits++;
        CHECK(execs == 1 && exits == 1);
    }
    /* (lot 74) the menu's Quit on the console: the same close work, bounded, then the title closes; no LoadExec of the eboot */
    trail[0] = 0, slept = 0, close_answer = 1, exec_answer = -1;
    hos_ps5_quit_run(&ops);
    {
        static const char *const want[] = {"say quit: the in-game menu's Quit: the close work first", "close 15000",
                                           "say quit: close work done", "say quit: hos_ps5_exit: the system closes the title", "exit"};
        CHECK(in_order(want, 5) && !strstr(trail, "exec ") && slept == 0);
    }
    trail[0] = 0, close_answer = 0;
    hos_ps5_quit_run(&ops);
    {
        static const char *const want[] = {"close 15000", "say quit: the close work did not end within 15 s: going on without it",
                                           "say quit: hos_ps5_exit", "exit"};
        CHECK(in_order(want, 4) && !strstr(trail, "close work done"));
    }
    puts("PASS: the title started again: close work, LoadExec, the safe way out; Quit: close work, then the title closes");
    return 0;
}
'''


def need(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def check_sources():
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    launcher = (ROOT / "gpu/vk/vk_launcher.cpp").read_text(encoding="utf-8")
    relaunch = (ROOT / "host/posix/ps5_relaunch.c").read_text(encoding="utf-8")
    build = (ROOT / "nx2/build.py").read_text(encoding="utf-8")
    platform = (ROOT / "host/posix/ps5_platform.c").read_text(encoding="utf-8")
    i18n = (ROOT / "gpu/vk/menu_i18n.h").read_text(encoding="utf-8")
    # ---- the row: on the Gameplay tab, before Bug report (Restart and Quit stay the last two), hidden without the switch ----
    table = block(menu, "Row rows[] = {")
    row = re.search(r'\{TAB_GAMEPLAY, "Back to the Grapnel screen", nullptr, 0, 0, 0, nullptr,\s*((?:"[^"]*"\s*)+)\}', table)
    need(row, "the row Back to the Grapnel screen is not on the Gameplay tab as an action")
    need(table.index('"Back to the Grapnel screen"') < table.index('{TAB_GAMEPLAY, "Bug report"') <
         table.index('{TAB_GAMEPLAY, "Restart"') < table.index('{TAB_GAMEPLAY, "Quit"'), "the row is not before Bug report")
    help_en = "".join(re.findall(r'"([^"]*)"', row.group(1)))
    need("not saved" in help_en and "lost" in help_en and "twice" in help_en, "the row's help does not say what is lost and how to go")
    need("(!is_back_row(rows[i]) || back_row())" in block(menu, "bool row_shown(int i) {"), "row_shown does not hide the row")
    shown = block(menu, "bool back_row() {")
    need(re.search(r'#ifdef __PROSPERO__\s*static const bool on = launcher::back_offered\(true, env_on\(launcher::kBackSwitch\), '
                   r'getenv\("HOOKSHOT_PS5_LAUNCHER"\)\);\s*return on;\s*#else\s*return false;\s*#endif', shown),
         "back_row is not the console's, behind HOOKSHOT_PS5_BACK_TO_LAUNCHER")
    # ---- its presses: twice, as Restart and Quit; the options saved and the pipeline cache kept, then the file, then the host ----
    tick = block(menu, "void menu_tick() {")
    need("const bool confirm_selected = &r == &rows[kQuitRow] || &r == &rows[kRestartRow] || is_back_row(r);" in tick,
         "the row does not ask twice")
    second = tick[tick.index("const bool confirm_selected"):]
    second = second[:second.index("} else if (r.cheat >= 0)")]
    need(second.index("save();") < second.index("pipeline_cache_save(true);") < second.index("if (is_back_row(r)) {") <
         second.index("back_to_launcher();"), "the options are not saved before the title is started again")
    need('logf("menu: Back to the Grapnel screen: asked; the next press confirms (B cancels)")' in second,
         "the first press is not said in the log")
    need('logf("menu: Back to the Grapnel screen: cancelled")' in tick, "a cancel is not said in the log")
    back = block(menu, "void back_to_launcher() {")
    need(back.index("#ifdef __PROSPERO__") < back.index("launcher::back_mark(game_folder_path() + launcher::kBackMarker") <
         back.index("hos_ps5_back_to_launcher()"), "the launch screen's file is not left before the host starts the title again")
    need(menu.count("hos_ps5_back_to_launcher()") == 1 and menu.count("back_to_launcher();") == 1,
         "the host is called from elsewhere than the row's second press")
    for prompts in ("const bool confirms = &selr == &rows[kRestartRow] || &selr == &rows[kQuitRow] || is_back_row(selr);",
                    "const bool confirms = &r == &rows[kRestartRow] || &r == &rows[kQuitRow] || is_back_row(r);"):
        need(prompts in menu, "the prompts do not say Confirm for the row")
    # ---- its French ----
    fr = between(i18n, "inline constexpr Entry kfr[] = {", "\n};")
    need('{"Back to the Grapnel screen", "Revenir à l\'écran Grapnel"},' in fr, "the row's name is not in French")
    need('{"%s", "Ferme le jeu et relance Grapnel sur son écran de lancement' % help_en in fr, "the row's help is not in French")
    # ---- the screen of the new process: the file looked at only with the switch, taken before it decides, shown for it ----
    run = block(launcher, "void launcher_run() {")
    need("if (L::back_offered(kConsole, env_on(L::kBackSwitch), env)) {" in run, "the screen looks for the file without the switch")
    # (lot 82: the shader preparation's own reasons beside the file's: its continue file, a preparation to do)
    need(run.index("back = L::back_take(game_folder_path() + L::kBackMarker, &said);") <
         run.index("if (!L::show(env, kConsole, mode, check.verdict, r1, back || prep_go_on, CP::asks_for_screen(prep_offer), "
                   "&why)) {"), "the screen decides before the file is taken")
    need("check.verdict != L::V_FAIL && !back && !prep_go_on && !CP::asks_for_screen(prep_offer)) {" in run,
         "R1 is still waited for when the game sent the player back")
    need(run.index('if (env && *env == \'0\') {') < run.index("L::back_offered"), "HOOKSHOT_PS5_LAUNCHER=0 no longer comes first")
    # ---- the host: a console file of its own, on a thread of its own, the close work bounded, LoadExec then hos_ps5_exit ----
    need(relaunch.index("#ifdef __PROSPERO__") < relaunch.index('#include "ps5_relaunch.h"') and
         relaunch.rstrip().endswith("#endif /* __PROSPERO__ */"), "host/posix/ps5_relaunch.c is not the console's alone")
    need('"ps5_compile_heap.c", "ps5_relaunch.c")]' in build, "nx2/build.py does not build host/posix/ps5_relaunch.c for the console")
    need("return sceSystemServiceLoadExec(path, NULL);" in relaunch, "the title is not started again by sceSystemServiceLoadExec(path, NULL)")
    need("static void exit_title(void) { hos_ps5_exit(0); }" in relaunch, "the safe way out is not the title's usual exit")
    need("mkwvk_close_asked();" in block(relaunch, "static DWORD WINAPI close_main(") and
         "WaitForSingleObject(close_done, wait_ms)" in relaunch, "the close work is not done on a thread of its own, bounded")
    entry = block(relaunch, "int hos_ps5_back_to_launcher(void) {")
    need("InterlockedExchange(&asked, 1)" in entry and "CreateThread(NULL, 1u << 20, relaunch_main" in entry,
         "the host does not start the title again once, on a thread of its own")
    # (no call into host/hos_relaunch.c, which the console's eboot does not link: taking it in brings execve, execvp and environ,
    # imports the console may not have; nor any other exec)
    code = re.sub(r"/\*.*?\*/", "", relaunch, flags=re.S)
    for name in ("hos_close_finish", "hos_window_closed_exit", "hos_relaunch_ask", "rx_close_asked", "execve", "execvp", "environ",
                 "system(", "fork("):
        need(name not in code, "host/posix/ps5_relaunch.c calls %s" % name)
    # ---- (lot 74) Quit on the console: on a thread of its own, once (with Back to the Grapnel screen: whichever came first) ----
    quit = block(relaunch, "int hos_ps5_quit(void) {")
    need("InterlockedExchange(&asked, 1)" in quit and "CreateThread(NULL, 1u << 20, quit_main" in quit,
         "the host does not close the title once, on a thread of its own")
    need("hos_ps5_quit_run(&ops);" in block(relaunch, "static DWORD WINAPI quit_main("), "Quit is not ps5_relaunch.h's steps")
    need(relaunch.count("static volatile LONG asked;") == 1, "Back to the Grapnel screen and Quit could both run")
    need(menu.count("hos_ps5_quit()") == 1 and "hos_ps5_quit()" in block(menu, "void quit_title() {"),
         "the host's Quit is called from elsewhere than the menu's Quit")
    # ---- the title's usual exit, the safe way out's, untouched ----
    exit_fn = block(platform, "void hos_ps5_exit(int code) {")
    need('(void)sceSystemServiceLoadExec("exit", NULL);' in exit_fn, "hos_ps5_exit no longer closes the title by LoadExec exit")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    check_sources()
    print("PASS: the row behind its switch, its presses and its French; the screen's file; the host's call and its safe way out")
    with tempfile.TemporaryDirectory(prefix="back-to-launcher-") as tmp:
        t = Path(tmp)
        (t / "back.cpp").write_text(BACK_TEST, encoding="utf-8")
        (t / "relaunch.c").write_text(RELAUNCH_TEST, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), str(t / "back.cpp"), "-o",
                        str(t / "back")], check=True)
        subprocess.run([str(t / "back"), tmp], check=True)
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host/posix"), str(t / "relaunch.c"), "-o",
                        str(t / "relaunch")], check=True)
        subprocess.run([str(t / "relaunch")], check=True)


if __name__ == "__main__":
    main()
