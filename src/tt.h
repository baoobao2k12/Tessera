// Shared, lock-free transposition table.
//
// Each entry is two 64-bit words: data and (key ^ data). Both words are read
// and written with relaxed std::atomic_ref accesses, so concurrent use by the
// Lazy SMP threads is free of data races. A torn entry (data from one writer,
// key word from another) fails the full 64-bit key check and is treated as a miss.
#pragma once

#include <atomic>
#include <cstddef>

#include "types.h"

enum Bound : uint8_t { BOUND_NONE = 0, BOUND_UPPER = 1, BOUND_LOWER = 2, BOUND_EXACT = 3 };

constexpr Depth DEPTH_QS = 0;           // first quiescence ply: captures and quiet checks
constexpr Depth DEPTH_QS_NO_CHECKS = -1; // deeper quiescence plies: captures only
constexpr Depth DEPTH_NONE = -6;   // entries that only carry a static evaluation
constexpr Depth DEPTH_OFFSET = -7; // stored depth = depth - DEPTH_OFFSET, so 0 means empty

struct TTData {
    Move move;
    Value value;
    Value eval;
    Depth depth;
    Bound bound;
    bool pv;
};

class TranspositionTable {
public:
    TranspositionTable() = default;
    ~TranspositionTable();
    TranspositionTable(const TranspositionTable&) = delete;
    TranspositionTable& operator=(const TranspositionTable&) = delete;

    void resize(size_t mb, int threads = 1);
    void clear(int threads = 1);
    void new_search() { generation8 = uint8_t(generation8 + GENERATION_DELTA); }
    uint8_t generation() const { return generation8; }

    bool probe(Key key, TTData& out) const;
    void store(Key key, Value v, bool pv, Bound b, Depth d, Move m, Value eval);
    int hashfull() const;
    size_t size_mb() const { return sizeMB; }

    void prefetch(Key key) const {
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(&table[bucket_index(key)]);
#elif defined(_MSC_VER)
        _mm_prefetch(reinterpret_cast<const char*>(&table[bucket_index(key)]), _MM_HINT_T0);
#endif
    }

    // Packing helpers, public for the unit tests
    static constexpr uint8_t GENERATION_BITS = 3;
    static constexpr uint8_t GENERATION_DELTA = 1 << GENERATION_BITS;
    static constexpr int GENERATION_CYCLE = 255 + GENERATION_DELTA;
    static constexpr int GENERATION_MASK = (0xFF << GENERATION_BITS) & 0xFF;

private:
    struct Entry {
        uint64_t keyx;
        uint64_t data;
    };
    struct alignas(64) Bucket {
        Entry e[4];
    };
    static_assert(sizeof(Bucket) == 64, "bucket must fill one cache line");

    size_t bucket_index(Key key) const {
#if defined(__SIZEOF_INT128__)
        return size_t((unsigned __int128)key * (unsigned __int128)bucketCount >> 64);
#else
        return size_t(key % bucketCount);
#endif
    }
    static uint64_t load(const uint64_t& x) {
        return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(x)).load(std::memory_order_relaxed);
    }
    static void save(uint64_t& x, uint64_t v) { std::atomic_ref<uint64_t>(x).store(v, std::memory_order_relaxed); }
    static uint64_t pack(Move m, Value v, Value ev, Depth d, uint8_t genBound);

    Bucket* table = nullptr;
    void* mem = nullptr;
    size_t bucketCount = 0;
    size_t sizeMB = 0;
    uint8_t generation8 = 0;
};

extern TranspositionTable TT;

// Mate scores are stored relative to the node, not the root.
inline Value value_to_tt(Value v, int ply) {
    if (v == VALUE_NONE) return VALUE_NONE;
    return v >= VALUE_MATE_IN_MAX_PLY ? v + ply : v <= VALUE_MATED_IN_MAX_PLY ? v - ply : v;
}

// Converts back to a root-relative score. A mate that cannot be delivered
// before the fifty-move rule triggers is downgraded to a non-mate bound.
inline Value value_from_tt(Value v, int ply, int r50) {
    if (v == VALUE_NONE) return VALUE_NONE;
    if (v >= VALUE_MATE_IN_MAX_PLY) {
        if (VALUE_MATE - v > 99 - r50) return VALUE_MATE_IN_MAX_PLY - 1;
        return v - ply;
    }
    if (v <= VALUE_MATED_IN_MAX_PLY) {
        if (VALUE_MATE + v > 99 - r50) return VALUE_MATED_IN_MAX_PLY + 1;
        return v + ply;
    }
    return v;
}
