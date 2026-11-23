// (ps5, lot 69) The launch screen's quick shader check: what the cache files beside the game say, read without writing
// anything and without loading a program (launcher_logic.h judge turns these facts into the verdict). Only the files' heads
// and the program index are read whole (20 bytes a program: ~90 KB for 4 566); the pipeline notes are read in pieces for
// their programs' keys alone. No Vulkan: tests/test_launcher_check.py runs it on files it makes.
//
// The formats are the renderer's own (vk_progcache.cpp disk_init, vk_draw.cpp pipe_notes_refs, pipeline_share.h check_list):
//   hookshot_programs.bin   "HSP1" (or "HSP2", zstd), 4 bytes, then the 8-byte program cache stamp
//   hookshot_programs.idx   "HPIX", version 1, the same stamp, then 20 bytes a record: key, size, kind
//   hookshot_pipelines.bin  the stamp, the size of a note (sizeof PipeNote), then the notes: key, program key, ...
//   hookshot_pipeline_states.bin  "HKSTATE2" with 552-byte records (or "HKSTATE1", 528), the record size at byte 8
//   skip.txt                one program key a line (RX_VK_SKIP_FILE), '#' a comment
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "launcher_logic.h"

namespace mkwvk::launcher {

struct CheckPaths {
    std::string programs, index, notes, states, skip;
};

namespace check_detail {
inline int64_t file_size(FILE* f) {
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    return n;
}
}  // namespace check_detail

/// The facts from the files at p, for the stamp this build makes (program_cache_stamp); driver_bytes and driver_known are the
/// caller's (the driver's folder: vk_draw.cpp driver_cache_bytes)
inline ShaderFacts read_facts(const CheckPaths& p, uint64_t stamp) {
    using namespace check_detail;
    const auto t0 = std::chrono::steady_clock::now();
    ShaderFacts f;
    if (FILE* x = fopen(p.programs.c_str(), "rb")) {
        f.programs_present = true;
        const int64_t n = file_size(x);
        f.programs_bytes = n > 0 ? (uint64_t)n : 0;
        uint8_t head[16];
        if (n >= 16 && fread(head, 1, 16, x) == 16) {
            f.programs_magic = !memcmp(head, "HSP1", 4) || !memcmp(head, "HSP2", 4);
            memcpy(&f.programs_stamp, head + 8, 8);
        }
        fclose(x);
    }
    std::vector<uint64_t> keys;   // (the programs the index names, sorted: the notes' coverage)
    if (FILE* x = fopen(p.index.c_str(), "rb")) {
        f.index_present = true;
        const int64_t n = file_size(x);
        uint8_t head[16];
        if (n >= 16 && fread(head, 1, 16, x) == 16) {
            uint32_t version = 0;
            uint64_t s = 0;
            memcpy(&version, head + 4, 4), memcpy(&s, head + 8, 8);
            f.index_ok = !memcmp(head, "HPIX", 4) && version == 1 && s == stamp;
            if (f.index_ok) {
                const size_t records = (size_t)((n - 16) / 20);
                std::vector<uint8_t> all(records * 20);
                if (records && fread(all.data(), 1, all.size(), x) == all.size()) {
                    keys.resize(records);
                    for (size_t i = 0; i < records; i++) memcpy(&keys[i], all.data() + i * 20, 8);
                    std::sort(keys.begin(), keys.end());
                    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
                }
                f.programs = keys.size();
            }
        }
        fclose(x);
    }
    if (FILE* x = fopen(p.notes.c_str(), "rb")) {
        f.notes_present = true;
        const int64_t n = file_size(x);
        uint64_t head[2] = {0, 0};
        if (n >= 16 && fread(head, 8, 2, x) == 2 && head[0] == stamp && head[1] >= 16 && head[1] <= (1u << 20)) {
            f.notes_ok = true;
            const size_t note = (size_t)head[1];
            f.notes = (size_t)((n - 16) / (int64_t)note);
            // (the program keys, a note's second word, read a piece of notes at a time: never the whole file in memory)
            const size_t per = std::max<size_t>(1, (256u << 10) / note);
            std::vector<uint8_t> piece(per * note);
            for (size_t done = 0; done < f.notes;) {
                const size_t take = std::min(per, f.notes - done);
                if (fread(piece.data(), note, take, x) != take) {
                    f.notes = done;   // (cut short: what was read)
                    break;
                }
                for (size_t i = 0; i < take; i++) {
                    uint64_t prog = 0;
                    memcpy(&prog, piece.data() + i * note + 8, 8);
                    f.notes_covered += std::binary_search(keys.begin(), keys.end(), prog);
                }
                done += take;
            }
        }
        fclose(x);
    }
    if (FILE* x = fopen(p.states.c_str(), "rb")) {
        f.states_present = true;
        const int64_t n = file_size(x);
        char head[12];
        if (n >= 12 && fread(head, 1, 12, x) == 12) {
            uint32_t size = 0;
            memcpy(&size, head + 8, 4);
            const bool v2 = !memcmp(head, "HKSTATE2", 8) && size == 552, v1 = !memcmp(head, "HKSTATE1", 8) && size == 528;
            if (v1 || v2) f.states_ok = true, f.states = (size_t)((n - 12) / size);
        }
        fclose(x);
    }
    if (!p.skip.empty())
        if (FILE* x = fopen(p.skip.c_str(), "r")) {
            char line[256];
            while (fgets(line, sizeof line, x)) {
                const char* c = line;
                while (*c == ' ' || *c == '\t') c++;
                if (*c && *c != '#' && *c != '\r' && *c != '\n') f.skipped++;
            }
            fclose(x);
        }
    f.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return f;
}

}  // namespace mkwvk::launcher
