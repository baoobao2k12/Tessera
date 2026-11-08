#include "tt.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

TranspositionTable TT;

namespace {

void* aligned_large_alloc(size_t size) {
#if defined(_WIN32)
    return _aligned_malloc(size, 4096);
#else
    constexpr size_t alignment = 2 * 1024 * 1024;  // allows transparent huge pages
    size_t rounded = ((size + alignment - 1) / alignment) * alignment;
    void* p = std::aligned_alloc(alignment, rounded);
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    if (p) madvise(p, rounded, MADV_HUGEPAGE);
#endif
    return p;
#endif
}

void aligned_large_free(void* p) {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

// data layout: move(16) | value(16) | eval(16) | depth8(8) | genBound8(8)
inline Move d_move(uint64_t d) { return Move(d & 0xFFFF); }
inline Value d_value(uint64_t d) { return int16_t(uint16_t(d >> 16)); }
inline Value d_eval(uint64_t d) { return int16_t(uint16_t(d >> 32)); }
inline int d_depth8(uint64_t d) { return int((d >> 48) & 0xFF); }
inline uint8_t d_genbound(uint64_t d) { return uint8_t(d >> 56); }

}  // namespace

uint64_t TranspositionTable::pack(Move m, Value v, Value ev, Depth d, uint8_t genBound) {
    return uint64_t(m) | (uint64_t(uint16_t(int16_t(v))) << 16) | (uint64_t(uint16_t(int16_t(ev))) << 32) |
           (uint64_t(uint8_t(d - DEPTH_OFFSET)) << 48) | (uint64_t(genBound) << 56);
}

TranspositionTable::~TranspositionTable() { aligned_large_free(mem); }

void TranspositionTable::resize(size_t mb, int threads) {
    if (mb == 0) mb = 1;
    aligned_large_free(mem);
    mem = nullptr;
    table = nullptr;
    sizeMB = mb;
    bucketCount = mb * 1024 * 1024 / sizeof(Bucket);
    mem = aligned_large_alloc(bucketCount * sizeof(Bucket));
    if (!mem) {
        std::cerr << "info string failed to allocate " << mb << " MB for the hash table" << std::endl;
        std::exit(EXIT_FAILURE);
    }
    table = static_cast<Bucket*>(mem);
    clear(threads);
}

// Zeroes the table, splitting the work across threads for large hash sizes.
void TranspositionTable::clear(int threads) {
    if (!table) return;
    threads = std::max(1, threads);
    const size_t total = bucketCount * sizeof(Bucket);
    if (threads == 1 || total < (64u << 20)) {
        std::memset(static_cast<void*>(table), 0, total);
    } else {
        std::vector<std::thread> workers;
        const size_t stride = bucketCount / size_t(threads);
        for (int i = 0; i < threads; ++i) {
            workers.emplace_back([this, i, stride, threads]() {
                size_t start = stride * size_t(i);
                size_t len = i == threads - 1 ? bucketCount - start : stride;
                std::memset(static_cast<void*>(&table[start]), 0, len * sizeof(Bucket));
            });
        }
        for (auto& w : workers) w.join();
    }
    generation8 = 0;
}

bool TranspositionTable::probe(Key key, TTData& out) const {
    const Bucket& b = table[bucket_index(key)];
    for (const Entry& e : b.e) {
        uint64_t d = load(e.data);
        if (d && (load(e.keyx) ^ d) == key) {
            uint8_t gb = d_genbound(d);
            out.move = d_move(d);
            out.value = d_value(d);
            out.eval = d_eval(d);
            out.depth = d_depth8(d) + DEPTH_OFFSET;
            out.bound = Bound(gb & 3);
            out.pv = gb & 4;
            return true;
        }
    }
    return false;
}

void TranspositionTable::store(Key key, Value v, bool pv, Bound bound, Depth d, Move m, Value eval) {
    Bucket& b = table[bucket_index(key)];
    Entry* replace = nullptr;
    uint64_t old = 0;
    bool sameKey = false;
    int worst = 1 << 30;

    for (Entry& e : b.e) {
        uint64_t data = load(e.data);
        if (!data) {
            replace = &e;
            old = 0;
            break;
        }
        if ((load(e.keyx) ^ data) == key) {
            replace = &e;
            old = data;
            sameKey = true;
            break;
        }
        // Prefer replacing shallow entries from older searches
        int age = (GENERATION_CYCLE + generation8 - d_genbound(data)) & GENERATION_MASK;
        int worth = d_depth8(data) - age;
        if (worth < worst) {
            worst = worth;
            replace = &e;
            old = data;
        }
    }

    if (sameKey) {
        if (m == MOVE_NONE) m = d_move(old);
        int oldDepth8 = d_depth8(old);
        int age = (GENERATION_CYCLE + generation8 - d_genbound(old)) & GENERATION_MASK;
        // Keep a deeper entry from this search unless the new one is exact
        if (bound != BOUND_EXACT && d - DEPTH_OFFSET + 2 * pv <= oldDepth8 - 4 && !age) {
            if (m != d_move(old)) {  // still record a newly found best move
                uint64_t nd = (old & ~uint64_t(0xFFFF)) | m;
                save(replace->data, nd);
                save(replace->keyx, key ^ nd);
            }
            return;
        }
    }

    uint8_t genBound = uint8_t(generation8 | (uint8_t(pv) << 2) | uint8_t(bound));
    uint64_t nd = pack(m, v, eval, d, genBound);
    save(replace->data, nd);
    save(replace->keyx, key ^ nd);
}

int TranspositionTable::hashfull() const {
    int cnt = 0;
    const size_t buckets = std::min<size_t>(250, bucketCount);
    for (size_t i = 0; i < buckets; ++i)
        for (const Entry& e : table[i].e) {
            uint64_t d = load(e.data);
            if (d && (d_genbound(d) & GENERATION_MASK) == generation8) ++cnt;
        }
    return int(cnt * 1000 / (buckets * 4));
}
