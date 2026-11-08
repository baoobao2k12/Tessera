// Tessera - a handcrafted-evaluation UCI chess engine.
// Basic types, constants and small helpers shared by every module.
#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <string>

#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(USE_PEXT)
#include <immintrin.h>
#endif

#define ENGINE_NAME "Tessera"
#define ENGINE_VERSION "1.0"
#define ENGINE_AUTHOR "Tessera developers"

using Bitboard = uint64_t;
using Key = uint64_t;
using Move = uint16_t;
using Value = int;
using Depth = int;
// A Score packs a middlegame and an endgame value into one int:
// the low 16 bits hold mg, the high 16 bits hold eg (two's complement).
using Score = int;

constexpr int MAX_MOVES = 256;
constexpr int MAX_PLY = 200;

// Colors, piece types and pieces are plain ints for speed and brevity.
enum : int { WHITE = 0, BLACK = 1, COLOR_NB = 2 };
enum : int { ALL_PIECES = 0, NO_PIECE_TYPE = 0, PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING, PIECE_TYPE_NB = 7 };
enum : int {
    NO_PIECE = 0,
    W_PAWN = 1, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
    B_PAWN = 9, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING,
    PIECE_NB = 16
};

enum : int {
    SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
    SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
    SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
    SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
    SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
    SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
    SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
    SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
    SQ_NONE = 64, SQUARE_NB = 64
};

enum : int { FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H };
enum : int { RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8 };

enum : int {
    NORTH = 8, EAST = 1, SOUTH = -8, WEST = -1,
    NORTH_EAST = 9, NORTH_WEST = 7, SOUTH_EAST = -7, SOUTH_WEST = -9
};

enum : int {
    NO_CASTLING = 0, WHITE_OO = 1, WHITE_OOO = 2, BLACK_OO = 4, BLACK_OOO = 8,
    WHITE_CASTLING = 3, BLACK_CASTLING = 12, ANY_CASTLING = 15
};

constexpr Value VALUE_ZERO = 0;
constexpr Value VALUE_DRAW = 0;
constexpr Value VALUE_KNOWN_WIN = 10000;
constexpr Value VALUE_MATE = 32000;
constexpr Value VALUE_INFINITE = 32001;
constexpr Value VALUE_NONE = 32002;
constexpr Value VALUE_MATE_IN_MAX_PLY = VALUE_MATE - MAX_PLY;
constexpr Value VALUE_MATED_IN_MAX_PLY = -VALUE_MATE_IN_MAX_PLY;

constexpr Value mate_in(int ply) { return VALUE_MATE - ply; }
constexpr Value mated_in(int ply) { return -VALUE_MATE + ply; }
constexpr bool is_mate_score(Value v) { return v >= VALUE_MATE_IN_MAX_PLY || v <= VALUE_MATED_IN_MAX_PLY; }

// Piece values used for SEE, move ordering and search heuristics.
// They are fixed and independent from the (tunable) evaluation material values.
constexpr Value PawnValue = 100, KnightValue = 320, BishopValue = 330, RookValue = 500, QueenValue = 950;
constexpr Value PieceValue[PIECE_NB] = {
    0, PawnValue, KnightValue, BishopValue, RookValue, QueenValue, 0, 0,
    0, PawnValue, KnightValue, BishopValue, RookValue, QueenValue, 0, 0 };

// Move encoding (16 bits):
//   bits 0-5  : destination square
//   bits 6-11 : origin square
//   bits 12-13: promotion piece type - KNIGHT
//   bits 14-15: special move flag (promotion, en passant, castling)
// Castling is encoded as the king moving two squares (e1g1, e1c1, ...).
enum : int { NORMAL = 0, PROMOTION = 1 << 14, EN_PASSANT = 2 << 14, CASTLING = 3 << 14 };
constexpr Move MOVE_NONE = 0;
constexpr Move MOVE_NULL = 65;

constexpr int from_sq(Move m) { return (m >> 6) & 63; }
constexpr int to_sq(Move m) { return m & 63; }
constexpr int from_to(Move m) { return m & 0xFFF; }
constexpr int type_of_move(Move m) { return m & (3 << 14); }
constexpr int promotion_type(Move m) { return ((m >> 12) & 3) + KNIGHT; }
constexpr Move make_move(int from, int to) { return Move((from << 6) | to); }
constexpr Move make_special(int type, int from, int to, int pt = KNIGHT) {
    return Move(type | ((pt - KNIGHT) << 12) | (from << 6) | to);
}
constexpr bool is_ok_move(Move m) { return from_sq(m) != to_sq(m); }  // false for NONE and NULL

constexpr int make_piece(int c, int pt) { return (c << 3) | pt; }
constexpr int type_of(int pc) { return pc & 7; }
constexpr int color_of(int pc) { return pc >> 3; }
constexpr int file_of(int s) { return s & 7; }
constexpr int rank_of(int s) { return s >> 3; }
constexpr int make_square(int f, int r) { return (r << 3) | f; }
constexpr int flip_rank(int s) { return s ^ 56; }
constexpr int flip_file(int s) { return s ^ 7; }
constexpr int relative_square(int c, int s) { return s ^ (c * 56); }
constexpr int relative_rank(int c, int r) { return r ^ (c * 7); }
constexpr int relative_rank_of(int c, int s) { return relative_rank(c, rank_of(s)); }
constexpr int pawn_push(int c) { return c == WHITE ? NORTH : SOUTH; }
constexpr bool is_ok_square(int s) { return s >= SQ_A1 && s <= SQ_H8; }

// ---------------------------------------------------------------------------
// Packed middlegame/endgame scores.
// C++20 guarantees two's complement conversions, so the packing is well defined.
constexpr Score S(int mg, int eg) { return int(unsigned(eg) << 16) + mg; }
constexpr int mg_value(Score s) { return int16_t(uint16_t(unsigned(s))); }
constexpr int eg_value(Score s) { return int16_t(uint16_t((unsigned(s) + 0x8000u) >> 16)); }
constexpr Score SCORE_ZERO = 0;

// Time in milliseconds
using TimePoint = int64_t;
