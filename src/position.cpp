#include "position.h"

#include <cstddef>
#include <cstring>
#include <sstream>
#include <vector>

#include "movegen.h"

namespace Zobrist {
Key psq[PIECE_NB][SQUARE_NB];
Key enpassant[8];
Key castling[16];
Key side;
Key noPawns;
}  // namespace Zobrist

namespace {
const std::string PieceToChar(" PNBRQK  pnbrqk");

struct ZobristPRNG {
    uint64_t s = 1070372ULL;
    uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ULL;
    }
};
}  // namespace

std::string square_to_string(int s) {
    if (s == SQ_NONE) return "-";
    return std::string{char('a' + file_of(s)), char('1' + rank_of(s))};
}

std::string move_to_uci(Move m) {
    if (m == MOVE_NONE) return "0000";
    if (m == MOVE_NULL) return "0000";
    std::string s = square_to_string(from_sq(m)) + square_to_string(to_sq(m));
    if (type_of_move(m) == PROMOTION) s += " pnbrqk"[promotion_type(m)];
    return s;
}

Move uci_to_move(const Position& pos, const std::string& str) {
    std::string s = str;
    if (s.size() == 5) s[4] = char(std::tolower(static_cast<unsigned char>(s[4])));
    for (const ExtMove& m : MoveList<LEGAL>(pos))
        if (move_to_uci(m.move) == s) return m.move;
    return MOVE_NONE;
}

void Position::init() {
    ZobristPRNG rng;
    for (int pc = 0; pc < PIECE_NB; ++pc)
        for (int s = 0; s < 64; ++s) Zobrist::psq[pc][s] = rng.next();
    for (int f = 0; f < 8; ++f) Zobrist::enpassant[f] = rng.next();
    for (int cr = 0; cr < 16; ++cr) Zobrist::castling[cr] = rng.next();
    Zobrist::side = rng.next();
    Zobrist::noPawns = rng.next();
}

// ---------------------------------------------------------------------------
// Low level board manipulation

inline void Position::put_piece(int pc, int s) {
    board[s] = pc;
    byTypeBB[ALL_PIECES] |= byTypeBB[type_of(pc)] |= sq_bb(s);
    byColorBB[color_of(pc)] |= sq_bb(s);
    pieceCount[pc]++;
    pieceCount[make_piece(color_of(pc), ALL_PIECES)]++;
}

inline void Position::remove_piece(int s) {
    int pc = board[s];
    byTypeBB[ALL_PIECES] ^= sq_bb(s);
    byTypeBB[type_of(pc)] ^= sq_bb(s);
    byColorBB[color_of(pc)] ^= sq_bb(s);
    board[s] = NO_PIECE;
    pieceCount[pc]--;
    pieceCount[make_piece(color_of(pc), ALL_PIECES)]--;
}

inline void Position::move_piece(int from, int to) {
    int pc = board[from];
    Bitboard fromTo = sq_bb(from) | sq_bb(to);
    byTypeBB[ALL_PIECES] ^= fromTo;
    byTypeBB[type_of(pc)] ^= fromTo;
    byColorBB[color_of(pc)] ^= fromTo;
    board[from] = NO_PIECE;
    board[to] = pc;
}

void Position::set_castling_right(int c, int rookSq) {
    int ksq = king_sq(c);
    bool kingSide = rookSq > ksq;
    int cr = (kingSide ? WHITE_OO : WHITE_OOO) << (2 * c);
    st->castlingRights |= cr;
    castlingRightsMask[ksq] |= cr;
    castlingRightsMask[rookSq] |= cr;
    castlingRookSquare[cr] = rookSq;
    int kto = relative_square(c, kingSide ? SQ_G1 : SQ_C1);
    int rto = relative_square(c, kingSide ? SQ_F1 : SQ_D1);
    castlingPath[cr] = (BetweenBB[rookSq][rto] | BetweenBB[ksq][kto] | sq_bb(rto) | sq_bb(kto)) &
                       ~(sq_bb(ksq) | sq_bb(rookSq));
    castlingKingPath[cr] = BetweenBB[ksq][kto] | sq_bb(kto);
}

// ---------------------------------------------------------------------------
// FEN

bool Position::set(const std::string& fenStr, StateInfo* si) {
    std::memset(static_cast<void*>(this), 0, sizeof(Position));
    std::memset(static_cast<void*>(si), 0, sizeof(StateInfo));
    st = si;
    for (int i = 0; i < 16; ++i) castlingRookSquare[i] = SQ_NONE;

    std::istringstream ss(fenStr);
    std::string boardStr, stm = "w", castle = "-", ep = "-";
    int r50 = 0, fullMove = 1;
    ss >> boardStr;
    if (!(ss >> stm)) stm = "w";
    if (!(ss >> castle)) castle = "-";
    if (!(ss >> ep)) ep = "-";
    if (!(ss >> r50)) r50 = 0;
    if (!(ss >> fullMove)) fullMove = 1;

    int r = RANK_8, f = FILE_A;
    for (char c : boardStr) {
        if (c == '/') {
            if (f != 8) return false;
            --r;
            f = FILE_A;
            if (r < RANK_1) return false;
        } else if (c >= '1' && c <= '8') {
            f += c - '0';
            if (f > 8) return false;
        } else {
            size_t idx = PieceToChar.find(c);
            if (idx == std::string::npos || c == ' ' || f > 7) return false;
            put_piece(int(idx), make_square(f, r));
            ++f;
        }
    }
    if (r != RANK_1 || f != 8) return false;
    if (count(WHITE, KING) != 1 || count(BLACK, KING) != 1) return false;
    if (pieces(PAWN) & (Rank1BB | Rank8BB)) return false;
    if (stm != "w" && stm != "b") return false;
    sideToMove = stm == "w" ? WHITE : BLACK;

    for (char c : castle) {
        if (c == '-') break;
        int color = std::islower(static_cast<unsigned char>(c)) ? BLACK : WHITE;
        char uc = char(std::toupper(static_cast<unsigned char>(c)));
        int ksq = king_sq(color);
        if (ksq != relative_square(color, SQ_E1)) continue;
        int rsq;
        if (uc == 'K') rsq = relative_square(color, SQ_H1);
        else if (uc == 'Q') rsq = relative_square(color, SQ_A1);
        else continue;
        if (piece_on(rsq) != make_piece(color, ROOK)) continue;
        set_castling_right(color, rsq);
    }

    st->epSquare = SQ_NONE;
    if (ep.size() == 2 && ep[0] >= 'a' && ep[0] <= 'h' && (ep[1] == '3' || ep[1] == '6')) {
        int epSq = make_square(ep[0] - 'a', ep[1] - '1');
        int us = sideToMove, them = us ^ 1;
        // Only keep the square if an en passant capture is actually possible.
        if (relative_rank_of(us, epSq) == RANK_6 && empty(epSq) && empty(epSq + pawn_push(us)) &&
            piece_on(epSq - pawn_push(us)) == make_piece(them, PAWN) &&
            (PawnAttacks[them][epSq] & pieces(us, PAWN)))
            st->epSquare = epSq;
    }

    st->rule50 = std::max(0, r50);
    gamePly = std::max(2 * (fullMove - 1), 0) + (sideToMove == BLACK);

    set_state_from_scratch(st);
    // The side that just moved must not be in check.
    if (attackers_to(king_sq(sideToMove ^ 1)) & pieces_c(sideToMove)) return false;
    return true;
}

std::string Position::fen() const {
    std::ostringstream ss;
    for (int r = RANK_8; r >= RANK_1; --r) {
        for (int f = FILE_A; f <= FILE_H; ++f) {
            int emptyCnt = 0;
            for (; f <= FILE_H && empty(make_square(f, r)); ++f) ++emptyCnt;
            if (emptyCnt) ss << emptyCnt;
            if (f <= FILE_H) ss << PieceToChar[piece_on(make_square(f, r))];
        }
        if (r > RANK_1) ss << '/';
    }
    ss << (sideToMove == WHITE ? " w " : " b ");
    if (can_castle(WHITE_OO)) ss << 'K';
    if (can_castle(WHITE_OOO)) ss << 'Q';
    if (can_castle(BLACK_OO)) ss << 'k';
    if (can_castle(BLACK_OOO)) ss << 'q';
    if (!can_castle(ANY_CASTLING)) ss << '-';
    ss << ' ' << square_to_string(st->epSquare) << ' ' << st->rule50 << ' '
       << 1 + (gamePly - (sideToMove == BLACK)) / 2;
    return ss.str();
}

std::string Position::pretty() const {
    std::ostringstream ss;
    ss << "\n +---+---+---+---+---+---+---+---+\n";
    for (int r = RANK_8; r >= RANK_1; --r) {
        for (int f = FILE_A; f <= FILE_H; ++f) ss << " | " << PieceToChar[piece_on(make_square(f, r))];
        ss << " | " << (r + 1) << "\n +---+---+---+---+---+---+---+---+\n";
    }
    ss << "   a   b   c   d   e   f   g   h\n\nFen: " << fen() << "\nKey: " << std::hex << std::uppercase
       << key() << std::dec << "\nCheckers:";
    for (Bitboard b = checkers(); b;) ss << ' ' << square_to_string(pop_lsb(b));
    ss << '\n';
    return ss.str();
}

// ---------------------------------------------------------------------------
// State computation

Key Position::compute_key() const {
    Key k = 0;
    for (Bitboard b = pieces(); b;) {
        int s = pop_lsb(b);
        k ^= Zobrist::psq[piece_on(s)][s];
    }
    if (st->epSquare != SQ_NONE) k ^= Zobrist::enpassant[file_of(st->epSquare)];
    if (sideToMove == BLACK) k ^= Zobrist::side;
    k ^= Zobrist::castling[st->castlingRights];
    return k;
}

Key Position::compute_pawn_key() const {
    Key k = Zobrist::noPawns;
    for (Bitboard b = pieces(PAWN); b;) {
        int s = pop_lsb(b);
        k ^= Zobrist::psq[piece_on(s)][s];
    }
    return k;
}

Key Position::compute_material_key() const {
    Key k = 0;
    for (int c = WHITE; c <= BLACK; ++c)
        for (int pt = PAWN; pt <= KING; ++pt)
            for (int n = 0; n < count(c, pt); ++n) k ^= Zobrist::psq[make_piece(c, pt)][n];
    return k;
}

Score Position::compute_psq() const {
    Score s = SCORE_ZERO;
    for (Bitboard b = pieces(); b;) {
        int sq = pop_lsb(b);
        s += PSQ[piece_on(sq)][sq];
    }
    return s;
}

void Position::set_state_from_scratch(StateInfo* si) const {
    si->key = compute_key();
    si->pawnKey = compute_pawn_key();
    si->materialKey = compute_material_key();
    si->psq = compute_psq();
    si->nonPawnMaterial[WHITE] = si->nonPawnMaterial[BLACK] = 0;
    for (int c = WHITE; c <= BLACK; ++c)
        for (int pt = KNIGHT; pt <= QUEEN; ++pt) si->nonPawnMaterial[c] += count(c, pt) * PieceValue[pt];
    si->checkers = attackers_to(king_sq(sideToMove)) & pieces_c(sideToMove ^ 1);
    si->captured = NO_PIECE;
    si->repetition = 0;
    si->pliesFromNull = 0;
    si->previous = nullptr;
    set_check_info(si);
}

void Position::set_check_info(StateInfo* si) const {
    si->blockersForKing[WHITE] = slider_blockers(pieces_c(BLACK), king_sq(WHITE), si->pinners[BLACK]);
    si->blockersForKing[BLACK] = slider_blockers(pieces_c(WHITE), king_sq(BLACK), si->pinners[WHITE]);
    int them = sideToMove ^ 1;
    int ksq = king_sq(them);
    si->checkSquares[PAWN] = PawnAttacks[them][ksq];
    si->checkSquares[KNIGHT] = PseudoAttacks[KNIGHT][ksq];
    si->checkSquares[BISHOP] = bishop_attacks(ksq, pieces());
    si->checkSquares[ROOK] = rook_attacks(ksq, pieces());
    si->checkSquares[QUEEN] = si->checkSquares[BISHOP] | si->checkSquares[ROOK];
    si->checkSquares[KING] = 0;
}

Bitboard Position::attackers_to(int s, Bitboard occ) const {
    return (PawnAttacks[BLACK][s] & pieces(WHITE, PAWN)) | (PawnAttacks[WHITE][s] & pieces(BLACK, PAWN)) |
           (PseudoAttacks[KNIGHT][s] & pieces(KNIGHT)) | (rook_attacks(s, occ) & pieces_pt(ROOK, QUEEN)) |
           (bishop_attacks(s, occ) & pieces_pt(BISHOP, QUEEN)) | (PseudoAttacks[KING][s] & pieces(KING));
}

// Pieces (of both colors) that block a slider attack from 'sliders' towards square s.
Bitboard Position::slider_blockers(Bitboard sliders, int s, Bitboard& pinnersOut) const {
    Bitboard blockers = 0;
    pinnersOut = 0;
    Bitboard snipers = ((PseudoAttacks[ROOK][s] & pieces_pt(QUEEN, ROOK)) | (PseudoAttacks[BISHOP][s] & pieces_pt(QUEEN, BISHOP))) & sliders;
    Bitboard occupancy = pieces() ^ snipers;
    while (snipers) {
        int sniperSq = pop_lsb(snipers);
        Bitboard b = BetweenBB[s][sniperSq] & occupancy;
        if (b && !more_than_one(b)) {
            blockers |= b;
            if (b & pieces_c(color_of(piece_on(s)))) pinnersOut |= sq_bb(sniperSq);
        }
    }
    return blockers;
}

// ---------------------------------------------------------------------------
// Move legality

bool Position::legal(Move m) const {
    const int us = sideToMove, them = us ^ 1;
    const int from = from_sq(m), to = to_sq(m);
    const int ksq = king_sq(us);

    if (type_of_move(m) == EN_PASSANT) {
        int capsq = to - pawn_push(us);
        Bitboard occ = (pieces() ^ sq_bb(from) ^ sq_bb(capsq)) | sq_bb(to);
        return !(attackers_to(ksq, occ) & pieces_c(them) & ~sq_bb(capsq));
    }
    if (type_of_move(m) == CASTLING) {
        if (checkers()) return false;
        int cr = (to > from ? WHITE_OO : WHITE_OOO) << (2 * us);
        for (Bitboard b = castlingKingPath[cr]; b;)
            if (attackers_to(pop_lsb(b)) & pieces_c(them)) return false;
        return true;
    }
    if (type_of(piece_on(from)) == KING) return !(attackers_to(to, pieces() ^ sq_bb(from)) & pieces_c(them));

    return !(blockers_for_king(us) & sq_bb(from)) || aligned(from, to, ksq);
}

// Validates moves coming from the transposition table or killer slots, which may
// be garbage for the current position. Assumes legal() will be checked afterwards.
bool Position::pseudo_legal(Move m) const {
    const int us = sideToMove, them = us ^ 1;
    const int from = from_sq(m), to = to_sq(m);
    const int pc = moved_piece(m);

    if (!is_ok_move(m)) return false;

    if (type_of_move(m) != NORMAL) {
        if (checkers()) {
            for (const ExtMove& em : MoveList<EVASIONS>(*this))
                if (em.move == m) return true;
        } else {
            for (const ExtMove& em : MoveList<NON_EVASIONS>(*this))
                if (em.move == m) return true;
        }
        return false;
    }

    if (m & 0x3000) return false;  // promotion bits set on a non-promotion move
    if (pc == NO_PIECE || color_of(pc) != us) return false;
    if (pieces_c(us) & sq_bb(to)) return false;

    if (type_of(pc) == PAWN) {
        if ((Rank8BB | Rank1BB) & sq_bb(to)) return false;  // must be encoded as promotion
        const int up = pawn_push(us);
        bool isCapture = PawnAttacks[us][from] & pieces_c(them) & sq_bb(to);
        bool isPush = from + up == to && empty(to);
        bool isDouble = from + 2 * up == to && relative_rank_of(us, from) == RANK_2 && empty(to) && empty(to - up);
        if (!isCapture && !isPush && !isDouble) return false;
    } else if (!(attacks_bb(type_of(pc), from, pieces()) & sq_bb(to)))
        return false;

    // In check, non-king moves must block the check or capture the checker.
    // King safety itself is verified by legal().
    if (checkers() && type_of(pc) != KING) {
        if (more_than_one(checkers())) return false;
        int checkSq = lsb(checkers());
        if (!((BetweenBB[king_sq(us)][checkSq] | checkers()) & sq_bb(to))) return false;
    }
    return true;
}

bool Position::gives_check(Move m) const {
    const int us = sideToMove, them = us ^ 1;
    const int from = from_sq(m), to = to_sq(m);
    const int ksq = king_sq(them);

    // Direct check
    if (check_squares(type_of(piece_on(from))) & sq_bb(to)) return true;
    // Discovered check
    if ((blockers_for_king(them) & sq_bb(from)) && !aligned(from, to, ksq)) return true;

    switch (type_of_move(m)) {
    case NORMAL: return false;
    case PROMOTION: return attacks_bb(promotion_type(m), to, pieces() ^ sq_bb(from)) & sq_bb(ksq);
    case EN_PASSANT: {
        int capsq = make_square(file_of(to), rank_of(from));
        Bitboard b = (pieces() ^ sq_bb(from) ^ sq_bb(capsq)) | sq_bb(to);
        return (rook_attacks(ksq, b) & pieces(us, QUEEN, ROOK)) | (bishop_attacks(ksq, b) & pieces(us, QUEEN, BISHOP));
    }
    default: {  // CASTLING
        bool kingSide = to > from;
        int rfrom = relative_square(us, kingSide ? SQ_H1 : SQ_A1);
        int rto = relative_square(us, kingSide ? SQ_F1 : SQ_D1);
        Bitboard occ = (pieces() ^ sq_bb(from) ^ sq_bb(rfrom)) | sq_bb(rto) | sq_bb(to);
        return rook_attacks(rto, occ) & sq_bb(ksq);
    }
    }
}

// ---------------------------------------------------------------------------
// Make / unmake

void Position::do_castling_move(bool doIt, int us, int from, int& to, int& rfrom, int& rto) {
    bool kingSide = to > from;
    rfrom = relative_square(us, kingSide ? SQ_H1 : SQ_A1);
    rto = relative_square(us, kingSide ? SQ_F1 : SQ_D1);
    if (doIt) {
        remove_piece(from);
        remove_piece(rfrom);
        put_piece(make_piece(us, KING), to);
        put_piece(make_piece(us, ROOK), rto);
    } else {
        remove_piece(to);
        remove_piece(rto);
        put_piece(make_piece(us, KING), from);
        put_piece(make_piece(us, ROOK), rfrom);
    }
}

void Position::do_move(Move m, StateInfo& newSt, bool givesCheck) {
    assert(is_ok_move(m));
    Key k = st->key ^ Zobrist::side;

    std::memcpy(static_cast<void*>(&newSt), st, offsetof(StateInfo, key));
    newSt.previous = st;
    st = &newSt;

    ++gamePly;
    ++st->rule50;
    ++st->pliesFromNull;

    const int us = sideToMove, them = us ^ 1;
    const int from = from_sq(m);
    int to = to_sq(m);
    const int pc = piece_on(from);
    int captured = type_of_move(m) == EN_PASSANT ? make_piece(them, PAWN) : piece_on(to);

    if (type_of_move(m) == CASTLING) {
        int rfrom, rto;
        do_castling_move(true, us, from, to, rfrom, rto);
        const int rook = make_piece(us, ROOK);
        k ^= Zobrist::psq[rook][rfrom] ^ Zobrist::psq[rook][rto];
        st->psq += PSQ[rook][rto] - PSQ[rook][rfrom];
        captured = NO_PIECE;
    }

    if (captured) {
        int capsq = to;
        if (type_of(captured) == PAWN) {
            if (type_of_move(m) == EN_PASSANT) capsq -= pawn_push(us);
            st->pawnKey ^= Zobrist::psq[captured][capsq];
        } else
            st->nonPawnMaterial[them] -= PieceValue[captured];
        remove_piece(capsq);
        k ^= Zobrist::psq[captured][capsq];
        st->materialKey ^= Zobrist::psq[captured][pieceCount[captured]];
        st->psq -= PSQ[captured][capsq];
        st->rule50 = 0;
    }

    k ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];

    if (st->epSquare != SQ_NONE) {
        k ^= Zobrist::enpassant[file_of(st->epSquare)];
        st->epSquare = SQ_NONE;
    }

    if (st->castlingRights && (castlingRightsMask[from] | castlingRightsMask[to])) {
        k ^= Zobrist::castling[st->castlingRights];
        st->castlingRights &= ~(castlingRightsMask[from] | castlingRightsMask[to]);
        k ^= Zobrist::castling[st->castlingRights];
    }

    if (type_of_move(m) != CASTLING) move_piece(from, to);
    st->psq += PSQ[pc][to] - PSQ[pc][from];

    if (type_of(pc) == PAWN) {
        if ((to ^ from) == 16 && (PawnAttacks[us][to - pawn_push(us)] & pieces(them, PAWN))) {
            st->epSquare = to - pawn_push(us);
            k ^= Zobrist::enpassant[file_of(st->epSquare)];
        } else if (type_of_move(m) == PROMOTION) {
            const int promotion = make_piece(us, promotion_type(m));
            remove_piece(to);
            put_piece(promotion, to);
            k ^= Zobrist::psq[pc][to] ^ Zobrist::psq[promotion][to];
            st->pawnKey ^= Zobrist::psq[pc][to];
            st->materialKey ^= Zobrist::psq[promotion][pieceCount[promotion] - 1] ^ Zobrist::psq[pc][pieceCount[pc]];
            st->psq += PSQ[promotion][to] - PSQ[pc][to];
            st->nonPawnMaterial[us] += PieceValue[promotion];
        }
        st->pawnKey ^= Zobrist::psq[pc][from] ^ Zobrist::psq[pc][to];
        st->rule50 = 0;
    }

    st->captured = captured;
    st->key = k;
    st->checkers = givesCheck ? attackers_to(king_sq(them)) & pieces_c(us) : 0;
    sideToMove = them;
    set_check_info(st);

    // Repetition info: distance to the previous occurrence of this position,
    // negative if that occurrence was itself a repetition (i.e. threefold).
    st->repetition = 0;
    int end = std::min(st->rule50, st->pliesFromNull);
    if (end >= 4) {
        StateInfo* stp = st->previous->previous;
        for (int i = 4; i <= end; i += 2) {
            stp = stp->previous->previous;
            if (stp->key == st->key) {
                st->repetition = stp->repetition ? -i : i;
                break;
            }
        }
    }
}

void Position::undo_move(Move m) {
    sideToMove ^= 1;
    const int us = sideToMove;
    const int from = from_sq(m);
    int to = to_sq(m);

    if (type_of_move(m) == PROMOTION) {
        remove_piece(to);
        put_piece(make_piece(us, PAWN), to);
    }

    if (type_of_move(m) == CASTLING) {
        int rfrom, rto;
        do_castling_move(false, us, from, to, rfrom, rto);
    } else {
        move_piece(to, from);
        if (st->captured) {
            int capsq = to;
            if (type_of_move(m) == EN_PASSANT) capsq -= pawn_push(us);
            put_piece(st->captured, capsq);
        }
    }
    st = st->previous;
    --gamePly;
}

void Position::do_null_move(StateInfo& newSt) {
    assert(!checkers());
    std::memcpy(static_cast<void*>(&newSt), st, sizeof(StateInfo));
    newSt.previous = st;
    st = &newSt;
    if (st->epSquare != SQ_NONE) {
        st->key ^= Zobrist::enpassant[file_of(st->epSquare)];
        st->epSquare = SQ_NONE;
    }
    st->key ^= Zobrist::side;
    ++st->rule50;
    st->pliesFromNull = 0;
    st->captured = NO_PIECE;
    sideToMove ^= 1;
    set_check_info(st);
    st->repetition = 0;
}

void Position::undo_null_move() {
    st = st->previous;
    sideToMove ^= 1;
}

Key Position::key_after(Move m) const {
    const int from = from_sq(m), to = to_sq(m);
    const int pc = piece_on(from), captured = piece_on(to);
    Key k = st->key ^ Zobrist::side;
    if (captured) k ^= Zobrist::psq[captured][to];
    return k ^ Zobrist::psq[pc][to] ^ Zobrist::psq[pc][from];
}

// ---------------------------------------------------------------------------
// Static exchange evaluation (swap algorithm with x-rays and pins)

bool Position::see_ge(Move m, Value threshold) const {
    if (type_of_move(m) != NORMAL) return VALUE_ZERO >= threshold;

    const int from = from_sq(m), to = to_sq(m);
    int swap = PieceValue[piece_on(to)] - threshold;
    if (swap < 0) return false;
    swap = PieceValue[piece_on(from)] - swap;
    if (swap <= 0) return true;

    Bitboard occupied = pieces() ^ sq_bb(from) ^ sq_bb(to);
    int stm = sideToMove;
    Bitboard attackers = attackers_to(to, occupied);
    Bitboard stmAttackers, bb;
    int res = 1;

    while (true) {
        stm ^= 1;
        attackers &= occupied;
        if (!(stmAttackers = attackers & pieces_c(stm))) break;
        // Pinned pieces may not take part while their pinner is still on the board
        if (pinners(stm ^ 1) & occupied) {
            stmAttackers &= ~blockers_for_king(stm);
            if (!stmAttackers) break;
        }
        res ^= 1;

        if ((bb = stmAttackers & pieces(PAWN))) {
            if ((swap = PawnValue - swap) < res) break;
            occupied ^= sq_bb(lsb(bb));
            attackers |= bishop_attacks(to, occupied) & pieces_pt(BISHOP, QUEEN);
        } else if ((bb = stmAttackers & pieces(KNIGHT))) {
            if ((swap = KnightValue - swap) < res) break;
            occupied ^= sq_bb(lsb(bb));
        } else if ((bb = stmAttackers & pieces(BISHOP))) {
            if ((swap = BishopValue - swap) < res) break;
            occupied ^= sq_bb(lsb(bb));
            attackers |= bishop_attacks(to, occupied) & pieces_pt(BISHOP, QUEEN);
        } else if ((bb = stmAttackers & pieces(ROOK))) {
            if ((swap = RookValue - swap) < res) break;
            occupied ^= sq_bb(lsb(bb));
            attackers |= rook_attacks(to, occupied) & pieces_pt(ROOK, QUEEN);
        } else if ((bb = stmAttackers & pieces(QUEEN))) {
            if ((swap = QueenValue - swap) < res) break;
            occupied ^= sq_bb(lsb(bb));
            attackers |= (bishop_attacks(to, occupied) & pieces_pt(BISHOP, QUEEN)) | (rook_attacks(to, occupied) & pieces_pt(ROOK, QUEEN));
        } else  // King: can only capture if the opponent has no attackers left
            return (attackers & ~pieces_c(stm)) ? res ^ 1 : res;
    }
    return bool(res);
}

// ---------------------------------------------------------------------------
// Draws

bool Position::is_draw(int ply) const {
    if (st->rule50 > 99 && (!checkers() || MoveList<LEGAL>(*this).size())) return true;
    // A repetition inside the search tree counts as a draw; before the root we require threefold.
    return st->repetition && st->repetition < ply;
}

bool Position::has_repeated() const { return st->repetition != 0; }

bool Position::is_insufficient_material() const {
    if (pieces_pt(PAWN, ROOK) | pieces(QUEEN)) return false;
    Bitboard minors = pieces_pt(KNIGHT, BISHOP);
    if (!more_than_one(minors)) return true;
    // Only bishops, all on the same square color
    if (!pieces(KNIGHT) && (!(pieces(BISHOP) & DarkSquares) || !(pieces(BISHOP) & LightSquares))) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Debug helpers

void Position::flip(StateInfo* si) {
    std::string f = fen();
    std::istringstream ss(f);
    std::string boardStr, stm, castle, ep, r50, full;
    ss >> boardStr >> stm >> castle >> ep >> r50 >> full;
    // Reverse ranks and swap case
    std::string out, rank;
    std::vector<std::string> ranks;
    std::istringstream bs(boardStr);
    while (std::getline(bs, rank, '/')) ranks.push_back(rank);
    for (int i = int(ranks.size()) - 1; i >= 0; --i) {
        for (char& c : ranks[i])
            c = std::islower(static_cast<unsigned char>(c)) ? char(std::toupper(static_cast<unsigned char>(c)))
                                                            : char(std::tolower(static_cast<unsigned char>(c)));
        out += ranks[i];
        if (i) out += '/';
    }
    out += stm == "w" ? " b " : " w ";
    std::string cr;
    for (char c : castle) {
        if (c == '-') continue;
        cr += std::islower(static_cast<unsigned char>(c)) ? char(std::toupper(static_cast<unsigned char>(c)))
                                                          : char(std::tolower(static_cast<unsigned char>(c)));
    }
    // canonical order KQkq
    std::string ordered;
    for (char c : std::string("KQkq"))
        if (cr.find(c) != std::string::npos) ordered += c;
    out += ordered.empty() ? "-" : ordered;
    out += ' ';
    if (ep == "-") out += '-';
    else {
        out += ep[0];
        out += ep[1] == '3' ? '6' : '3';
    }
    out += ' ' + r50 + ' ' + full;
    set(out, si);
}

bool Position::is_consistent(std::string* why) const {
    auto fail = [&](const char* msg) {
        if (why) *why = msg;
        return false;
    };
    if (count(WHITE, KING) != 1 || count(BLACK, KING) != 1) return fail("king count");
    Bitboard all = 0;
    for (int pt = PAWN; pt <= KING; ++pt) {
        if (all & pieces(pt)) return fail("piece type overlap");
        all |= pieces(pt);
    }
    if (all != pieces()) return fail("all-pieces bitboard");
    if (pieces_c(WHITE) & pieces_c(BLACK)) return fail("color overlap");
    if ((pieces_c(WHITE) | pieces_c(BLACK)) != pieces()) return fail("color union");
    for (int s = 0; s < 64; ++s) {
        int pc = piece_on(s);
        if (pc == NO_PIECE) {
            if (pieces() & sq_bb(s)) return fail("board/bitboard mismatch (empty)");
        } else if (!(pieces(color_of(pc), type_of(pc)) & sq_bb(s)))
            return fail("board/bitboard mismatch");
    }
    for (int c = WHITE; c <= BLACK; ++c)
        for (int pt = PAWN; pt <= KING; ++pt)
            if (count(c, pt) != popcount(pieces(c, pt))) return fail("piece count");
    if (st->key != compute_key()) return fail("zobrist key");
    if (st->pawnKey != compute_pawn_key()) return fail("pawn key");
    if (st->materialKey != compute_material_key()) return fail("material key");
    if (st->psq != compute_psq()) return fail("incremental psq");
    for (int c = WHITE; c <= BLACK; ++c) {
        int npm = 0;
        for (int pt = KNIGHT; pt <= QUEEN; ++pt) npm += count(c, pt) * PieceValue[pt];
        if (npm != st->nonPawnMaterial[c]) return fail("non-pawn material");
    }
    if (st->checkers != (attackers_to(king_sq(sideToMove)) & pieces_c(sideToMove ^ 1))) return fail("checkers");
    if (attackers_to(king_sq(sideToMove ^ 1)) & pieces_c(sideToMove)) return fail("side not to move in check");
    for (int cr : {WHITE_OO, WHITE_OOO, BLACK_OO, BLACK_OOO})
        if (can_castle(cr)) {
            int c = cr <= WHITE_OOO ? WHITE : BLACK;
            if (piece_on(castlingRookSquare[cr]) != make_piece(c, ROOK) || king_sq(c) != relative_square(c, SQ_E1))
                return fail("castling rights");
        }
    return true;
}
