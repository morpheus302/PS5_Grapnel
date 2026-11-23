// (ps5) The GPU trail's rules, run alone: gpu/vk/gpu_trail.h (tests/test_gpu_trail.py compiles and runs this).
//   test_gpu_trail            the checks
//   test_gpu_trail --example  a small dump, as the stage trail would show it
//   test_gpu_trail --bench    what a draw's record call costs here
#include "../gpu/vk/gpu_trail.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace mkwvk::gpu_trail;

static int failed;
#define CHECK(cond, what) \
    do { \
        if (!(cond)) std::printf("FAIL %s (%s:%d: %s)\n", what, __FILE__, __LINE__, #cond), failed++; \
        else std::printf("PASS %s\n", what); \
    } while (0)

static std::atomic<uint64_t> fake_us;
static uint64_t clock_us() { return fake_us += 250; }

static Trail* make() {
    Trail* t = new Trail();
    t->now_us = clock_us;
    return t;
}
static Entry rec(uint64_t seq, uint64_t value, uint64_t batch, int slot, unsigned ncb = 2) {
    Entry e{};
    e.kind = E_REC, e.seq = seq, e.value = value, e.batch = batch, e.ncb = (uint16_t)ncb, e.slot = (uint8_t)slot, e.tid = 101768;
    return e;
}
static Entry flush(uint64_t batch, uint64_t signal, uint64_t recs, unsigned ncb, bool call = true) {
    Entry e{};
    e.kind = E_FLUSH, e.batch = batch, e.signal = signal, e.seq = recs, e.ncb = (uint16_t)ncb, e.reason = 4, e.tid = 101768;
    e.call = call ? CALL_IN : CALL_NONE;
    return e;
}
static std::vector<std::string> lines_of(Trail& t, const Now& now, unsigned id, size_t cap = 192u << 10, bool* cut = nullptr, int* count = nullptr) {
    std::vector<char> buf(cap);
    Text text;
    text.reset(buf.data(), buf.size());
    const int n = t.dump(now, text);
    if (cut) *cut = text.cut;
    if (count) *count = n;
    std::vector<std::string> out;
    emit(text, id, [&](const char* l) { out.push_back(l); });
    return out;
}
static bool has(const std::vector<std::string>& lines, const char* what) {
    for (const std::string& l : lines)
        if (l.find(what) != std::string::npos) return true;
    return false;
}

// ---- a recording's programs: one record a key, whatever the order of its uses ----
static void keys() {
    Trail* t = make();
    Acc& a = t->acc[3];
    a.reset();
    for (int i = 0; i < 40; i++) t->draw(a, 0x528c39df51bae6e5ull, K_DRAW);
    CHECK(a.nkeys == 1 && a.keys[0].draws == 40 && a.draws == 40 && a.keys[0].key == 0x528c39df51bae6e5ull, "40 draws of one program: one key, counted 40");
    CHECK((a.keys[0].flags & K_FIRST) && (a.keys[0].flags & K_DRAW), "its first use in the run is flagged");
    for (int i = 0; i < 300; i++) t->draw(a, 0x1000 + (uint64_t)(i % 7), i % 7 == 2 ? K_FALLBACK : K_DRAW);
    CHECK(a.nkeys == 8 && a.draws == 340, "seven more programs drawn in turn, 300 draws: eight keys");
    bool ok = true;
    for (int k = 1; k < 8; k++) ok = ok && a.keys[k].draws == (k - 1 < 300 % 7 ? 43 : 42);
    CHECK(ok, "each with its own count");
    CHECK((a.keys[3].flags & K_FALLBACK) && !(a.keys[2].flags & K_FALLBACK), "a fallback draw marks its key alone");
    t->dispatch(a, 0x53509044401d78d2ull, K_DISPATCH, 64, 36, 1);
    t->dispatch(a, 0x53509044401d78d2ull, K_DISPATCH, 8, 8, 2);
    t->dispatch(a, 0x53509044401d78d2ull, K_DISPATCH | K_INDIRECT, 1, 1, 1);
    const KeyRec& c = a.keys[8];
    CHECK(a.nkeys == 9 && c.disp == 3 && c.draws == 0 && c.groups == 64 * 36 + 128 + 1 && c.x == 64 && c.y == 36 && c.z == 1 && (c.flags & K_INDIRECT),
          "three dispatches of one program: its workgroups summed, its largest kept, indirect noted");
    CHECK(a.dispatches == 3 && a.fb_dispatches == 0, "counted as dispatches");
    t->dispatch(a, 0x1002, K_FB_DISPATCH, 16, 1, 1);
    CHECK(a.nkeys == 9 && a.keys[3].disp == 1 && a.keys[3].draws == 43 && (a.keys[3].flags & K_FB_DISPATCH) && a.fb_dispatches == 1,
          "the fallback's dispatch joins its program's key, beside its draws");
    // more programs than a recording keeps
    for (int i = 0; i < 400; i++) t->draw(a, 0x900000 + (uint64_t)i, K_DRAW);
    CHECK(a.nkeys == kAccKeys && a.key_lost == 400 - (kAccKeys - 9) && a.draws == 740, "past the keys a recording keeps, the uses are counted as lost and nothing is written past the table");
    for (int i = 0; i < 70000; i++) t->draw(a, 0x528c39df51bae6e5ull, K_DRAW);
    CHECK(a.keys[0].draws == 0xFFFF && a.draws == 70740, "a key's count stops at 65535, the recording's goes on");
    // the next recording of the slot starts empty; the programs are no longer new
    a.reset();
    t->draw(a, 0x528c39df51bae6e5ull, K_DRAW);
    CHECK(a.nkeys == 1 && a.keys[0].draws == 1 && !(a.keys[0].flags & K_FIRST), "the slot's next recording starts empty, and a program used before is not new again");
    CHECK(t->seen_n.load() == 8 + 1 + (kAccKeys - 9) && t->n_new_progs == t->seen_n.load(), "the run's programs: each counted once");
    delete t;
}

// ---- a recording's place by its command buffer: what a draw looks up, with no thread-local ----
static void places() {
    Trail* t = make();
    static char cb[400];   // (addresses to stand for command buffers)
    CHECK(!t->by_cmd(&cb[0]), "a command buffer no recording began in has no place");
    t->draw_cmd(&cb[0], 0x11, K_DRAW);
    t->dispatch_cmd(&cb[0], 0x12, K_DISPATCH, 1, 1, 1);
    CHECK(t->unplaced.load() == 2 && t->acc[0].nkeys == 0, "a draw and a dispatch into it are counted as unplaced, and written nowhere");
    bool ok = true;
    for (int s = 0; s < 48; s++) t->acc[s].reset(), ok = ok && t->bind_cmd(&cb[s * 8], &t->acc[s]);
    for (int s = 0; s < 48; s++) ok = ok && t->by_cmd(&cb[s * 8]) == &t->acc[s];
    CHECK(ok && !t->by_cmd(&cb[4]) && !t->bind_cmd(nullptr, &t->acc[0]), "48 recordings' command buffers each find their own; another finds none");
    t->draw_cmd(&cb[17 * 8], 0x528c39df51bae6e5ull, K_DRAW);
    t->draw_cmd(&cb[17 * 8], 0x528c39df51bae6e5ull, K_DRAW);
    t->dispatch_cmd(&cb[18 * 8], 0x53509044401d78d2ull, K_DISPATCH, 4, 4, 1);
    CHECK(t->acc[17].nkeys == 1 && t->acc[17].keys[0].draws == 2 && t->acc[18].dispatches == 1 && t->acc[16].empty() && t->unplaced.load() == 2,
          "a draw goes to the recording of the command buffer it is recorded into");
    CHECK(t->bind_cmd(&cb[17 * 8], &t->acc[20]) && t->by_cmd(&cb[17 * 8]) == &t->acc[20], "a command buffer given to another slot is that slot's from then on");
    ok = true;
    for (int i = 0; i < 400; i++) ok = t->bind_cmd(&cb[i], &t->acc[i % 48]) && ok;
    int found = 0;
    for (int i = 0; i < 400; i++) found += t->by_cmd(&cb[i]) != nullptr;
    CHECK(!ok && found >= kCmds / 2 && found <= kCmds, "the table takes 128 to 256 command buffers and says when it is full: nothing is written past it");
    Now now;
    std::vector<char> buf(64 << 10);
    Text text;
    text.reset(buf.data(), buf.size());
    t->dump(now, text);
    CHECK(std::string(buf.data(), text.len).find("2 draws and dispatches of the run were recorded into no known recording") != std::string::npos,
          "the dump says how many draws found no recording");
    delete t;
}

// ---- the other commands: folded by kind, dimension and format; the run's first uses ----
static void notes() {
    bool named = true;
    for (unsigned k = 0; k < KINDS; k++) {
        named = named && kind_info(k).name && std::strlen(kind_info(k).name) > 3 && std::strlen(kind_info(k).name) < 48;
        for (unsigned j = 0; j < k; j++) named = named && std::strcmp(kind_info(j).name, kind_info(k).name);
    }
    CHECK(named && !std::strcmp(kind_info(KINDS).name, "?") && !std::strcmp(kind_info(200).name, "?"), "every kind has a name of its own, and a number past them reads ?");
    Trail* t = make();
    Acc& a = t->acc[0];
    a.reset();
    for (int i = 0; i < 5; i++) t->note(&a, UP_STAGING, 2, 37, 0x25, 256u << (i % 3), 256, 1, 0);
    CHECK(a.ndet == 1 && a.det[0].count == 5 && a.det[0].w == 1024 && a.det[0].h == 256 && (a.det[0].flags & D_FIRST) && (a.det[0].flags & D_DIM) == 2,
          "five staging uploads of one format: one detail, the largest extent, first of the run");
    t->note(&a, UP_STAGING, 2, 43, 0x38, 64, 64, 1, 0);
    t->note(&a, UP_STAGING, 3, 37, 0x25, 32, 32, 32, 0);
    CHECK(a.ndet == 3 && !(a.det[1].flags & D_FIRST) && (a.det[2].flags & D_FIRST), "another format folds apart and is not a first (uploads go by dimension); a 3D one is");
    t->note(&a, CLEAR_NEW_COLOR, 3, 37, 0x25, 32, 32, 32, 0);
    t->note(&a, UP_ZERO_COLOR, 3, 37, 0x25, 32, 32, 32, 0);
    t->note(&a, CLEAR_NEW_COLOR, 3, 122, 0x3f, 128, 72, 96, 0);
    CHECK(a.ndet == 6 && (a.det[3].flags & D_FIRST) && (a.det[4].flags & D_FIRST) && (a.det[5].flags & D_FIRST),
          "a new image's clear and the zero upload's are kinds of their own, and a clear's first use goes by format too");
    t->note(&a, WRITE_BACK, 0, 0, 0, 0, 0, 0, 65536);
    t->note(&a, WRITE_BACK, 0, 0, 0, 0, 0, 0, 4096);
    t->note(&a, IMG_DESTROY, 0, 0, 0, 0, 0, 0, 181230);
    t->note(&a, IMG_DESTROY, 0, 0, 0, 0, 0, 0, 181100);
    CHECK(a.ndet == 8 && a.det[6].num() == 69632 && a.det[6].count == 2 && a.det[7].num() == 181230 && a.det[7].count == 2,
          "bytes are summed, a value keeps its newest");
    CHECK(t->n_whats == 7, "seven rows in the run's table (the two staging formats share one)");
    // a recording with more (kind, format) than it keeps: folded into its kind, then counted as lost
    for (uint32_t f = 0; f < 40; f++) t->note(&a, CLEAR_ATT_COLOR, 2, 1000 + f, 0, 1536, 864, 1, 0);
    int folded = 0;
    for (int i = 0; i < a.ndet; i++) folded += (a.det[i].flags & D_FOLDED) != 0;
    CHECK(a.ndet == kAccDet && folded == 1 && a.det_lost == 0, "past the details a recording keeps, a kind's formats fold into one of its details");
    t->note(&a, COPY_T2T, 2, 37, 0, 4, 4, 1, 0);
    CHECK(a.det_lost == 1, "and a kind with none of its own there is counted as lost");
    CHECK(t->n_whats == 7 + 40 + 1 && t->whats_lost == 0, "the run's table has them all");
    // outside any recording: for the next entry
    t->note(nullptr, IMPORT, 0, 0, 0, 0, 0, 0, 4u << 20);
    CHECK(t->loose.ndet == 1 && !t->loose.empty(), "a note outside any recording waits for the next entry");
    const uint64_t n = t->push(rec(1, 100, 1, 0), &a);
    const Entry& e = t->entries[0];
    CHECK(n == 1 && e.ordinal == 1 && e.det_n == kAccDet + 1 && e.det_lost == 1 && a.empty() && t->loose.empty(),
          "the entry takes the recording's details and the waiting notes, and both are empty after");
    CHECK(t->dets[(e.det_first + kAccDet) % kDetRing].kind == IMPORT && t->dets[(e.det_first + kAccDet) % kDetRing].num() == (4u << 20), "the import is its last detail");
    delete t;
}

// ---- the rings wrap: the newest kEntries entries are kept, and an entry says when its keys were overwritten ----
static void wrap() {
    Trail* t = make();
    const uint64_t total = kEntries * 2 + 137;
    for (uint64_t i = 1; i <= total; i++) {
        Acc& a = t->acc[i % 48];
        a.reset();
        for (int k = 0; k < 30; k++) t->draw(a, (i << 16) | (uint64_t)k, K_DRAW);
        t->note(&a, CLEAR_ATT_COLOR, 2, 97, 0, 1536, 864, 1, 0);
        t->push(rec(i, 5000 + i / 8, 1 + i / 16, (int)(i % 48)), &a);
    }
    CHECK(t->n_entries == total && t->oldest() == total - kEntries + 1, "2,185 entries written: the oldest kept is the 1,162nd");
    bool ok = true;
    for (uint64_t o = t->oldest(); o <= total; o++) ok = ok && t->entries[(o - 1) % kEntries].ordinal == o && t->entries[(o - 1) % kEntries].seq == o;
    CHECK(ok, "every entry kept is in its place");
    const Entry& last = t->entries[(total - 1) % kEntries];
    ok = last.key_n == 30;
    for (unsigned k = 0; k < last.key_n; k++) ok = ok && t->keys[(last.key_first + k) % kKeyRing].key == ((total << 16) | k);
    CHECK(ok, "the newest entry's 30 keys are its own, across the key ring's end");
    // 30 keys an entry, 16,384 kept: the newest 546 entries have theirs
    const Entry& old = t->entries[(t->oldest() - 1) % kEntries];
    CHECK(old.key_first + kKeyRing < t->n_keys, "the oldest entry's keys are known to be overwritten");
    Now now;
    now.batch_done = now.rec_done = now.value_done = ~0ull;
    std::vector<char> buf(64 << 10);
    Text text;
    text.reset(buf.data(), buf.size());
    t->lock();
    t->entry_text(old, ST_UNKNOWN, text);
    t->entry_text(last, ST_UNKNOWN, text);
    t->unlock();
    const std::string all(buf.data(), text.len);
    CHECK(all.find("its 30 program keys were overwritten") != std::string::npos, "and its line says so instead of showing another entry's");
    char want[40];
    std::snprintf(want, sizeof want, " %016llx:1", (unsigned long long)((total << 16) | 29));
    CHECK(all.find(want) != std::string::npos, "the newest shows its own");
    // 65,550 programs in this run: more than the run's table takes
    CHECK(t->seen_full() && t->seen_n.load() == kSeen / 4 * 3 && all.find("*:1") == std::string::npos, "past 12,288 programs no use is called a first any more");
    Text first;
    first.reset(buf.data(), buf.size());
    t->dump(now, first);
    CHECK(std::string(buf.data(), first.len).find("12288 in the run (the table is full: no more are told)") != std::string::npos, "and the dump says so");
    delete t;
}

// ---- done, not done, held, in the driver's call: what a dump says of each entry ----
static Trail* stalled(int frames, int stuck_at) {   // (frames of 6 recordings and a flush; the flush of frame stuck_at never returns)
    Trail* t = make();
    uint64_t seq = 0;
    for (int f = 1; f <= frames; f++) {
        for (int r = 0; r < 6; r++) {
            Acc& a = t->acc[r];
            a.reset();
            for (int d = 0; d < 118; d++) t->draw(a, 0x528c39df51bae6e5ull + (uint64_t)((d / 5) % 23) * 0x1111, d % 40 == 7 ? K_FALLBACK : K_DRAW);
            t->dispatch(a, 0x53509044401d78d2ull + (uint64_t)r, K_DISPATCH, 64, 36, 1);
            if (f == 1 && r == 2) t->note(&a, UP_ZERO_COLOR, 3, 122, 0x3f, 128, 72, 96, 0);   // (the run's one zero upload)
            t->note(&a, CLEAR_ATT_COLOR, 2, 97, 0, 1536, 864, 1, 0);
            t->push(rec(++seq, 1000 + (uint64_t)f, (uint64_t)f, r), &a);
        }
        const uint64_t o = t->push(flush((uint64_t)f, 1000 + (uint64_t)f, seq, 12), nullptr);
        if (f != stuck_at) t->back(o, 0);
        else break;
    }
    return t;
}
static void states() {
    Trail* t = stalled(40, 40);
    Now now;
    now.value_done = 1037, now.rec_done = 37 * 6, now.batch_done = 37, now.handed = 1040, now.us = fake_us.load();
    int count = 0;
    const std::vector<std::string> l = lines_of(*t, now, 1, 192u << 10, nullptr, &count);
    CHECK((int)l.size() == count && count > 0, "the dump's lines are numbered to their count");
    CHECK(has(l, "batches done to 37 of 40 sent") && has(l, "recordings done to 222 of 240 submitted") && has(l, "values done to 1037 of 1040 handed out"),
          "its first line gives the three timelines against what was submitted");
    CHECK(has(l, "THE DRIVER'S CALL IS NOT BACK: #280 flush batch 40 (12 cb)"), "and the call into the driver that did not return");
    CHECK(has(l, "21 entries not done, the first #260 rec 223 (batch 38)"), "and the first entry not done: the first recording of batch 38");
    CHECK(has(l, "#259 flush done: batch 37 by vkQueueSubmit2") && has(l, "#260 rec 223 NOT DONE: batch 38") && has(l, "#266 flush NOT DONE: batch 38") &&
              has(l, "#274 rec 235 CALL NOT BACK: batch 40") && has(l, "#280 flush CALL NOT BACK: batch 40"),
          "done up to batch 37's flush, not done from batch 38, and batch 40's recordings in a call that is not back");
    CHECK(has(l, "#252 flush done: batch 36") && has(l, "#253 rec 217 done") && !has(l, "#251 rec"), "eight done entries are shown before the first that is not, none older");
    CHECK(has(l, "upload:zero/ClearColorImage 3D fmt 122/0x3f: x1,"), "the run's first uses keep the one zero upload, 39 frames after it left the entries shown");
    CHECK(has(l, "end of the dump: the first entry not done is #260, 21 are not done"), "the last line says it again");
    size_t longest = 0;
    bool prefixed = true;
    for (size_t i = 0; i < l.size(); i++) {
        char want[80];
        std::snprintf(want, sizeof want, "gpu trail: d1 %zu/%zu: ", i + 1, l.size());
        prefixed = prefixed && l[i].rfind(want, 0) == 0;
        longest = std::max(longest, l[i].size());
    }
    CHECK(prefixed, "every line says which dump it is of and where in it");
    // (the stage trail's line: 1,024 bytes with its "[%8.3f] " stamp, host/posix/ps5_early.c vtrail; the log's the same)
    CHECK(longest <= 1000, "no line is longer than 1,000 characters");
    std::printf("     (the dump: %zu lines, the longest %zu characters)\n", l.size(), longest);

    // the timelines one step on: nothing is pending but the call
    now.batch_done = 39, now.rec_done = 234, now.value_done = 1039;
    CHECK(t->moved(now), "a timeline that moved is news");
    const std::vector<std::string> l2 = lines_of(*t, now, 2);
    CHECK(has(l2, "7 entries not done, the first #274 rec 235 (batch 40)") && has(l2, "#273 flush done: batch 39"), "one step on, only the batch in the call is not done");
    CHECK(!t->moved(now), "asked again with nothing written and no timeline moved: nothing new to say");
    uint64_t written = 0, call = 0;
    CHECK(!t->written_since_dump(&written, &call) && written == 280 && call == 280, "nor before the driver is asked");
    // the call returns, a recording is held for the next batch, a present goes out
    t->back(280, 0);
    Acc& a = t->acc[7];
    a.reset();
    t->draw(a, 0x77, K_MESH);
    t->push(rec(241, 1041, 41, 7), &a);
    CHECK(t->written_since_dump(&written, &call) && written == 281 && call == 0, "an entry written since is news, and no call is open");
    const std::vector<std::string> l3 = lines_of(*t, now, 3);
    CHECK(has(l3, "no submit is in the driver's call") && has(l3, "#281 rec 241 held (not sent yet): batch 41") && has(l3, "0000000000000077*:1M"),
          "a recording whose batch was not sent is held, and a mesh shader draw is marked M");
    CHECK(!has(l3, "vkQueuePresentKHR"), "no present is in the driver's call");
    t->other_begin("vkQueuePresentKHR");
    const std::vector<std::string> l3p = lines_of(*t, now, 4);
    t->other_end();
    CHECK(has(l3p, " vkQueuePresentKHR, ENTERED AT 4") && has(l3p, " s, IS NOT BACK;") && !has(lines_of(*t, now, 5), "vkQueuePresentKHR"),
          "a present that is in the driver's call is said, with when it entered, and no longer once it is back");
    CHECK(has(l3, "#280 flush NOT DONE: batch 40") && has(l3, "call 0."), "the flush that came back is not done yet, and says how long its call took");
    delete t;

    // entries the timelines say nothing of: done when a later one is, not known after the last that is
    t = make();
    t->back(t->push(flush(1, 10, 0, 4), nullptr), 0);
    Entry p{};
    p.kind = E_PRESENT, p.ncb = 1, p.call = CALL_IN, p.tid = 7;
    t->back(t->push(p, nullptr), 0);
    t->back(t->push(flush(2, 11, 0, 4), nullptr), 0);
    t->back(t->push(p, nullptr), 0);
    Now n2;
    n2.batch_done = 2, n2.rec_done = 0, n2.value_done = 11, n2.handed = 11;
    const std::vector<std::string> l4 = lines_of(*t, n2, 1);
    CHECK(has(l4, "#2 present done (a later one is)") && has(l4, "#4 present not known") && has(l4, "every entry kept is done or not known"),
          "a present before a batch that is done is done; the last one, after it, is not known");
    Now n3;   // (the driver gave no answer)
    const std::vector<std::string> l5 = lines_of(*t, n3, 2);
    CHECK(has(l5, "batches done to ? of 2 sent") && has(l5, "#3 flush not known"), "with no answer from the driver nothing is called done or not done");
    delete t;

    // a recording submitted by itself (no batching): its own number on the recordings' timeline
    t = make();
    Entry own = rec(1, 50, 0, 0);
    own.call = CALL_IN, own.signal = 50;
    t->back(t->push(own, nullptr), 0);
    own.seq = 2, own.value = own.signal = 51;
    t->push(own, nullptr);
    Now n4;
    n4.rec_done = 1, n4.value_done = 50, n4.handed = 51;
    const std::vector<std::string> l6 = lines_of(*t, n4, 1);
    CHECK(has(l6, "#1 rec 1 done: its own vkQueueSubmit2, value 50, signals 50") && has(l6, "#2 rec 2 CALL NOT BACK: its own vkQueueSubmit2"),
          "without batches a recording is its own submit: done by its number, or in the driver's call");
    delete t;
}

// ---- the dump's size: many entries not done, entries with every key they can keep, a buffer too small ----
static void sizes() {
    Trail* t = make();
    for (uint64_t i = 1; i <= 300; i++) {
        Acc& a = t->acc[i % 48];
        a.reset();
        for (int k = 0; k < kAccKeys; k++) {
            t->draw(a, 0xfedcba9800000000ull | (i << 12) | (uint64_t)k, K_FALLBACK);
            t->dispatch(a, 0xfedcba9800000000ull | (i << 12) | (uint64_t)k, K_FB_DISPATCH | K_INDIRECT, 65535, 65535, 65535);
        }
        for (uint32_t f = 0; f < (uint32_t)kAccDet; f++) t->note(&a, CLEAR_NEW_DS, 3, 1000000000u + f, 0xff, 65535, 65535, 65535, 0);
        t->push(rec(i, 9000 + i, 1 + i / 4, (int)(i % 48)), &a);
        if (i % 4 == 3) t->back(t->push(flush(1 + i / 4, 9000 + i, i, 8), nullptr), 0);
    }
    Now now;
    now.batch_done = 20, now.rec_done = 79, now.value_done = 9079, now.handed = 9300, now.us = fake_us.load();
    bool cut = false;
    const std::vector<std::string> l = lines_of(*t, now, 7, 192u << 10, &cut);
    size_t longest = 0, entries = 0;
    for (const std::string& s : l) {
        longest = std::max(longest, s.size());
        entries += s.find(": #") != std::string::npos && s.find("(more)") == std::string::npos;
    }
    CHECK(!cut, "fifty entries with every key and detail they can keep fit the dump's 192 KB");
    CHECK(longest <= 1000, "and no line of it is longer than 1,000 characters");
    CHECK(entries == (size_t)(kBefore + kFirstBad + kTail) && kBefore == 8 && kFirstBad == 34 && kTail == 8,
          "eight done, thirty-four from the first not done (a batch of 32 command buffers and its flush fit), the newest eight");
    CHECK(has(l, "are not shown:") && has(l, "left out)"), "and it says which entries it leaves out");
    CHECK(!has(l, "IT IS THE OLDEST ENTRY KEPT"), "the first entry not done is not the oldest kept: nothing says it may be older");
    // the same with nothing done among the entries kept: the first that did not finish may have been overwritten
    for (uint64_t i = 301; i <= 300 + kEntries; i++) t->push(rec(i, 9000 + i, 1 + i / 4, (int)(i % 48)), nullptr);
    now.batch_done = 60, now.rec_done = 239;
    const std::vector<std::string> old = lines_of(*t, now, 9);
    CHECK(has(old, "IT IS THE OLDEST ENTRY KEPT: the first that did not finish may be older, and overwritten"),
          "when the oldest entry kept is not done, the dump says the first that did not finish may be older");
    CHECK(has(l, " (more): "), "an entry's keys go on in lines that say whose they are");
    std::printf("     (the largest dump: %zu lines, the longest %zu characters)\n", l.size(), longest);
    // a buffer too small: cut, said, and the last line still there
    const std::vector<std::string> small = lines_of(*t, now, 8, 8u << 10, &cut);
    CHECK(cut && !small.empty() && small.back().find("end of the dump") != std::string::npos && small.back().find("THE TEXT WAS CUT") != std::string::npos,
          "a buffer too small cuts the dump, which says so in a last line it kept room for");
    delete t;

    // an empty trail
    t = make();
    Now none;
    const std::vector<std::string> e = lines_of(*t, none, 1);
    CHECK(e.size() == 4 && has(e, "the ring keeps #0 to #0") && has(e, "first uses in the run, oldest first") && has(e, " none"), "nothing recorded: four lines that say so");
    delete t;
}

// ---- waits, as the watcher sees them ----
static void waits() {
    static Waits w;
    const int a = w.begin("submission value", 181234, 42);
    CHECK(a == 0 && w.overdue(6) < 0, "a wait just begun is not overdue");
    for (int i = 0; i < 5; i++) w.advance();
    CHECK(w.overdue(6) < 0, "nor after five ticks");
    w.advance();
    const int b = w.begin("a recording slot's batch", 52111, 43);
    CHECK(w.overdue(6) == a && w.slot[a].value.load() == 181234 && w.slot[a].tid.load() == 42, "at the sixth it is told, with what it waits for");
    CHECK(w.overdue(6) < 0, "once");
    w.end(a);
    for (int i = 0; i < 6; i++) w.advance();
    CHECK(w.overdue(6) == b && w.overdue(6) < 0, "the other one in its turn");
    const int c = w.begin("submission value", 1, 44);
    CHECK(c == a && w.overdue(6) < 0, "an ended wait's slot serves again, not overdue");
    w.end(b), w.end(c);
    int slots[kWaits + 1];
    for (int i = 0; i <= kWaits; i++) slots[i] = w.begin("x", (uint64_t)i, 1);
    CHECK(slots[kWaits - 1] == kWaits - 1 && slots[kWaits] == -1, "sixteen waits are watched at once; one more is not, and still waits as before");
    for (int i = 0; i < kWaits; i++) w.end(slots[i]);
}

// ---- several threads: recordings on their own threads, notes from others, one that dumps ----
static void threads() {
    Trail* t = make();
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> seq{0}, pushed{0};
    const auto recorder = [&](int slot) {
        for (int i = 0; i < 4000; i++) {
            Acc& a = t->acc[slot];
            a.reset();
            for (int d = 0; d < 60; d++) t->draw(a, 0xabc000 + (uint64_t)((d * 7 + slot) % 50), K_DRAW);
            t->note(&a, UP_STAGING, 2, 37, 0x25, 64, 64, 1, 0);
            t->back(t->push(rec(++seq, 1, 0, slot), &a), 0);
            pushed++;
        }
    };
    const auto noter = [&] {
        while (!stop) t->note(nullptr, IMPORT, 0, 0, 0, 0, 0, 0, 4096), std::this_thread::yield();
    };
    std::atomic<int> dumps{0};
    const auto dumper = [&] {   // (a dump holds the trail's lock while it formats: a few a run in the renderer, one a millisecond here)
        std::vector<char> buf(192u << 10);
        while (!stop) {
            Now now;
            now.rec_done = seq.load() / 2;
            Text text;
            text.reset(buf.data(), buf.size());
            dumps += t->dump(now, text) > 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    std::thread r1(recorder, 1), r2(recorder, 2), r3(recorder, 3), n1(noter), d1(dumper);
    r1.join(), r2.join(), r3.join();
    stop = true;
    n1.join(), d1.join();
    uint64_t draws = 0, uploads = 0;
    for (int i = 0; i < t->n_whats; i++)
        if (t->whats[i].kind == UP_STAGING) uploads = t->whats[i].count;
    for (uint64_t o = t->oldest(); o <= t->n_entries; o++) draws += t->entries[(o - 1) % kEntries].draws;
    CHECK(t->n_entries == 12000 && uploads == 12000 && draws == (uint64_t)kEntries * 60 && t->seen_n.load() == 50 && dumps.load() > 0,
          "three threads recording, one noting, one dumping: 12,000 entries, each with its own 60 draws, 50 programs seen");
    delete t;
}

// ---- what a draw's record call costs: gt_draw as vk_gputrail.h has it, the switch and the look-up by command buffer ----
static std::atomic<int> g_state{1};
static Trail* g_trail;
static Acc* g_acc;
static char g_cmds[48 * 64];   // (48 recordings' command buffers)
static const void* g_cmd = &g_cmds[17 * 64];
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static void gt_draw_like(const void* cmd, uint64_t key, uint8_t flags) {
    if (g_state.load(std::memory_order_acquire) <= 0) return;
    g_trail->draw_cmd(cmd, key, flags);
}
static double per_call_ns(const std::vector<uint64_t>& keys, int rounds, int per_recording) {
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t calls = 0;
    for (int r = 0; r < rounds; r++)
        for (size_t i = 0; i < keys.size(); i++, calls++) {
            if (per_recording && i % (size_t)per_recording == 0) g_acc->reset();
            gt_draw_like(g_cmd, keys[i], K_DRAW);
        }
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (double)calls;
}
static void bench() {
    g_trail = make();
    for (int s = 0; s < 48; s++) g_trail->acc[s].reset(), g_trail->bind_cmd(&g_cmds[s * 64], &g_trail->acc[s]);
    g_acc = &g_trail->acc[17];
    uint64_t x = 0x9E3779B97F4A7C15ull;
    const auto rnd = [&] { return x ^= x << 13, x ^= x >> 7, x ^= x << 17, x; };
    std::vector<uint64_t> same(120000, 0x528c39df51bae6e5ull), runs, each;
    // a frame as the game's: 3,000 draws in recordings of 120, a program for 1 to 8 draws in a row, out of 300 programs
    while (runs.size() < 120000) {
        const uint64_t key = 0x1000000000000000ull | (rnd() % 300) << 20;
        for (uint64_t n = 1 + rnd() % 8; n && runs.size() < 120000; n--) runs.push_back(key);
    }
    for (size_t i = 0; i < 120000; i++) each.push_back(0x2000000000000000ull | (uint64_t)(i % 120) << 8 | (i / 120) % 3);
    std::printf("a draw's record call, a recording of 120 draws (nanoseconds a call, best of 5):\n");
    const auto best = [](const std::vector<uint64_t>& k, int per) {
        double b = 1e9;
        for (int i = 0; i < 5; i++) b = std::min(b, per_call_ns(k, 40, per));
        return b;
    };
    const double a = best(same, 120), b = best(runs, 120), c = best(each, 120);
    g_state = -1;
    const double off = best(runs, 0);
    std::printf("  the same program every draw: %.2f\n  programs in runs of 1 to 8 draws, 300 programs: %.2f\n  a program a draw, 120 in the recording: %.2f\n"
                "  the trail off: %.2f\n", a, b, c, off);
    std::printf("  at 4,000 draws a frame and 32 frames a second, the runs' figure is %.2f ms a second of the replay's thread\n", b * 4000 * 32 / 1e6);
    // the rare calls: a note (the trail's lock, the run's table, the recording's details), and a submit's entry with 30 keys
    g_state = 1;
    g_acc->reset();
    const int kNotes = 2000000;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kNotes; i++) g_trail->note(g_acc, i & 1 ? UP_STAGING : CLEAR_NEW_COLOR, 2, 37 + (uint32_t)(i & 6), 0x25, 256, 256, 1, 0);
    const double note_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kNotes;
    const int kPushes = 200000;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kPushes; i++) {
        g_acc->reset();
        for (int k = 0; k < 30; k++) g_trail->draw(*g_acc, 0x3000000000000000ull | (uint64_t)k, K_DRAW);
        Entry e{};
        e.kind = E_REC, e.seq = (uint64_t)i + 1, e.batch = 1 + (uint64_t)i / 16, e.ncb = 2;
        g_trail->push(e, g_acc);
    }
    const double push_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kPushes;
    std::printf("  a note (an upload, a clear): %.1f; a submit's entry with its 30 keys, their draws included, the test's clock: %.0f\n", note_ns, push_ns);
    // a dump's text, formatted with the lock held: the largest the trail makes
    Trail* big = make();
    for (uint64_t i = 1; i <= 300; i++) {
        Acc& a = big->acc[i % 48];
        a.reset();
        for (int k = 0; k < kAccKeys; k++) big->dispatch(a, 0xfedcba9800000000ull | (i << 12) | (uint64_t)k, K_FB_DISPATCH | K_INDIRECT, 65535, 65535, 65535);
        Entry e{};
        e.kind = E_REC, e.seq = i, e.batch = 1 + i / 4, e.ncb = 2;
        big->push(e, &a);
    }
    std::vector<char> buf(192u << 10);
    Now now;
    now.batch_done = 20, now.rec_done = 79;
    double dump_us = 1e9;
    int lines = 0;
    for (int i = 0; i < 20; i++) {
        Text text;
        text.reset(buf.data(), buf.size());
        t0 = std::chrono::steady_clock::now();
        lines = big->dump(now, text);
        dump_us = std::min(dump_us, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }
    std::printf("  a dump's text, the trail's lock held: %.0f microseconds for the largest (%d lines)\n", dump_us, lines);
}

static void example() {
    Trail* t = stalled(3, 3);
    t->note(nullptr, IMG_DESTROY, 0, 0, 0, 0, 0, 0, 1001);
    Now now;
    now.value_done = 1001, now.rec_done = 6, now.batch_done = 1, now.handed = 1003, now.us = fake_us.load() + 5000000;
    for (const std::string& l : lines_of(*t, now, 1)) std::printf("%s\n", l.c_str());
}

int main(int argc, char** argv) {
    fake_us = 470000000;
    if (argc > 1 && !std::strcmp(argv[1], "--bench")) return bench(), 0;
    if (argc > 1 && !std::strcmp(argv[1], "--example")) return example(), 0;
    std::printf("     (the trail: %zu KB; an entry %zu bytes, a key %zu, a detail %zu, an open recording %zu)\n", sizeof(Trail) >> 10, sizeof(Entry),
                sizeof(KeyRec), sizeof(Detail), sizeof(Acc));
    keys();
    places();
    notes();
    wrap();
    states();
    sizes();
    waits();
    threads();
    if (failed) return std::printf("FAILED: %d\n", failed), 1;
    std::printf("PASSED: 0 failed\n");
    return 0;
}
