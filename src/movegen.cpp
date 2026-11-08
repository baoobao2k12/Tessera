#include "movegen.h"

namespace {

template <GenType Type, int D>
ExtMove* make_promotions(ExtMove* list, int to) {
    constexpr bool all = Type == EVASIONS || Type == NON_EVASIONS;
    if constexpr (Type == CAPTURES || all) *list++ = make_special(PROMOTION, to - D, to, QUEEN);
    if constexpr (Type == QUIETS || all) {
        *list++ = make_special(PROMOTION, to - D, to, ROOK);
        *list++ = make_special(PROMOTION, to - D, to, BISHOP);
        *list++ = make_special(PROMOTION, to - D, to, KNIGHT);
    }
    return list;
}

template <int Us, GenType Type>
ExtMove* generate_pawn_moves(const Position& pos, ExtMove* list, Bitboard target) {
    constexpr int Them = Us ^ 1;
    constexpr Bitboard TRank7BB = Us == WHITE ? Rank7BB : Rank2BB;
    constexpr Bitboard TRank3BB = Us == WHITE ? Rank3BB : Rank6BB;
    constexpr int Up = Us == WHITE ? NORTH : SOUTH;
    constexpr int UpRight = Us == WHITE ? NORTH_EAST : SOUTH_WEST;
    constexpr int UpLeft = Us == WHITE ? NORTH_WEST : SOUTH_EAST;

    const Bitboard emptySquares = ~pos.pieces();
    const Bitboard enemies = Type == EVASIONS ? pos.checkers() : pos.pieces_c(Them);
    const Bitboard pawnsOn7 = pos.pieces(Us, PAWN) & TRank7BB;
    const Bitboard pawnsNotOn7 = pos.pieces(Us, PAWN) & ~TRank7BB;

    // Single and double pushes, no promotions
    if constexpr (Type != CAPTURES) {
        Bitboard b1 = shift<Up>(pawnsNotOn7) & emptySquares;
        Bitboard b2 = shift<Up>(b1 & TRank3BB) & emptySquares;
        if constexpr (Type == EVASIONS) {
            b1 &= target;
            b2 &= target;
        }
        while (b1) {
            int to = pop_lsb(b1);
            *list++ = make_move(to - Up, to);
        }
        while (b2) {
            int to = pop_lsb(b2);
            *list++ = make_move(to - Up - Up, to);
        }
    }

    // Promotions and under-promotions
    if (pawnsOn7) {
        Bitboard b1 = shift<UpRight>(pawnsOn7) & enemies;
        Bitboard b2 = shift<UpLeft>(pawnsOn7) & enemies;
        Bitboard b3 = shift<Up>(pawnsOn7) & emptySquares;
        if constexpr (Type == EVASIONS) b3 &= target;
        while (b1) list = make_promotions<Type, UpRight>(list, pop_lsb(b1));
        while (b2) list = make_promotions<Type, UpLeft>(list, pop_lsb(b2));
        while (b3) list = make_promotions<Type, Up>(list, pop_lsb(b3));
    }

    // Standard and en passant captures
    if constexpr (Type == CAPTURES || Type == EVASIONS || Type == NON_EVASIONS) {
        Bitboard b1 = shift<UpRight>(pawnsNotOn7) & enemies;
        Bitboard b2 = shift<UpLeft>(pawnsNotOn7) & enemies;
        while (b1) {
            int to = pop_lsb(b1);
            *list++ = make_move(to - UpRight, to);
        }
        while (b2) {
            int to = pop_lsb(b2);
            *list++ = make_move(to - UpLeft, to);
        }
        if (pos.ep_square() != SQ_NONE) {
            // An en passant capture can only evade a check given by the pawn that just moved.
            if (Type == EVASIONS && !(target & sq_bb(pos.ep_square() - Up))) return list;
            b1 = pawnsNotOn7 & PawnAttacks[Them][pos.ep_square()];
            while (b1) *list++ = make_special(EN_PASSANT, pop_lsb(b1), pos.ep_square());
        }
    }
    return list;
}

template <int Us, int Pt>
ExtMove* generate_piece_moves(const Position& pos, ExtMove* list, Bitboard target) {
    Bitboard bb = pos.pieces(Us, Pt);
    while (bb) {
        int from = pop_lsb(bb);
        Bitboard b = attacks_bb<Pt>(from, pos.pieces()) & target;
        while (b) *list++ = make_move(from, pop_lsb(b));
    }
    return list;
}

template <int Us, GenType Type>
ExtMove* generate_all(const Position& pos, ExtMove* list) {
    const int ksq = pos.king_sq(Us);
    Bitboard target = 0;

    // Skip non-king moves in double check
    if (Type != EVASIONS || !more_than_one(pos.checkers())) {
        target = Type == EVASIONS      ? BetweenBB[ksq][lsb(pos.checkers())] | pos.checkers()
                 : Type == NON_EVASIONS ? ~pos.pieces_c(Us)
                 : Type == CAPTURES     ? pos.pieces_c(Us ^ 1)
                                        : ~pos.pieces();
        list = generate_pawn_moves<Us, Type>(pos, list, target);
        list = generate_piece_moves<Us, KNIGHT>(pos, list, target);
        list = generate_piece_moves<Us, BISHOP>(pos, list, target);
        list = generate_piece_moves<Us, ROOK>(pos, list, target);
        list = generate_piece_moves<Us, QUEEN>(pos, list, target);
    }

    Bitboard b = PseudoAttacks[KING][ksq] & (Type == EVASIONS ? ~pos.pieces_c(Us) : target);
    while (b) *list++ = make_move(ksq, pop_lsb(b));

    if constexpr (Type == QUIETS || Type == NON_EVASIONS) {
        constexpr int OO = Us == WHITE ? WHITE_OO : BLACK_OO;
        constexpr int OOO = Us == WHITE ? WHITE_OOO : BLACK_OOO;
        if (pos.can_castle(OO) && !pos.castling_impeded(OO))
            *list++ = make_special(CASTLING, ksq, relative_square(Us, SQ_G1));
        if (pos.can_castle(OOO) && !pos.castling_impeded(OOO))
            *list++ = make_special(CASTLING, ksq, relative_square(Us, SQ_C1));
    }
    return list;
}

}  // namespace

template <GenType Type>
ExtMove* generate(const Position& pos, ExtMove* list) {
    static_assert(Type != LEGAL, "LEGAL has its own specialisation");
    assert((Type == EVASIONS) == bool(pos.checkers()) || Type == CAPTURES || Type == QUIETS);
    return pos.side_to_move() == WHITE ? generate_all<WHITE, Type>(pos, list) : generate_all<BLACK, Type>(pos, list);
}

template ExtMove* generate<CAPTURES>(const Position&, ExtMove*);
template ExtMove* generate<QUIETS>(const Position&, ExtMove*);
template ExtMove* generate<EVASIONS>(const Position&, ExtMove*);
template ExtMove* generate<NON_EVASIONS>(const Position&, ExtMove*);

template <>
ExtMove* generate<LEGAL>(const Position& pos, ExtMove* list) {
    const int us = pos.side_to_move();
    const Bitboard pinned = pos.blockers_for_king(us) & pos.pieces_c(us);
    const int ksq = pos.king_sq(us);
    ExtMove* cur = list;
    list = pos.checkers() ? generate<EVASIONS>(pos, list) : generate<NON_EVASIONS>(pos, list);
    while (cur != list) {
        Move m = cur->move;
        // Only pinned pieces, king moves, en passant and castling need the full legality check
        if (((pinned & sq_bb(from_sq(m))) || from_sq(m) == ksq || type_of_move(m) == EN_PASSANT) && !pos.legal(m))
            *cur = (--list)->move;
        else
            ++cur;
    }
    return list;
}
