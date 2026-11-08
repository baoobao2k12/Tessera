#include "movepick.h"
#include "tt.h"

namespace {

enum Stages {
    MAIN_TT, CAPTURE_INIT, GOOD_CAPTURE, REFUTATION, QUIET_INIT, QUIET, BAD_CAPTURE,
    EVASION_TT, EVASION_INIT, EVASION,
    PROBCUT_TT, PROBCUT_INIT, PROBCUT,
    QSEARCH_TT, QCAPTURE_INIT, QCAPTURE, QCHECK_INIT, QCHECK
};

// Sort moves in descending order, but only those scored above the limit
void partial_insertion_sort(ExtMove* begin, ExtMove* end, int limit) {
    for (ExtMove *sortedEnd = begin, *p = begin + 1; p < end; ++p)
        if (p->value >= limit) {
            ExtMove tmp = *p, *q;
            *p = *++sortedEnd;
            for (q = sortedEnd; q != begin && (q - 1)->value < tmp.value; --q) *q = *(q - 1);
            *q = tmp;
        }
}

}  // namespace

MovePicker::MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh, const CaptureHistory* cph,
                       const PieceToHistory** ch, Move cm, const Move* killers)
    : pos(p), mainHistory(mh), captureHistory(cph), contHistory(ch),
      refutations{{killers[0], 0}, {killers[1], 0}, {cm, 0}}, threshold(0), depth(d) {
    stage = pos.checkers() ? EVASION_TT : MAIN_TT;
    ttMove = ttm && pos.pseudo_legal(ttm) ? ttm : MOVE_NONE;
    stage += (ttMove == MOVE_NONE);
    cur = endMoves = endBadCaptures = moves;
}

MovePicker::MovePicker(const Position& p, Move ttm, Depth d, const ButterflyHistory* mh, const CaptureHistory* cph,
                       const PieceToHistory** ch)
    : pos(p), mainHistory(mh), captureHistory(cph), contHistory(ch), threshold(0), depth(d) {
    stage = pos.checkers() ? EVASION_TT : QSEARCH_TT;
    ttMove = ttm && pos.pseudo_legal(ttm) && (pos.checkers() || pos.capture_or_promotion(ttm)) ? ttm : MOVE_NONE;
    stage += (ttMove == MOVE_NONE);
    cur = endMoves = endBadCaptures = moves;
}

MovePicker::MovePicker(const Position& p, Move ttm, Value th, const CaptureHistory* cph)
    : pos(p), mainHistory(nullptr), captureHistory(cph), contHistory(nullptr), threshold(th), depth(0) {
    stage = PROBCUT_TT;
    ttMove = ttm && pos.capture_or_promotion(ttm) && pos.pseudo_legal(ttm) && pos.see_ge(ttm, threshold) ? ttm : MOVE_NONE;
    stage += (ttMove == MOVE_NONE);
    cur = endMoves = endBadCaptures = moves;
}

template <GenType Type>
void MovePicker::score() {
    for (ExtMove& m : *this) {
        if constexpr (Type == CAPTURES) {
            const int captured = type_of_move(m.move) == EN_PASSANT ? PAWN : type_of(pos.piece_on(to_sq(m.move)));
            m.value = 7 * PieceValue[captured] + (*captureHistory)[pos.moved_piece(m.move)][to_sq(m.move)][captured];
            if (type_of_move(m.move) == PROMOTION) m.value += 7 * (QueenValue - PawnValue);
        } else if constexpr (Type == QUIETS) {
            const int pc = pos.moved_piece(m.move), to = to_sq(m.move);
            m.value = (*mainHistory)[pos.side_to_move()][from_to(m.move)] + 2 * (*contHistory[0])[pc][to] +
                      (*contHistory[1])[pc][to] + (*contHistory[3])[pc][to] + (*contHistory[5])[pc][to] / 2;
            if (type_of_move(m.move) == PROMOTION) m.value -= 20000;  // under-promotions last
        } else {  // EVASIONS
            if (pos.capture_or_promotion(m.move)) {
                const int captured = type_of_move(m.move) == EN_PASSANT ? PAWN : type_of(pos.piece_on(to_sq(m.move)));
                m.value = PieceValue[captured] - type_of(pos.moved_piece(m.move)) + (1 << 28);
            } else
                m.value = (*mainHistory)[pos.side_to_move()][from_to(m.move)] +
                          (*contHistory[0])[pos.moved_piece(m.move)][to_sq(m.move)];
        }
    }
}

template <bool Best, typename Pred>
Move MovePicker::select(Pred filter) {
    while (cur < endMoves) {
        if constexpr (Best) std::swap(*cur, *std::max_element(cur, endMoves));
        if (cur->move != ttMove && filter()) return (cur++)->move;
        cur++;
    }
    return MOVE_NONE;
}

Move MovePicker::next_move(bool skipQuiets) {
top:
    switch (stage) {
    case MAIN_TT:
    case EVASION_TT:
    case QSEARCH_TT:
    case PROBCUT_TT:
        ++stage;
        return ttMove;

    case CAPTURE_INIT:
    case PROBCUT_INIT:
    case QCAPTURE_INIT:
        cur = endBadCaptures = moves;
        endMoves = generate<CAPTURES>(pos, cur);
        score<CAPTURES>();
        ++stage;
        goto top;

    case GOOD_CAPTURE:
        if (select<true>([&]() {
                // Losing captures are kept for the end
                return pos.see_ge(cur->move, Value(-69 * cur->value / 1024)) ? true : (*endBadCaptures++ = *cur, false);
            }))
            return *(cur - 1);
        // Prepare refutations: killers and countermove, skipping duplicates and captures
        cur = std::begin(refutations);
        endMoves = std::end(refutations);
        if (refutations[0].move == refutations[2].move || refutations[1].move == refutations[2].move) --endMoves;
        ++stage;
        [[fallthrough]];

    case REFUTATION:
        if (select<false>([&]() {
                return cur->move != MOVE_NONE && !pos.capture_or_promotion(cur->move) && pos.pseudo_legal(cur->move);
            }))
            return *(cur - 1);
        ++stage;
        [[fallthrough]];

    case QUIET_INIT:
        // Quiets are generated even when move-count pruning is already active,
        // because quiet checks are still searched (they may be mating).
        if (!skipQuiets) {
            cur = endBadCaptures;
            endMoves = generate<QUIETS>(pos, cur);
            score<QUIETS>();
            partial_insertion_sort(cur, endMoves, -3000 * depth);
        }
        ++stage;
        [[fallthrough]];

    case QUIET:
        if (!skipQuiets && select<false>([&]() {
                return cur->move != refutations[0].move && cur->move != refutations[1].move &&
                       cur->move != refutations[2].move;
            }))
            return *(cur - 1);
        cur = moves;
        endMoves = endBadCaptures;
        ++stage;
        [[fallthrough]];

    case BAD_CAPTURE:
        return select<false>([]() { return true; });

    case EVASION_INIT:
        cur = moves;
        endMoves = generate<EVASIONS>(pos, cur);
        score<EVASIONS>();
        ++stage;
        [[fallthrough]];

    case EVASION:
        return select<true>([]() { return true; });

    case PROBCUT:
        return select<true>([&]() { return pos.see_ge(cur->move, threshold); });

    case QCAPTURE:
        if (select<true>([]() { return true; })) return *(cur - 1);
        // Only the first quiescence ply also tries quiet checks
        if (depth != DEPTH_QS) return MOVE_NONE;
        ++stage;
        [[fallthrough]];

    case QCHECK_INIT:
        cur = moves;
        endMoves = generate<QUIETS>(pos, cur);
        ++stage;
        [[fallthrough]];

    case QCHECK:
        return select<false>([&]() { return type_of_move(cur->move) != PROMOTION && pos.gives_check(cur->move); });
    }
    return MOVE_NONE;
}
