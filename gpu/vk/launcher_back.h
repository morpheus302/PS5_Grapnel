// (ps5, lot 73) Back to the Grapnel screen from the game (its name since lot 80). The in-game menu's row of that name
// (vk_menu.cpp, the Gameplay tab) leaves a file beside the game, then the title asks the system to start it again
// (host/posix/ps5_relaunch.c and .h: sceSystemServiceLoadExec on /app0/eboot.bin); the launch screen of the new process
// (vk_launcher.cpp launcher_run) finds the file, erases it and shows, whatever its mode (launcher_logic.h show). Plain files
// only, no Vulkan or console call:
// tests/test_ps5_back_to_launcher.py compiles it on the PC.
//
// A trial switch for now: HOOKSHOT_PS5_BACK_TO_LAUNCHER=1 in hookshot_env.txt. Without it the row is not there and the file is
// never looked at: the title does what it did before.
//
// The file holds one line, for the log: who asked and when. The screen shows for the file being there, not for what it says;
// it is erased as it is found, so it counts for one start only. When the system refuses to start the title again, the title
// closes and the file stays: the player's next start from the home screen shows the screen, as asked.
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace mkwvk::launcher {

/// The file's name, beside the game (/data/grapnel on the console, lot 80), and the switch
inline constexpr const char* kBackMarker = "hookshot_back_to_launcher.txt";
inline constexpr const char* kBackSwitch = "HOOKSHOT_PS5_BACK_TO_LAUNCHER";

/// Whether it is offered (the menu's row there, the file looked for): on the console, its switch on (switch_on:
/// env_on(kBackSwitch)), and the screen not switched off for the measurement runs (launcher_env: HOOKSHOT_PS5_LAUNCHER, nullptr
/// when unset; "0" switches the screen off and nothing of it is read)
inline bool back_offered(bool console, bool switch_on, const char* launcher_env) {
    return console && switch_on && !(launcher_env && *launcher_env == '0');
}

/// Leaves the file at path with line in it (a newline added). False, and why, when it cannot be written whole
inline bool back_mark(const std::string& path, const char* line, std::string* why) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return *why = path + ": not written (" + std::strerror(errno) + ")", false;
    const size_t n = std::strlen(line);
    bool ok = std::fwrite(line, 1, n, f) == n && std::fputc('\n', f) != EOF;
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        std::remove(path.c_str());   // (a part of it: none rather than one that says nothing)
        return *why = path + ": not written whole", false;
    }
    return *why = path + ": written", true;
}

/// Whether the file is at path: when it is, its line is read (for the log, at most 200 bytes) and the file erased. *said: what
/// was found and done, for the log ("" when there is no file)
inline bool back_take(const std::string& path, std::string* said) {
    said->clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[201] = {};
    const size_t n = std::fread(line, 1, sizeof line - 1, f);
    std::fclose(f);
    line[n] = 0;
    for (char* p = line; *p; p++)
        if (*p == '\n' || *p == '\r') *p = 0;
    const bool gone = std::remove(path.c_str()) == 0;
    *said = path + " found (\"" + line + "\"), " +
            (gone ? std::string("erased") : std::string("NOT erased (") + std::strerror(errno) + "): the next start shows the screen too");
    return true;
}

}  // namespace mkwvk::launcher
