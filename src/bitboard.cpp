#include "bitboard.h"

#include <cstring>

uint8_t SquareDistance[SQUARE_NB][SQUARE_NB];
Bitboard PawnAttacks[COLOR_NB][SQUARE_NB];
Bitboard PseudoAttacks[PIECE_TYPE_NB][SQUARE_NB];
Bitboard BetweenBB[SQUARE_NB][SQUARE_NB];
Bitboard LineBB[SQUARE_NB][SQUARE_NB];
Bitboard ForwardRanksBB[COLOR_NB][8];
Bitboard ForwardFileBB[COLOR_NB][SQUARE_NB];
Bitboard PawnAttackSpan[COLOR_NB][SQUARE_NB];
Bitboard PassedPawnSpan[COLOR_NB][SQUARE_NB];
Bitboard AdjacentFilesBB[8];
Bitboard KingFlank[8];

Magic RookMagics[SQUARE_NB];
Magic BishopMagics[SQUARE_NB];

namespace {

Bitboard RookTable[0x19000];   // 102400 entries
Bitboard BishopTable[0x1480];  // 5248 entries

constexpr int RookDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
constexpr int BishopDirs[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

Bitboard sliding_attack(const int dirs[4][2], int sq, Bitboard occupied) {
    Bitboard attacks = 0;
    for (int d = 0; d < 4; ++d) {
        int f = file_of(sq) + dirs[d][0], r = rank_of(sq) + dirs[d][1];
        while (f >= 0 && f < 8 && r >= 0 && r < 8) {
            int s = make_square(f, r);
            attacks |= sq_bb(s);
            if (occupied & sq_bb(s)) break;
            f += dirs[d][0];
            r += dirs[d][1];
        }
    }
    return attacks;
}

// Deterministic xorshift64* generator, used only to search for magic numbers.
struct PRNG {
    uint64_t s;
    explicit PRNG(uint64_t seed) : s(seed) {}
    uint64_t rand64() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
    uint64_t sparse() { return rand64() & rand64() & rand64(); }
};

// Builds "fancy" magic bitboards. With USE_PEXT the magic search is skipped and
// the table is indexed directly by pext(occupancy, mask).
void init_magics(Bitboard table[], Magic magics[], const int dirs[4][2]) {
    static Bitboard occupancy[4096], reference[4096];
#if !defined(USE_PEXT)
    static int epoch[4096];
    std::memset(epoch, 0, sizeof(epoch));
    int attempt = 0;
    PRNG rng(0x5DEECE66DULL);
#endif
    Bitboard* next = table;

    for (int s = 0; s < 64; ++s) {
        Bitboard edges = ((Rank1BB | Rank8BB) & ~rank_bb_of(s)) | ((FileABB | FileHBB) & ~file_bb_of(s));
        Magic& m = magics[s];
        m.mask = sliding_attack(dirs, s, 0) & ~edges;
        m.shift = 64 - popcount(m.mask);
        m.attacks = next;
        m.magic = 0;

        int size = 0;
        Bitboard b = 0;
        do {
            occupancy[size] = b;
            reference[size] = sliding_attack(dirs, s, b);
            (void)occupancy;
#if defined(USE_PEXT)
            m.attacks[_pext_u64(b, m.mask)] = reference[size];
#endif
            ++size;
            b = (b - m.mask) & m.mask;
        } while (b);
        next += size;

#if !defined(USE_PEXT)
        for (int i = 0; i < size;) {
            do {
                m.magic = rng.sparse();
            } while (popcount((m.magic * m.mask) >> 56) < 6);
            ++attempt;
            for (i = 0; i < size; ++i) {
                unsigned idx = m.index(occupancy[i]);
                if (epoch[idx] < attempt) {
                    epoch[idx] = attempt;
                    m.attacks[idx] = reference[i];
                } else if (m.attacks[idx] != reference[i])
                    break;
            }
        }
#endif
    }
}

}  // namespace

void Bitboards::init() {
    for (int a = 0; a < 64; ++a)
        for (int b = 0; b < 64; ++b)
            SquareDistance[a][b] = uint8_t(std::max(file_distance(a, b), rank_distance(a, b)));

    for (int f = 0; f < 8; ++f)
        AdjacentFilesBB[f] = (f > 0 ? file_bb(f - 1) : 0) | (f < 7 ? file_bb(f + 1) : 0);

    for (int r = 0; r < 8; ++r) {
        ForwardRanksBB[WHITE][r] = 0;
        ForwardRanksBB[BLACK][r] = 0;
        for (int r2 = r + 1; r2 < 8; ++r2) ForwardRanksBB[WHITE][r] |= rank_bb(r2);
        for (int r2 = r - 1; r2 >= 0; --r2) ForwardRanksBB[BLACK][r] |= rank_bb(r2);
    }

    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < 64; ++s) {
            ForwardFileBB[c][s] = ForwardRanksBB[c][rank_of(s)] & file_bb_of(s);
            PawnAttackSpan[c][s] = ForwardRanksBB[c][rank_of(s)] & AdjacentFilesBB[file_of(s)];
            PassedPawnSpan[c][s] = ForwardFileBB[c][s] | PawnAttackSpan[c][s];
        }

    KingFlank[FILE_A] = QueenSide ^ FileDBB;
    KingFlank[FILE_B] = QueenSide;
    KingFlank[FILE_C] = QueenSide;
    KingFlank[FILE_D] = CenterFiles;
    KingFlank[FILE_E] = CenterFiles;
    KingFlank[FILE_F] = KingSide;
    KingFlank[FILE_G] = KingSide;
    KingFlank[FILE_H] = KingSide ^ FileEBB;

    for (int s = 0; s < 64; ++s) {
        Bitboard b = sq_bb(s);
        PawnAttacks[WHITE][s] = pawn_attacks_bb<WHITE>(b);
        PawnAttacks[BLACK][s] = pawn_attacks_bb<BLACK>(b);

        PseudoAttacks[KING][s] = 0;
        PseudoAttacks[KNIGHT][s] = 0;
        static const int kingSteps[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
        static const int knightSteps[8][2] = {{1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}};
        for (auto& st : kingSteps) {
            int f = file_of(s) + st[0], r = rank_of(s) + st[1];
            if (f >= 0 && f < 8 && r >= 0 && r < 8) PseudoAttacks[KING][s] |= sq_bb(make_square(f, r));
        }
        for (auto& st : knightSteps) {
            int f = file_of(s) + st[0], r = rank_of(s) + st[1];
            if (f >= 0 && f < 8 && r >= 0 && r < 8) PseudoAttacks[KNIGHT][s] |= sq_bb(make_square(f, r));
        }
    }

    init_magics(RookTable, RookMagics, RookDirs);
    init_magics(BishopTable, BishopMagics, BishopDirs);

    for (int s = 0; s < 64; ++s) {
        PseudoAttacks[BISHOP][s] = bishop_attacks(s, 0);
        PseudoAttacks[ROOK][s] = rook_attacks(s, 0);
        PseudoAttacks[QUEEN][s] = PseudoAttacks[BISHOP][s] | PseudoAttacks[ROOK][s];
    }

    for (int a = 0; a < 64; ++a)
        for (int b = 0; b < 64; ++b) {
            BetweenBB[a][b] = 0;
            LineBB[a][b] = 0;
            if (a == b) continue;
            for (int pt : {BISHOP, ROOK})
                if (PseudoAttacks[pt][a] & sq_bb(b)) {
                    LineBB[a][b] = (attacks_bb(pt, a, 0) & attacks_bb(pt, b, 0)) | sq_bb(a) | sq_bb(b);
                    BetweenBB[a][b] = attacks_bb(pt, a, sq_bb(b)) & attacks_bb(pt, b, sq_bb(a));
                }
        }
}

std::string Bitboards::pretty(Bitboard b) {
    std::string s = "+---+---+---+---+---+---+---+---+\n";
    for (int r = RANK_8; r >= RANK_1; --r) {
        for (int f = FILE_A; f <= FILE_H; ++f) s += (b & sq_bb(make_square(f, r))) ? "| X " : "|   ";
        s += "| " + std::to_string(r + 1) + "\n+---+---+---+---+---+---+---+---+\n";
    }
    s += "  a   b   c   d   e   f   g   h\n";
    return s;
}
