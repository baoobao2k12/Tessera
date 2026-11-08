#include "endgame.h"

#include <vector>

#include "movegen.h"

// ---------------------------------------------------------------------------
// KPK bitbase, generated at startup by retrograde iteration.

namespace {

constexpr unsigned KPK_SIZE = 2 * 24 * 64 * 64;  // stm * pawn squares (files a-d, ranks 2-7) * wk * bk
uint32_t KPKBitbase[KPK_SIZE / 32];

unsigned kpk_index(int stm, int bksq, int wksq, int psq) {
    return unsigned(wksq) | (unsigned(bksq) << 6) | (unsigned(stm) << 12) | (unsigned(file_of(psq)) << 13) |
           (unsigned(RANK_7 - rank_of(psq)) << 15);
}

enum : uint8_t { R_INVALID = 0, R_UNKNOWN = 1, R_DRAW = 2, R_WIN = 4 };

struct KPKPos {
    int stm, ksq[2], psq;
    uint8_t result;

    void init(unsigned idx) {
        ksq[WHITE] = int(idx & 63);
        ksq[BLACK] = int((idx >> 6) & 63);
        stm = int((idx >> 12) & 1);
        psq = make_square(int((idx >> 13) & 3), RANK_7 - int((idx >> 15) & 7));

        if (distance(ksq[WHITE], ksq[BLACK]) <= 1 || ksq[WHITE] == psq || ksq[BLACK] == psq ||
            (stm == WHITE && (PawnAttacks[WHITE][psq] & sq_bb(ksq[BLACK]))))
            result = R_INVALID;
        // White promotes safely
        else if (stm == WHITE && rank_of(psq) == RANK_7 && ksq[WHITE] != psq + NORTH &&
                 (distance(ksq[BLACK], psq + NORTH) > 1 || distance(ksq[WHITE], psq + NORTH) == 1))
            result = R_WIN;
        // Black is stalemated or captures an undefended pawn
        else if (stm == BLACK &&
                 (!(PseudoAttacks[KING][ksq[BLACK]] & ~(PseudoAttacks[KING][ksq[WHITE]] | PawnAttacks[WHITE][psq])) ||
                  (PseudoAttacks[KING][ksq[BLACK]] & sq_bb(psq) & ~PseudoAttacks[KING][ksq[WHITE]])))
            result = R_DRAW;
        else
            result = R_UNKNOWN;
    }

    uint8_t classify(const std::vector<KPKPos>& db) {
        const uint8_t good = stm == WHITE ? uint8_t(R_WIN) : uint8_t(R_DRAW);
        const uint8_t bad = stm == WHITE ? uint8_t(R_DRAW) : uint8_t(R_WIN);
        uint8_t r = R_INVALID;
        Bitboard b = PseudoAttacks[KING][ksq[stm]];
        while (b) {
            int s = pop_lsb(b);
            r |= stm == WHITE ? db[kpk_index(BLACK, ksq[BLACK], s, psq)].result
                              : db[kpk_index(WHITE, s, ksq[WHITE], psq)].result;
        }
        if (stm == WHITE) {
            if (rank_of(psq) < RANK_7) r |= db[kpk_index(BLACK, ksq[BLACK], ksq[WHITE], psq + NORTH)].result;
            if (rank_of(psq) == RANK_2 && psq + NORTH != ksq[WHITE] && psq + NORTH != ksq[BLACK])
                r |= db[kpk_index(BLACK, ksq[BLACK], ksq[WHITE], psq + NORTH + NORTH)].result;
        }
        return result = (r & good) ? good : (r & R_UNKNOWN) ? uint8_t(R_UNKNOWN) : bad;
    }
};

}  // namespace

void Bitbases::init() {
    std::vector<KPKPos> db(KPK_SIZE);
    for (unsigned i = 0; i < KPK_SIZE; ++i) db[i].init(i);
    bool repeat = true;
    while (repeat) {
        repeat = false;
        for (unsigned i = 0; i < KPK_SIZE; ++i)
            if (db[i].result == R_UNKNOWN && db[i].classify(db) != R_UNKNOWN) repeat = true;
    }
    for (auto& w : KPKBitbase) w = 0;
    for (unsigned i = 0; i < KPK_SIZE; ++i)
        if (db[i].result == R_WIN) KPKBitbase[i / 32] |= 1u << (i & 31);
}

bool Bitbases::probe_kpk(int wksq, int wpsq, int bksq, int stm) {
    if (file_of(wpsq) >= FILE_E) {
        wksq = flip_file(wksq);
        wpsq = flip_file(wpsq);
        bksq = flip_file(bksq);
    }
    unsigned idx = kpk_index(stm, bksq, wksq, wpsq);
    return KPKBitbase[idx / 32] & (1u << (idx & 31));
}

// ---------------------------------------------------------------------------
// Helpers

namespace {

int push_to_edge(int s) {
    int rd = edge_distance(rank_of(s)), fd = edge_distance(file_of(s));
    return 90 - (7 * fd * fd / 2 + 7 * rd * rd / 2);
}
int push_to_corner(int s) { return std::abs(7 - rank_of(s) - file_of(s)); }  // towards a1/h8
int push_close(int a, int b) { return 140 - 20 * distance(a, b); }
int push_away(int a, int b) { return 120 - push_close(a, b); }

// Map a square so that the strong side plays "up" the board as white
int normalize(int strongSide, int s) { return relative_square(strongSide, s); }

Value kxk(const Position& pos, int strong) {
    const int weak = strong ^ 1;
    if (pos.side_to_move() == weak && !pos.in_check() && MoveList<LEGAL>(pos).size() == 0) return VALUE_DRAW;
    const int wk = pos.king_sq(strong), lk = pos.king_sq(weak);
    Value result = pos.non_pawn_material(strong) + pos.count(strong, PAWN) * PawnValue + push_to_edge(lk) +
                   push_close(wk, lk);
    if (pos.count(strong, QUEEN) || pos.count(strong, ROOK) ||
        (pos.count(strong, BISHOP) && pos.count(strong, KNIGHT)) ||
        ((pos.pieces(strong, BISHOP) & DarkSquares) && (pos.pieces(strong, BISHOP) & LightSquares)))
        result = std::min(result + VALUE_KNOWN_WIN, VALUE_MATE_IN_MAX_PLY - 1);
    return strong == pos.side_to_move() ? result : -result;
}

Value kbnk(const Position& pos, int strong) {
    const int weak = strong ^ 1;
    const int wk = pos.king_sq(strong), lk = pos.king_sq(weak);
    const int bsq = lsb(pos.pieces(strong, BISHOP));
    // Drive the king towards a corner of the bishop's color
    int target = opposite_colors(bsq, SQ_A1) ? flip_file(lk) : lk;
    Value result = VALUE_KNOWN_WIN + KnightValue + BishopValue + push_close(wk, lk) + 100 * push_to_corner(target);
    return strong == pos.side_to_move() ? result : -result;
}

Value kpk(const Position& pos, int strong) {
    const int wksq = normalize(strong, pos.king_sq(strong));
    const int bksq = normalize(strong, pos.king_sq(strong ^ 1));
    const int psq = normalize(strong, lsb(pos.pieces(strong, PAWN)));
    const int us = strong == pos.side_to_move() ? WHITE : BLACK;
    if (!Bitbases::probe_kpk(wksq, psq, bksq, us)) return VALUE_DRAW;
    Value result = VALUE_KNOWN_WIN + PawnValue + 10 * rank_of(psq);
    return strong == pos.side_to_move() ? result : -result;
}

Value krkp(const Position& pos, int strong) {
    const int weak = strong ^ 1;
    const int wksq = normalize(strong, pos.king_sq(strong));
    const int bksq = normalize(strong, pos.king_sq(weak));
    const int rsq = normalize(strong, lsb(pos.pieces(strong, ROOK)));
    const int psq = normalize(strong, lsb(pos.pieces(weak, PAWN)));
    const int queeningSq = make_square(file_of(psq), RANK_1);
    const bool weakToMove = pos.side_to_move() == weak, strongToMove = !weakToMove;
    Value result;
    if (ForwardFileBB[WHITE][wksq] & sq_bb(psq))  // strong king in front of the pawn
        result = RookValue - distance(wksq, psq);
    else if (distance(bksq, psq) >= 3 + weakToMove && distance(bksq, rsq) >= 3)
        result = RookValue - distance(wksq, psq);
    else if (rank_of(bksq) <= RANK_3 && distance(bksq, psq) == 1 && rank_of(wksq) >= RANK_4 &&
             distance(wksq, psq) > 2 + strongToMove)
        result = 40 - 4 * distance(wksq, psq);
    else
        result = 100 - 4 * (distance(wksq, psq + SOUTH) - distance(bksq, psq + SOUTH) - distance(psq, queeningSq));
    return strong == pos.side_to_move() ? result : -result;
}

Value krkb(const Position& pos, int strong) {
    Value result = push_to_edge(pos.king_sq(strong ^ 1));
    return strong == pos.side_to_move() ? result : -result;
}

Value krkn(const Position& pos, int strong) {
    const int lk = pos.king_sq(strong ^ 1);
    const int nsq = lsb(pos.pieces(strong ^ 1, KNIGHT));
    Value result = push_to_edge(lk) + push_away(lk, nsq);
    return strong == pos.side_to_move() ? result : -result;
}

Value kqkp(const Position& pos, int strong) {
    const int weak = strong ^ 1;
    const int wk = pos.king_sq(strong), lk = pos.king_sq(weak);
    const int psq = lsb(pos.pieces(weak, PAWN));
    Value result = push_close(wk, lk);
    if (relative_rank_of(weak, psq) != RANK_7 || distance(lk, psq) != 1 ||
        !((FileABB | FileCBB | FileFBB | FileHBB) & sq_bb(psq)))
        result += QueenValue - PawnValue;
    return strong == pos.side_to_move() ? result : -result;
}

Value kqkr(const Position& pos, int strong) {
    const int wk = pos.king_sq(strong), lk = pos.king_sq(strong ^ 1);
    Value result = QueenValue - RookValue + push_to_edge(lk) + push_close(wk, lk);
    return strong == pos.side_to_move() ? result : -result;
}

// --- scaling functions

int kbpsk(const Position& pos, int strong) {
    const int weak = strong ^ 1;
    const Bitboard strongPawns = pos.pieces(strong, PAWN);
    const int bsq = lsb(pos.pieces(strong, BISHOP));
    const int weakKing = pos.king_sq(weak);
    // All pawns on one rook file with a bishop of the wrong color: draw if the
    // defending king reaches the corner.
    if (!(strongPawns & ~FileABB) || !(strongPawns & ~FileHBB)) {
        int queeningSq = relative_square(strong, make_square(file_of(lsb(strongPawns)), RANK_8));
        if (opposite_colors(queeningSq, bsq) && distance(queeningSq, weakKing) <= 1) return SCALE_DRAW;
    }
    return SCALE_NONE;
}

int kpsk(const Position& pos, int strong) {
    const int weak = strong ^ 1;
    const Bitboard strongPawns = pos.pieces(strong, PAWN);
    const int weakKing = pos.king_sq(weak);
    // All pawns on a single rook file and the defending king in front of them
    if ((!(strongPawns & ~FileABB) || !(strongPawns & ~FileHBB)) &&
        !(strongPawns & ~PassedPawnSpan[weak][weakKing]))
        return SCALE_DRAW;
    return SCALE_NONE;
}

int krpkr(const Position& pos, int strong) {
    const int weak = strong ^ 1;
    const int wksq = normalize(strong, pos.king_sq(strong));
    const int bksq = normalize(strong, pos.king_sq(weak));
    const int wrsq = normalize(strong, lsb(pos.pieces(strong, ROOK)));
    const int wpsq = normalize(strong, lsb(pos.pieces(strong, PAWN)));
    const int brsq = normalize(strong, lsb(pos.pieces(weak, ROOK)));
    const int f = file_of(wpsq), r = rank_of(wpsq);
    const int queeningSq = make_square(f, RANK_8);
    const int tempo = pos.side_to_move() == strong;

    // Third-rank (Philidor) defence
    if (r <= RANK_5 && distance(bksq, queeningSq) <= 1 && wksq <= SQ_H5 &&
        (rank_of(brsq) == RANK_6 || (r <= RANK_3 && rank_of(wrsq) != RANK_6)))
        return SCALE_DRAW;
    // Checking from behind once the pawn reached the sixth rank
    if (r == RANK_6 && distance(bksq, queeningSq) <= 1 && rank_of(wksq) + tempo <= RANK_6 &&
        (rank_of(brsq) == RANK_1 || (!tempo && file_distance(brsq, wpsq) >= 3)))
        return SCALE_DRAW;
    if (r >= RANK_6 && bksq == queeningSq && rank_of(brsq) == RANK_1 && (!tempo || distance(wksq, wpsq) >= 2))
        return SCALE_DRAW;
    // Pawn on a7, rook in front of it and the defending king near
    if (wpsq == SQ_A7 && wrsq == SQ_A8 && (bksq == SQ_H7 || bksq == SQ_G7) && file_of(brsq) == FILE_A &&
        (rank_of(brsq) <= RANK_3 || file_of(wksq) >= FILE_D || rank_of(wksq) <= RANK_5))
        return SCALE_DRAW;
    // Defending king blocks the pawn and the attacking king is far away
    if (r <= RANK_5 && bksq == wpsq + NORTH && distance(wksq, wpsq) - tempo >= 2 && distance(wksq, brsq) - tempo >= 2)
        return SCALE_DRAW;
    // Defending king in front of the pawn: drawish but not dead
    if (ForwardFileBB[WHITE][wpsq] & sq_bb(bksq)) return 24;
    return SCALE_NONE;
}

int kbpkb(const Position& pos, int strong) {
    const int weak = strong ^ 1;
    const int psq = lsb(pos.pieces(strong, PAWN));
    const int sb = lsb(pos.pieces(strong, BISHOP)), wb = lsb(pos.pieces(weak, BISHOP));
    const int weakKing = pos.king_sq(weak);
    if ((ForwardFileBB[strong][psq] & sq_bb(weakKing)) &&
        (opposite_colors(weakKing, sb) || relative_rank_of(strong, weakKing) <= RANK_6))
        return SCALE_DRAW;
    if (opposite_colors(sb, wb)) return SCALE_DRAW;
    return SCALE_NONE;
}

int kbpkn(const Position& pos, int strong) {
    const int weak = strong ^ 1;
    const int psq = lsb(pos.pieces(strong, PAWN));
    const int sb = lsb(pos.pieces(strong, BISHOP));
    const int weakKing = pos.king_sq(weak);
    if (file_of(weakKing) == file_of(psq) && relative_rank_of(strong, psq) < relative_rank_of(strong, weakKing) &&
        (opposite_colors(weakKing, sb) || relative_rank_of(strong, weakKing) <= RANK_6))
        return SCALE_DRAW;
    return SCALE_NONE;
}

int kpkp(const Position& pos, int strong) {
    const int weak = strong ^ 1;
    const int wksq = normalize(strong, pos.king_sq(strong));
    const int bksq = normalize(strong, pos.king_sq(weak));
    const int psq = normalize(strong, lsb(pos.pieces(strong, PAWN)));
    const int us = strong == pos.side_to_move() ? WHITE : BLACK;
    // Too dangerous to assume anything about advanced non-rook pawns
    if (rank_of(psq) >= RANK_5 && file_of(psq) != FILE_A && file_of(psq) != FILE_H) return SCALE_NONE;
    // If the KPK position without the defender's pawn is a draw, it is probably a draw with it too
    return Bitbases::probe_kpk(wksq, psq, bksq, us) ? SCALE_NONE : SCALE_DRAW;
}

}  // namespace

Value endgame_value(int fn, int strong, const Position& pos) {
    switch (fn) {
    case EG_DRAW: return VALUE_DRAW;
    case EG_KXK: return kxk(pos, strong);
    case EG_KBNK: return kbnk(pos, strong);
    case EG_KPK: return kpk(pos, strong);
    case EG_KRKP: return krkp(pos, strong);
    case EG_KRKB: return krkb(pos, strong);
    case EG_KRKN: return krkn(pos, strong);
    case EG_KQKP: return kqkp(pos, strong);
    case EG_KQKR: return kqkr(pos, strong);
    default: return VALUE_NONE;
    }
}

int endgame_scale(int fn, int strong, const Position& pos) {
    switch (fn) {
    case SC_KBPSK: return kbpsk(pos, strong);
    case SC_KPSK: return kpsk(pos, strong);
    case SC_KRPKR: return krpkr(pos, strong);
    case SC_KBPKB: return kbpkb(pos, strong);
    case SC_KBPKN: return kbpkn(pos, strong);
    case SC_KPKP: return kpkp(pos, strong);
    default: return SCALE_NONE;
    }
}

void classify_material(const Position& pos, MaterialEntry& e) {
    e.key = pos.material_key();
    e.evalFn = EG_NONE;
    e.evalStrong = WHITE;
    e.scaleFn[WHITE] = e.scaleFn[BLACK] = SC_NONE;
    e.factor[WHITE] = e.factor[BLACK] = SCALE_NORMAL;
    int phase = pos.count(KNIGHT) + pos.count(BISHOP) + 2 * pos.count(ROOK) + 4 * pos.count(QUEEN);
    e.phase = int16_t(std::min(phase, 24));

    if (pos.is_insufficient_material()) {
        e.evalFn = EG_DRAW;
        return;
    }

    for (int c = WHITE; c <= BLACK; ++c) {
        const int weak = c ^ 1;
        const int npmS = pos.non_pawn_material(c), npmW = pos.non_pawn_material(weak);
        const int pS = pos.count(c, PAWN), pW = pos.count(weak, PAWN);
        auto set = [&](uint8_t fn) {
            e.evalFn = fn;
            e.evalStrong = uint8_t(c);
        };

        if (npmW == 0 && pW == 0) {
            if (npmS == 0 && pS == 1) return set(EG_KPK);
            if (pS == 0 && npmS == KnightValue + BishopValue && pos.count(c, KNIGHT) == 1) return set(EG_KBNK);
            if (pS == 0 && npmS == 2 * KnightValue && pos.count(c, KNIGHT) == 2) return set(EG_DRAW);
            if (npmS >= RookValue) return set(EG_KXK);
        }
        if (pS == 0 && npmS == RookValue && pos.count(c, ROOK) == 1) {
            if (npmW == 0 && pW == 1) return set(EG_KRKP);
            if (pW == 0 && npmW == BishopValue && pos.count(weak, BISHOP) == 1) return set(EG_KRKB);
            if (pW == 0 && npmW == KnightValue && pos.count(weak, KNIGHT) == 1) return set(EG_KRKN);
        }
        if (pS == 0 && npmS == QueenValue && pos.count(c, QUEEN) == 1) {
            if (npmW == 0 && pW == 1) return set(EG_KQKP);
            if (pW == 0 && npmW == RookValue && pos.count(weak, ROOK) == 1) return set(EG_KQKR);
        }
    }

    for (int c = WHITE; c <= BLACK; ++c) {
        const int weak = c ^ 1;
        const int npmS = pos.non_pawn_material(c), npmW = pos.non_pawn_material(weak);
        const int pS = pos.count(c, PAWN), pW = pos.count(weak, PAWN);
        const bool loneBishop = npmS == BishopValue && pos.count(c, BISHOP) == 1;
        if (loneBishop && pS >= 1) e.scaleFn[c] = SC_KBPSK;
        if (npmS == 0 && pS >= 2 && npmW == 0 && pW == 0) e.scaleFn[c] = SC_KPSK;
        if (npmS == RookValue && pos.count(c, ROOK) == 1 && pS == 1 && npmW == RookValue &&
            pos.count(weak, ROOK) == 1 && pW == 0)
            e.scaleFn[c] = SC_KRPKR;
        if (loneBishop && pS == 1 && npmW == BishopValue && pos.count(weak, BISHOP) == 1 && pW == 0)
            e.scaleFn[c] = SC_KBPKB;
        if (loneBishop && pS == 1 && npmW == KnightValue && pos.count(weak, KNIGHT) == 1 && pW == 0)
            e.scaleFn[c] = SC_KBPKN;
        if (npmS == 0 && npmW == 0 && pS == 1 && pW == 1) e.scaleFn[c] = SC_KPKP;

        if (!pS && npmS - npmW <= BishopValue)
            e.factor[c] = uint8_t(npmS < RookValue ? SCALE_DRAW : npmW <= BishopValue ? 4 : 14);
        if (pS == 1 && npmS - npmW <= BishopValue) e.factor[c] = SCALE_ONEPAWN;
    }
}
