// Every evaluation weight lives in one flat struct of packed Scores so that the
// Texel tuner can enumerate, trace and rewrite them generically.
//
// Kinds:
//   PK_LINEAR       - mg/eg Score added (times an integer count) to the evaluation
//   PK_KING_DANGER  - integer weight (stored in the mg half) of the king-danger sum,
//                     which is then transformed quadratically into a penalty
//   PK_COMPLEXITY   - integer weight (mg half) of the endgame "complexity" term
#pragma once

#include <vector>

#include "types.h"

enum ParamKind { PK_LINEAR = 0, PK_KING_DANGER = 1, PK_COMPLEXITY = 2 };

// clang-format off
#define TESSERA_EVAL_PARAMS(X)                         \
    X(Material,            7, PK_LINEAR)               \
    X(PsqtPawn,           64, PK_LINEAR)               \
    X(PsqtKnight,         32, PK_LINEAR)               \
    X(PsqtBishop,         32, PK_LINEAR)               \
    X(PsqtRook,           32, PK_LINEAR)               \
    X(PsqtQueen,          32, PK_LINEAR)               \
    X(PsqtKing,           32, PK_LINEAR)               \
    X(BishopPair,          1, PK_LINEAR)               \
    X(ImbKnightPawns,      1, PK_LINEAR)               \
    X(ImbRookPawns,        1, PK_LINEAR)               \
    X(ImbRookPair,         1, PK_LINEAR)               \
    X(ImbQueenMinors,      1, PK_LINEAR)               \
    X(PawnIsolated,        4, PK_LINEAR)               \
    X(PawnWeakUnopposed,   1, PK_LINEAR)               \
    X(PawnDoubled,         1, PK_LINEAR)               \
    X(PawnDoubledIsolated, 1, PK_LINEAR)               \
    X(PawnBackward,        1, PK_LINEAR)               \
    X(PawnWeakLever,       1, PK_LINEAR)               \
    X(PawnBlocked,         8, PK_LINEAR)               \
    X(ConnectedBase,       8, PK_LINEAR)               \
    X(ConnectedPhalanx,    8, PK_LINEAR)               \
    X(ConnectedOpposed,    8, PK_LINEAR)               \
    X(PawnSupport,         1, PK_LINEAR)               \
    X(CandidatePassed,     8, PK_LINEAR)               \
    X(PawnIslands,         1, PK_LINEAR)               \
    X(PawnMajority,        1, PK_LINEAR)               \
    X(MinorityAttack,      1, PK_LINEAR)               \
    X(WeakSquare,          1, PK_LINEAR)               \
    X(Shelter,            32, PK_LINEAR)               \
    X(UnblockedStorm,     32, PK_LINEAR)               \
    X(BlockedStorm,        8, PK_LINEAR)               \
    X(KingOnFile,          4, PK_LINEAR)               \
    X(KingPawnDistance,    1, PK_LINEAR)               \
    X(PawnlessFlank,       1, PK_LINEAR)               \
    X(FlankAttacks,        1, PK_LINEAR)               \
    X(MobilityKnight,      9, PK_LINEAR)               \
    X(MobilityBishop,     14, PK_LINEAR)               \
    X(MobilityRook,       15, PK_LINEAR)               \
    X(MobilityQueen,      28, PK_LINEAR)               \
    X(KnightOutpost,       1, PK_LINEAR)               \
    X(BishopOutpost,       1, PK_LINEAR)               \
    X(ReachableOutpost,    1, PK_LINEAR)               \
    X(MinorBehindPawn,     1, PK_LINEAR)               \
    X(KingProtector,       2, PK_LINEAR)               \
    X(BishopPawns,         1, PK_LINEAR)               \
    X(BishopXrayPawns,     1, PK_LINEAR)               \
    X(LongDiagonalBishop,  1, PK_LINEAR)               \
    X(BishopOnKingRing,    1, PK_LINEAR)               \
    X(TrappedBishop,       1, PK_LINEAR)               \
    X(RookOnKingRing,      1, PK_LINEAR)               \
    X(RookOnFile,          2, PK_LINEAR)               \
    X(RookOnClosedFile,    1, PK_LINEAR)               \
    X(RookOnQueenFile,     1, PK_LINEAR)               \
    X(RookOn7th,           1, PK_LINEAR)               \
    X(RooksConnected,      1, PK_LINEAR)               \
    X(TrappedRook,         1, PK_LINEAR)               \
    X(WeakQueen,           1, PK_LINEAR)               \
    X(QueenInfiltration,   1, PK_LINEAR)               \
    X(ThreatByMinor,       7, PK_LINEAR)               \
    X(ThreatByRook,        7, PK_LINEAR)               \
    X(ThreatByKing,        1, PK_LINEAR)               \
    X(Hanging,             1, PK_LINEAR)               \
    X(WeakQueenProtection, 1, PK_LINEAR)               \
    X(RestrictedPiece,     1, PK_LINEAR)               \
    X(ThreatBySafePawn,    1, PK_LINEAR)               \
    X(ThreatByPawnPush,    1, PK_LINEAR)               \
    X(KnightOnQueen,       1, PK_LINEAR)               \
    X(SliderOnQueen,       1, PK_LINEAR)               \
    X(PassedRank,          8, PK_LINEAR)               \
    X(PassedEnemyKing,     8, PK_LINEAR)               \
    X(PassedOwnKing,       8, PK_LINEAR)               \
    X(PassedFree,          8, PK_LINEAR)               \
    X(PassedSafePath,      8, PK_LINEAR)               \
    X(PassedSafeBlock,     8, PK_LINEAR)               \
    X(PassedDefended,      8, PK_LINEAR)               \
    X(PassedBlocked,       8, PK_LINEAR)               \
    X(PassedFile,          1, PK_LINEAR)               \
    X(PassedConnected,     1, PK_LINEAR)               \
    X(PassedProtected,     1, PK_LINEAR)               \
    X(RookBehindPassed,    1, PK_LINEAR)               \
    X(PassedUnstoppable,   1, PK_LINEAR)               \
    X(Space,               1, PK_LINEAR)               \
    X(Tempo,               1, PK_LINEAR)               \
    X(KDAttackerWeight,    7, PK_KING_DANGER)          \
    X(KDSafeCheck,         7, PK_KING_DANGER)          \
    X(KDSafeCheckMulti,    7, PK_KING_DANGER)          \
    X(KDWeakRing,          1, PK_KING_DANGER)          \
    X(KDUnsafeChecks,      1, PK_KING_DANGER)          \
    X(KDBlockers,          1, PK_KING_DANGER)          \
    X(KDRingAttacks,       1, PK_KING_DANGER)          \
    X(KDFlankAttackSq,     1, PK_KING_DANGER)          \
    X(KDNoQueen,           1, PK_KING_DANGER)          \
    X(KDKnightDefender,    1, PK_KING_DANGER)          \
    X(KDFlankDefense,      1, PK_KING_DANGER)          \
    X(KDBase,              1, PK_KING_DANGER)          \
    X(CxPassed,            1, PK_COMPLEXITY)           \
    X(CxPawns,             1, PK_COMPLEXITY)           \
    X(CxOutflanking,       1, PK_COMPLEXITY)           \
    X(CxBothFlanks,        1, PK_COMPLEXITY)           \
    X(CxInfiltration,      1, PK_COMPLEXITY)           \
    X(CxPawnEnding,        1, PK_COMPLEXITY)           \
    X(CxUnwinnable,        1, PK_COMPLEXITY)           \
    X(CxBase,              1, PK_COMPLEXITY)
// clang-format on

struct EvalParams {
#define TESSERA_DECLARE_PARAM(name, n, kind) Score name[n];
    TESSERA_EVAL_PARAMS(TESSERA_DECLARE_PARAM)
#undef TESSERA_DECLARE_PARAM
};

constexpr int NUM_PARAM_SLOTS = int(sizeof(EvalParams) / sizeof(Score));

struct ParamInfo {
    const char* name;
    int offset;  // in Score slots from the start of EvalParams
    int count;
    ParamKind kind;
};

extern EvalParams P;  // the parameters used by the evaluation

void set_default_params(EvalParams& p);       // hand-written starting values
void set_params(EvalParams& p, bool tuned);  // defaults plus the tuned overrides from params_tuned.cpp
const std::vector<ParamInfo>& param_table();
ParamKind param_kind_of_slot(int slot);

inline int param_slot(const Score& p) {
    return int((reinterpret_cast<uintptr_t>(&p) - reinterpret_cast<uintptr_t>(&P)) / sizeof(Score));
}

// Divisors transforming king danger units into a (mg, eg) penalty.
constexpr int KD_MG_DIV = 6000;
constexpr int KD_EG_DIV = 30;
