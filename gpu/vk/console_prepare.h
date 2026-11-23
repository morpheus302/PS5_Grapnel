// (ps5, lot 82) The console's own shader preparation, before its first game: its rules, with no Vulkan, file or console call in
// them (tests/test_console_prepare.py compiles this header alone). The work itself is vk_draw.cpp's (console_prepare_*), the
// screen that offers it and shows it the launch screen's Shaders page (vk_launcher.cpp, launcher_logic.h).
//
// Why: a new player has no cache. Every pipeline is then compiled by the driver in play, when first drawn: stops of a second
// and more, and 160 to 560 MiB of the title's heap each time, which a console title never gets back
// (notes-agents/compilations-pilote-en-jeu.md). Our console played with caches made on the PC; a player's console cannot.
// What: for the driver's own disk cache only (the PS5 driver keeps it in the title's folder, radv-shader-cache), every compute
// program of the game's shader census (hookshot_shaders.bin, made by the builder from the player's own game: never shipped),
// the mesh programs' fallback compute pipelines, and the graphics pipelines the state lists name (the shipped list, merged from
// Longshot's assets/pipeline_states: hashes and states, no game code; the console's own list when there is one). Each pipeline
// built and let go at once. Nothing is written to the program cache: play translates its programs when first met, which costs
// milliseconds; what costs is the driver's compile, and that is what is prepared.
// Safety, the rules below: the free direct memory is read before each item is taken (fewer at once as it falls, a stop under
// a floor: the title then starts again with a heap of its own and goes on); the progress file says how far it got (an item that
// was being built when two starts in a row ended is left to play); a finished preparation for this translator, driver, census
// and lists does nothing again. HOOKSHOT_PS5_PREPARE=1 in hookshot_env.txt: offered on the Shaders page; =force: offered even
// when done; unset or 0: nothing of it, the launch screen as in lot 80.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace mkwvk::console_prep {

// ---- the switch ----
enum class Mode { kOff, kOn, kForce };
inline Mode mode_of(const char* v) {
    if (!v || !*v || !std::strcmp(v, "0")) return Mode::kOff;
    if (!std::strcmp(v, "force")) return Mode::kForce;
    return Mode::kOn;
}

// ---- the items, in the order they are made ----
/// First the mesh programs (their fallback's compute pipelines: the heaviest compiles, up to ~2.7 GiB of heap for one, made
/// while the heap is new), then every compute program of the census with its texture variants, then the lists' pipelines, a
/// program at a time
enum Kind { kMesh = 0, kCompute = 1, kGraphics = 2 };
struct Plan {
    size_t mesh = 0, compute = 0, groups = 0;   // (items of each kind)
    size_t records = 0;                         // (the lists' records, in the groups)
    size_t total() const { return mesh + compute + groups; }
};
inline Kind kind_of(const Plan& p, size_t i) { return i < p.mesh ? kMesh : i < p.mesh + p.compute ? kCompute : kGraphics; }
/// What one item costs, in seconds of one worker on the console, before any is measured: the PC's worker time for each kind
/// (build/lot82/runs/full: the PC model of the console's driver, 4 workers on a loaded PC, mesh 365 s for 1,312 programs,
/// compute 1,327 s for 4,128, the lists 5,268 s for 16,039 programs and 48,147 records; about 1.2 times what a PC with nothing
/// else running takes, the lots 16 and 18 say), divided by 1.2 and times 2.7, the ratio of 58 compiles the console made in play
/// to the PC's (notes-agents/preparation-premier-lancement.md 2.3). Only the first estimate: the time left comes from the rate
/// once there is one (eta_s)
constexpr double kSecMesh = 0.63, kSecCompute = 0.72, kSecGroup = 0.10, kSecRecord = 0.21;
inline double weight(const Plan& p, size_t i, size_t group_records) {
    switch (kind_of(p, i)) {
    case kMesh: return kSecMesh;
    case kCompute: return kSecCompute;
    default: return kSecGroup + kSecRecord * (double)group_records;
    }
}
inline double plan_seconds(const Plan& p) {
    return kSecMesh * (double)p.mesh + kSecCompute * (double)p.compute + kSecGroup * (double)p.groups + kSecRecord * (double)p.records;
}
/// The workers do not scale fully (the driver's cache lock, the heap's arenas): 85 % each past the first
inline double workers_speed(unsigned workers) { return workers <= 1 ? 1.0 : 1.0 + 0.85 * (double)(workers - 1); }
/// The mesh programs at most this many at once (Gate::mesh_at_once): the first step goes no wider whatever the workers
constexpr unsigned kMeshAtOnce = 2;
/// The whole preparation's first estimate, in seconds of the clock, with this many workers
inline double estimate_s(const Plan& p, unsigned workers) {
    const double mesh = kSecMesh * (double)p.mesh;
    return mesh / workers_speed(std::min(workers, kMeshAtOnce)) + (plan_seconds(p) - mesh) / workers_speed(workers);
}
/// Before the census and the lists are read (the launch screen's first look): the plan from their sizes alone. The census's
/// programs from its head; mesh and compute programs as this game's census has them (1,312 and 4,128 of 38,809); the lists'
/// records from their sizes (StateRec2: 552 bytes) and a program for every 3 of them (48,147 records in 16,039 programs)
inline Plan plan_from_sizes(uint32_t census_programs, uint64_t list_bytes) {
    Plan p;
    p.mesh = (size_t)census_programs * 1312 / 38809, p.compute = (size_t)census_programs * 4128 / 38809;
    p.records = list_bytes > 12 ? (size_t)((list_bytes - 12) / 552) : 0;
    p.groups = p.records / 3;
    return p;
}

// ---- the progress file: how far it got (text, a line a fact) ----
struct Progress {
    uint64_t stamp = 0;          // (what it was made for: the translator, the driver, the census, the lists, the recipe)
    uint64_t translator = 0, driver = 0;   // (two of those apart: the launch screen reads them without the census or the lists)
    size_t total = 0, done = 0;  // (items; every item before done is made, or left to play)
    double units_done = 0, units_total = 0;   // (the work, in weight's seconds: what is done of it, before done and beyond)
    bool finished = false;
    size_t started_at = (size_t)-1;   // (done when the last start began; -1: it stopped by itself, for memory or the player)
    unsigned starts = 0;              // (the starts that worked on it)
    double seconds = 0;               // (the time it took in all of them)
    double units = 0;                 // (and the work they did, in weight's seconds: the rate)
    std::vector<size_t> inflight, strikes, skip;
};
constexpr const char* kMagic = "grapnel-prepare 1";
inline std::string format(const Progress& p) {
    std::string s = std::string(kMagic) + "\n";
    char b[256];
    std::snprintf(b, sizeof b, "stamp %016llx\ntranslator %016llx\ndriver %016llx\ntotal %zu\ndone %zu\nfinished %d\n"
                  "started_at %lld\nstarts %u\nseconds %.1f\nunits %.1f\nunits_done %.1f\nunits_total %.1f\n",
                  (unsigned long long)p.stamp, (unsigned long long)p.translator, (unsigned long long)p.driver, p.total, p.done,
                  p.finished ? 1 : 0, p.started_at == (size_t)-1 ? -1LL : (long long)p.started_at, p.starts, p.seconds, p.units,
                  p.units_done, p.units_total);
    s += b;
    const auto list = [&](const char* name, const std::vector<size_t>& v) {
        s += name;
        for (size_t x : v) s += " " + std::to_string(x);
        s += "\n";
    };
    list("inflight", p.inflight), list("strikes", p.strikes), list("skip", p.skip);
    return s;
}
/// false: not a progress file of this kind (a new start then begins from the first item)
inline bool parse(const std::string& text, Progress* out) {
    Progress p;
    size_t at = 0;
    bool magic = false;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (line == kMagic) { magic = true; continue; }
        const size_t sp = line.find(' ');
        const std::string word = line.substr(0, sp), rest = sp == std::string::npos ? "" : line.substr(sp + 1);
        std::vector<size_t>* v = word == "inflight" ? &p.inflight : word == "strikes" ? &p.strikes : word == "skip" ? &p.skip : nullptr;
        if (v) {
            const char* c = rest.c_str();
            char* e = nullptr;
            for (;;) {
                const unsigned long long x = std::strtoull(c, &e, 10);
                if (e == c) break;
                v->push_back((size_t)x), c = e;
            }
            continue;
        }
        if (word == "stamp") p.stamp = std::strtoull(rest.c_str(), nullptr, 16);
        else if (word == "translator") p.translator = std::strtoull(rest.c_str(), nullptr, 16);
        else if (word == "driver") p.driver = std::strtoull(rest.c_str(), nullptr, 16);
        else if (word == "units_done") p.units_done = std::atof(rest.c_str());
        else if (word == "units_total") p.units_total = std::atof(rest.c_str());
        else if (word == "total") p.total = (size_t)std::strtoull(rest.c_str(), nullptr, 10);
        else if (word == "done") p.done = (size_t)std::strtoull(rest.c_str(), nullptr, 10);
        else if (word == "finished") p.finished = std::atoi(rest.c_str()) != 0;
        else if (word == "started_at") {
            const long long v2 = std::atoll(rest.c_str());
            p.started_at = v2 < 0 ? (size_t)-1 : (size_t)v2;
        } else if (word == "starts") p.starts = (unsigned)std::strtoul(rest.c_str(), nullptr, 10);
        else if (word == "seconds") p.seconds = std::atof(rest.c_str());
        else if (word == "units") p.units = std::atof(rest.c_str());
    }
    if (!magic || p.done > p.total) return false;
    *out = p;
    return true;
}
inline bool has(const std::vector<size_t>& v, size_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

/// A start's beginning: where it goes on from, and what it leaves to play now. p is the file as read (parsed or not: a Progress{}
/// when there was none); stamp and total are this start's. fresh: from the first item (none before, another stamp or count,
/// or force over a finished one)
struct Resume {
    size_t from = 0;
    bool fresh = true;
    std::vector<size_t> newly_skipped;   // (being built when two starts in a row ended: left to play from now on)
    const char* why = "";
};
inline Resume resume(Progress& p, uint64_t stamp, size_t total, unsigned workers, bool force) {
    Resume r;
    if (p.stamp != stamp || p.total != total || (force && p.finished) || !p.stamp) {
        r.why = !p.stamp ? "no earlier preparation" : p.stamp != stamp ? "another translator, driver, census or lists than the last one"
                : p.total != total ? "another count of items" : "asked again (HOOKSHOT_PS5_PREPARE=force)";
        p = Progress{};
    } else {
        r.fresh = false;
        r.why = p.finished ? "finished already" : "goes on where the last start stopped";
        // (an item that ends the process within the few seconds between two saves is in no snapshot: a start that got nowhere,
        // the first items not done count as in flight too, one a worker; a struck item is built alone (gate), so none after it,
        // and it alone when it is the first)
        if (!p.finished && p.started_at != (size_t)-1 && p.done == p.started_at)
            for (size_t w = 0; w < workers && p.done + w < p.total; w++) {
                const size_t i = p.done + w;
                if (has(p.skip, i)) continue;
                if (has(p.strikes, i) && w > 0) break;
                if (!has(p.inflight, i)) p.inflight.push_back(i);
                if (has(p.strikes, i)) break;
            }
        for (size_t i : p.inflight)
            if (has(p.strikes, i) && !has(p.skip, i)) p.skip.push_back(i), r.newly_skipped.push_back(i);
        p.strikes = p.inflight;
        p.inflight.clear();
    }
    p.stamp = stamp, p.total = total;
    r.from = std::min(p.done, total);
    p.started_at = r.from;
    p.done = r.from;
    if (!p.finished) p.starts++;
    return r;
}
/// The first item from done on that is neither made nor left to play: made[i] true for those this start made
inline size_t advance(size_t done, const std::vector<bool>& made, const std::vector<size_t>& skip) {
    while (done < made.size() && (made[done] || has(skip, done))) done++;
    return done;
}

// ---- the memory gate, before each item is taken ----
/// The free direct memory in MiB (-1: not known, a PC); how many items are being built, how many of them mesh programs; the
/// workers there are. kGo: take it; kWait: not now (memory, or another mesh program), take it when one ends; kStop: stop here
/// (under the floor, or a mesh program with no room even alone): the title starts again with a heap of its own and goes on
enum class Take { kGo, kWait, kStop };
struct Gate {
    int64_t floor_mb = 2048;        // (HOOKSHOT_PS5_PREPARE_MIN_FREE_MB: under it nothing is taken; the host's guard stops the title at 192)
    int64_t mesh_room_mb = 3072;    // (what a mesh program may need above the floor: ~2.7 GiB measured for the heaviest)
    int64_t all_mb = 5120;          // (above the floor: every worker; above half of it, half of them; else one)
    unsigned mesh_at_once = kMeshAtOnce;   // (mesh programs at once, at most: the two heaviest together would be ~5.5 GiB)
};
inline unsigned allowed(const Gate& g, int64_t free_mb, unsigned workers) {
    if (free_mb < 0) return workers;
    const int64_t above = free_mb - g.floor_mb;
    if (above >= g.all_mb) return workers;
    if (above >= g.all_mb / 2) return std::max(1u, workers / 2);
    return 1;
}
/// struck: the item was under way when the last start ended (Progress::strikes after resume): it is built alone, so that if it
/// ends this start too, it alone is left to play and not the items built beside it; solo_busy: such an item is being built
inline Take gate(const Gate& g, int64_t free_mb, Kind kind, unsigned busy, unsigned busy_mesh, unsigned workers, bool struck = false,
                 bool solo_busy = false) {
    if (free_mb >= 0 && free_mb < g.floor_mb) return Take::kStop;
    if (solo_busy || (struck && busy)) return Take::kWait;
    if (kind == kMesh) {
        if (busy_mesh >= g.mesh_at_once) return Take::kWait;
        if (free_mb >= 0 && free_mb - g.floor_mb < g.mesh_room_mb * (busy_mesh + 1)) return busy ? Take::kWait : Take::kStop;
    }
    if (busy >= allowed(g, free_mb, workers)) return Take::kWait;
    return Take::kGo;
}

// ---- why it stopped, and what the screen does then ----
enum class End { kNone, kFinished, kPlayer, kMemory, kStall, kNothing };
/// After a stop for memory the title starts again and goes on by itself (no button), only when this start got somewhere:
/// a start that made nothing before it ran short would start again for ever
inline bool goes_on_by_itself(End e, size_t started_at, size_t done) { return e == End::kMemory && done > started_at; }

// ---- what the Shaders page offers ----
struct Facts {
    Mode mode = Mode::kOff;
    bool census = false;          // (hookshot_shaders.bin beside the game, a census)
    bool same = false;            // (the progress file is for this translator and this driver: the census and the lists are
                                  // checked when it starts, console_prep::resume)
    bool finished = false;        // (and it finished)
    size_t done = 0, total = 0;   // (its count)
    bool caches_made = false;     // (the program cache of this build holds many programs and the driver's cache is large: play
                                  // made them, or a PC did)
};
enum class Offer { kOff, kNoCensus, kDone, kResume, kNeeded, kOptional };
constexpr size_t kMadePrograms = 1000;        // (caches_made: our console has 4,572 after weeks of play, a first hour ~600)
constexpr uint64_t kMadeDriverMB = 256;       // (and a driver cache of 1,009 MB; a full preparation makes ~250-400)
inline bool caches_made(uint64_t program_stamp, uint64_t stamp_now, size_t programs, uint64_t driver_mb) {
    return program_stamp == stamp_now && programs >= kMadePrograms && driver_mb >= kMadeDriverMB;
}
inline Offer offer(const Facts& f) {
    if (f.mode == Mode::kOff) return Offer::kOff;
    if (!f.census) return Offer::kNoCensus;
    if (f.same && f.finished && f.mode != Mode::kForce) return Offer::kDone;
    if (f.same && !f.finished && f.done > 0) return Offer::kResume;
    if (f.caches_made && f.mode != Mode::kForce) return Offer::kOptional;
    return Offer::kNeeded;
}
/// Whether the screen shows for it in its When needed mode (as for a shader check that finds a problem)
inline bool asks_for_screen(Offer o) { return o == Offer::kNeeded || o == Offer::kResume; }
/// Whether the Shaders page has its Prepare button
inline bool button(Offer o) { return o == Offer::kNeeded || o == Offer::kResume || o == Offer::kOptional; }

// ---- the time: the rate measured over every start (units of work done in seconds of the clock), else the first estimate ----
/// Seconds left: units_left at the rate every start so far had (units_worked in seconds of the clock), once 20 s and 1 % of the
/// work are behind it; before that planned_left_s, the first estimate of what is left (-1: none)
inline double eta_s(double units_worked, double seconds, double units_left, double planned_left_s) {
    if (units_left <= 0) return 0;
    if (seconds >= 20 && units_worked > 0 && units_worked >= (units_worked + units_left) * 0.01) return units_left / (units_worked / seconds);
    return planned_left_s;
}
/// "45 min", "1 h 05", "< 1 min" (the same in English and French)
inline std::string duration_text(double s) {
    if (s < 0) return "?";
    if (s < 60) return "< 1 min";
    const long m = std::lround(s / 60);
    char b[32];
    if (m < 60) std::snprintf(b, sizeof b, "%ld min", m);
    else std::snprintf(b, sizeof b, "%ld h %02ld", m / 60, m % 60);
    return b;
}
/// Percent done, by the work's units (whole: 99 until it is all done)
inline int percent(double units_done, double units_total, bool finished) {
    if (finished) return 100;
    if (units_total <= 0) return 0;
    return std::clamp((int)(units_done * 100 / units_total), 0, 99);
}

}  // namespace mkwvk::console_prep
