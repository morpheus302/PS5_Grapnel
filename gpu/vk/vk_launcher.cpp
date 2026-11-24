// (ps5, lot 69) The launch screen: on the TV right after the renderer is up and before the game's first instruction
// (vk_main.cpp started_up), drawn by Dear ImGui over frames that hold no picture of the game (present_image with none, as the
// start-up screen of a PC preparation is). Home (JOUER, RÉGLAGES, SHADERS, QUITTER; since lot 71 no countdown, the
// player starts the game, and no CONTINUER, which went through the title skip's automatic taps), Settings (the in-game menu's own rows that suit the console, on five tabs, saved at each change),
// Shaders (the caches beside the game, read without writing; the driver asked about 64 pipelines only when the player asks).
// Its logic and texts: launcher_logic.h; its file check: launcher_check.h. notes-agents/menu-lancement.md has the design.
//
// What it costs is let go before the game starts: the background picture (hookshot_menu_fond.rgba beside the game, made on
// the PC by scripts/ps5/menu_fond.py, never in the eboot: on the console in a block of direct memory of its own, given back),
// its GPU copy, and Dear ImGui altogether (vk_imgui.cpp shutdown: the game's first overlay makes it again, as a start without
// the screen does). Its frames are not the game's: present_image without a picture counts nothing the FPS counter, the frame
// trace, the cold units (host/rx_backend.c rx_ps5_presents) or the memory watch's frame count reads.
//
// HOOKSHOT_PS5_LAUNCHER=0 in hookshot_env.txt: not shown, nothing read (the measurement runs, as before the screen); =1: shown
// whatever the mode. (lot 73) With HOOKSHOT_PS5_BACK_TO_LAUNCHER=1, the in-game menu's Back to the Grapnel screen leaves a file
// beside the game and has the system start the title again: found here (and erased), it shows the screen whatever the mode.
// Off the console it shows only with RX_VK_LAUNCHER=1 (the PC's tests: its screenshots), and
// RX_VK_LAUNCHER_SCRIPT="<seconds>:<KEY>,..." (UP DOWN LEFT RIGHT OK BACK START L1 R1 TRI) presses for it.
// (lot 82) With HOOKSHOT_PS5_PREPARE=1 the Shaders page offers the console's own shader preparation (vk_draw.cpp console_prepare_*,
// its rules console_prep in console_prepare.h) when the census is beside the game: its page shows the step, the bar, the time left
// and the free memory, Later (circle, or Options) stops it, and at its end the title starts again with a heap of its own
// (host/posix/ps5_relaunch.c, as the in-game menu's Back to the Grapnel screen); after a stop for memory with some work done, the
// next start goes on by itself (hookshot_prepare_continue.txt beside the game). Unset: the screen as in lot 80.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "vk_launcher.h"
#include "vk_internal.h"
#include "vk_ui.h"
#include "vk_menu.h"
#include "vk_menu_keys.h"
#include "vk_paths.h"
#include "vk_shader.h"     // (program_cache_stamp: what the caches are good for)
#include "vk_framegen.h"   // (present_image)
#include "launcher_logic.h"
#include "launcher_check.h"
#include "launcher_back.h"   // (lot 73: the in-game menu's Back to the Grapnel screen, the file it leaves)

#ifdef __PROSPERO__
extern "C" {
void* hos_ps5_block(size_t size);   // (host/posix/ps5_platform.c: direct memory of its own, backed at once, given back)
void hos_ps5_block_free(void* p, size_t size);
int hos_ps5_memory_free(uint64_t* direct_mb, uint64_t* flexible_mb, uint64_t* largest_mb);
uint32_t hos_pad_raw_early(void);   // (host/nn_hid.c: the controllers before the game reads them)
int hos_ps5_back_to_launcher(void);   // (lot 82: host/posix/ps5_relaunch.c, the title started again by the system)
int hos_ps5_game_missing(char* buf, size_t cap);   // (lot 88: host/hos_main.c, the game file that cannot be read)
}
#endif

namespace mkwvk {
uint64_t driver_cache_bytes(std::string* where);   // (vk_draw.cpp: the driver's shader cache folders, in bytes)

namespace {
namespace L = launcher;
using clock_type = std::chrono::steady_clock;

#ifdef __PROSPERO__
constexpr bool kConsole = true;
#else
constexpr bool kConsole = false;
#endif

int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count(); }

// ---- what the screen shows: read by the drawing (the present's thread, the same as launcher_run's) ----
std::atomic<bool> showing{false};
L::State st;
L::ShaderFacts facts;
L::Check check;
// (lot 88) the game file cannot be read (host/hos_main.c hos_ps5_game_missing): the screen shows, says so, and nothing starts the
// game, which stopped later without a word
bool no_game = false;
char no_game_line[1200];
uint64_t stamp_now = 0;
L::Lang lang = L::FR;
std::string version;
float fade = 0;                // (0 to 1: the end, to black)
std::atomic<int64_t> drawn{0};  // (frames the screen drew: none in its first 3 s, it gives way to the game)
// the driver asked (the Shaders page's button): 0 not asked, 1 asking, 2 answered, 3 nothing to ask
std::atomic<int> driver_state{0};
std::atomic<int> driver_kept{0}, driver_asked{0};
std::atomic<int64_t> driver_ms{0};
CompileThread* driver_thread = nullptr;
// (lot 82) the console's own preparation: the switch, what the files say, what is offered, and its end
namespace CP = console_prep;
CP::Mode prep_mode = CP::Mode::kOff;
CP::Offer prep_offer = CP::Offer::kOff;
ConsolePrepFacts prep_facts;
ConsolePrepView prep_view;           // (the last read, for the drawing)
int64_t prep_ended_ns = 0;           // (when its end was seen: the page says it, then the title starts again)
bool prep_restart_asked = false;
bool prep_fresh_needed = false;      // (it could not begin in this process: the program cache was opened here)
constexpr const char* kPrepMarker = "hookshot_prepare_continue.txt";   // (beside the game: the next start goes on by itself)

// ---- the background: hookshot_menu_fond.rgba beside the game ("HKBG", version 1, width, height, then RGBA rows; version 2 adds
// the panel's place on the picture, x0 y0 x1 y1 in thousandths of its width and height, before the rows) ----
struct Picture {
    unsigned char* px = nullptr;
    size_t bytes = 0;
    int w = 0, h = 0;
    ImTextureData* tex = nullptr;
    bool panel = false;                   // (version 2: the panel where the picture says)
    float px0 = 0, py0 = 0, px1 = 0, py1 = 0;
};
Picture bg;
constexpr float kBannerAspect = 1920.0f / 620.0f;   // (the picture's shape; the panel keeps its place without it too)
std::string picture_path() { return game_folder_path() + "hookshot_menu_fond.rgba"; }
void picture_free() {
    if (!bg.px) return;
#ifdef __PROSPERO__
    hos_ps5_block_free(bg.px, bg.bytes);
#else
    free(bg.px);
#endif
    bg.px = nullptr;
}
/// The picture read whole into memory of its own (on the console: a block of direct memory, outside the flexible memory and
/// the host's heap, which would keep it); false, and why, when it is not there or not one
bool picture_load(std::string* why) {
    const std::string path = picture_path();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return *why = path + " not found: the plain background", false;
    uint32_t head[8] = {};
    bool ok = fread(head, 4, 4, f) == 4 && !memcmp(head, "HKBG", 4) && (head[1] == 1 || head[1] == 2) && head[2] >= 16 &&
              head[2] <= 4096 && head[3] >= 16 && head[3] <= 4096;
    // (version 2: the panel's place, thousandths, inside the picture and at least a tenth of it each way)
    const long at = ok && head[1] == 2 ? 32 : 16;
    if (ok && at == 32)
        ok = fread(head + 4, 4, 4, f) == 4 && head[4] < head[6] && head[6] <= 1000 && head[5] < head[7] && head[7] <= 1000 &&
             head[6] - head[4] >= 100 && head[7] - head[5] >= 100;
    const size_t bytes = ok ? (size_t)head[2] * head[3] * 4 : 0;
    if (ok) {
        fseek(f, 0, SEEK_END);
        ok = ftell(f) == (long)(at + bytes);
        fseek(f, at, SEEK_SET);
    }
    if (!ok) {
        fclose(f);
        return *why = path + " is not a picture of this screen (scripts/ps5/menu_fond.py makes one): the plain background", false;
    }
#ifdef __PROSPERO__
    bg.px = (unsigned char*)hos_ps5_block(bytes);
#else
    bg.px = (unsigned char*)malloc(bytes);
#endif
    bg.bytes = bytes;
    if (!bg.px) {
        fclose(f);
        return *why = "no memory for the picture: the plain background", false;
    }
    ok = fread(bg.px, 1, bytes, f) == bytes;
    fclose(f);
    if (!ok) {
        picture_free();
        return *why = path + " could not be read whole: the plain background", false;
    }
    bg.w = (int)head[2], bg.h = (int)head[3];
    bg.panel = at == 32;
    if (bg.panel) bg.px0 = head[4] / 1000.0f, bg.py0 = head[5] / 1000.0f, bg.px1 = head[6] / 1000.0f, bg.py1 = head[7] / 1000.0f;
    char b[200];
    char where[80] = "the panel in its usual place";
    if (bg.panel)
        snprintf(where, sizeof where, "the panel at %.1f-%.1f%% x %.1f-%.1f%% of it", bg.px0 * 100, bg.px1 * 100, bg.py0 * 100,
                 bg.py1 * 100);
    snprintf(b, sizeof b, "the background %dx%d (%.1f MiB, %s), %s", bg.w, bg.h, bytes / 1048576.0,
             kConsole ? "a block of direct memory of its own" : "the heap", where);
    *why = b;
    return true;
}

// ---- drawing helpers: the screen's own texts, drawn as they are (not through the in-game menu's translation) ----
ImVec2 measure(ImFont* f, float px, const char* t, float wrap = 0) { return f->CalcTextSizeA(px, FLT_MAX, wrap, t); }
float put(ImDrawList* d, ImFont* f, float px, ImVec2 p, ImU32 col, const char* t) {
    d->AddText(f, px, p, col, t);
    return measure(f, px, t).x;
}
/// Wrapped at wrap pixels, lines 1.25 em apart; the height (draw false: measured only)
float para(ImDrawList* d, ImFont* f, float px, ImVec2 p, ImU32 col, const char* t, float wrap, bool draw = true) {
    const char* end = t + strlen(t);
    float y = 0;
    while (t < end) {
        const char* cut = f->CalcWordWrapPosition(px, t, end, wrap);
        if (cut == t) {
            cut = t + 1;
            while (cut < end && ((unsigned char)*cut & 0xC0) == 0x80) cut++;
        }
        if (draw) d->AddText(f, px, ImVec2(p.x, p.y + y), col, t, cut);
        y += px * 1.25f;
        t = cut;
        while (t < end && *t == ' ') t++;
    }
    return y > 0 ? y - px * 0.25f : 0;
}
const char* T(L::Text id) { return L::text(id, lang); }
/// A number with its thousands apart, as the language writes them (4 566 / 4,566)
std::string grouped(unsigned long long n) {
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, lang == L::FR ? "\xC2\xA0" : ",");
    return s;
}
ImU32 verdict_colour(L::Verdict v) {
    return v == L::V_OK ? ui::rgba(126, 196, 110) : v == L::V_WARN ? ui::rgba(232, 168, 64) : ui::rgba(226, 94, 72);
}
void dot(ImDrawList* d, ImVec2 m, float r, L::Verdict v) {
    d->AddCircleFilled(m, r, verdict_colour(v), 16);
    d->AddCircle(m, r, ui::rgba(20, 14, 8, 200), 16, std::max(1.0f, r * 0.25f));
}

// ---- the layout: the picture across the screen's width, the panel where the player drew it over its tapestry ----
struct Layout {
    float bx, by, bw, bh;   // (the banner)
    float x0, y0, x1, y1;   // (the panel)
    float k;                // (pixels per point of the panel, 340 points high)
};
Layout layout_for(float w, float h) {
    Layout l{};
    const float aspect = bg.tex ? (float)bg.w / (float)bg.h : kBannerAspect;
    l.bw = w, l.bh = w / aspect;
    if (l.bh > h) l.bh = h, l.bw = h * aspect;
    l.bx = (w - l.bw) * 0.5f, l.by = (h - l.bh) * 0.5f;
    // (the rectangle drawn on the picture: 2.0% to 27.1% of its width, 8% to 91% of its height; a version 2 picture says its own)
    const bool own = bg.tex && bg.panel;
    const float fx0 = own ? bg.px0 : 0.020f, fx1 = own ? bg.px1 : 0.271f, fy0 = own ? bg.py0 : 0.08f, fy1 = own ? bg.py1 : 0.91f;
    l.x0 = std::floor(l.bx + fx0 * l.bw), l.x1 = std::floor(l.bx + fx1 * l.bw);
    l.y0 = std::floor(l.by + fy0 * l.bh), l.y1 = std::floor(l.by + fy1 * l.bh);
    l.k = (l.y1 - l.y0) / 340.0f;
    return l;
}

void draw_background(ImDrawList* d, float w, float h, const Layout& l) {
    d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), ui::rgba(0, 0, 0));
    if (bg.tex) {
        // (scaled by the GPU's linear filter: nothing sharpened)
        d->AddImage(bg.tex->GetTexRef(), ImVec2(l.bx, l.by), ImVec2(l.bx + l.bw, l.by + l.bh));
        // (its edges into the black: top and bottom, and a little at the left)
        const float fy = l.bh * 0.07f, fx = l.bw * 0.025f;
        const ImU32 black = ui::rgba(0, 0, 0, 255), clear = ui::rgba(0, 0, 0, 0);
        d->AddRectFilledMultiColor(ImVec2(l.bx, l.by), ImVec2(l.bx + l.bw, l.by + fy), black, black, clear, clear);
        d->AddRectFilledMultiColor(ImVec2(l.bx, l.by + l.bh - fy), ImVec2(l.bx + l.bw, l.by + l.bh), clear, clear, black, black);
        d->AddRectFilledMultiColor(ImVec2(l.bx, l.by), ImVec2(l.bx + fx, l.by + l.bh), black, clear, clear, black);
        return;
    }
    // (no picture: the start-up screen's warm dark, and its soft glow where the picture's figures would be)
    d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), ui::rgba(14, 10, 6));
    const ImVec2 m(l.bx + l.bw * 0.62f, l.by + l.bh * 0.5f);
    for (int i = 40; i >= 1; i--) d->AddCircleFilled(m, h * 0.021f * i, ui::rgba(46, 33, 18, 4), 96);
}

void draw_panel(ImDrawList* d, const Layout& l) {
    const ImVec2 a(l.x0, l.y0), b(l.x1, l.y1);
    d->AddRectFilledMultiColor(a, b, ui::rgba(18, 12, 7, 214), ui::rgba(18, 12, 7, 214), ui::rgba(10, 7, 4, 226),
                               ui::rgba(10, 7, 4, 226));
    d->AddRect(a, b, ui::rgba(150, 118, 70, 170), 0, std::max(1.0f, 1.2f * l.k));
    const float in = 4 * l.k;
    d->AddRect(ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), ui::rgba(150, 118, 70, 60), 0, std::max(1.0f, 0.8f * l.k));
}

/// The prompts along the panel's foot: label then button, as the game's own; their row's top
struct Prompt {
    int kind;   // (0 confirm, 1 back, 2 Options, 3 triangle, 4 the shoulders)
    const char* what;
};
float draw_prompts(ui::Ctx& c, const Layout& l, const Prompt* list, int n) {
    const float k = l.k, px = 10.5f * k, gap = 9 * k;
    ui::Ctx g = c;
    g.s = 0.56f * k;   // (the glyphs' own scale: px + 10 points high)
    g.pad = true;
    const float hh = px + 10 * g.s, top = l.y1 - 9 * k - hh;
    float total = 0;
    const auto glyph_of = [&](const Prompt& p, ui::Ctx& gc) {
        gc.confirm_dot = c.confirm_dot, gc.back_dot = c.back_dot;
        if (p.kind == 3) gc.confirm_dot = 0;   // (the top face button: drawn as a confirm whose dot is the top one)
        const ui::Prompt pr = p.kind == 0 || p.kind == 3 ? ui::Prompt{ui::kConfirm, ""} : p.kind == 1 ? ui::Prompt{ui::kBack, ""}
                              : p.kind == 2 ? ui::Prompt{ui::kKey, "", "Options"} : ui::Prompt{ui::kTabs, ""};
        return pr;
    };
    for (int i = 0; i < n; i++) {
        ui::Ctx gc = g;
        total += measure(c.f.body, px, list[i].what).x + 4 * k + ui::glyph(gc, px, ImVec2(0, 0), glyph_of(list[i], gc), false);
        if (i + 1 < n) total += gap;
    }
    const float room = l.x1 - l.x0 - 16 * k;
    float scale = total > room ? room / total : 1.0f;
    float x = l.x0 + 8 * k + std::max(0.0f, (room - total * scale) * 0.5f);
    c.d->AddLine(ImVec2(l.x0 + 8 * k, top - 6 * k), ImVec2(l.x1 - 8 * k, top - 6 * k), ui::rgba(150, 118, 70, 70), std::max(1.0f, 0.8f * k));
    for (int i = 0; i < n; i++) {
        ui::Ctx gc = g;
        gc.s *= scale;
        const float p = px * scale, h2 = p + 10 * gc.s;
        const float y = top + (hh - h2) * 0.5f;
        c.d->AddText(c.f.body, p, ImVec2(x, ui::mid_top(c.f.body, p, y + h2 * 0.5f)), ui::kInk, list[i].what);
        x += measure(c.f.body, p, list[i].what).x + 4 * k * scale;
        x += ui::glyph(gc, p, ImVec2(x, y), glyph_of(list[i], gc), true) + gap * scale;
    }
    return top - 6 * k;
}

/// The title at the panel's head: GRAPNEL (lot 80: the port's public name), the flourish, the version; the y under it
float draw_title(ui::Ctx& c, const Layout& l, const char* page) {
    const float k = l.k, cx = (l.x0 + l.x1) * 0.5f;
    float y = l.y0 + 12 * k;
    if (!page) {
        ui::spaced(c, c.f.heading, 27 * k, ImVec2(cx, y), ui::rgba(246, 232, 204), "GRAPNEL", 5 * k);
        y += 33 * k;
        ui::flourish(c, l.x0 + 26 * k, l.x1 - 26 * k, y, ui::kAccent);
        y += 8 * k;
        const std::string v = version + "  \xC2\xB7  " + T(L::T_PLATFORM);
        const float vw = measure(c.f.body, 9.5f * k, v.c_str()).x;
        put(c.d, c.f.body, 9.5f * k, ImVec2(cx - vw * 0.5f, y), ui::kDim, v.c_str());
        return y + 18 * k;
    }
    ui::spaced(c, c.f.heading, 17 * k, ImVec2(cx, y), ui::rgba(246, 232, 204), page, 3 * k);
    y += 23 * k;
    ui::flourish(c, l.x0 + 26 * k, l.x1 - 26 * k, y, ui::kAccent);
    return y + 9 * k;
}

/// A button of the home page: gold when chosen, as the game's pause menu draws its own
void home_button(ui::Ctx& c, ImVec2 a, ImVec2 b, const char* label, bool sel, float k) {
    if (sel) {
        ui::gold(c, a, b);
        c.d->AddRect(a + ImVec2(3 * k, 3 * k), b - ImVec2(3 * k, 3 * k), ui::rgba(36, 24, 8, 60), 0, std::max(1.0f, 0.6f * k));
    } else {
        c.d->AddRectFilled(a, b, ui::rgba(60, 47, 32, 150));
        c.d->AddRect(a, b, ui::rgba(150, 118, 70, 110), 0, std::max(1.0f, 0.7f * k));
    }
    ImFont* f = sel ? c.f.strong : c.f.body;
    const float px = 14 * k;
    c.d->AddText(f, px, ImVec2(a.x + 12 * k, ui::mid_top(f, px, (a.y + b.y) * 0.5f)), sel ? ui::kGoldInk : ui::kInk, label);
}

void draw_home(ui::Ctx& c, const Layout& l) {
    const float k = l.k;
    float y = draw_title(c, l, nullptr);
    const float x0 = l.x0 + 14 * k, x1 = l.x1 - 14 * k, bh = 25 * k, gap = 5 * k;
    for (int i = 0; i < L::H_N; i++) {
        const bool sel = st.home == i;
        const ImVec2 a(x0, y), b(x1, y + bh);
        home_button(c, a, b, T(L::kHomeNames[i]), sel, k);
        if (i == L::H_SHADERS) {   // (the check's state, at the button's right; lot 82: the preparation's when it is to do)
            const char* shortw = T(L::shaders_word(check.verdict, prep_offer));
            const float px = 10.5f * k, tw = measure(c.f.body, px, shortw).x;
            const float tx = b.x - 10 * k - tw;
            c.d->AddText(c.f.body, px, ImVec2(tx, ui::mid_top(c.f.body, px, (a.y + b.y) * 0.5f)), sel ? ui::kGoldInk : ui::kSoft, shortw);
            dot(c.d, ImVec2(tx - 8 * k, (a.y + b.y) * 0.5f), 3.6f * k, check.verdict);
        }
        if (sel) ui::glow(c, a, b);
        y += bh + gap;
    }
    y += 4 * k;
    const float wrap = x1 - x0;
    const Prompt q[3] = {{0, T(L::T_PROMPT_CONFIRM)}, {2, T(L::T_PROMPT_PLAY_NOW)}, {3, T(L::T_PROMPT_SETTINGS)}};
    float foot = draw_prompts(c, l, q, 3) - 3 * k;
    {   // (lot 79: the in-game menu's shortcut, just above the prompts; the lines below keep above it)
        const char* hint = T(L::T_MENU_SHORTCUT);
        const float hint_h = para(c.d, c.f.body, 9.8f * k, ImVec2(0, 0), 0, hint, wrap, false);
        para(c.d, c.f.body, 9.8f * k, ImVec2(x0, foot - hint_h), ui::kSoft, hint, wrap);
        foot -= hint_h + 6 * k;
    }
    // (under the buttons, each where it fits above the prompts: the chosen button's line (QUIT armed: the second press's), a
    // problem's reason, the countdown and its bar)
    const auto said = [&](float px, ImU32 col, const char* t, float after) {
        const float h = para(c.d, c.f.body, px, ImVec2(x0, y), col, t, wrap, false);
        if (y + h > foot) return false;
        para(c.d, c.f.body, px, ImVec2(x0, y), col, t, wrap);
        y += h + after;
        return true;
    };
    const int left = L::countdown_left(st, now_ns());
    char line[200];
    if (left > 0) snprintf(line, sizeof line, T(L::T_COUNTDOWN), left);
    const float countdown_h = left > 0 ? para(c.d, c.f.body, 10.5f * k, ImVec2(0, 0), 0, line, wrap, false) + 8 * k : 0;
    const char* help = st.quit_armed && st.home == L::H_QUIT ? T(L::T_QUIT_AGAIN)
                       : st.home == L::H_SHADERS && CP::button(prep_offer) ? T(L::T_HELP_SHADERS_PREP) : T(L::kHomeHelp[st.home]);
    {
        const float room_foot = foot - countdown_h;   // (the countdown keeps its place at the foot)
        const auto said_above = [&](float px, ImU32 col, const char* t, float after) {
            const float h = para(c.d, c.f.body, px, ImVec2(x0, y), col, t, wrap, false);
            if (y + h > room_foot) return false;
            para(c.d, c.f.body, px, ImVec2(x0, y), col, t, wrap);
            y += h + after;
            return true;
        };
        if (st.no_game) {   // (lot 88: the game file cannot be read: said above all, in the problem's colour)
            char said_no_game[1600];
            snprintf(said_no_game, sizeof said_no_game, T(L::T_NO_GAME), no_game_line);
            said_above(10.5f * k, verdict_colour(L::V_FAIL), said_no_game, 7 * k);
        }
        said_above(10.5f * k, st.quit_armed ? ui::rgba(246, 200, 120) : ui::kSoft, help, 7 * k);
        // (lot 82: a preparation to do or to resume is the answer to the check's problem: the help above says it, not the problem)
        if (check.verdict != L::V_OK && !CP::asks_for_screen(prep_offer))
            said_above(9.8f * k, verdict_colour(check.verdict), T(L::why_text(check.why)), 6 * k);
    }
    if (left > 0) {
        said(10.5f * k, ui::kAccent, line, 4 * k);
        // (a thin bar that empties with it)
        const double gone = (double)(now_ns() - st.start_ns) / (L::kCountdownSeconds * (double)L::kSecond);
        const float f = (float)std::clamp(1.0 - gone, 0.0, 1.0);
        c.d->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + 3 * k), ui::rgba(60, 47, 32, 180));
        c.d->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * f, y + 3 * k), ui::kGold);
    } else if (check.verdict == L::V_FAIL && left < 0 && st.home == L::H_SHADERS && !st.quit_armed) {
        said(9.4f * k, ui::kDim, T(L::T_COUNTDOWN_NONE), 0);
    }
}

/// The values the settings page shows, read from the rows each frame it is drawn (menu_setting_get)
struct Shown {
    int value = 0;
    bool known = false;
    const char* held = nullptr;   // (the development switch that holds it this run)
};
Shown shown_of(const L::Setting& s) {
    Shown v;
    if (!strcmp(s.key, L::kLaunchModeKey)) return v.value = menu_launcher_mode(), v.known = true, v;
    MkwVkSetting r{};
    if (menu_setting_get(s.key, &r)) v.value = r.value, v.known = true;
    v.held = L::held_by(s, [](const char* e) { return getenv(e); });
    return v;
}

void draw_settings(ui::Ctx& c, const Layout& l) {
    const float k = l.k;
    float y = draw_title(c, l, T(L::T_SETTINGS));
    const float x0 = l.x0 + 12 * k, x1 = l.x1 - 12 * k, cx = (l.x0 + l.x1) * 0.5f;
    {   // (the tabs: the chosen one's name between the shoulder buttons, a dot for each)
        const float px = 12.5f * k;
        const char* name = T(L::kTabNames[st.tab]);
        const float tw = measure(c.f.strong, px, name).x;
        c.d->AddText(c.f.strong, px, ImVec2(cx - tw * 0.5f, y), ui::kGoldHi, name);
        ui::Ctx g = c;
        g.s = 0.5f * k, g.pad = true;
        const float gh = 9.5f * k + 10 * g.s, gy = y + (px - gh) * 0.5f + 1 * k;
        ui::Ctx gl = g;
        // (L at the left, R at the right: the shoulders' glyph is the pair; drawn as two halves)
        const float cap_w = gh * 1.25f;
        c.d->AddRectFilled(ImVec2(x0, gy + 2 * g.s), ImVec2(x0 + cap_w, gy + gh - 2 * g.s), ui::kInk, (gh - 4 * g.s) * 0.5f);
        ui::text_mid_centre(gl, c.f.strong, 9.5f * k * 0.8f, x0 + cap_w * 0.5f, gy + gh * 0.5f, ui::rgba(40, 30, 20), "L1");
        c.d->AddRectFilled(ImVec2(x1 - cap_w, gy + 2 * g.s), ImVec2(x1, gy + gh - 2 * g.s), ui::kInk, (gh - 4 * g.s) * 0.5f);
        ui::text_mid_centre(gl, c.f.strong, 9.5f * k * 0.8f, x1 - cap_w * 0.5f, gy + gh * 0.5f, ui::rgba(40, 30, 20), "R1");
        y += px + 6 * k;
        for (int t = 0; t < L::TAB_N; t++)
            c.d->AddCircleFilled(ImVec2(cx + (t - 2) * 9 * k, y), 2.2f * k, t == st.tab ? ui::kGoldHi : ui::rgba(150, 118, 70, 120), 12);
        y += 7 * k;
    }
    // (the help box at the foot, above the prompts: as high as the tab's longest help, so the rows above keep their place)
    const Prompt q[4] = {{0, T(L::T_PROMPT_CHANGE)}, {1, T(L::T_PROMPT_BACK)}, {4, T(L::T_PROMPT_TABS)}, {3, T(L::T_PROMPT_DEFAULT)}};
    const float prompts_top = draw_prompts(c, l, q, 4);
    const float hpx = 9.5f * k, wrap = x1 - x0;
    float help_h = 0;
    const int n = L::tab_count(st.tab);
    for (int i = 0; i < n; i++) {
        const L::Setting& s = L::kSettings[L::tab_setting(st.tab, i)];
        char held[200];
        snprintf(held, sizeof held, T(L::T_HELD_BY_ENV), s.env[0] ? s.env[0] : "");
        help_h = std::max(help_h, para(c.d, c.f.body, hpx, ImVec2(0, 0), 0, T(s.help), wrap, false) + 4 * k +
                                      (s.env[0] ? para(c.d, c.f.body, hpx, ImVec2(0, 0), 0, held, wrap, false) + 3 * k : 0));
    }
    const float saved_h = 9 * k * 1.25f;
    const float help_top = prompts_top - 6 * k - saved_h - help_h - 6 * k;
    // (the rows, scrolled to keep the chosen one in view)
    const float rh = 21 * k, rows_top = y, rows_bottom = help_top - 4 * k;
    const int fit = std::max(1, (int)((rows_bottom - rows_top) / rh));
    const int sel = st.row[st.tab];
    int first = std::clamp(sel - fit / 2, 0, std::max(0, n - fit));
    char vb[96];
    for (int i = first; i < n && i < first + fit; i++) {
        const L::Setting& s = L::kSettings[L::tab_setting(st.tab, i)];
        const Shown v = shown_of(s);
        const bool chosen = i == sel;
        const ImVec2 a(x0 - 4 * k, rows_top + (i - first) * rh), b(x1 + 4 * k, a.y + rh - 2 * k);
        if (chosen) {
            if (v.held) c.d->AddRectFilled(a, b, ui::rgba(92, 70, 42, 200));
            else ui::gold(c, a, b);
        }
        const float px = 11.2f * k, my = (a.y + b.y) * 0.5f;
        ImFont* f = chosen ? c.f.strong : c.f.body;
        const ImU32 ink = v.held ? ui::kDim : chosen ? ui::kGoldInk : ui::kInk;
        c.d->AddText(f, px, ImVec2(x0, ui::mid_top(f, px, my)), ink, T(s.name));
        const char* value = v.known ? L::value_text(s, v.value, lang, vb, sizeof vb) : "-";
        // (the value at the right, smaller when it would run into the name: never over it)
        const float arrow = 5.5f * k, room = x1 - arrow - 4 * k - 6 * k - (x0 + measure(f, px, T(s.name)).x + 8 * k);
        const float vpx = ui::fit_px(px, 7.5f * k, 0.25f * k, room, [&](float q) { return measure(c.f.strong, q, value).x; });
        const float use = vpx > 0 ? vpx : 7.5f * k;
        const float vw = measure(c.f.strong, use, value).x;
        const float vx = x1 - arrow - 4 * k - vw;
        c.d->AddText(c.f.strong, use, ImVec2(vx, ui::mid_top(c.f.strong, use, my)), ink, value);
        if (!v.held) {
            ui::triangle(c, ImVec2(vx - 6 * k, my), arrow, 0, chosen ? ui::kGoldInk : ui::kAccent);
            ui::triangle(c, ImVec2(x1 - arrow * 0.5f, my), arrow, 1, chosen ? ui::kGoldInk : ui::kAccent);
        }
        if (chosen && !v.held) ui::glow(c, a, b);
    }
    if (first > 0) ui::triangle(c, ImVec2(cx, rows_top - 3 * k), 5 * k, 2, ui::kAccent);
    if (first + fit < n) ui::triangle(c, ImVec2(cx, rows_bottom), 5 * k, 3, ui::kAccent);
    // (the chosen row's help, and what holds it)
    c.d->AddLine(ImVec2(x0, help_top - 3 * k), ImVec2(x1, help_top - 3 * k), ui::rgba(150, 118, 70, 70), std::max(1.0f, 0.8f * k));
    float hy = help_top + 2 * k;
    if (const int idx = L::tab_setting(st.tab, sel); idx >= 0) {
        const L::Setting& s = L::kSettings[idx];
        hy += para(c.d, c.f.body, hpx, ImVec2(x0, hy), ui::kSoft, T(s.help), wrap) + 4 * k;
        if (const char* e = shown_of(s).held) {
            char held[200];
            snprintf(held, sizeof held, T(L::T_HELD_BY_ENV), e);
            para(c.d, c.f.body, hpx, ImVec2(x0, hy), verdict_colour(L::V_WARN), held, wrap);
        }
    }
    put(c.d, c.f.body, 9 * k, ImVec2(x0, prompts_top - 6 * k - saved_h), ui::kDim, T(L::T_SAVED_EACH_CHANGE));
}

void draw_shaders(ui::Ctx& c, const Layout& l) {
    const float k = l.k;
    float y = draw_title(c, l, T(L::T_SHADERS));
    const float x0 = l.x0 + 12 * k, x1 = l.x1 - 12 * k, wrap = x1 - x0;
    {   // (the verdict)
        const char* word = T(check.verdict == L::V_OK ? L::T_READY : check.verdict == L::V_WARN ? L::T_TO_COMPLETE : L::T_PROBLEM);
        const float px = 11.5f * k;
        dot(c.d, ImVec2(x0 + 4 * k, y + px * 0.55f), 4 * k, check.verdict);
        put(c.d, c.f.strong, px, ImVec2(x0 + 13 * k, y), verdict_colour(check.verdict), word);
        y += px + 8 * k;
    }
    // (a line a fact: its name at the left, its value at the right, a dot for its state)
    char b[200];
    const float px = 10 * k, pitch = 15.5f * k;
    const auto line = [&](L::Text label, const char* value, L::Verdict v, bool show_dot = true) {
        if (show_dot) dot(c.d, ImVec2(x0 + 3 * k, y + px * 0.6f), 2.8f * k, v);
        put(c.d, c.f.body, px, ImVec2(x0 + 10 * k, y), ui::kSoft, T(label));
        const float vw = measure(c.f.strong, px, value).x;
        put(c.d, c.f.strong, px, ImVec2(x1 - vw, y), ui::kInk, value);
        y += pitch;
    };
    if (!facts.programs_present || !facts.programs_magic) snprintf(b, sizeof b, "%s", T(L::T_SH_NONE_YET));
    else {
        char hex[24];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)facts.programs_stamp);
        snprintf(b, sizeof b, T(facts.programs_stamp == stamp_now ? L::T_SH_VERSION_OK : L::T_SH_VERSION_OTHER), hex);
    }
    line(L::T_SH_VERSION, b, facts.programs_present && facts.programs_magic ? check.version : check.programs);
    if (facts.programs)
        snprintf(b, sizeof b, T(L::T_SH_PROGRAMS_N), grouped(facts.programs).c_str(), grouped(facts.programs_bytes >> 20).c_str());
    else snprintf(b, sizeof b, "%s", T(L::T_SH_NONE_YET));
    line(L::T_SH_PROGRAMS, b, check.programs);
    if (facts.notes_ok && facts.notes) snprintf(b, sizeof b, T(L::T_SH_NOTES_N), grouped(facts.notes).c_str(), check.coverage);
    else snprintf(b, sizeof b, "%s", T(L::T_SH_NONE_F));
    line(L::T_SH_NOTES, b, check.notes);
    if (facts.states_ok) snprintf(b, sizeof b, T(L::T_SH_STATES_N), grouped(facts.states).c_str());
    else snprintf(b, sizeof b, "%s", T(L::T_SH_NONE_F));
    line(L::T_SH_STATES, b, facts.states_ok ? L::V_OK : L::V_WARN);
    if (facts.driver_bytes) snprintf(b, sizeof b, T(L::T_SH_DRIVER_MB), grouped(facts.driver_bytes >> 20).c_str());
    else snprintf(b, sizeof b, "%s", T(L::T_SH_DRIVER_EMPTY));
    line(L::T_SH_DRIVER, b, check.driver);
    {
        const int ds = driver_state.load();
        L::Verdict v = L::V_WARN;
        if (ds == 0) snprintf(b, sizeof b, "%s", T(L::T_SH_CHECK_NOT_ASKED));
        else if (ds == 1) snprintf(b, sizeof b, "%s", T(L::T_SH_CHECK_RUNNING));
        else if (ds == 3) snprintf(b, sizeof b, "%s", T(L::T_SH_CHECK_IMPOSSIBLE));
        else {
            snprintf(b, sizeof b, T(L::T_SH_CHECK_RESULT), driver_kept.load(), driver_asked.load());
            v = driver_asked.load() && driver_kept.load() == driver_asked.load() ? L::V_OK : L::V_WARN;
        }
        line(L::T_SH_DRIVER_CHECK, b, v, ds == 2);
    }
    if (facts.skipped) {
        snprintf(b, sizeof b, T(L::T_SH_SKIPPED_N), grouped(facts.skipped).c_str());
        line(L::T_SH_SKIPPED, b, L::V_OK, false);
    }
    if (prep_offer != CP::Offer::kOff) {   // (lot 82: the console's own preparation)
        L::Verdict v = L::V_WARN;
        if (prep_offer == CP::Offer::kDone) snprintf(b, sizeof b, "%s", T(L::T_SH_PREP_DONE)), v = L::V_OK;
        else if (prep_offer == CP::Offer::kNoCensus) snprintf(b, sizeof b, "%s", T(L::T_SH_PREP_NO_CENSUS)), v = L::V_FAIL;
        else if (prep_offer == CP::Offer::kResume) snprintf(b, sizeof b, T(L::T_SH_PREP_BEGUN), prep_facts.percent);
        else snprintf(b, sizeof b, "%s", T(L::T_SH_PREP_NONE)), v = prep_offer == CP::Offer::kOptional ? L::V_OK : L::V_WARN;
        line(L::T_SH_PREP, b, v);
    }
    y += 4 * k;
    // (why, in words; what the driver said when it was asked. Lot 82: a preparation to do or to resume says it in its own words
    // below, in the check's place: the page has room for one of them above its two buttons)
    const float hpx = 9.6f * k;
    if (prep_offer != CP::Offer::kNeeded && prep_offer != CP::Offer::kResume)
        y += para(c.d, c.f.body, hpx, ImVec2(x0, y), check.verdict == L::V_OK ? ui::kSoft : verdict_colour(check.verdict),
                  T(L::why_text(check.why)), wrap) + 4 * k;
    if (driver_state.load() == 2 && driver_kept.load() < driver_asked.load())
        y += para(c.d, c.f.body, hpx, ImVec2(x0, y), verdict_colour(L::V_WARN), T(L::T_SH_WHY_PARTIAL), wrap) + 4 * k;
    if (prep_offer != CP::Offer::kOff && prep_offer != CP::Offer::kDone)   // (lot 82: what a preparation would do, or why none)
        y += para(c.d, c.f.body, hpx, ImVec2(x0, y), prep_offer == CP::Offer::kNoCensus ? verdict_colour(L::V_FAIL) : ui::kSoft,
                  T(L::prep_reason(prep_offer)), wrap) + 4 * k;
    // (the buttons: Ask the driver; lot 82: Prepare above it when offered, the chosen one gold)
    const auto button = [&](const char* label, bool chosen, bool busy) {
        const float bh = 21 * k;
        const ImVec2 a(x0, y + 3 * k), e(x1, y + 3 * k + bh);
        if (busy) c.d->AddRectFilled(a, e, ui::rgba(92, 70, 42, 200));
        else if (chosen) ui::gold(c, a, e);
        else c.d->AddRectFilled(a, e, ui::rgba(60, 47, 32, 150)), c.d->AddRect(a, e, ui::rgba(150, 118, 70, 110), 0, std::max(1.0f, 0.7f * k));
        const float bpx = 10.8f * k;
        const float tw = measure(c.f.strong, bpx, label).x;
        const float fpx = tw > (x1 - x0 - 10 * k) ? bpx * (x1 - x0 - 10 * k) / tw : bpx;
        const float fw = measure(c.f.strong, fpx, label).x;
        c.d->AddText(c.f.strong, fpx, ImVec2((x0 + x1) * 0.5f - fw * 0.5f, ui::mid_top(c.f.strong, fpx, (a.y + e.y) * 0.5f)),
                     busy ? ui::kDim : chosen ? ui::kGoldInk : ui::kInk, label);
        if (!busy && chosen) ui::glow(c, a, e);
        y = e.y + 4 * k;
    };
    if (st.prep_button) {
        const std::string when = CP::duration_text(prep_offer == CP::Offer::kResume ? prep_facts.left_s : prep_facts.planned_s);
        snprintf(b, sizeof b, T(prep_offer == CP::Offer::kResume ? L::T_SH_PREP_RESUME_BUTTON
                                : prep_offer == CP::Offer::kOptional ? L::T_SH_PREP_AGAIN_BUTTON : L::T_SH_PREP_BUTTON), when.c_str());
        button(b, st.shaders_sel == 0, false);
    }
    button(T(L::T_SH_ASK_DRIVER), !st.prep_button || st.shaders_sel == 1, driver_state.load() == 1);
    y += 3 * k;
    const Prompt q[3] = {{0, T(L::T_PROMPT_CONFIRM)}, {1, T(L::T_PROMPT_BACK)}, {2, T(L::T_PROMPT_PLAY_NOW)}};
    const float foot = draw_prompts(c, l, q, 3) - 4 * k;
    // (how it was checked, and that nothing is built here: where there is room above the prompts)
    snprintf(b, sizeof b, T(L::T_SH_CHECKED_IN), facts.seconds);
    if (lang == L::FR)
        for (char* p = b; *p; p++)
            if (*p == '.' && p[1] >= '0' && p[1] <= '9') *p = ',';
    std::string small = std::string(b) + " " + T(prep_mode != CP::Mode::kOff ? L::T_SH_PREP_NOTE : L::T_SH_NOTHING_BUILT);
    const float spx = 8.8f * k;
    if (y + para(c.d, c.f.body, spx, ImVec2(x0, y), 0, small.c_str(), wrap, false) <= foot)
        para(c.d, c.f.body, spx, ImVec2(x0, y), ui::kDim, small.c_str(), wrap);
}

/// (lot 82) The preparation under way: the step, the bar, how far, the time left, the free memory; Later
void draw_preparing(ui::Ctx& c, const Layout& l) {
    const float k = l.k;
    float y = draw_title(c, l, T(L::T_PREPARING));
    const float x0 = l.x0 + 12 * k, x1 = l.x1 - 12 * k, wrap = x1 - x0;
    const ConsolePrepView& v = prep_view;
    char b[200];
    {   // (what it is doing)
        const L::Text what = v.phase <= 1 ? L::T_PREP_READING : v.phase == 3 ? L::T_PREP_SETTLING
                             : v.phase == 4 ? (prep_fresh_needed ? L::T_PREP_CANNOT_START : L::prep_end_text((CP::End)v.end))
                             : v.kind == CP::kMesh ? L::T_PREP_STEP_MESH : v.kind == CP::kCompute ? L::T_PREP_STEP_COMPUTE
                                                                                                 : L::T_PREP_STEP_PIPELINES;
        const float px = 11.5f * k;
        y += para(c.d, c.f.strong, px, ImVec2(x0, y), v.phase == 4 && v.end != (int)CP::End::kFinished ? verdict_colour(L::V_WARN)
                                                                                                         : ui::kInk, T(what), wrap) + 9 * k;
    }
    {   // (the bar, and its percent at its right)
        const float bh = 7 * k;
        c.d->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + bh), ui::rgba(60, 47, 32, 180));
        const float f = std::clamp(v.percent / 100.0f, 0.0f, 1.0f);
        c.d->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (x1 - x0) * f, y + bh), ui::kGold);
        c.d->AddRect(ImVec2(x0, y), ImVec2(x1, y + bh), ui::rgba(150, 118, 70, 140), 0, std::max(1.0f, 0.7f * k));
        y += bh + 6 * k;
        snprintf(b, sizeof b, "%d %%", v.percent);
        if (lang == L::EN) snprintf(b, sizeof b, "%d%%", v.percent);
        const float px = 10 * k;
        put(c.d, c.f.strong, px, ImVec2(x1 - measure(c.f.strong, px, b).x, y), ui::kInk, b);
        if (v.total) {
            snprintf(b, sizeof b, T(L::T_PREP_COUNT), grouped(v.done).c_str(), grouped(v.total).c_str());
            put(c.d, c.f.body, px, ImVec2(x0, y), ui::kSoft, b);
        }
        y += px + 8 * k;
    }
    const float px = 10 * k;
    if (v.phase < 4) {
        if (v.left_s >= 0) snprintf(b, sizeof b, T(L::T_PREP_LEFT), CP::duration_text(v.left_s).c_str());
        else snprintf(b, sizeof b, "%s", T(L::T_PREP_LEFT_UNKNOWN));
        put(c.d, c.f.body, px, ImVec2(x0, y), ui::kInk, b);
        y += px * 1.5f;
    }
    if (v.free_mb >= 0) {
        snprintf(b, sizeof b, T(L::T_PREP_MEMORY), grouped((unsigned long long)v.free_mb).c_str());
        put(c.d, c.f.body, px, ImVec2(x0, y), ui::kSoft, b);
        y += px * 1.5f;
    }
    y += 6 * k;
    const char* help = v.phase == 4 ? T(L::T_PREP_RESTARTING) : v.stopping ? T(L::T_PREP_STOPPING) : T(L::T_PREP_HELP);
    y += para(c.d, c.f.body, 9.6f * k, ImVec2(x0, y), v.stopping || v.phase == 4 ? ui::kAccent : ui::kSoft, help, wrap);
    if (v.phase < 4 && !v.stopping) {
        const Prompt q[1] = {{1, T(L::T_PREP_LATER)}};
        draw_prompts(c, l, q, 1);
    }
}

// ---- the controllers (and on the PC the keys, and RX_VK_LAUNCHER_SCRIPT) ----
struct Reader {
    uint32_t last = ~0u;   // (the first read takes what is held already as held: not a press)
    int64_t since[4] = {}, next[4] = {};   // (up, down, left, right held: their repeat)
    int confirm = menu_keys::kEast, back = menu_keys::kSouth;
    const char* script = nullptr;
    int64_t script_t0 = 0;
    bool seen = false;     // (lot 71: an input from the controllers seen, said once in the log)
};
Reader rd;
bool key_press(int vk) {
    if (host && host->take_key_press) return host->take_key_press(vk) != 0;
    return false;
}
/// The buttons that confirm and go back: those the game's A and B are on (the Button layout row): Nintendo, circle and cross
void read_layout() {
    if (host && host->binding)
        menu_keys::confirm_back(host->binding(HOS_BIND_PAD, HOS_SW_A, 0), host->binding(HOS_BIND_PAD, HOS_SW_B, 0), &rd.confirm,
                                &rd.back);
}
/// The controllers now, 1 << HOS_PAD_* (on the console read by the screen itself: the game has not read them yet; elsewhere
/// the host's last read, the keys being the PC's way through the screen)
uint32_t raw_now() {
#ifdef __PROSPERO__
    return hos_pad_raw_early();
#else
    return host && host->pad_raw ? host->pad_raw() : 0;
#endif
}
L::Input read_input(int64_t now) {
    L::Input in;
    const uint32_t raw = raw_now();
    if (raw && !rd.seen) {   // (lot 71: the controllers read before the game, or not: the log says)
        rd.seen = true;
        logf("launcher: the controllers' first input after %.1f s on the screen (0x%x)", (now - rd.script_t0) / 1e9, raw);
    }
    if (rd.last == ~0u) rd.last = raw;
    const uint32_t press = raw & ~rd.last;
    rd.last = raw;
    const auto bit = [&](int b) { return (press >> b & 1) != 0; };
    in.confirm = bit(rd.confirm), in.back = bit(rd.back), in.start = bit(HOS_PAD_START), in.triangle = bit(HOS_PAD_NORTH);
    in.l1 = bit(HOS_PAD_LSHOULDER) || bit(HOS_PAD_LTRIGGER), in.r1 = bit(HOS_PAD_RSHOULDER) || bit(HOS_PAD_RTRIGGER);
    bool* way[4] = {&in.up, &in.down, &in.left, &in.right};
    const int pads[4] = {HOS_PAD_UP, HOS_PAD_DOWN, HOS_PAD_LEFT, HOS_PAD_RIGHT};
    for (int i = 0; i < 4; i++) {   // (held: again after 0.35 s, then 12 a second, 30 after 1.5 s)
        if (!(raw >> pads[i] & 1)) { rd.since[i] = 0; continue; }
        if (!rd.since[i]) { rd.since[i] = now, rd.next[i] = now + 350000000LL, *way[i] = bit(pads[i]); continue; }
        if (now >= rd.next[i]) *way[i] = true, rd.next[i] = now + (now - rd.since[i] > 1500000000LL ? 33000000LL : 83000000LL);
    }
    // (the keys, on a PC)
    in.up |= key_press(VK_UP), in.down |= key_press(VK_DOWN), in.left |= key_press(VK_LEFT), in.right |= key_press(VK_RIGHT);
    in.confirm |= key_press(VK_RETURN), in.back |= key_press(VK_ESCAPE) || key_press(VK_BACK);
    in.l1 |= key_press('Q'), in.r1 |= key_press('E'), in.triangle |= key_press('T'), in.start |= key_press(VK_SPACE);
    if (rd.script) {   // ("<seconds>:<KEY>,...", from the screen's first frame)
        double at = 0;
        char key[8] = {};
        int n = 0;
        if (sscanf(rd.script, "%lf:%7[A-Z0-9]%n", &at, key, &n) == 2 && (now - rd.script_t0) / 1e9 >= at) {
            const std::string k = key;
            in.up |= k == "UP", in.down |= k == "DOWN", in.left |= k == "LEFT", in.right |= k == "RIGHT", in.confirm |= k == "OK";
            in.back |= k == "BACK", in.start |= k == "START", in.l1 |= k == "L1", in.r1 |= k == "R1", in.triangle |= k == "TRI";
            rd.script = rd.script[n] == ',' ? rd.script + n + 1 : nullptr;
        }
    }
    return in;
}

/// The settings' change asked by a press, done: the row through the in-game menu's own path (set and saved), the screen's mode
/// in the options file
void apply(const L::Action& a) {
    if (a.setting < 0 || a.setting >= L::kSettingCount) return;
    const L::Setting& s = L::kSettings[a.setting];
    const Shown v = shown_of(s);
    if (!v.known || v.held) return;
    const int to = a.kind == L::A_DEFAULT ? s.fallback : L::next_value(s, v.value, a.dir);
    if (to == v.value) return;
    if (!strcmp(s.key, L::kLaunchModeKey)) {
        menu_launcher_mode_set(to);
    } else {
        MkwVkSetting out{};
        const int r = menu_setting_set(s.key, to, &out);
        if (r != 1) logf("launcher: %s could not be set to %d (%d)", s.key, to, r);
    }
    logf("launcher: %s %d -> %d (saved)", s.key, v.value, to);
    if (!strcmp(s.key, "layout_preset")) read_layout();   // (the buttons that confirm and go back follow the layout)
    if (!strcmp(s.key, "menu_language") || !strcmp(s.key, "game_language"))
        lang = L::lang_for(opts.menu_lang, i18n::game_lang_code(opts.game_lang));
}

void driver_check_start() {
    if (driver_state.load() == 1) return;
    if (driver_thread) driver_thread->join(), delete driver_thread, driver_thread = nullptr;
    driver_state.store(1);
    logf("launcher: the driver asked about 64 of the pipelines play noted (the player asked)");
    driver_thread = new CompileThread([] {
        const auto t0 = clock_type::now();
        size_t asked = 0;
        const int kept = launcher_driver_check(64, &asked);
        const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - t0).count();
        driver_kept.store(kept < 0 ? 0 : kept), driver_asked.store((int)asked), driver_ms.store(ms);
        driver_state.store(kept < 0 ? 3 : 2);
        if (kept < 0) logf("launcher: the driver: nothing to ask (no notes of this build, or the driver cannot answer without a compile)");
        else logf("launcher: the driver's cache had %d of the %zu pipelines asked, without a compile, in %lld ms", kept, asked, (long long)ms);
    });
}

void memory_line(const char* when) {
#ifdef __PROSPERO__
    uint64_t direct = 0, flexible = 0, largest = 0;
    if (hos_ps5_memory_free(&direct, &flexible, &largest))
        logf("launcher: memory %s: direct %llu MiB free (largest piece %llu), flexible %llu MiB free", when,
             (unsigned long long)direct, (unsigned long long)largest, (unsigned long long)flexible);
#else
    (void)when;
#endif
}

void present_once() {
    if (dev.swap_extent.width && dev.swap_extent.height) present_image(nullptr, dev.swap_extent.width, dev.swap_extent.height, nullptr);
}

// ---- (lot 82) the console's own preparation, from the screen ----
const char* offer_name(CP::Offer o) {
    static const char* const n[] = {"off", "no census", "done", "to resume", "to do", "optional (the caches look made)"};
    return n[(int)o];
}
/// The title started again by the system, as the in-game menu's Back to the Grapnel screen does it (host/posix/ps5_relaunch.c):
/// the screen's file left when its switch is on (it then shows whatever the mode), and go_on: the preparation's, which starts it
/// at once in the new process. Off the console: nothing (the PC's tests come back to the Shaders page)
void prep_restart(bool go_on, const char* why) {
    if (prep_restart_asked) return;
    prep_restart_asked = true;
    std::string said;
    if (go_on) {
        L::back_mark(game_folder_path() + kPrepMarker, why, &said);
        logf("launcher: preparation: the next start goes on by itself: %s", said.c_str());
    }
    if (L::back_offered(kConsole, env_on(L::kBackSwitch), getenv("HOOKSHOT_PS5_LAUNCHER"))) {
        L::back_mark(game_folder_path() + L::kBackMarker, why, &said);
        logf("launcher: preparation: the screen's file %s", said.c_str());
    }
    logf("launcher: preparation: the title starts again (%s)", why);
#ifdef __PROSPERO__
    if (!hos_ps5_back_to_launcher()) logf("launcher: preparation: the title could not be started again (above): the PS button closes it");
#endif
}
/// Begins it (A_PREPARE, or the continue file): false when it could not start here (its page then says why)
void prep_begin() {
    std::string why;
    const bool ok = console_prepare_start(prep_mode == CP::Mode::kForce, &why);
    logf("launcher: preparation: %s (%s)", ok ? "begun" : "not begun", why.c_str());
    L::preparing(st);
    prep_ended_ns = 0;
    prep_view = console_prepare_view();
    if (!ok) {   // (the program cache was opened here (the driver asked): a start of its own begins it at once)
        prep_view = ConsolePrepView{};
        prep_view.phase = 4, prep_view.end = (int)CP::End::kNothing;
        prep_ended_ns = now_ns();
        prep_fresh_needed = program_cache_opened();
        if (prep_fresh_needed) prep_restart(true, "the preparation needs a start of its own: the program cache was opened by the driver's check");
    }
}
/// A frame on its page: the view read; at its end, what it says for 2.5 s, then the title starts again (on the PC: back to the
/// Shaders page, the facts read again)
void prep_tick(int64_t now) {
    if (st.page != L::PREPARING) return;
    if (!prep_restart_asked) prep_view = console_prepare_view();
    if (prep_view.phase != 4) return;
    if (!prep_ended_ns) {
        prep_ended_ns = now;
        static const char* const ends[] = {"none", "finished", "stopped (Later)", "stopped for memory", "stopped: a stall", "nothing made"};
        logf("launcher: preparation: %s at %d%% (%zu of %zu items, %zu pipelines built by this start, %.0f s in all)%s",
             ends[std::clamp(prep_view.end, 0, 5)], prep_view.percent, prep_view.done, prep_view.total, prep_view.pipelines,
             prep_view.seconds, prep_view.goes_on ? ": the next start goes on by itself" : "");
    }
    if (now - prep_ended_ns < 5 * L::kSecond / 2 || prep_restart_asked) return;
    if (kConsole && prep_view.end != (int)CP::End::kNothing) {
        prep_restart(prep_view.goes_on, prep_view.end == (int)CP::End::kFinished ? "the shaders' preparation finished"
                                        : prep_view.goes_on ? "the shaders' preparation ran short of memory: it goes on"
                                                            : "the shaders' preparation stopped");
        return;
    }
    // (the PC, or nothing made: the Shaders page again, as the files now say)
    prep_facts = console_prepare_quick();
    CP::Facts pf;
    pf.mode = prep_mode, pf.census = prep_facts.census, pf.same = prep_facts.same, pf.finished = prep_facts.finished;
    pf.done = prep_facts.done, pf.total = prep_facts.total;
    prep_offer = CP::offer(pf);
    st.prep_button = CP::button(prep_offer), st.shaders_sel = 0, st.page = L::SHADERS;
}

}  // namespace

bool launcher_showing() { return showing.load(std::memory_order_acquire); }

void launcher_draw(uint32_t w, uint32_t h) {
    drawn.fetch_add(1, std::memory_order_relaxed);
    if (bg.px && !bg.tex) bg.tex = ui::texture_rgba(bg.w, bg.h, bg.px);   // (copied to the GPU with this frame)
    else if (bg.tex && bg.px && ui::texture_uploaded(bg.tex)) picture_free();   // (on the GPU: the memory goes)
    ui::Ctx c{ImGui::GetForegroundDrawList(), ui::fonts(), 1, (float)w, (float)h, 0, true};
    c.confirm_dot = menu_keys::dot(rd.confirm), c.back_dot = menu_keys::dot(rd.back);
    const Layout l = layout_for((float)w, (float)h);
    c.s = l.k;
    draw_background(c.d, (float)w, (float)h, l);
    draw_panel(c.d, l);
    switch (st.page) {
    case L::HOME: draw_home(c, l); break;
    case L::SETTINGS: draw_settings(c, l); break;
    case L::SHADERS: draw_shaders(c, l); break;
    case L::PREPARING: draw_preparing(c, l); break;
    }
    if (fade > 0) c.d->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), ui::rgba(0, 0, 0, (int)(std::min(fade, 1.0f) * 255 + 0.5f)));
}

void launcher_run() {
    const char* env = getenv(kConsole ? "HOOKSHOT_PS5_LAUNCHER" : "RX_VK_LAUNCHER");
    if (env && *env == '0') {   // (the measurement runs: nothing read, nothing shown, the start as before the screen)
        logf("launcher: off (%s=0)", kConsole ? "HOOKSHOT_PS5_LAUNCHER" : "RX_VK_LAUNCHER");
        return;
    }
    if (!kConsole && !(env && *env)) return;
    if (dev.offscreen || !dev.swapchain) {
        logf("launcher: no screen to show it on");
        return;
    }
    const auto t0 = clock_type::now();
    menu_init();   // (the options file read: the mode, the rows the screen shows)
#ifdef __PROSPERO__
    no_game = hos_ps5_game_missing(no_game_line, sizeof no_game_line) != 0;
    if (no_game) logf("launcher: the game file cannot be read (%s): the screen says so and does not start the game", no_game_line);
#endif
    const int mode = menu_launcher_mode();
    // (lot 73) the in-game menu's Back to the Grapnel screen, before the system started the title again: its file taken (read and
    // erased, one start only), and the screen shown whatever the mode. Only with HOOKSHOT_PS5_BACK_TO_LAUNCHER=1: without it the
    // file is not looked at
    bool back = false;
    if (L::back_offered(kConsole, env_on(L::kBackSwitch), env)) {
        std::string said;
        back = L::back_take(game_folder_path() + L::kBackMarker, &said);
        logf("launcher: back to the Grapnel screen (HOOKSHOT_PS5_BACK_TO_LAUNCHER=1): %s",
             back ? said.c_str() : "no file from the game: a start from the home screen");
    }
    // (the quick check: the files' heads, the index, the notes' program keys; nothing written)
    {
        L::CheckPaths p;
        // (the renderer's own names: vk_progcache.cpp disk_init, vk_draw.cpp pipe_notes_path and state_list_path; hookshot_* on
        // the console, cache_path's console_name)
        p.programs = cache_path("longshot_programs.bin");
        p.index = cache_path("longshot_programs.idx");
        p.notes = cache_path("longshot_pipelines.bin");
        p.states = cache_path("longshot_pipeline_states.bin");
        if (const char* e = getenv("RX_VK_SKIP_FILE")) p.skip = e;
        stamp_now = program_cache_stamp();
        facts = L::read_facts(p, stamp_now);
        std::string where;
        const auto d0 = clock_type::now();
        facts.driver_bytes = driver_cache_bytes(&where);
        facts.driver_known = !where.empty();
        facts.seconds += std::chrono::duration<double>(clock_type::now() - d0).count();
        // (lot 82) the console's own preparation: what the files say of it, in a few milliseconds (no census nor list read whole)
        prep_mode = CP::mode_of(getenv("HOOKSHOT_PS5_PREPARE"));
        // (lot 83) read whatever the mode: a preparation finished for this translator and driver, the builder's on the PC (its
        // progress file comes with the driver's cache it made) or the console's own, makes the driver's part, and the check says
        // so without HOOKSHOT_PS5_PREPARE, which only offers the console's own
        prep_facts = console_prepare_quick();
        facts.prepared = prep_facts.same && prep_facts.finished;
        if (prep_mode == CP::Mode::kOff && prep_facts.record)
            logf("launcher: shaders' preparation: %s", prep_facts.why.c_str());
        if (prep_mode != CP::Mode::kOff) {
            CP::Facts pf;
            pf.mode = prep_mode, pf.census = prep_facts.census, pf.same = prep_facts.same, pf.finished = prep_facts.finished;
            pf.done = prep_facts.done, pf.total = prep_facts.total;
            pf.caches_made = CP::caches_made(facts.programs_stamp, stamp_now, facts.programs, facts.driver_bytes >> 20);
            prep_offer = CP::offer(pf);
            logf("launcher: preparation (HOOKSHOT_PS5_PREPARE=%s): %s; %s; the whole about %s with %u workers, %s left",
                 getenv("HOOKSHOT_PS5_PREPARE"), offer_name(prep_offer), prep_facts.why.c_str(),
                 CP::duration_text(prep_facts.planned_s).c_str(), prep_facts.workers, CP::duration_text(prep_facts.left_s).c_str());
        }
        check = L::judge(facts, stamp_now);
        logf("launcher: shader check in %.2f s: %s; programs %zu (%llu MB, stamp %016llx, this build's %016llx), notes %zu (%d%% of "
             "their programs translated), state list %zu, driver cache %llu MB, %zu left out",
             facts.seconds, check.verdict == L::V_OK ? "ready" : check.verdict == L::V_WARN ? "to complete" : "PROBLEM",
             facts.programs, (unsigned long long)(facts.programs_bytes >> 20), (unsigned long long)facts.programs_stamp,
             (unsigned long long)stamp_now, facts.notes, check.coverage, facts.states, (unsigned long long)(facts.driver_bytes >> 20),
             facts.skipped);
    }
    // (lot 82) the preparation's file, left by a start that ran short of memory part-way: this start goes on with it at once
    bool prep_go_on = false;
    if (prep_mode != CP::Mode::kOff && !(env && *env == '0')) {
        std::string said;
        prep_go_on = L::back_take(game_folder_path() + kPrepMarker, &said);
        if (prep_go_on) prep_go_on = prep_offer == CP::Offer::kResume || prep_offer == CP::Offer::kNeeded;
        if (!said.empty()) logf("launcher: preparation: %s%s", said.c_str(), prep_go_on ? ": it goes on" : ": nothing to go on with");
    }
    bool r1 = false;
    // (lot 88: not without the game file either: the screen shows then, no_game)
    if (kConsole && !(env && *env) && mode == 1 && !no_game &&
        check.verdict != L::V_FAIL && !back && !prep_go_on && !CP::asks_for_screen(prep_offer)) {
        // (only when needed: R1 held as the game starts shows it. The controllers come up with the first read: a little while)
        for (int i = 0; i < 30 && !r1; i++) {
            r1 = (raw_now() >> HOS_PAD_RSHOULDER & 1) != 0;
            if (!r1) Sleep(20);
        }
    }
    const char* why = "";
    if (!L::show(env, kConsole, mode, check.verdict, r1, back || prep_go_on, CP::asks_for_screen(prep_offer), no_game, &why)) {
        logf("launcher: not shown (%s)", why);
        return;
    }
    memory_line("before the screen");
    lang = L::lang_for(opts.menu_lang, i18n::game_lang_code(opts.game_lang));
    version = menu_version();
    {
        std::string said;
        const bool have = picture_load(&said);
        logf("launcher: shown (%s), in %s; %s", why, lang == L::FR ? "French" : "English", said.c_str());
        (void)have;
    }
    rd = Reader{};
    read_layout();
    rd.script = getenv("RX_VK_LAUNCHER_SCRIPT");
    L::begin(st, now_ns(), check.verdict, no_game);
    st.prep_button = CP::button(prep_offer);   // (lot 82)
    if (CP::asks_for_screen(prep_offer)) st.home = L::H_SHADERS;
    rd.script_t0 = st.start_ns;
    showing.store(true, std::memory_order_release);
    drawn.store(0);
    if (prep_go_on) prep_begin();
    while (st.outcome == L::NONE) {
        const int64_t now = now_ns();
        if (!drawn.load(std::memory_order_relaxed) && now - st.start_ns > 3 * L::kSecond) {
            // (nothing on the TV after 3 s: Dear ImGui did not start, or no image came back from the display. Not a screen the
            // player can use: the game starts, as without it)
            logf("launcher: nothing drawn in 3 s: the game starts without the screen");
            st.outcome = no_game ? L::QUIT : L::PLAY;   // (lot 88: no game file: the title closes rather than stop later)
            break;
        }
        const L::Input in = read_input(now);
        const int sel = L::selected_setting(st);
        const bool held = sel >= 0 && shown_of(L::kSettings[sel]).held;
        const L::Page was = st.page;
        const L::Action a = L::step(st, in, now, held);
        if (a.kind == L::A_STEP || a.kind == L::A_DEFAULT) apply(a);
        else if (a.kind == L::A_DRIVER_CHECK) driver_check_start();
        else if (a.kind == L::A_PREPARE) prep_begin();
        else if (a.kind == L::A_PREPARE_STOP) console_prepare_stop();
        prep_tick(now);
        if (st.page != was) {
            static const char* const names[] = {"home", "settings", "shaders", "preparing"};
            logf("launcher: page %s", names[st.page]);
        }
        present_once();
        Sleep(4);
    }
    static const char* const outcomes[] = {"", "PLAY", "CONTINUE", "QUIT"};
    logf("launcher: %s after %.1f s%s%s", outcomes[st.outcome], std::chrono::duration<double>(clock_type::now() - t0).count(),
         st.counting ? " (the countdown ran out)" : "", rd.seen ? "" : " (no input from the controllers seen)");
    if (driver_thread) driver_thread->join(), delete driver_thread, driver_thread = nullptr;   // (a check asked: its end, before the game)
    if (st.outcome == L::QUIT) {
        logf("launcher: the title closes (QUIT)");
        Sleep(200);   // (the log's last lines out)
        ExitProcess(0);
    }
    // (the end: to black over 0.4 s, then black until every button is let go, a second at most: the press that started the
    // game is not the game's first)
    {
        const auto f0 = clock_type::now();
        for (;;) {
            const double f = std::chrono::duration<double>(clock_type::now() - f0).count() / 0.4;
            fade = (float)std::min(f, 1.0);
            present_once();
            if (f >= 1) break;
            Sleep(4);
        }
        const auto r0 = clock_type::now();
        int frames = 0;
        while (raw_now() && std::chrono::duration<double>(clock_type::now() - r0).count() < 1.0) present_once(), frames++, Sleep(4);
        if (raw_now()) logf("launcher: a button still held after 1 s: the game starts all the same");
    }
    showing.store(false, std::memory_order_release);
    // (everything the screen took let go before the game's first instruction: the picture and its GPU copy, Dear ImGui, the
    // in-game menu's prebake made again at the game's first overlay)
    if (bg.tex) ui::texture_drop(bg.tex), bg.tex = nullptr;
    picture_free();
    ui::shutdown();
    menu_ui_forgotten();
    memory_line("after the screen, all of it let go");
    logf("launcher: done in %.1f s: the game starts", std::chrono::duration<double>(clock_type::now() - t0).count());
}

}  // namespace mkwvk
