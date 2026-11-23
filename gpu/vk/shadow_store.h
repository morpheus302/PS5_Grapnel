// (hookshot, ps5, 2026-11-15) The texture shadow's 4 KB copies (vk_memory.cpp, "texture storage in the capture") in blocks
// of their own, 4 MB (1024 pages) each, and the blocks given back to the system when they hold nothing.
//
// Each copy was a 4 KB allocation of the host heap: 47,000 of them (185 MB) in Hyrule Field on the console, beside the hash
// table's nodes and the replay thread's other allocations, in the 16 MB blocks of the console's heap (dlmalloc), which gives
// a block back only when all of it is free. When the field's copies went, the heap kept their blocks: +288 MB at the field's
// arrival (1,256 -> 1,544 MB) that never came back (logs/udp/essai-lot33-1.txt, notes-agents/captures-tas-hote.md).
//
// Here a copy is a slot of a block. A page goes into the lowest-numbered block that holds pages and has room, then into the
// lowest empty block held, and only then into a new block: the copies stay packed at the low end. A block that empties waits
// before it goes back: a take frees every page the game rewrote before it copies them again, and the sweeps drop thousands of
// copies that the next takes bring back (a block given back at once was made again at once: 324 and then, given back at the
// take's end only, 207 blocks made in a 5-minute PC run). compact() gives back the empty blocks that have waited `empty_calls`
// calls, and the lowest-numbered one (the spare) after `spare_calls`; then it empties the block that holds the fewest pages into
// the others' room, a few pages a call, once the blocks in use have a block and a half of room between them. So what is held
// is the pages' own size, rounded up to a block, plus at most a block and a half of room, plus the blocks emptied in the last
// `empty_calls` calls and the spare.
//
// No locking here: the caller's (shadow_mu). A moved page's copy is at its new slot before moved() is called, and the owner's
// entry must take the new slot there (its old slot is free from then on).
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mkwvk {

class ShadowSlabs {
public:
    static constexpr uint32_t kPages = 1024;                       // (pages a block: 4 MB)
    static constexpr uint64_t kBytes = (uint64_t)kPages << 12;
    static constexpr uint32_t kMaxBlocks = 128;                    // (512 MB: twice what the shadow may hold, kShadowMax)
    static constexpr uint32_t kNone = ~0u;
    using MakeFn = void* (*)(size_t bytes);
    using GiveFn = void (*)(void* p, size_t bytes);

    ShadowSlabs(MakeFn make, GiveFn give, uint64_t spare_calls = 3600, uint64_t empty_calls = 600)
        : make_(make), give_(give), spare_calls_(spare_calls), empty_calls_(empty_calls) {
        blocks_.reserve(kMaxBlocks);   // (never moved: compact() keeps a reference across place())
    }
    ShadowSlabs(const ShadowSlabs&) = delete;
    ShadowSlabs& operator=(const ShadowSlabs&) = delete;
    ~ShadowSlabs() {
        for (Block& b : blocks_)
            if (b.mem) give_(b.mem, kBytes);
    }

    /// The 4 KB of a slot take() gave
    uint8_t* at(uint32_t slot) const { return blocks_[slot / kPages].mem + (uint64_t)(slot % kPages) * 4096; }
    /// The page a slot holds the copy of (kNone: a free slot)
    uint32_t owner(uint32_t slot) const { return blocks_[slot / kPages].owner[slot % kPages]; }

    /// A slot for page's copy (its bytes are not set); kNone when no block could be made
    uint32_t take(uint32_t page) {
        int i = lowest_room();
        if (i < 0) i = lowest(empty_bits_);
        if (i < 0) i = make_block();
        if (i < 0) return kNone;
        return place((uint32_t)i, page);
    }
    /// The slot is free again (its owner's entry is gone)
    void give(uint32_t slot) {
        const uint32_t i = slot / kPages, k = slot % kPages;
        Block& b = blocks_[i];
        b.owner[k] = kNone;
        b.room.push_back((uint16_t)k);
        b.used--;
        pages_--;
        settle(i);
    }
    /// Up to budget pages moved out of the block that holds the fewest, into the room of the others that hold pages, when
    /// those have a block and a half of room between them; moved(page, from, to) for each. The pages moved
    template <class Moved>
    uint32_t compact(uint32_t budget, Moved&& moved) {
        calls_++;
        trim_empty();
        uint64_t room = 0;
        int from = -1;
        for (uint32_t i = 0; i < blocks_.size(); i++) {
            const Block& b = blocks_[i];
            if (!b.mem || !b.used) continue;
            room += kPages - b.used;
            if (from < 0 || b.used < blocks_[from].used || (b.used == blocks_[from].used && (int)i > from)) from = (int)i;
        }
        if (from < 0 || room < kPages + kPages / 2) return 0;
        Block& src = blocks_[from];
        set_room((uint32_t)from, false);   // (nothing goes into the block being emptied)
        uint32_t n = 0;
        for (uint32_t k = 0; k < kPages && n < budget && src.used; k++) {
            const uint32_t page = src.owner[k];
            if (page == kNone) continue;
            const int to = lowest_room();
            if (to < 0) break;   // (cannot happen: the others have src.used pages of room at least)
            const uint32_t slot = place((uint32_t)to, page);
            std::memcpy(at(slot), src.mem + (uint64_t)k * 4096, 4096);
            src.owner[k] = kNone;
            src.room.push_back((uint16_t)k);
            src.used--;
            pages_--;
            moved(page, (uint32_t)from * kPages + k, slot);
            n++;
        }
        moves_ += n;
        settle((uint32_t)from);
        trim_empty();
        return n;
    }

    uint64_t pages() const { return pages_; }                 // (copies held)
    uint32_t blocks() const { return held_; }                 // (blocks held, the spare too)
    uint64_t bytes() const { return (uint64_t)held_ * kBytes; }
    uint64_t peak_bytes() const { return (uint64_t)peak_ * kBytes; }
    uint64_t made() const { return made_; }
    uint64_t given_back() const { return given_back_; }
    uint64_t moves() const { return moves_; }
    uint64_t refused() const { return refused_; }             // (blocks that could not be made)
    bool has_spare() const { return lowest(empty_bits_) >= 0; }

private:
    struct Block {
        uint8_t* mem = nullptr;
        uint32_t used = 0;
        uint64_t empty_since = 0;      // (the compact() call it emptied at)
        std::vector<uint32_t> owner;   // (by slot: the page whose copy it holds, kNone when free)
        std::vector<uint16_t> room;    // (the free slots; the lowest on top)
    };
    MakeFn make_;
    GiveFn give_;
    uint64_t spare_calls_, empty_calls_;
    std::vector<Block> blocks_;     // (a block's number is its place here; one given back leaves its place for the next)
    uint64_t room_bits_[2] = {0, 0};   // (the blocks that hold pages and have room)
    uint64_t empty_bits_[2] = {0, 0};  // (the blocks held that hold nothing)
    uint64_t calls_ = 0;               // (compact()'s)
    uint64_t pages_ = 0;
    uint32_t held_ = 0, peak_ = 0;
    uint64_t made_ = 0, given_back_ = 0, moves_ = 0, refused_ = 0;

    static void set_bit(uint64_t* bits, uint32_t i, bool on) {
        const uint64_t bit = 1ull << (i % 64);
        if (on) bits[i / 64] |= bit;
        else bits[i / 64] &= ~bit;
    }
    static int lowest(const uint64_t* bits) {
        if (bits[0]) return std::countr_zero(bits[0]);
        if (bits[1]) return 64 + std::countr_zero(bits[1]);
        return -1;
    }
    void set_room(uint32_t i, bool on) { set_bit(room_bits_, i, on); }
    int lowest_room() const { return lowest(room_bits_); }
    /// The empty blocks that have waited empty_calls_ calls given back, the lowest-numbered (the spare) after spare_calls_
    void trim_empty() {
        const int keep = lowest(empty_bits_);
        if (keep < 0) return;
        for (uint32_t i = (uint32_t)keep; i < blocks_.size(); i++)
            if ((empty_bits_[i / 64] >> (i % 64) & 1) && calls_ - blocks_[i].empty_since >= ((int)i == keep ? spare_calls_ : empty_calls_))
                release(i);
    }
    uint32_t place(uint32_t i, uint32_t page) {
        Block& b = blocks_[i];
        const uint16_t k = b.room.back();
        b.room.pop_back();
        b.owner[k] = page;
        b.used++;
        pages_++;
        set_bit(empty_bits_, i, false);
        set_room(i, b.used < kPages);
        return i * kPages + k;
    }
    /// After pages left block i: room again, or empty (it waits from now: trim_empty)
    void settle(uint32_t i) {
        Block& b = blocks_[i];
        set_room(i, b.used != 0);
        if (!b.used) set_bit(empty_bits_, i, true), b.empty_since = calls_;
    }
    int make_block() {
        uint32_t i = 0;
        while (i < blocks_.size() && blocks_[i].mem) i++;
        if (i == kMaxBlocks) return -1;
        void* p = make_(kBytes);
        if (!p) {
            refused_++;
            return -1;
        }
        if (i == blocks_.size()) blocks_.emplace_back();
        Block& b = blocks_[i];
        b.mem = (uint8_t*)p;
        b.used = 0;
        b.owner.assign(kPages, kNone);
        b.room.resize(kPages);
        for (uint32_t k = 0; k < kPages; k++) b.room[k] = (uint16_t)(kPages - 1 - k);
        set_bit(empty_bits_, i, true), b.empty_since = calls_;
        held_++, made_++;
        if (held_ > peak_) peak_ = held_;
        return (int)i;
    }
    void release(uint32_t i) {
        Block& b = blocks_[i];
        give_(b.mem, kBytes);
        b.mem = nullptr;
        b.used = 0;
        std::vector<uint32_t>().swap(b.owner);
        std::vector<uint16_t>().swap(b.room);
        set_room(i, false);
        set_bit(empty_bits_, i, false);
        held_--, given_back_++;
    }
};

}  // namespace mkwvk
