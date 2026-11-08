// Board state: bitboards, mailbox, incremental keys/material/PSQT and the
// make/unmake machinery used by search.
#pragma once

#include <deque>
#include <memory>
#include <string>

#include "bitboard.h"

namespace Zobrist {
extern Key psq[PIECE_NB][SQUARE_NB];
extern Key enpassant[8];
extern Key castling[16];
extern Key side;
extern Key noPawns;
}  // namespace Zobrist

// Material + piece-square value of a piece on a square (white point of view).
// Filled from the evaluation parameters by Eval::init_psqt().
extern Score PSQ[PIECE_NB][SQUARE_NB];

struct StateInfo {
    // --- copied from the previous state by do_move()
    Key pawnKey;
    Key materialKey;
    Score psq;
    int nonPawnMaterial[COLOR_NB];
    int castlingRights;
    int epSquare;
    int rule50;
    int pliesFromNull;
    // --- recomputed after every move
    Key key;
    Bitboard checkers;
    Bitboard blockersForKing[COLOR_NB];
    Bitboard pinners[COLOR_NB];
    Bitboard checkSquares[PIECE_TYPE_NB];
    int captured;
    int repetition;
    StateInfo* previous;
};

using StateListPtr = std::unique_ptr<std::deque<StateInfo>>;

class Position {
public:
    static void init();

    Position() = default;
    Position(const Position&) = default;
    Position& operator=(const Position&) = default;

    // FEN I/O. set() returns false on malformed input (the position is then undefined).
    bool set(const std::string& fen, StateInfo* si);
    std::string fen() const;
    std::string pretty() const;

    // Board queries
    Bitboard pieces() const { return byTypeBB[ALL_PIECES]; }
    Bitboard pieces(int pt) const { return byTypeBB[pt]; }
    Bitboard pieces_pt(int pt1, int pt2) const { return byTypeBB[pt1] | byTypeBB[pt2]; }
    Bitboard pieces_c(int c) const { return byColorBB[c]; }
    Bitboard pieces(int c, int pt) const { return byColorBB[c] & byTypeBB[pt]; }
    Bitboard pieces(int c, int pt1, int pt2) const { return byColorBB[c] & (byTypeBB[pt1] | byTypeBB[pt2]); }
    int piece_on(int s) const { return board[s]; }
    bool empty(int s) const { return board[s] == NO_PIECE; }
    int count(int c, int pt) const { return pieceCount[make_piece(c, pt)]; }
    int count(int pt) const { return count(WHITE, pt) + count(BLACK, pt); }
    int king_sq(int c) const { return lsb(pieces(c, KING)); }
    int side_to_move() const { return sideToMove; }
    int game_ply() const { return gamePly; }
    int rule50_count() const { return st->rule50; }
    int ep_square() const { return st->epSquare; }
    int castling_rights() const { return st->castlingRights; }
    bool can_castle(int cr) const { return st->castlingRights & cr; }
    bool castling_impeded(int cr) const { return pieces() & castlingPath[cr]; }
    int castling_rook_square(int cr) const { return castlingRookSquare[cr]; }

    // Checking and pins
    Bitboard checkers() const { return st->checkers; }
    bool in_check() const { return st->checkers != 0; }
    Bitboard blockers_for_king(int c) const { return st->blockersForKing[c]; }
    Bitboard pinners(int c) const { return st->pinners[c]; }
    Bitboard check_squares(int pt) const { return st->checkSquares[pt]; }
    Bitboard attackers_to(int s) const { return attackers_to(s, pieces()); }
    Bitboard attackers_to(int s, Bitboard occupied) const;
    Bitboard slider_blockers(Bitboard sliders, int s, Bitboard& pinners) const;
    bool attacked_by(int c, int s) const { return attackers_to(s) & pieces_c(c); }

    // Move properties
    bool legal(Move m) const;
    bool pseudo_legal(Move m) const;
    bool capture(Move m) const {
        return (!empty(to_sq(m)) && type_of_move(m) != CASTLING) || type_of_move(m) == EN_PASSANT;
    }
    bool capture_or_promotion(Move m) const {
        return type_of_move(m) == NORMAL ? !empty(to_sq(m)) : type_of_move(m) != CASTLING;
    }
    bool gives_check(Move m) const;
    int moved_piece(Move m) const { return board[from_sq(m)]; }
    int captured_piece() const { return st->captured; }
    bool advanced_pawn_push(Move m) const {
        return type_of(moved_piece(m)) == PAWN && relative_rank_of(sideToMove, from_sq(m)) > RANK_4;
    }

    // Static exchange evaluation: is the exchange on to_sq(m) worth at least threshold?
    bool see_ge(Move m, Value threshold = 0) const;

    // Doing and undoing moves
    void do_move(Move m, StateInfo& newSt) { do_move(m, newSt, gives_check(m)); }
    void do_move(Move m, StateInfo& newSt, bool givesCheck);
    void undo_move(Move m);
    void do_null_move(StateInfo& newSt);
    void undo_null_move();

    // Hashing and incremental evaluation data
    Key key() const { return st->key; }
    Key pawn_key() const { return st->pawnKey; }
    Key material_key() const { return st->materialKey; }
    Key key_after(Move m) const;
    Score psq_score() const { return st->psq; }
    int non_pawn_material(int c) const { return st->nonPawnMaterial[c]; }
    int non_pawn_material() const { return st->nonPawnMaterial[WHITE] + st->nonPawnMaterial[BLACK]; }

    // Draw detection
    bool is_draw(int ply) const;
    bool has_repeated() const;
    bool is_insufficient_material() const;

    bool opposite_bishops() const {
        return count(WHITE, BISHOP) == 1 && count(BLACK, BISHOP) == 1 &&
               opposite_colors(lsb(pieces(WHITE, BISHOP)), lsb(pieces(BLACK, BISHOP)));
    }
    bool pawn_passed(int c, int s) const { return !(pieces(c ^ 1, PAWN) & PassedPawnSpan[c][s]); }

    // Recomputation from scratch, used by consistency tests.
    Key compute_key() const;
    Key compute_pawn_key() const;
    Key compute_material_key() const;
    Score compute_psq() const;
    bool is_consistent(std::string* why = nullptr) const;

    // Mirror the position (swap colors and flip ranks); used by symmetry tests.
    void flip(StateInfo* si);

    StateInfo* state() const { return st; }
    void set_state(StateInfo* si) { st = si; }

private:
    void put_piece(int pc, int s);
    void remove_piece(int s);
    void move_piece(int from, int to);
    void set_castling_right(int c, int rookSq);
    void set_state_from_scratch(StateInfo* si) const;
    void set_check_info(StateInfo* si) const;
    void do_castling_move(bool doIt, int us, int from, int& to, int& rfrom, int& rto);

    int board[SQUARE_NB];
    Bitboard byTypeBB[PIECE_TYPE_NB];
    Bitboard byColorBB[COLOR_NB];
    int pieceCount[PIECE_NB];
    int castlingRightsMask[SQUARE_NB];
    int castlingRookSquare[16];
    Bitboard castlingPath[16];
    Bitboard castlingKingPath[16];
    StateInfo* st;
    int gamePly;
    int sideToMove;
};

std::string move_to_uci(Move m);
Move uci_to_move(const Position& pos, const std::string& str);
std::string square_to_string(int s);
