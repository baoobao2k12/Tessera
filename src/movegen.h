// Pseudo-legal and legal move generation.
#pragma once

#include "position.h"

struct ExtMove {
    Move move;
    int value;
    operator Move() const { return move; }
    void operator=(Move m) { move = m; }
};

inline bool operator<(const ExtMove& a, const ExtMove& b) { return a.value < b.value; }

enum GenType {
    CAPTURES,      // captures and queen promotions
    QUIETS,        // non-captures, castling and under-promotions (including capturing ones)
    EVASIONS,      // all pseudo-legal check evasions
    NON_EVASIONS,  // all pseudo-legal moves when not in check
    LEGAL          // all legal moves
};

template <GenType T>
ExtMove* generate(const Position& pos, ExtMove* moveList);

template <GenType T>
struct MoveList {
    explicit MoveList(const Position& pos) : last(generate<T>(pos, moveList)) {}
    const ExtMove* begin() const { return moveList; }
    const ExtMove* end() const { return last; }
    size_t size() const { return size_t(last - moveList); }
    bool contains(Move m) const {
        for (const ExtMove* it = moveList; it != last; ++it)
            if (it->move == m) return true;
        return false;
    }

private:
    ExtMove moveList[MAX_MOVES];
    ExtMove* last;
};
