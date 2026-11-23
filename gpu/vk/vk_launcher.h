// (ps5, lot 69) The launch screen (vk_launcher.cpp): before the game's first instruction, on the TV, with JOUER, CONTINUER,
// RÉGLAGES, SHADERS and QUITTER. Its logic is launcher_logic.h's, its shader check launcher_check.h's.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mkwvk {

/// (vk_main.cpp started_up, after the renderer is up) The screen when it is to show (HOOKSHOT_PS5_LAUNCHER, launcher= in the
/// options file, the shader check, R1 held; lot 73: the in-game menu's Back to the Grapnel screen, launcher_back.h), until the
/// player starts the game (or the countdown does): returns then, with
/// everything it took let go. Closes the title when the player quits from it. Off the console only with RX_VK_LAUNCHER=1
void launcher_run();
/// Whether the screen shows now: the in-game menu's tick and drawing leave the frame to it (vk_menu.cpp)
bool launcher_showing();
/// Its drawing, into Dear ImGui's frame begun for a w x h window image (vk_menu.cpp menu_record)
void launcher_draw(uint32_t w, uint32_t h);
/// (vk_draw.cpp) Up to want of the pipelines play noted, asked of the driver without a compile: how many it had (-1: none
/// can be asked); *asked: how many were
int launcher_driver_check(int want, size_t* asked);

// ---- (ps5, lot 82) The console's own shader preparation (vk_draw.cpp console_prepare_*; its rules console_prepare.h). The
// screen (HOOKSHOT_PS5_PREPARE=1) reads its facts, starts it, shows its progress and stops it; nothing of it runs otherwise ----
/// What the files say, read in a few milliseconds (heads and sizes, and the progress file): the census there, how far an earlier
/// preparation got and for which translator and driver, and the first estimate of the whole (from the files' sizes, or the record)
struct ConsolePrepFacts {
    bool census = false;              // (hookshot_shaders.bin beside the game, a census by its head)
    uint32_t census_programs = 0;
    bool record = false;              // (a progress file in the driver's cache folder)
    bool same = false;                // (made for this translator and this driver)
    bool finished = false;
    size_t done = 0, total = 0;       // (its items)
    int percent = 0;                  // (its work done)
    double planned_s = -1;            // (the whole preparation's first estimate, seconds of the clock)
    double left_s = -1;               // (what is left of it)
    unsigned workers = 0;
    std::string why;                  // (a line for the log)
};
ConsolePrepFacts console_prepare_quick();
/// Its progress, for the screen (a copy): phase 0 not started, 1 reading the census and the lists, 2 building, 3 the driver's
/// cache written out, 4 ended (end says how: console_prep::End)
struct ConsolePrepView {
    int phase = 0;
    int kind = 0;                     // (console_prep::Kind of the items being built)
    size_t done = 0, total = 0;       // (items)
    size_t pipelines = 0;             // (built by this start)
    int percent = 0;
    double left_s = -1, seconds = 0;  // (time left, and spent in all starts)
    int64_t free_mb = -1;
    unsigned busy = 0, workers = 0;
    int end = 0;                      // (console_prep::End)
    bool stopping = false;            // (asked to stop: the builds under way finish first)
    bool goes_on = false;             // (stopped for memory after some work: the next start goes on by itself)
};
/// Starts it on threads of its own (false: it could not: no census, or the program cache is open in this process already,
/// *why says; the title then has to start again first). force: HOOKSHOT_PS5_PREPARE=force, again from the first item
bool console_prepare_start(bool force, std::string* why);
ConsolePrepView console_prepare_view();
/// Asks it to stop where it is: the items under way finish, what is made is kept, the next start goes on from there
void console_prepare_stop();
/// Whether the program cache was opened in this process (the preparation must run before it is: it keeps nothing there)
bool program_cache_opened();

}  // namespace mkwvk
