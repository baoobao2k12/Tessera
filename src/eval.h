// Handcrafted, tapered evaluation.
#pragma once

#include <memory>
#include <string>

#include "endgame.h"
#include "evalparams.h"
#include "position.h"

struct PawnEntry {
    Key key;
    Score scores[COLOR_NB];
    Bitboard passedPawns[COLOR_NB];
    Bitboard pawnAttacks[COLOR_NB];
    Bitboard pawnAttacksSpan[COLOR_NB];
    int blockedCount;
    int passedCount;
    // King shelter cache: valid while the king square and castling rights match
    int kingSquare[COLOR_NB];
    int castling[COLOR_NB];
    Score kingSafety[COLOR_NB];
};

// Linear coefficients of every parameter for one position, used by the tuner.
struct EvalTrace {
    int lin[NUM_PARAM_SLOTS][COLOR_NB];  // count of each linear term per color
    int kd[COLOR_NB][NUM_PARAM_SLOTS];   // king-danger term counts, per defending king color
    int kdConst[COLOR_NB];               // non-parameter part of king danger
    int cx[NUM_PARAM_SLOTS];             // complexity term counts
    int phase;
    int scale;   // final scale factor (0..64), applied to eg
    bool special;  // evaluated by a specialised endgame function; not tunable
    Value eval;    // final evaluation, white point of view
    void clear();
};

namespace Eval {

struct Cache {
    static constexpr size_t PawnEntries = 1 << 14;
    static constexpr size_t MaterialEntries = 1 << 13;
    PawnEntry pawns[PawnEntries];
    MaterialEntry material[MaterialEntries];
    void clear();
};

void init();       // bitbases + PSQT
void init_psqt();  // rebuild PSQ[][] from the active parameters
Value evaluate(const Position& pos, Cache& cache);
// Full evaluation recording every term, with no caching and no lazy exit.
Value evaluate_trace(const Position& pos, EvalTrace& trace);
std::string describe(const Position& pos);

constexpr int LazyThreshold = 1500;

}  // namespace Eval
