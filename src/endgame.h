// Rule-based endgame knowledge: KPK bitbase, specialised evaluation functions
// for well-known material signatures and scale factors for drawish endings.
#pragma once

#include "position.h"

namespace Bitbases {
void init();
// Strong side is white; the caller normalises colors. stm: side to move (WHITE = strong side).
bool probe_kpk(int wksq, int wpsq, int bksq, int stm);
}  // namespace Bitbases

constexpr int SCALE_NORMAL = 64;
constexpr int SCALE_DRAW = 0;
constexpr int SCALE_ONEPAWN = 48;
constexpr int SCALE_NONE = 255;  // scaling function had nothing to say

enum EndgameEval : uint8_t {
    EG_NONE, EG_DRAW, EG_KXK, EG_KBNK, EG_KPK, EG_KRKP, EG_KRKB, EG_KRKN, EG_KQKP, EG_KQKR
};

enum EndgameScale : uint8_t {
    SC_NONE, SC_KBPSK, SC_KPSK, SC_KRPKR, SC_KBPKB, SC_KBPKN, SC_KPKP
};

struct MaterialEntry {
    Key key;
    int16_t phase;          // 0 (pawn endgame) .. 24 (all pieces)
    uint8_t evalFn;         // EndgameEval
    uint8_t evalStrong;     // strong side for evalFn
    uint8_t scaleFn[COLOR_NB];
    uint8_t factor[COLOR_NB];  // default scale factor when that side is the stronger one
};

void classify_material(const Position& pos, MaterialEntry& e);
// Returns a value from the side to move's point of view.
Value endgame_value(int fn, int strongSide, const Position& pos);
// Returns a scale factor (0..64) or SCALE_NONE.
int endgame_scale(int fn, int strongSide, const Position& pos);
