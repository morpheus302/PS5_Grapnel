"""(ps5, lot 82) The console's own shader preparation, without the game, a GPU or a console: its rules (gpu/vk/console_prepare.h,
compiled alone) and what the launch screen does with them (gpu/vk/launcher_logic.h). The progress file read back as written, a
file of another kind refused; a start going on where the last one stopped, an item under way when two starts in a row ended
left to play, a start that got nowhere counted as one, another stamp or force starting again; the memory gate (nothing under
the floor, fewer workers as the free memory falls, a mesh program waiting for room or stopping the start, two at most at
once); a stop for memory going on by itself only after some work; what the Shaders page offers, the time left and its words; a
whole preparation run through starts that end at random places, which still finishes with every item made or left to play.
Then the screen's logic: the Prepare button above Ask the driver, its page where nothing starts the game, the screen shown in
its When needed mode for a preparation to do, the verdict once prepared. Then, by reading the sources: the work keeps nothing in
the program cache and builds for the driver's cache alone, the screen reads the switch before anything else of it, the
translator's files are not the preparation's.

    python -I -B tests/test_console_prepare.py [--cxx g++]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (python -I leaves the check's own folder off the path)
from srccut import block   # noqa: E402

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''#include "console_prepare.h"
#include "launcher_logic.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
using namespace mkwvk;
namespace CP = mkwvk::console_prep;
namespace L = mkwvk::launcher;
L::Input press(const char* which) {
    L::Input in;
    const std::string w = which;
    in.up = w == "up", in.down = w == "down", in.confirm = w == "confirm", in.back = w == "back", in.start = w == "start";
    return in;
}
int main() {
    // ---- the switch ----
    CHECK(CP::mode_of(nullptr) == CP::Mode::kOff && CP::mode_of("") == CP::Mode::kOff && CP::mode_of("0") == CP::Mode::kOff);
    CHECK(CP::mode_of("1") == CP::Mode::kOn && CP::mode_of("force") == CP::Mode::kForce);
    // ---- the items: mesh programs first (the heaviest, on a new heap), then compute, then the lists' groups ----
    CP::Plan plan;
    plan.mesh = 3, plan.compute = 4, plan.groups = 5, plan.records = 12;
    CHECK(plan.total() == 12);
    CHECK(CP::kind_of(plan, 0) == CP::kMesh && CP::kind_of(plan, 2) == CP::kMesh && CP::kind_of(plan, 3) == CP::kCompute);
    CHECK(CP::kind_of(plan, 6) == CP::kCompute && CP::kind_of(plan, 7) == CP::kGraphics && CP::kind_of(plan, 11) == CP::kGraphics);
    CHECK(CP::weight(plan, 0, 0) == CP::kSecMesh && CP::weight(plan, 4, 0) == CP::kSecCompute);
    CHECK(CP::weight(plan, 8, 3) == CP::kSecGroup + 3 * CP::kSecRecord);
    CHECK(CP::estimate_s(plan, 1) == CP::plan_seconds(plan) && CP::estimate_s(plan, 4) < CP::plan_seconds(plan) / 2.5);
    CHECK(CP::estimate_s(plan, 8) > CP::kSecMesh * 3 / CP::workers_speed(2));   // (the mesh programs two at a time, whatever the workers)
    {   // (this game's: 38,809 census programs, the shipped list 61,721 records: about the whole)
        const CP::Plan p = CP::plan_from_sizes(38809, 12 + 61721ull * 552);
        CHECK(p.mesh == 1312 && p.compute == 4128 && p.records == 61721 && p.groups > 15000 && p.groups < 25000);
        CHECK(CP::estimate_s(p, 4) > 20 * 60 && CP::estimate_s(p, 4) < 4 * 3600);
        CHECK(CP::plan_from_sizes(0, 0).total() == 0);
    }
    // ---- the progress file ----
    CP::Progress p;
    p.stamp = 0x1234abcd5678ef01ull, p.translator = 0xc1f75256405efa1eull, p.driver = 77, p.total = 100, p.done = 40;
    p.finished = false, p.started_at = 12, p.starts = 3, p.seconds = 120.5, p.units = 55.5, p.units_done = 60, p.units_total = 150;
    p.inflight = {40, 41}, p.strikes = {40}, p.skip = {7, 9};
    CP::Progress q;
    CHECK(CP::parse(CP::format(p), &q));
    CHECK(q.stamp == p.stamp && q.translator == p.translator && q.driver == p.driver && q.total == 100 && q.done == 40);
    CHECK(!q.finished && q.started_at == 12 && q.starts == 3 && q.seconds > 120.4 && q.seconds < 120.6 && q.units > 55.4 && q.units < 55.6);
    CHECK(q.units_done == 60 && q.units_total == 150);
    CHECK(q.inflight == p.inflight && q.strikes == p.strikes && q.skip == p.skip);
    p.started_at = (size_t)-1;
    CHECK(CP::parse(CP::format(p), &q) && q.started_at == (size_t)-1);
    CHECK(!CP::parse("stamp 12\ntotal 3\ndone 1\n", &q));                       // (no magic: another kind of file)
    CHECK(!CP::parse(std::string(CP::kMagic) + "\ntotal 3\ndone 9\n", &q));     // (done past the total)
    CHECK(!CP::parse("", &q));
    // ---- a start's beginning ----
    {   // (none before: from the first item, one start counted)
        CP::Progress f;
        const CP::Resume r = CP::resume(f, 5, 100, 2, false);
        CHECK(r.fresh && r.from == 0 && f.stamp == 5 && f.total == 100 && f.started_at == 0 && f.starts == 1);
    }
    {   // (the same: on from done, the items under way struck once, not left yet)
        CP::Progress f;
        f.stamp = 5, f.total = 100, f.done = 30, f.started_at = 10, f.inflight = {30, 33}, f.starts = 1;
        const CP::Resume r = CP::resume(f, 5, 100, 2, false);
        CHECK(!r.fresh && r.from == 30 && f.strikes == std::vector<size_t>({30, 33}) && f.inflight.empty() && f.skip.empty());
        CHECK(r.newly_skipped.empty() && f.started_at == 30 && f.starts == 2);
        // (it ends again with 33 under way: left to play from now on; 30 was made meanwhile)
        f.done = 31, f.inflight = {33, 34};
        const CP::Resume r2 = CP::resume(f, 5, 100, 2, false);
        CHECK(r2.newly_skipped == std::vector<size_t>({33}) && f.skip == std::vector<size_t>({33}) && r2.from == 31);
        CHECK(f.strikes == std::vector<size_t>({33, 34}));
    }
    {   // (a start that got nowhere: the first items, one a worker, count as under way)
        CP::Progress f;
        f.stamp = 5, f.total = 100, f.done = 30, f.started_at = 30;
        CP::resume(f, 5, 100, 3, false);
        CHECK(f.strikes == std::vector<size_t>({30, 31, 32}));
        f.started_at = 30;   // (and again: the first, struck, was built alone, so it alone is left to play)
        const CP::Resume r = CP::resume(f, 5, 100, 3, false);
        CHECK(r.newly_skipped == std::vector<size_t>({30}) && f.skip == std::vector<size_t>({30}));
        CHECK(f.strikes == std::vector<size_t>({30}));
    }
    {   // (a stop of its own, for memory or by the player, is not a start that got nowhere)
        CP::Progress f;
        f.stamp = 5, f.total = 100, f.done = 30, f.started_at = (size_t)-1;
        CP::resume(f, 5, 100, 3, false);
        CHECK(f.strikes.empty());
    }
    {   // (another stamp, another count, force over a finished one: from the start; a finished one otherwise stays finished)
        CP::Progress f;
        f.stamp = 5, f.total = 100, f.done = 100, f.finished = true, f.skip = {3};
        CP::Progress g = f;
        CHECK(CP::resume(g, 6, 100, 2, false).fresh && g.skip.empty() && g.done == 0);
        g = f;
        CHECK(CP::resume(g, 5, 101, 2, false).fresh);
        g = f;
        CHECK(CP::resume(g, 5, 100, 2, true).fresh && !g.finished);
        g = f;
        const CP::Resume r = CP::resume(g, 5, 100, 2, false);
        CHECK(!r.fresh && g.finished && r.from == 100);
    }
    {
        std::vector<bool> made = {true, true, false, true, false};
        CHECK(CP::advance(0, made, {}) == 2 && CP::advance(0, made, {2}) == 4 && CP::advance(0, made, {2, 4}) == 5);
    }
    // ---- the memory gate ----
    CP::Gate g;   // (floor 2048, mesh room 3072, all workers 5120 above the floor, two mesh programs at once)
    CHECK(CP::gate(g, 2047, CP::kGraphics, 0, 0, 4) == CP::Take::kStop);
    CHECK(CP::gate(g, -1, CP::kMesh, 3, 1, 4) == CP::Take::kGo);   // (unknown: a PC)
    CHECK(CP::allowed(g, 2048 + 5120, 4) == 4 && CP::allowed(g, 2048 + 2560, 4) == 2 && CP::allowed(g, 2100, 4) == 1);
    CHECK(CP::allowed(g, 2048 + 2560, 1) == 1);
    CHECK(CP::gate(g, 9000, CP::kCompute, 3, 0, 4) == CP::Take::kGo && CP::gate(g, 9000, CP::kCompute, 4, 0, 4) == CP::Take::kWait);
    CHECK(CP::gate(g, 3000, CP::kCompute, 1, 0, 4) == CP::Take::kWait && CP::gate(g, 3000, CP::kCompute, 0, 0, 4) == CP::Take::kGo);
    CHECK(CP::gate(g, 11000, CP::kMesh, 1, 1, 4) == CP::Take::kGo);    // (room for a second one)
    CHECK(CP::gate(g, 11000, CP::kMesh, 2, 2, 4) == CP::Take::kWait);  // (two at most)
    CHECK(CP::gate(g, 6000, CP::kMesh, 1, 1, 4) == CP::Take::kWait);   // (room for one only: the other's end first)
    CHECK(CP::gate(g, 4000, CP::kMesh, 0, 0, 4) == CP::Take::kStop);   // (no room even alone: a new heap)
    CHECK(CP::gate(g, 4000, CP::kMesh, 1, 0, 4) == CP::Take::kWait);
    CHECK(CP::gate(g, 5200, CP::kMesh, 0, 0, 4) == CP::Take::kGo);
    // (a PC, the builder's preparation: a new start frees little there, so nothing stops it for memory: what has no room is
    // taken alone once the others are done; and one worker per 512 MiB above the floor. A player's PC on 25/11/2026: 3504 MiB
    // available, a mesh program first: every start stopped at item 0, "short of memory again and again")
    CP::Gate pc;
    pc.restart_frees = false;
    pc.floor_mb = 1024;
    CHECK(CP::gate(pc, 3504, CP::kMesh, 0, 0, 12) == CP::Take::kGo);      // (no room for a mesh program: alone)
    CHECK(CP::gate(pc, 3504, CP::kMesh, 1, 0, 12) == CP::Take::kWait);    // (the others' end first)
    CHECK(CP::gate(pc, 800, CP::kGraphics, 0, 0, 12) == CP::Take::kGo);   // (under the floor: one at a time, never a stop)
    CHECK(CP::gate(pc, 800, CP::kGraphics, 1, 0, 12) == CP::Take::kWait);
    CHECK(CP::allowed(pc, 3504, 12) == 4 && CP::allowed(pc, 800, 12) == 1 && CP::allowed(pc, 1024 + 512 * 20, 12) == 12);
    CHECK(CP::gate(pc, 3504, CP::kCompute, 3, 0, 12) == CP::Take::kGo && CP::gate(pc, 3504, CP::kCompute, 4, 0, 12) == CP::Take::kWait);
    CHECK(CP::gate(pc, 9000, CP::kMesh, 1, 1, 12) == CP::Take::kGo);      // (room for two: as on the console)
    CHECK(CP::gate(pc, -1, CP::kMesh, 3, 1, 12) == CP::Take::kGo);
    CHECK(CP::gate(pc, 800, CP::kMesh, 0, 0, 1) == CP::Take::kGo && CP::allowed(pc, 9000, 1) == 1 && CP::allowed(pc, 9000, 0) == 1);
    CHECK(CP::gate(pc, 9000, CP::kMesh, 2, 2, 12) == CP::Take::kWait);   // (two mesh programs at most, as on the console)
    CHECK(CP::gate(pc, 800, CP::kCompute, 0, 0, 12, true) == CP::Take::kGo);         // (struck, under the floor: alone)
    CHECK(CP::gate(pc, 800, CP::kCompute, 1, 0, 12, false, true) == CP::Take::kWait);   // (a struck one being built alone)
    {   // (whatever the memory, floor, kind and load: never a stop on a PC, and nothing busy always goes)
        bool never_stops = true, idle_goes = true;
        for (int64_t floor : {0, 1024, 2048, 100000})
            for (int64_t free_mb = -1; free_mb <= 14000; free_mb += 97)
                for (CP::Kind kind : {CP::kGraphics, CP::kCompute, CP::kMesh})
                    for (unsigned workers : {1u, 4u, 12u, 32u})   // (0 workers: none to take anything, -1 memory waits)
                        for (unsigned busy = 0; busy <= workers; busy++)
                            for (unsigned busy_mesh = 0; busy_mesh <= std::min(busy, 2u); busy_mesh++)
                                for (int struck = 0; struck < 2; struck++) {
                                    CP::Gate h = pc;
                                    h.floor_mb = floor;
                                    const CP::Take t = CP::gate(h, free_mb, kind, busy, busy_mesh, workers, struck, false);
                                    if (t == CP::Take::kStop) never_stops = false;
                                    if (busy == 0 && t != CP::Take::kGo) idle_goes = false;
                                }
        CHECK(never_stops && idle_goes);
    }
    // ---- the end ----
    CHECK(CP::goes_on_by_itself(CP::End::kMemory, 10, 11) && !CP::goes_on_by_itself(CP::End::kMemory, 10, 10));
    CHECK(!CP::goes_on_by_itself(CP::End::kPlayer, 10, 50) && !CP::goes_on_by_itself(CP::End::kFinished, 0, 50));
    // ---- what the Shaders page offers ----
    CP::Facts f;
    CHECK(CP::offer(f) == CP::Offer::kOff);
    f.mode = CP::Mode::kOn;
    CHECK(CP::offer(f) == CP::Offer::kNoCensus);
    f.census = true;
    CHECK(CP::offer(f) == CP::Offer::kNeeded && CP::asks_for_screen(CP::Offer::kNeeded) && CP::button(CP::Offer::kNeeded));
    f.caches_made = true;   // (our console's caches: optional, the screen not asked for)
    CHECK(CP::offer(f) == CP::Offer::kOptional && !CP::asks_for_screen(CP::Offer::kOptional) && CP::button(CP::Offer::kOptional));
    f.same = true, f.done = 10, f.total = 100;
    CHECK(CP::offer(f) == CP::Offer::kResume && CP::asks_for_screen(CP::Offer::kResume));
    f.finished = true;
    CHECK(CP::offer(f) == CP::Offer::kDone && !CP::button(CP::Offer::kDone) && !CP::asks_for_screen(CP::Offer::kDone));
    f.mode = CP::Mode::kForce;
    CHECK(CP::offer(f) == CP::Offer::kNeeded);
    CHECK(!CP::button(CP::Offer::kNoCensus) && !CP::button(CP::Offer::kOff));
    CHECK(CP::caches_made(7, 7, 4572, 1009) && !CP::caches_made(7, 8, 4572, 1009) && !CP::caches_made(7, 7, 600, 1009));
    CHECK(!CP::caches_made(7, 7, 4572, 100));
    // ---- the time ----
    CHECK(CP::eta_s(0, 0, 100, 500) == 500 && CP::eta_s(10, 5, 100, 500) == 500);   // (too early: the plan's)
    CHECK(CP::eta_s(50, 100, 150, 500) == 300);   // (50 units in 100 s: 150 more in 300 s)
    CHECK(CP::eta_s(50, 100, 0, 500) == 0 && CP::eta_s(0, 0, 10, -1) < 0);
    CHECK(CP::duration_text(-1) == "?" && CP::duration_text(30) == "< 1 min" && CP::duration_text(45 * 60) == "45 min");
    CHECK(CP::duration_text(65 * 60) == "1 h 05" && CP::duration_text(2 * 3600 + 30) == "2 h 01");
    CHECK(CP::percent(50, 100, false) == 50 && CP::percent(100, 100, false) == 99 && CP::percent(100, 100, true) == 100);
    CHECK(CP::percent(5, 0, false) == 0);
    // ---- a whole preparation through starts that end anywhere: it finishes, every item made or left to play. With a save at each
    // change (the worker saves as it begins a struck item, the boss every 2 s) only an item that ends the title is left to play;
    // with saves that come late, a few more may be, never all, and it still finishes ----
    std::mt19937 rng(82);
    for (int stale = 0; stale < 2; stale++)
    for (int trial = 0; trial < 300; trial++) {
        const size_t total = 1 + rng() % 60;
        const unsigned workers = 1 + rng() % 4;
        CP::Progress file;   // (what the disk has between starts)
        std::vector<bool> made(total, false);
        int starts = 0;
        // (a few items end the title when built: each is under way at two ends in a row and so left to play)
        std::vector<bool> crashes(total, false);
        for (size_t i = 0; i < total; i++) crashes[i] = rng() % 25 == 0;
        for (;;) {
            CHECK(++starts < 2000);
            CP::Progress p = file;
            const CP::Resume r = CP::resume(p, 99, total, workers, false);
            if (p.finished) break;
            CP::Progress saved = p;   // (the last save of this start)
            std::vector<size_t> under_way;
            size_t next = r.from;
            bool solo = false;
            const auto snapshot = [&] {
                saved = p;
                saved.done = CP::advance(p.done, made, p.skip);
                saved.inflight = under_way;
            };
            for (;;) {
                while (next < total && (made[next] || CP::has(p.skip, next))) next++;
                if (next >= total && under_way.empty()) break;
                const int roll = (int)(rng() % 10);
                const bool struck = next < total && CP::has(p.strikes, next);
                const bool may_take = next < total && under_way.size() < workers && !solo && !(struck && !under_way.empty());
                if (may_take && roll < 6) {
                    under_way.push_back(next++);
                    solo = struck;
                    if (struck || !stale || rng() % 2 == 0) snapshot();   // (late saves: half the takes in none)
                } else if (!under_way.empty() && roll < 9) {
                    const size_t k = rng() % under_way.size(), i = under_way[k];
                    if (crashes[i] && !CP::has(p.skip, i)) break;   // (the title ends with it under way)
                    made[i] = true;
                    if (solo) solo = false;
                    under_way.erase(under_way.begin() + (long)k);
                    if (!stale || rng() % 3 == 0) snapshot();
                } else if (roll == 9 && rng() % 4 == 0) {
                    if (stale) break;   // (closed from the system's menu: what was under way is struck, as a crash)
                    // (Later: nothing more taken, what is under way finishes, the file says a stop of its own)
                    bool crashed = false;
                    for (size_t i : under_way) crashed |= crashes[i] && !CP::has(p.skip, i);
                    if (crashed) break;
                    for (size_t i : under_way) made[i] = true;
                    under_way.clear();
                    snapshot();
                    saved.started_at = (size_t)-1;
                    break;
                }
            }
            if (next >= total && under_way.empty()) {   // (the end: the last save says finished)
                snapshot();
                saved.finished = saved.done >= total;
            }
            file = saved;
        }
        size_t left = 0, crashing = 0;
        for (size_t i = 0; i < total; i++) {
            CHECK(made[i] || CP::has(file.skip, i));
            if (!stale && CP::has(file.skip, i)) CHECK(crashes[i]);   // (only an item that ends the title is left to play)
            left += CP::has(file.skip, i), crashing += crashes[i];
        }
        if (stale) CHECK(left <= crashing * (workers + 1) + workers);
    }
    // ---- the screen: the Prepare button above Ask the driver, its page ----
    L::State st;
    L::begin(st, 0, L::V_FAIL);
    st.page = L::SHADERS;
    CHECK(L::step(st, press("confirm"), 1).kind == L::A_DRIVER_CHECK);   // (without the button: as in lot 80)
    st.prep_button = true, st.shaders_sel = 0;
    CHECK(L::step(st, press("confirm"), 2).kind == L::A_PREPARE);
    L::step(st, press("down"), 3);
    CHECK(st.shaders_sel == 1 && L::step(st, press("confirm"), 4).kind == L::A_DRIVER_CHECK);
    L::step(st, press("up"), 5);
    CHECK(st.shaders_sel == 0);
    L::preparing(st);
    CHECK(st.page == L::PREPARING);
    CHECK(L::step(st, press("confirm"), 6).kind == L::A_NONE && st.page == L::PREPARING);
    CHECK(L::step(st, press("back"), 7).kind == L::A_PREPARE_STOP && st.page == L::PREPARING);
    CHECK(L::step(st, press("start"), 8).kind == L::A_PREPARE_STOP && st.outcome == L::NONE);   // (Options: no game while it runs)
    // ---- the screen shows in its When needed mode for a preparation to do ----
    const char* why = "";
    CHECK(L::show(nullptr, true, 1, L::V_OK, false, false, true, &why) && std::string(why).find("prepare") != std::string::npos);
    CHECK(!L::show(nullptr, true, 1, L::V_OK, false, false, false, &why));
    CHECK(!L::show("0", true, 1, L::V_OK, false, false, true, &why));   // (switched off for the measurement runs)
    // ---- the verdict once prepared: no program cache is no problem, the driver's part is in its cache ----
    L::ShaderFacts sf;
    sf.driver_known = true, sf.driver_bytes = 300ull << 20;
    CHECK(L::judge(sf, 5).verdict == L::V_FAIL);
    sf.prepared = true;
    CHECK(L::judge(sf, 5).verdict == L::V_OK);
    sf.driver_bytes = 0;   // (prepared, then the driver's cache emptied: a problem again)
    CHECK(L::judge(sf, 5).verdict == L::V_FAIL);
    CHECK(L::shaders_word(L::V_FAIL, CP::Offer::kNeeded) == L::T_TO_PREPARE && L::shaders_word(L::V_OK, CP::Offer::kResume) == L::T_TO_RESUME);
    CHECK(L::shaders_word(L::V_OK, CP::Offer::kDone) == L::T_READY && L::shaders_word(L::V_FAIL, CP::Offer::kOff) == L::T_PROBLEM);
    CHECK(L::prep_end_text(CP::End::kFinished) == L::T_PREP_DONE && L::prep_end_text(CP::End::kMemory) == L::T_PREP_LOW_MEMORY);
    CHECK(L::prep_end_text(CP::End::kPlayer) == L::T_PREP_STOPPED && L::prep_end_text(CP::End::kStall) == L::T_PREP_STALL);
    CHECK(!strcmp(L::text(L::T_PREP_LATER, L::FR), "Plus tard") && !strcmp(L::text(L::T_PREPARING, L::FR), "PRÉPARATION"));
    std::puts("PASS: the console's preparation: its file, its starts, its memory gate, its offer, its time, and the screen's part");
}
'''


def check_sources():
    vk = ROOT / "gpu" / "vk"
    draw = (vk / "vk_draw.cpp").read_text(encoding="utf-8")
    work = block(draw, "void prep_work(bool force) {")
    start = block(draw, "bool console_prepare_start(bool force, std::string* why) {")
    # (nothing into the program cache: the switch set before the census is opened, and refused where the cache is open already)
    if 'setenv("RX_VK_NO_PROGRAM_CACHE", "1", 1)' not in start or "program_cache_opened()" not in start:
        raise SystemExit("console_prepare_start: the program cache is not kept out of the preparation")
    if start.index("program_cache_opened()") > start.index('setenv("RX_VK_NO_PROGRAM_CACHE", "1", 1)'):
        raise SystemExit("console_prepare_start: the program cache's state is read after the switch is set")
    # (for the driver's cache alone, the pipelines built as play builds them, each let go)
    for needle in ("driver_cache_only = true;", "build_graphics(p, s, at, topo)", "compute_pipeline_info(p, true)",
                   "cached_compute(&ci, &pl)", "fallback_compute_pipelines(*p)", "vkDestroyPipeline(dev.device, pl, nullptr)",
                   "CP::gate(gate, free_mb, kind, busy, busy_mesh, workers, struck, solo_busy)", "skip_live(key)", "taau_key(p.key)",
                   "driver_cache_bytes(&where)"):
        if needle not in work and needle not in draw:
            raise SystemExit("the console's preparation lost: " + needle)
    # (0.2.3: on a PC, and there only, the gate never stops for memory, from a 1 GiB floor, with a 15-minute stall: set before the
    # floor's switch is read, so that HOOKSHOT_PS5_PREPARE_MIN_FREE_MB still sets the floor)
    pc_at = work.find("#if !defined(__PROSPERO__)\n    // (a PC, the builder's preparation")
    pc_end = work.find("#endif", pc_at)
    pc_block = work[pc_at:pc_end] if pc_at >= 0 else ""
    for needle in ("gate.restart_frees = false;", "gate.floor_mb = 1024;", "stall_default_s = 900;"):
        if needle not in pc_block:
            raise SystemExit("prep_work: the PC's gate lost: " + needle)
    if pc_end > work.find('gate.floor_mb = env_int("HOOKSHOT_PS5_PREPARE_MIN_FREE_MB", gate.floor_mb);'):
        raise SystemExit("prep_work: the PC's floor is set after its switch is read")
    if work.count("restart_frees = false") != 1:
        raise SystemExit("prep_work: restart_frees is set off outside the PC's block")
    for banned in ("program_cache_save(", "pipeline_note(", "pcache_init()", "pipeline_cache_save("):
        if banned in work:
            raise SystemExit("the console's preparation keeps something besides the driver's cache: " + banned)
    # (the screen: nothing of it without the switch; the switch read before the verdict, which takes "prepared")
    launcher = (vk / "vk_launcher.cpp").read_text(encoding="utf-8")
    run = block(launcher, "void launcher_run() {")
    mode = run.index('prep_mode = CP::mode_of(getenv("HOOKSHOT_PS5_PREPARE"));')
    if run.index("console_prepare_quick()") < mode or run.index("check = L::judge(facts, stamp_now);") < run.index("facts.prepared ="):
        raise SystemExit("launcher_run: the preparation's facts are read before the switch, or after the verdict")
    if "if (prep_mode != CP::Mode::kOff) {" not in run:
        raise SystemExit("launcher_run: the preparation is looked at without HOOKSHOT_PS5_PREPARE")
    # (the translator's files are not the preparation's: its stamp, and so every player's caches, stay as they are)
    sys.path.insert(0, str(ROOT / "nx2"))
    import translator_files   # noqa: E402
    for f in translator_files.files(str(vk)):
        text = Path(f).read_text(encoding="utf-8", errors="replace")
        if "console_prep" in text or "HOOKSHOT_PS5_PREPARE" in text:
            raise SystemExit("a file of the translator's stamp names the preparation: " + f)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_sources()
    with tempfile.TemporaryDirectory(prefix="console-prepare-") as tmp:
        src = Path(tmp) / "t.cpp"
        exe = Path(tmp) / "t"
        src.write_text(FIXTURE, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu" / "vk"), str(src), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
