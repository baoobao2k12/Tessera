// Bitboard utilities, precomputed attack tables and magic/PEXT slider lookup.
#pragma once

#include "types.h"

constexpr Bitboard FileABB = 0x0101010101010101ULL;
constexpr Bitboard FileBBB = FileABB << 1;
constexpr Bitboard FileCBB = FileABB << 2;
constexpr Bitboard FileDBB = FileABB << 3;
constexpr Bitboard FileEBB = FileABB << 4;
constexpr Bitboard FileFBB = FileABB << 5;
constexpr Bitboard FileGBB = FileABB << 6;
constexpr Bitboard FileHBB = FileABB << 7;

constexpr Bitboard Rank1BB = 0xFFULL;
constexpr Bitboard Rank2BB = Rank1BB << 8;
constexpr Bitboard Rank3BB = Rank1BB << 16;
constexpr Bitboard Rank4BB = Rank1BB << 24;
constexpr Bitboard Rank5BB = Rank1BB << 32;
constexpr Bitboard Rank6BB = Rank1BB << 40;
constexpr Bitboard Rank7BB = Rank1BB << 48;
constexpr Bitboard Rank8BB = Rank1BB << 56;

constexpr Bitboard DarkSquares = 0xAA55AA55AA55AA55ULL;
constexpr Bitboard LightSquares = ~DarkSquares;
constexpr Bitboard QueenSide = FileABB | FileBBB | FileCBB | FileDBB;
constexpr Bitboard KingSide = FileEBB | FileFBB | FileGBB | FileHBB;
constexpr Bitboard CenterFiles = FileCBB | FileDBB | FileEBB | FileFBB;
constexpr Bitboard Center = (FileDBB | FileEBB) & (Rank4BB | Rank5BB);

constexpr Bitboard sq_bb(int s) { return 1ULL << s; }
constexpr Bitboard file_bb(int f) { return FileABB << f; }
constexpr Bitboard rank_bb(int r) { return Rank1BB << (8 * r); }
constexpr Bitboard file_bb_of(int s) { return file_bb(file_of(s)); }
constexpr Bitboard rank_bb_of(int s) { return rank_bb(rank_of(s)); }

template <int D>
constexpr Bitboard shift(Bitboard b) {
    if constexpr (D == NORTH) return b << 8;
    else if constexpr (D == SOUTH) return b >> 8;
    else if constexpr (D == NORTH + NORTH) return b << 16;
    else if constexpr (D == SOUTH + SOUTH) return b >> 16;
    else if constexpr (D == EAST) return (b & ~FileHBB) << 1;
    else if constexpr (D == WEST) return (b & ~FileABB) >> 1;
    else if constexpr (D == NORTH_EAST) return (b & ~FileHBB) << 9;
    else if constexpr (D == NORTH_WEST) return (b & ~FileABB) << 7;
    else if constexpr (D == SOUTH_EAST) return (b & ~FileHBB) >> 7;
    else if constexpr (D == SOUTH_WEST) return (b & ~FileABB) >> 9;
    else return 0;
}

inline int popcount(Bitboard b) {
#if defined(_MSC_VER) && defined(USE_POPCNT) && defined(_M_X64)
    return int(__popcnt64(b));
#else
    return std::popcount(b);
#endif
}
inline int lsb(Bitboard b) { assert(b); return std::countr_zero(b); }
inline int msb(Bitboard b) { assert(b); return 63 - std::countl_zero(b); }
inline int pop_lsb(Bitboard& b) { int s = lsb(b); b &= b - 1; return s; }
constexpr bool more_than_one(Bitboard b) { return b & (b - 1); }
// Most advanced square of b from the point of view of color c
inline int frontmost_sq(int c, Bitboard b) { return c == WHITE ? msb(b) : lsb(b); }
// Least advanced square of b from the point of view of color c
inline int backmost_sq(int c, Bitboard b) { return c == WHITE ? lsb(b) : msb(b); }

extern uint8_t SquareDistance[SQUARE_NB][SQUARE_NB];
extern Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
extern Bitboard PseudoAttacks[PIECE_TYPE_NB][SQUARE_NB];
extern Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];  // squares strictly between, 0 if not aligned
extern Bitboard LineBB[SQUARE_NB][SQUARE_NB];     // whole line through both squares, 0 if not aligned
extern Bitboard ForwardRanksBB[COLOR_NB][8];      // ranks strictly in front of a rank
extern Bitboard ForwardFileBB[COLOR_NB][SQUARE_NB];
extern Bitboard PawnAttackSpan[COLOR_NB][SQUARE_NB];
extern Bitboard PassedPawnSpan[COLOR_NB][SQUARE_NB];
extern Bitboard AdjacentFilesBB[8];
extern Bitboard KingFlank[8];

struct Magic {
    Bitboard mask;
    Bitboard magic;
    Bitboard* attacks;
    unsigned shift;
    unsigned index(Bitboard occupied) const {
#if defined(USE_PEXT)
        return unsigned(_pext_u64(occupied, mask));
#else
        return unsigned(((occupied & mask) * magic) >> shift);
#endif
    }
};

extern Magic RookMagics[SQUARE_NB];
extern Magic BishopMagics[SQUARE_NB];

inline Bitboard bishop_attacks(int s, Bitboard occ) { const Magic& m = BishopMagics[s]; return m.attacks[m.index(occ)]; }
inline Bitboard rook_attacks(int s, Bitboard occ) { const Magic& m = RookMagics[s]; return m.attacks[m.index(occ)]; }
inline Bitboard queen_attacks(int s, Bitboard occ) { return bishop_attacks(s, occ) | rook_attacks(s, occ); }

template <int Pt>
inline Bitboard attacks_bb(int s, Bitboard occ) {
    if constexpr (Pt == BISHOP) return bishop_attacks(s, occ);
    else if constexpr (Pt == ROOK) return rook_attacks(s, occ);
    else if constexpr (Pt == QUEEN) return queen_attacks(s, occ);
    else return PseudoAttacks[Pt][s];
}

inline Bitboard attacks_bb(int pt, int s, Bitboard occ) {
    switch (pt) {
    case BISHOP: return bishop_attacks(s, occ);
    case ROOK: return rook_attacks(s, occ);
    case QUEEN: return queen_attacks(s, occ);
    default: return PseudoAttacks[pt][s];
    }
}

template <int C>
constexpr Bitboard pawn_attacks_bb(Bitboard b) {
    return C == WHITE ? shift<NORTH_WEST>(b) | shift<NORTH_EAST>(b) : shift<SOUTH_WEST>(b) | shift<SOUTH_EAST>(b);
}
inline Bitboard pawn_attacks_bb(int c, Bitboard b) { return c == WHITE ? pawn_attacks_bb<WHITE>(b) : pawn_attacks_bb<BLACK>(b); }
template <int C>
constexpr Bitboard pawn_double_attacks_bb(Bitboard b) {
    return C == WHITE ? shift<NORTH_WEST>(b) & shift<NORTH_EAST>(b) : shift<SOUTH_WEST>(b) & shift<SOUTH_EAST>(b);
}

inline bool aligned(int a, int b, int c) { return LineBB[a][b] & sq_bb(c); }
inline int distance(int a, int b) { return SquareDistance[a][b]; }
inline int file_distance(int a, int b) { return std::abs(file_of(a) - file_of(b)); }
inline int rank_distance(int a, int b) { return std::abs(rank_of(a) - rank_of(b)); }
constexpr int edge_distance(int fileOrRank) { return std::min(fileOrRank, 7 - fileOrRank); }
inline bool opposite_colors(int a, int b) { return ((a >> 3) ^ a ^ (b >> 3) ^ b) & 1; }

namespace Bitboards {
void init();
std::string pretty(Bitboard b);
}  // namespace Bitboards
