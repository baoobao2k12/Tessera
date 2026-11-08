#include "eval.h"

#include <cstring>
#include <iomanip>
#include <sstream>

Score PSQ[PIECE_NB][SQUARE_NB];

void EvalTrace::clear() { std::memset(static_cast<void*>(this), 0, sizeof(*this)); }

void Eval::Cache::clear() {
    std::memset(static_cast<void*>(pawns), 0, sizeof(pawns));
    std::memset(static_cast<void*>(material), 0, sizeof(material));
}

namespace {

constexpr Bitboard AllSquares = ~Bitboard(0);
constexpr int SpaceThreshold = 4800;  // non-pawn material (both sides) needed for the space term

int half_index(int s) { return rank_of(s) * 4 + std::min(file_of(s), 7 - file_of(s)); }

// Piece-square parameter for a piece of type pt on square s (white point of view)
const Score& psqt_param(int pt, int s) {
    switch (pt) {
    case PAWN: return P.PsqtPawn[s];
    case KNIGHT: return P.PsqtKnight[half_index(s)];
    case BISHOP: return P.PsqtBishop[half_index(s)];
    case ROOK: return P.PsqtRook[half_index(s)];
    case QUEEN: return P.PsqtQueen[half_index(s)];
    default: return P.PsqtKing[half_index(s)];
    }
}

const Score* mobility_table(int pt) {
    switch (pt) {
    case KNIGHT: return P.MobilityKnight;
    case BISHOP: return P.MobilityBishop;
    case ROOK: return P.MobilityRook;
    default: return P.MobilityQueen;
    }
}

struct LocalTrace {
    int n = 0;
    int slot[64];
    int coef[64];
    void add(int s, int c) {
        for (int i = 0; i < n; ++i)
            if (slot[i] == s) {
                coef[i] += c;
                return;
            }
        slot[n] = s;
        coef[n++] = c;
    }
};

template <bool T>
class Evaluation {
public:
    Evaluation(const Position& p, Eval::Cache* c, EvalTrace* t) : pos(p), cache(c), tr(t) {}
    Value value();

private:
    // Every parameter use goes through term(), which records the count when tracing.
    Score term(int c, const Score& p, int n = 1) {
        if constexpr (T) tr->lin[param_slot(p)][c] += n;
        return p * n;
    }
    Score lterm(LocalTrace* lt, const Score& p, int n = 1) {
        if constexpr (T) lt->add(param_slot(p), n);
        return p * n;
    }
    void add(int c, const Score& p, int n = 1) { sc[c] += term(c, p, n); }
    void add_kd(int c, int& kd, const Score& w, int n) {
        kd += mg_value(w) * n;
        if constexpr (T) tr->kd[c][param_slot(w)] += n;
    }
    int cx_term(const Score& w, int n) {
        if constexpr (T) tr->cx[param_slot(w)] += n;
        return mg_value(w) * n;
    }

    void probe_material();
    void probe_pawns();
    template <int Us> void eval_pawns(PawnEntry* e);
    template <int Us> Score shelter_at(int ksq, LocalTrace* lt);
    template <int Us> Score king_safety_entry();
    template <int Us> void initialize();
    template <int Us, int Pt> void pieces();
    template <int Us> void king();
    template <int Us> void threats();
    template <int Us> void passed();
    template <int Us> void space();
    void imbalance(int c);
    int scale_factor(int strong);
    Value finalize(Score s);

    const Position& pos;
    Eval::Cache* cache;
    EvalTrace* tr;
    MaterialEntry* me = nullptr;
    MaterialEntry localMe;
    PawnEntry* pe = nullptr;
    PawnEntry localPe;

    Score sc[COLOR_NB] = {SCORE_ZERO, SCORE_ZERO};
    Score mobility[COLOR_NB] = {SCORE_ZERO, SCORE_ZERO};
    Bitboard mobilityArea[COLOR_NB];
    Bitboard attackedBy[COLOR_NB][PIECE_TYPE_NB];
    Bitboard attackedBy2[COLOR_NB];
    Bitboard kingRing[COLOR_NB];
    int kingAttackersCount[COLOR_NB];
    int kingAttackersByType[COLOR_NB][PIECE_TYPE_NB];
    int kingAttacksCount[COLOR_NB];
};

template <bool T>
void Evaluation<T>::probe_material() {
    if constexpr (T) {
        classify_material(pos, localMe);
        me = &localMe;
    } else {
        me = &cache->material[pos.material_key() & (Eval::Cache::MaterialEntries - 1)];
        if (me->key != pos.material_key()) classify_material(pos, *me);
    }
}

template <bool T>
void Evaluation<T>::probe_pawns() {
    Key key = pos.pawn_key();
    if constexpr (T)
        pe = &localPe;
    else {
        pe = &cache->pawns[key & (Eval::Cache::PawnEntries - 1)];
        if (pe->key == key) return;
    }
    pe->key = key;
    pe->blockedCount = 0;
    eval_pawns<WHITE>(pe);
    eval_pawns<BLACK>(pe);
    pe->passedCount = popcount(pe->passedPawns[WHITE] | pe->passedPawns[BLACK]);
    pe->kingSquare[WHITE] = pe->kingSquare[BLACK] = SQ_NONE;
}

// ---------------------------------------------------------------------------
// Pawn structure (cached in the pawn hash table)

template <bool T>
template <int Us>
void Evaluation<T>::eval_pawns(PawnEntry* e) {
    constexpr int Them = Us ^ 1;
    constexpr int Up = Us == WHITE ? NORTH : SOUTH;
    constexpr int Down = -Up;

    const Bitboard ourPawns = pos.pieces(Us, PAWN);
    const Bitboard theirPawns = pos.pieces(Them, PAWN);
    const Bitboard doubleAttackThem = pawn_double_attacks_bb<Them>(theirPawns);
    Score score = SCORE_ZERO;

    e->passedPawns[Us] = 0;
    e->pawnAttacks[Us] = e->pawnAttacksSpan[Us] = pawn_attacks_bb<Us>(ourPawns);
    e->blockedCount += popcount(shift<Up>(ourPawns) & (theirPawns | doubleAttackThem));

    Bitboard b = ourPawns;
    while (b) {
        const int s = pop_lsb(b);
        const int r = relative_rank_of(Us, s);
        const int f = file_of(s);

        const Bitboard opposed = theirPawns & ForwardFileBB[Us][s];
        const Bitboard blocked = theirPawns & sq_bb(s + Up);
        const Bitboard stoppers = theirPawns & PassedPawnSpan[Us][s];
        const Bitboard lever = theirPawns & PawnAttacks[Us][s];
        const Bitboard leverPush = theirPawns & PawnAttacks[Us][s + Up];
        const bool doubled = ourPawns & sq_bb(s + Down);
        const Bitboard neighbours = ourPawns & AdjacentFilesBB[f];
        const Bitboard phalanx = neighbours & rank_bb_of(s);
        const Bitboard support = neighbours & rank_bb_of(s + Down);

        // Behind all pawns on the adjacent files and unable to advance safely
        const bool backward = !(neighbours & ForwardRanksBB[Them][rank_of(s + Up)]) && (leverPush | blocked);
        if (!backward && !blocked) e->pawnAttacksSpan[Us] |= PawnAttackSpan[Us][s];

        if (!stoppers)
            e->passedPawns[Us] |= sq_bb(s);
        else if (!(stoppers ^ lever) ||
                 (!(stoppers ^ leverPush) && popcount(phalanx) >= popcount(leverPush)) ||
                 (stoppers == blocked && r >= RANK_5 && (shift<Up>(support) & ~(theirPawns | doubleAttackThem))))
            score += term(Us, P.CandidatePassed[r]);

        if (support | phalanx) {
            score += term(Us, P.ConnectedBase[r]);
            if (phalanx) score += term(Us, P.ConnectedPhalanx[r]);
            if (opposed) score += term(Us, P.ConnectedOpposed[r]);
            score += term(Us, P.PawnSupport[0], popcount(support));
        } else if (!neighbours) {
            if (opposed && (ourPawns & ForwardFileBB[Them][s]) && !(theirPawns & AdjacentFilesBB[f]))
                score += term(Us, P.PawnDoubledIsolated[0]);
            else {
                score += term(Us, P.PawnIsolated[edge_distance(f)]);
                if (!opposed) score += term(Us, P.PawnWeakUnopposed[0]);
            }
        } else if (backward) {
            score += term(Us, P.PawnBackward[0]);
            if (!opposed) score += term(Us, P.PawnWeakUnopposed[0]);
        }

        if (!support) {
            if (doubled) score += term(Us, P.PawnDoubled[0]);
            if (more_than_one(lever)) score += term(Us, P.PawnWeakLever[0]);
        }
        if (blocked && r > RANK_4) score += term(Us, P.PawnBlocked[r]);
    }

    // Pawn islands
    unsigned files = 0;
    for (Bitboard p = ourPawns; p;) files |= 1u << file_of(pop_lsb(p));
    int islands = std::popcount(files & ~(files << 1));
    score += term(Us, P.PawnIslands[0], islands);

    // Flank majorities and minority attacks
    for (Bitboard flank : {QueenSide, KingSide}) {
        int ours = popcount(ourPawns & flank), theirs = popcount(theirPawns & flank);
        if (ours > theirs) score += term(Us, P.PawnMajority[0]);
        else if (ours && ours < theirs)
            score += term(Us, P.MinorityAttack[0], popcount(pawn_attacks_bb<Us>(ourPawns & flank) & theirPawns));
    }

    // Holes in our camp that no pawn of ours can ever defend
    constexpr Bitboard OwnCamp = CenterFiles & (Us == WHITE ? Rank3BB | Rank4BB : Rank6BB | Rank5BB);
    score += term(Us, P.WeakSquare[0], popcount(OwnCamp & ~e->pawnAttacksSpan[Us]));

    e->scores[Us] = score;
}

template <bool T>
template <int Us>
Score Evaluation<T>::shelter_at(int ksq, LocalTrace* lt) {
    constexpr int Them = Us ^ 1;
    Bitboard b = pos.pieces(PAWN) & ~ForwardRanksBB[Them][rank_of(ksq)];
    Bitboard ourPawns = b & pos.pieces_c(Us) & ~pe->pawnAttacks[Them];
    Bitboard theirPawns = b & pos.pieces_c(Them);
    Score bonus = SCORE_ZERO;

    int center = std::clamp(file_of(ksq), int(FILE_B), int(FILE_G));
    for (int f = center - 1; f <= center + 1; ++f) {
        b = ourPawns & file_bb(f);
        int ourRank = b ? relative_rank_of(Us, backmost_sq(Us, b)) : 0;
        b = theirPawns & file_bb(f);
        int theirRank = b ? relative_rank_of(Us, frontmost_sq(Them, b)) : 0;
        int d = edge_distance(f);
        bonus += lterm(lt, P.Shelter[d * 8 + ourRank]);
        if (ourRank && ourRank == theirRank - 1)
            bonus += lterm(lt, P.BlockedStorm[theirRank]);
        else
            bonus += lterm(lt, P.UnblockedStorm[d * 8 + theirRank]);
    }
    int semiUs = !(pos.pieces(Us, PAWN) & file_bb_of(ksq));
    int semiThem = !(pos.pieces(Them, PAWN) & file_bb_of(ksq));
    bonus += lterm(lt, P.KingOnFile[semiUs * 2 + semiThem]);
    return bonus;
}

template <bool T>
template <int Us>
Score Evaluation<T>::king_safety_entry() {
    const int ksq = pos.king_sq(Us);
    const int cr = pos.castling_rights() & (Us == WHITE ? WHITE_CASTLING : BLACK_CASTLING);
    if constexpr (!T)
        if (pe->kingSquare[Us] == ksq && pe->castling[Us] == cr) return pe->kingSafety[Us];

    LocalTrace best;
    Score shelter = shelter_at<Us>(ksq, &best);
    auto consider = [&](int sq) {
        LocalTrace lt;
        Score s = shelter_at<Us>(sq, &lt);
        if (mg_value(s) > mg_value(shelter)) {
            shelter = s;
            if constexpr (T) best = lt;
        }
    };
    if (pos.can_castle(Us == WHITE ? WHITE_OO : BLACK_OO)) consider(relative_square(Us, SQ_G1));
    if (pos.can_castle(Us == WHITE ? WHITE_OOO : BLACK_OOO)) consider(relative_square(Us, SQ_C1));
    if constexpr (T)
        for (int i = 0; i < best.n; ++i) tr->lin[best.slot[i]][Us] += best.coef[i];

    Bitboard pawns = pos.pieces(Us, PAWN);
    int minPawnDist = 6;
    if (pawns & PseudoAttacks[KING][ksq]) minPawnDist = 1;
    else
        while (pawns) minPawnDist = std::min(minPawnDist, distance(ksq, pop_lsb(pawns)));

    Score result = shelter + term(Us, P.KingPawnDistance[0], minPawnDist);
    if constexpr (!T) {
        pe->kingSquare[Us] = ksq;
        pe->castling[Us] = cr;
        pe->kingSafety[Us] = result;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Pieces

template <bool T>
template <int Us>
void Evaluation<T>::initialize() {
    constexpr int Them = Us ^ 1;
    constexpr int Down = Us == WHITE ? SOUTH : NORTH;
    constexpr Bitboard LowRanks = Us == WHITE ? Rank2BB | Rank3BB : Rank7BB | Rank6BB;
    const int ksq = pos.king_sq(Us);
    const Bitboard dblAttackByPawn = pawn_double_attacks_bb<Us>(pos.pieces(Us, PAWN));

    // Blocked pawns and pawns on the first two ranks do not count as mobility area
    Bitboard b = pos.pieces(Us, PAWN) & (shift<Down>(pos.pieces()) | LowRanks);
    mobilityArea[Us] = ~(b | pos.pieces(Us, KING, QUEEN) | pos.blockers_for_king(Us) | pe->pawnAttacks[Them]);

    attackedBy[Us][KING] = PseudoAttacks[KING][ksq];
    attackedBy[Us][PAWN] = pe->pawnAttacks[Us];
    attackedBy[Us][ALL_PIECES] = attackedBy[Us][KING] | attackedBy[Us][PAWN];
    attackedBy2[Us] = dblAttackByPawn | (attackedBy[Us][KING] & attackedBy[Us][PAWN]);

    int s = make_square(std::clamp(file_of(ksq), int(FILE_B), int(FILE_G)), std::clamp(rank_of(ksq), int(RANK_2), int(RANK_7)));
    kingRing[Us] = PseudoAttacks[KING][s] | sq_bb(s);
    kingAttackersCount[Them] = popcount(kingRing[Us] & pe->pawnAttacks[Them]);
    kingAttacksCount[Them] = 0;
    for (int pt = 0; pt < PIECE_TYPE_NB; ++pt) kingAttackersByType[Them][pt] = 0;
    kingRing[Us] &= ~dblAttackByPawn;
}

template <bool T>
template <int Us, int Pt>
void Evaluation<T>::pieces() {
    constexpr int Them = Us ^ 1;
    constexpr int Down = Us == WHITE ? SOUTH : NORTH;
    constexpr Bitboard OutpostRanks = Us == WHITE ? Rank4BB | Rank5BB | Rank6BB : Rank5BB | Rank4BB | Rank3BB;
    const Score* mobTable = mobility_table(Pt);

    Bitboard b1 = pos.pieces(Us, Pt);
    attackedBy[Us][Pt] = 0;

    while (b1) {
        const int s = pop_lsb(b1);
        // Attacks, with x-rays through queens (and own rooks for rooks)
        Bitboard b = Pt == BISHOP ? bishop_attacks(s, pos.pieces() ^ pos.pieces(QUEEN))
                     : Pt == ROOK ? rook_attacks(s, pos.pieces() ^ pos.pieces(QUEEN) ^ pos.pieces(Us, ROOK))
                                  : attacks_bb<Pt>(s, pos.pieces());
        if (pos.blockers_for_king(Us) & sq_bb(s)) b &= LineBB[pos.king_sq(Us)][s];

        attackedBy2[Us] |= attackedBy[Us][ALL_PIECES] & b;
        attackedBy[Us][Pt] |= b;
        attackedBy[Us][ALL_PIECES] |= b;

        if (b & kingRing[Them]) {
            kingAttackersCount[Us]++;
            kingAttackersByType[Us][Pt]++;
            kingAttacksCount[Us] += popcount(b & attackedBy[Them][KING]);
        } else if (Pt == ROOK && (file_bb_of(s) & kingRing[Them]))
            add(Us, P.RookOnKingRing[0]);
        else if (Pt == BISHOP && (bishop_attacks(s, pos.pieces(PAWN)) & kingRing[Them]))
            add(Us, P.BishopOnKingRing[0]);

        const int mob = popcount(b & mobilityArea[Us]);
        Score m = term(Us, mobTable[mob]);
        mobility[Us] += m;
        sc[Us] += m;

        if constexpr (Pt == BISHOP || Pt == KNIGHT) {
            const Bitboard outposts = OutpostRanks & attackedBy[Us][PAWN] & ~pe->pawnAttacksSpan[Them];
            if (outposts & sq_bb(s))
                add(Us, Pt == KNIGHT ? P.KnightOutpost[0] : P.BishopOutpost[0]);
            else if (Pt == KNIGHT && (b & outposts & ~pos.pieces_c(Us)))
                add(Us, P.ReachableOutpost[0]);

            if (shift<Down>(pos.pieces(PAWN)) & sq_bb(s)) add(Us, P.MinorBehindPawn[0]);
            add(Us, P.KingProtector[Pt == KNIGHT ? 0 : 1], distance(pos.king_sq(Us), s));

            if constexpr (Pt == BISHOP) {
                const Bitboard blocked = pos.pieces(Us, PAWN) & shift<Down>(pos.pieces());
                const Bitboard sameColor = (DarkSquares & sq_bb(s)) ? DarkSquares : LightSquares;
                add(Us, P.BishopPawns[0],
                    popcount(pos.pieces(Us, PAWN) & sameColor) *
                        (!(attackedBy[Us][PAWN] & sq_bb(s)) + popcount(blocked & CenterFiles)));
                add(Us, P.BishopXrayPawns[0], popcount(PseudoAttacks[BISHOP][s] & pos.pieces(Them, PAWN)));
                if (more_than_one(bishop_attacks(s, pos.pieces(PAWN)) & Center)) add(Us, P.LongDiagonalBishop[0]);

                // Bishop trapped on a7/h7 (a2/h2) by a pawn on b6/g6 (b3/g3)
                const int rs = relative_square(Us, s);
                if ((rs == SQ_A7 && pos.piece_on(relative_square(Us, SQ_B6)) == make_piece(Them, PAWN)) ||
                    (rs == SQ_H7 && pos.piece_on(relative_square(Us, SQ_G6)) == make_piece(Them, PAWN)) ||
                    (rs == SQ_A6 && pos.piece_on(relative_square(Us, SQ_B5)) == make_piece(Them, PAWN)) ||
                    (rs == SQ_H6 && pos.piece_on(relative_square(Us, SQ_G5)) == make_piece(Them, PAWN)))
                    add(Us, P.TrappedBishop[0]);
            }
        }

        if constexpr (Pt == ROOK) {
            if (!(pos.pieces(Us, PAWN) & file_bb_of(s)))
                add(Us, P.RookOnFile[!(pos.pieces(Them, PAWN) & file_bb_of(s))]);
            else {
                if (pos.pieces(Us, PAWN) & shift<Down>(pos.pieces()) & file_bb_of(s)) add(Us, P.RookOnClosedFile[0]);
                if (mob <= 3) {
                    int kf = file_of(pos.king_sq(Us));
                    if ((kf < FILE_E) == (file_of(s) < kf))
                        add(Us, P.TrappedRook[0], 1 + !pos.can_castle(Us == WHITE ? WHITE_CASTLING : BLACK_CASTLING));
                }
            }
            if (file_bb_of(s) & pos.pieces(QUEEN)) add(Us, P.RookOnQueenFile[0]);
            if (relative_rank_of(Us, s) == RANK_7 &&
                ((pos.pieces(Them, PAWN) & rank_bb(relative_rank(Us, RANK_7))) ||
                 relative_rank_of(Us, pos.king_sq(Them)) == RANK_8))
                add(Us, P.RookOn7th[0]);
            if (rook_attacks(s, pos.pieces()) & pos.pieces(Us, ROOK)) add(Us, P.RooksConnected[0]);
        }

        if constexpr (Pt == QUEEN) {
            Bitboard queenPinners;
            if (pos.slider_blockers(pos.pieces(Them, ROOK, BISHOP), s, queenPinners)) add(Us, P.WeakQueen[0]);
            if (relative_rank_of(Us, s) > RANK_4 && !(pe->pawnAttacksSpan[Them] & sq_bb(s)))
                add(Us, P.QueenInfiltration[0]);
        }
    }
}

// ---------------------------------------------------------------------------
// King safety

template <bool T>
template <int Us>
void Evaluation<T>::king() {
    constexpr int Them = Us ^ 1;
    constexpr Bitboard Camp =
        Us == WHITE ? AllSquares ^ Rank6BB ^ Rank7BB ^ Rank8BB : AllSquares ^ Rank1BB ^ Rank2BB ^ Rank3BB;
    const int ksq = pos.king_sq(Us);

    const Score shelter = king_safety_entry<Us>();
    sc[Us] += shelter;

    // Squares attacked by the enemy and defended at most once, by our king or queen only
    const Bitboard weak = attackedBy[Them][ALL_PIECES] & ~attackedBy2[Us] &
                          (~attackedBy[Us][ALL_PIECES] | attackedBy[Us][KING] | attackedBy[Us][QUEEN]);
    const Bitboard safe = ~pos.pieces_c(Them) & (~attackedBy[Us][ALL_PIECES] | (weak & attackedBy2[Them]));
    const Bitboard b1 = rook_attacks(ksq, pos.pieces() ^ pos.pieces(Us, QUEEN));
    const Bitboard b2 = bishop_attacks(ksq, pos.pieces() ^ pos.pieces(Us, QUEEN));

    int kd = 0;
    Bitboard unsafeChecks = 0;

    const Bitboard rookChecks = b1 & attackedBy[Them][ROOK] & safe;
    if (rookChecks)
        add_kd(Us, kd, more_than_one(rookChecks) ? P.KDSafeCheckMulti[ROOK] : P.KDSafeCheck[ROOK], 1);
    else
        unsafeChecks |= b1 & attackedBy[Them][ROOK];

    const Bitboard queenChecks = (b1 | b2) & attackedBy[Them][QUEEN] & safe & ~(attackedBy[Us][QUEEN] | rookChecks);
    if (queenChecks)
        add_kd(Us, kd, more_than_one(queenChecks) ? P.KDSafeCheckMulti[QUEEN] : P.KDSafeCheck[QUEEN], 1);

    const Bitboard bishopChecks = b2 & attackedBy[Them][BISHOP] & safe & ~queenChecks;
    if (bishopChecks)
        add_kd(Us, kd, more_than_one(bishopChecks) ? P.KDSafeCheckMulti[BISHOP] : P.KDSafeCheck[BISHOP], 1);
    else
        unsafeChecks |= b2 & attackedBy[Them][BISHOP];

    const Bitboard knightChecks = PseudoAttacks[KNIGHT][ksq] & attackedBy[Them][KNIGHT];
    if (knightChecks & safe)
        add_kd(Us, kd, more_than_one(knightChecks & safe) ? P.KDSafeCheckMulti[KNIGHT] : P.KDSafeCheck[KNIGHT], 1);
    else
        unsafeChecks |= knightChecks;

    const Bitboard flank = KingFlank[file_of(ksq)] & Camp;
    const Bitboard fAtt = attackedBy[Them][ALL_PIECES] & flank;
    const int kingFlankAttack = popcount(fAtt) + popcount(fAtt & attackedBy2[Them]);
    const int kingFlankDefense = popcount(attackedBy[Us][ALL_PIECES] & flank);

    for (int pt = KNIGHT; pt <= QUEEN; ++pt)
        add_kd(Us, kd, P.KDAttackerWeight[pt], kingAttackersCount[Them] * kingAttackersByType[Them][pt]);
    add_kd(Us, kd, P.KDWeakRing[0], popcount(kingRing[Us] & weak));
    add_kd(Us, kd, P.KDUnsafeChecks[0], popcount(unsafeChecks));
    add_kd(Us, kd, P.KDBlockers[0], popcount(pos.blockers_for_king(Us)));
    add_kd(Us, kd, P.KDRingAttacks[0], kingAttacksCount[Them]);
    add_kd(Us, kd, P.KDFlankAttackSq[0], kingFlankAttack * kingFlankAttack / 8);
    add_kd(Us, kd, P.KDNoQueen[0], !pos.count(Them, QUEEN));
    add_kd(Us, kd, P.KDKnightDefender[0], bool(attackedBy[Us][KNIGHT] & attackedBy[Us][KING]));
    add_kd(Us, kd, P.KDFlankDefense[0], kingFlankDefense);
    add_kd(Us, kd, P.KDBase[0], 1);

    const int kdConst = mg_value(mobility[Them] - mobility[Us]) - 6 * mg_value(shelter) / 8;
    kd += kdConst;
    if constexpr (T) tr->kdConst[Us] += kdConst;

    if (kd > 100) {
        const int k = std::min(kd, 15000);
        sc[Us] -= S(k * k / KD_MG_DIV, k / KD_EG_DIV);
    }

    if (!(pos.pieces(PAWN) & KingFlank[file_of(ksq)])) add(Us, P.PawnlessFlank[0]);
    add(Us, P.FlankAttacks[0], kingFlankAttack);
}

// ---------------------------------------------------------------------------
// Threats

template <bool T>
template <int Us>
void Evaluation<T>::threats() {
    constexpr int Them = Us ^ 1;
    constexpr int Up = Us == WHITE ? NORTH : SOUTH;
    constexpr Bitboard TRank3BB = Us == WHITE ? Rank3BB : Rank6BB;

    const Bitboard nonPawnEnemies = pos.pieces_c(Them) & ~pos.pieces(PAWN);
    const Bitboard stronglyProtected = attackedBy[Them][PAWN] | (attackedBy2[Them] & ~attackedBy2[Us]);
    const Bitboard defended = nonPawnEnemies & stronglyProtected;
    const Bitboard weak = pos.pieces_c(Them) & ~stronglyProtected & attackedBy[Us][ALL_PIECES];
    Bitboard b;

    if (defended | weak) {
        b = (defended | weak) & (attackedBy[Us][KNIGHT] | attackedBy[Us][BISHOP]);
        while (b) add(Us, P.ThreatByMinor[type_of(pos.piece_on(pop_lsb(b)))]);
        b = weak & attackedBy[Us][ROOK];
        while (b) add(Us, P.ThreatByRook[type_of(pos.piece_on(pop_lsb(b)))]);
        if (weak & attackedBy[Us][KING]) add(Us, P.ThreatByKing[0]);
        b = ~attackedBy[Them][ALL_PIECES] | (nonPawnEnemies & attackedBy2[Us]);
        add(Us, P.Hanging[0], popcount(weak & b));
        add(Us, P.WeakQueenProtection[0], popcount(weak & attackedBy[Them][QUEEN]));
    }

    // Restricting the enemy's moves
    b = attackedBy[Them][ALL_PIECES] & ~stronglyProtected & attackedBy[Us][ALL_PIECES];
    add(Us, P.RestrictedPiece[0], popcount(b));

    // Pawn threats, now and after a safe push
    Bitboard safe = ~attackedBy[Them][ALL_PIECES] | attackedBy[Us][ALL_PIECES];
    b = pawn_attacks_bb<Us>(pos.pieces(Us, PAWN) & safe) & nonPawnEnemies;
    add(Us, P.ThreatBySafePawn[0], popcount(b));

    b = shift<Up>(pos.pieces(Us, PAWN)) & ~pos.pieces();
    b |= shift<Up>(b & TRank3BB) & ~pos.pieces();
    b &= ~attackedBy[Them][PAWN] & safe;
    b = pawn_attacks_bb<Us>(b) & nonPawnEnemies;
    add(Us, P.ThreatByPawnPush[0], popcount(b));

    // Threats against the enemy queen on the next move
    if (pos.count(Them, QUEEN) == 1) {
        const bool queenImbalance = pos.count(QUEEN) == 1;
        const int s = lsb(pos.pieces(Them, QUEEN));
        safe = mobilityArea[Us] & ~pos.pieces(Us, PAWN) & ~stronglyProtected;
        b = attackedBy[Us][KNIGHT] & PseudoAttacks[KNIGHT][s];
        add(Us, P.KnightOnQueen[0], popcount(b & safe) * (1 + queenImbalance));
        b = (attackedBy[Us][BISHOP] & bishop_attacks(s, pos.pieces())) | (attackedBy[Us][ROOK] & rook_attacks(s, pos.pieces()));
        add(Us, P.SliderOnQueen[0], popcount(b & safe & attackedBy2[Us]) * (1 + queenImbalance));
    }
}

// ---------------------------------------------------------------------------
// Passed pawns

template <bool T>
template <int Us>
void Evaluation<T>::passed() {
    constexpr int Them = Us ^ 1;
    constexpr int Up = Us == WHITE ? NORTH : SOUTH;
    auto king_proximity = [&](int c, int s) { return std::min(distance(pos.king_sq(c), s), 5); };

    Bitboard b = pe->passedPawns[Us];
    while (b) {
        const int s = pop_lsb(b);
        const int r = relative_rank_of(Us, s);
        add(Us, P.PassedRank[r]);

        if (r > RANK_3) {
            const int blockSq = s + Up;
            add(Us, P.PassedEnemyKing[r], king_proximity(Them, blockSq));
            add(Us, P.PassedOwnKing[r], king_proximity(Us, blockSq));

            if (pos.empty(blockSq)) {
                const Bitboard squaresToQueen = ForwardFileBB[Us][s];
                Bitboard unsafeSquares = PassedPawnSpan[Us][s];
                const Bitboard behind = ForwardFileBB[Them][s] & pos.pieces_pt(ROOK, QUEEN);
                if (!(pos.pieces_c(Them) & behind)) unsafeSquares &= attackedBy[Them][ALL_PIECES];

                if (!unsafeSquares) add(Us, P.PassedFree[r]);
                else if (!(unsafeSquares & squaresToQueen)) add(Us, P.PassedSafePath[r]);
                else if (!(unsafeSquares & sq_bb(blockSq))) add(Us, P.PassedSafeBlock[r]);

                if ((pos.pieces_c(Us) & behind) || (attackedBy[Us][ALL_PIECES] & sq_bb(blockSq)))
                    add(Us, P.PassedDefended[r]);
            } else if (pos.pieces_c(Them) & sq_bb(blockSq))
                add(Us, P.PassedBlocked[r]);
        }

        add(Us, P.PassedFile[0], edge_distance(file_of(s)));
        if (AdjacentFilesBB[file_of(s)] & pe->passedPawns[Us]) add(Us, P.PassedConnected[0]);
        if (attackedBy[Us][PAWN] & sq_bb(s)) add(Us, P.PassedProtected[0]);
        if (rook_attacks(s, pos.pieces()) & ForwardFileBB[Them][s] & pos.pieces(Us, ROOK)) add(Us, P.RookBehindPassed[0]);

        // Rule of the square in pawn endings
        if (pos.non_pawn_material(Them) == 0) {
            const int queenSq = make_square(file_of(s), relative_rank(Us, RANK_8));
            const int pawnDist = std::min(5, 7 - r);
            const int kingDist = distance(pos.king_sq(Them), queenSq) - (pos.side_to_move() == Them);
            if (pawnDist < kingDist && !(ForwardFileBB[Us][s] & pos.pieces_c(Us))) add(Us, P.PassedUnstoppable[0]);
        }
    }
}

// ---------------------------------------------------------------------------
// Space

template <bool T>
template <int Us>
void Evaluation<T>::space() {
    if (pos.non_pawn_material() < SpaceThreshold) return;
    constexpr int Them = Us ^ 1;
    constexpr int Down = Us == WHITE ? SOUTH : NORTH;
    constexpr Bitboard SpaceMask =
        Us == WHITE ? CenterFiles & (Rank2BB | Rank3BB | Rank4BB) : CenterFiles & (Rank7BB | Rank6BB | Rank5BB);

    const Bitboard safe = SpaceMask & ~pos.pieces(Us, PAWN) & ~attackedBy[Them][PAWN];
    Bitboard behind = pos.pieces(Us, PAWN);
    behind |= shift<Down>(behind);
    behind |= shift<Down + Down>(behind);
    const int bonus = popcount(safe) + popcount(behind & safe & ~attackedBy[Them][ALL_PIECES]);
    const int weight = pos.count(Us, ALL_PIECES) - 3 + std::min(pe->blockedCount, 9);
    add(Us, P.Space[0], bonus * weight * weight / 64);
}

template <bool T>
void Evaluation<T>::imbalance(int c) {
    const int pawns = pos.count(c, PAWN);
    if (pos.count(c, BISHOP) >= 2) add(c, P.BishopPair[0]);
    add(c, P.ImbKnightPawns[0], pos.count(c, KNIGHT) * (pawns - 5));
    add(c, P.ImbRookPawns[0], pos.count(c, ROOK) * (pawns - 5));
    if (pos.count(c, ROOK) >= 2) add(c, P.ImbRookPair[0]);
    add(c, P.ImbQueenMinors[0], pos.count(c, QUEEN) * (pos.count(c, KNIGHT) + pos.count(c, BISHOP)));
}

// ---------------------------------------------------------------------------
// Final assembly

template <bool T>
int Evaluation<T>::scale_factor(int strong) {
    const int weak = strong ^ 1;
    int sf = SCALE_NONE;
    if (me->scaleFn[strong] != SC_NONE) sf = endgame_scale(me->scaleFn[strong], strong, pos);
    if (sf == SCALE_NONE) sf = me->factor[strong];

    if (sf == SCALE_NORMAL) {
        if (pos.opposite_bishops()) {
            if (pos.non_pawn_material() == 2 * BishopValue) sf = 18 + 4 * popcount(pe->passedPawns[strong]);
            else sf = 22 + 3 * pos.count(strong, ALL_PIECES);
        } else if (pos.non_pawn_material(WHITE) == RookValue && pos.non_pawn_material(BLACK) == RookValue &&
                   pos.count(strong, PAWN) - pos.count(weak, PAWN) <= 1 &&
                   bool(KingSide & pos.pieces(strong, PAWN)) != bool(QueenSide & pos.pieces(strong, PAWN)) &&
                   (PseudoAttacks[KING][pos.king_sq(weak)] & pos.pieces(weak, PAWN)))
            sf = 36;
        else if (pos.count(QUEEN) == 1)
            sf = 37 + 3 * (pos.count(WHITE, QUEEN) == 1 ? pos.count(BLACK, BISHOP) + pos.count(BLACK, KNIGHT)
                                                         : pos.count(WHITE, BISHOP) + pos.count(WHITE, KNIGHT));
        else
            sf = std::min(sf, 36 + 7 * pos.count(strong, PAWN));
    }
    return std::min(sf, int(SCALE_NORMAL));
}

template <bool T>
Value Evaluation<T>::finalize(Score s) {
    int mg = mg_value(s), eg = eg_value(s);

    // Complexity: adjust the endgame score towards (or away from) zero, never flipping its sign
    const int wk = pos.king_sq(WHITE), bk = pos.king_sq(BLACK);
    const int outflanking = file_distance(wk, bk) - rank_distance(wk, bk);
    const bool bothFlanks = (pos.pieces(PAWN) & QueenSide) && (pos.pieces(PAWN) & KingSide);
    const bool almostUnwinnable = outflanking < 0 && !bothFlanks;
    const bool infiltration = rank_of(wk) > RANK_4 || rank_of(bk) < RANK_5;
    const int complexity = cx_term(P.CxPassed[0], pe->passedCount) + cx_term(P.CxPawns[0], pos.count(PAWN)) +
                           cx_term(P.CxOutflanking[0], outflanking) + cx_term(P.CxBothFlanks[0], bothFlanks) +
                           cx_term(P.CxInfiltration[0], infiltration) +
                           cx_term(P.CxPawnEnding[0], !pos.non_pawn_material()) +
                           cx_term(P.CxUnwinnable[0], almostUnwinnable) + cx_term(P.CxBase[0], 1);
    eg += ((eg > 0) - (eg < 0)) * std::max(complexity, -std::abs(eg));

    const int strong = eg > 0 ? WHITE : BLACK;
    const int sf = scale_factor(strong);
    const int phase = me->phase;
    Value v = (mg * phase + eg * (24 - phase) * sf / 64) / 24;

    if constexpr (T) {
        tr->phase = phase;
        tr->scale = sf;
        tr->eval = v;
    }
    v = pos.side_to_move() == WHITE ? v : -v;
    // Damp the evaluation as the fifty-move counter grows
    v = v * (200 - std::min(pos.rule50_count(), 100)) / 200;
    return v;
}

template <bool T>
Value Evaluation<T>::value() {
    probe_material();
    if (me->evalFn != EG_NONE) {
        if constexpr (T) tr->special = true;
        return endgame_value(me->evalFn, me->evalStrong, pos);
    }
    probe_pawns();

    Score base = SCORE_ZERO;
    if constexpr (T) {
        for (Bitboard b = pos.pieces(); b;) {
            const int s = pop_lsb(b);
            const int pc = pos.piece_on(s), c = color_of(pc), pt = type_of(pc);
            add(c, P.Material[pt]);
            add(c, psqt_param(pt, relative_square(c, s)));
        }
    } else
        base = pos.psq_score();

    imbalance(WHITE);
    imbalance(BLACK);
    sc[WHITE] += pe->scores[WHITE];
    sc[BLACK] += pe->scores[BLACK];

    if constexpr (!T) {
        // Lazy exit for clearly decided positions
        const Score s = base + sc[WHITE] - sc[BLACK];
        const int v = (mg_value(s) * me->phase + eg_value(s) * (24 - me->phase)) / 24;
        if (std::abs(v) > Eval::LazyThreshold) return pos.side_to_move() == WHITE ? v : -v;
    }

    initialize<WHITE>();
    initialize<BLACK>();
    pieces<WHITE, KNIGHT>();
    pieces<BLACK, KNIGHT>();
    pieces<WHITE, BISHOP>();
    pieces<BLACK, BISHOP>();
    pieces<WHITE, ROOK>();
    pieces<BLACK, ROOK>();
    pieces<WHITE, QUEEN>();
    pieces<BLACK, QUEEN>();
    king<WHITE>();
    king<BLACK>();
    threats<WHITE>();
    threats<BLACK>();
    passed<WHITE>();
    passed<BLACK>();
    space<WHITE>();
    space<BLACK>();
    add(pos.side_to_move(), P.Tempo[0]);

    return finalize(base + sc[WHITE] - sc[BLACK]);
}

}  // namespace

void Eval::init_psqt() {
    for (int pt = PAWN; pt <= KING; ++pt)
        for (int s = 0; s < 64; ++s) {
            Score v = P.Material[pt] + psqt_param(pt, s);
            PSQ[make_piece(WHITE, pt)][s] = v;
            PSQ[make_piece(BLACK, pt)][flip_rank(s)] = -v;
        }
}

void Eval::init() {
    set_params(P, true);
    init_psqt();
    Bitbases::init();
}

Value Eval::evaluate(const Position& pos, Cache& cache) {
    assert(!pos.checkers());
    return Evaluation<false>(pos, &cache, nullptr).value();
}

Value Eval::evaluate_trace(const Position& pos, EvalTrace& trace) {
    trace.clear();
    return Evaluation<true>(pos, nullptr, &trace).value();
}

std::string Eval::describe(const Position& pos) {
    auto tr = std::make_unique<EvalTrace>();
    Value v = evaluate_trace(pos, *tr);
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    if (tr->special) {
        ss << "Specialised endgame evaluation: " << v << " (side to move)\n";
        return ss.str();
    }
    ss << "      Term      |   White mg   eg |   Black mg   eg |   Total mg   eg\n";
    ss << "----------------+-----------------+-----------------+----------------\n";
    for (const ParamInfo& pi : param_table()) {
        if (pi.kind != PK_LINEAR) continue;
        int wmg = 0, weg = 0, bmg = 0, beg = 0;
        for (int i = 0; i < pi.count; ++i) {
            int slot = pi.offset + i;
            Score val = reinterpret_cast<const Score*>(&P)[slot];
            wmg += mg_value(val) * tr->lin[slot][WHITE];
            weg += eg_value(val) * tr->lin[slot][WHITE];
            bmg += mg_value(val) * tr->lin[slot][BLACK];
            beg += eg_value(val) * tr->lin[slot][BLACK];
        }
        if (!wmg && !weg && !bmg && !beg) continue;
        ss << std::setw(15) << pi.name << " | " << std::setw(6) << wmg << ' ' << std::setw(5) << weg << " | "
           << std::setw(6) << bmg << ' ' << std::setw(5) << beg << " | " << std::setw(6) << wmg - bmg << ' '
           << std::setw(5) << weg - beg << '\n';
    }
    ss << "\nKing danger units: white king " << "(see kd terms), phase " << tr->phase << "/24, scale "
       << tr->scale << "/64\n";
    ss << "Final evaluation: " << tr->eval << " (white side), " << (pos.side_to_move() == WHITE ? tr->eval : -tr->eval)
       << " (side to move, before fifty-move damping)\n";
    return ss.str();
}
