// History tables and the staged move picker.
#pragma once

#include <cstring>

#include "movegen.h"

// Saturating history entry: bonus with gravity towards zero.
template <int D>
inline void update_history(int16_t& entry, int bonus) {
    bonus = std::clamp(bonus, -D, D);
    entry += int16_t(bonus - entry * std::abs(bonus) / D);
}

constexpr int HISTORY_MAX = 16384;
constexpr int CONT_HISTORY_MAX = 30000;

using ButterflyHistory = int16_t[COLOR_NB][64 * 64];
using CaptureHistory = int16_t[PIECE_NB][SQUARE_NB][PIECE_TYPE_NB];
using PieceToHistory = int16_t[PIECE_NB][SQUARE_NB];
using ContinuationHistory = PieceToHistory[PIECE_NB][SQUARE_NB];
using CounterMoveTable = Move[PIECE_NB][SQUARE_NB];

class MovePicker {
public:
    // Main search
    MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh, const CaptureHistory* cph,
               const PieceToHistory** ch, Move cm, const Move* killers);
    // Quiescence search
    MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh, const CaptureHistory* cph,
               const PieceToHistory** ch);
    // ProbCut: captures with SEE above a threshold
    MovePicker(const Position& p, Move ttm, Value threshold, const CaptureHistory* cph);

    Move next_move(bool skipQuiets = false);
    int stage_index() const { return stage; }

private:
    template <GenType Type> void score();
    template <bool Best, typename Pred> Move select(Pred filter);
    ExtMove* begin() { return cur; }
    ExtMove* end() { return endMoves; }

    const Position& pos;
    const ButterflyHistory* mainHistory;
    const CaptureHistory* captureHistory;
    const PieceToHistory** contHistory;
    Move ttMove;
    ExtMove refutations[3];
    ExtMove *cur, *endMoves, *endBadCaptures;
    int stage;
    Value threshold;
    Depth depth;
    ExtMove moves[MAX_MOVES];
};
