// Tessera correctness tests.
//   tessera_tests unit     - everything except the long perft suite
//   tessera_tests perft    - full perft reference suite
//   tessera_tests perftfen <fen> <depth>   - print perft count (used by crosscheck.py)
//   tessera_tests playout <n> <seed>       - print random legal positions (used by crosscheck.py)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bitboard.h"
#include "endgame.h"
#include "eval.h"
#include "movegen.h"
#include "position.h"
#include "search.h"
#include "tt.h"

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++checks;                                                                         \
        if (!(cond)) {                                                                    \
            ++failures;                                                                   \
            std::fprintf(stderr, "  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                                 \
    } while (0)

#define CHECK_MSG(cond, msg)                                                                          \
    do {                                                                                              \
        ++checks;                                                                                     \
        if (!(cond)) {                                                                                \
            ++failures;                                                                               \
            std::fprintf(stderr, "  FAILED %s:%d: %s [%s]\n", __FILE__, __LINE__, #cond, (msg).c_str()); \
        }                                                                                             \
    } while (0)

struct PerftCase {
    const char* fen;
    int depth;
    uint64_t nodes;
};

// Reference counts from the Chess Programming Wiki perft pages and the
// well-known collection of tricky positions.
const PerftCase QuickPerft[] = {
    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 4, 197281},
    {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97862},
    {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624},
    {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333},
    {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62379},
    {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 3, 89890},
};

const PerftCase FullPerft[] = {
    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 6, 119060324},
    {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 5, 193690690},
    {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 7, 178633661},
    {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 5, 15833292},
    {"r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1", 5, 15833292},
    {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 5, 89941194},
    {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 5, 164075551},
    // Tricky positions: illegal en passant, castling through/into check, promotions, stalemate, ...
    {"3k4/3p4/8/K1P4r/8/8/8/8 b - - 0 1", 6, 1134888},
    {"8/8/4k3/8/2p5/8/B2P2K1/8 w - - 0 1", 6, 1015133},
    {"8/8/1k6/2b5/2pP4/8/5K2/8 b - d3 0 1", 6, 1440467},
    {"5k2/8/8/8/8/8/8/4K2R w K - 0 1", 6, 661072},
    {"3k4/8/8/8/8/8/8/R3K3 w Q - 0 1", 6, 803711},
    {"r3k2r/1b4bq/8/8/8/8/7B/R3K2R w KQkq - 0 1", 4, 1274206},
    {"r3k2r/8/3Q4/8/8/5q2/8/R3K2R b KQkq - 0 1", 4, 1720476},
    {"2K2r2/4P3/8/8/8/8/8/3k4 w - - 0 1", 6, 3821001},
    {"8/8/1P2K3/8/2n5/1q6/8/5k2 b - - 0 1", 5, 1004658},
    {"4k3/1P6/8/8/8/8/K7/8 w - - 0 1", 6, 217342},
    {"8/P1k5/K7/8/8/8/8/8 w - - 0 1", 6, 92683},
    {"K1k5/8/P7/8/8/8/8/8 w - - 0 1", 6, 2217},
    {"8/k1P5/8/1K6/8/8/8/8 w - - 0 1", 7, 567584},
    {"8/8/2k5/5q2/5n2/8/5K2/8 b - - 0 1", 4, 23527},
};

uint64_t perft(Position& pos, int depth) {
    MoveList<LEGAL> ml(pos);
    if (depth == 1) return ml.size();
    StateInfo st;
    uint64_t n = 0;
    for (const ExtMove& m : ml) {
        pos.do_move(m.move, st);
        n += perft(pos, depth - 1);
        pos.undo_move(m.move);
    }
    return n;
}

bool run_perft(const PerftCase* cases, size_t n) {
    bool ok = true;
    for (size_t i = 0; i < n; ++i) {
        Position pos;
        StateInfo si;
        CHECK(pos.set(cases[i].fen, &si));
        auto t0 = std::chrono::steady_clock::now();
        uint64_t got = perft(pos, cases[i].depth);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        bool pass = got == cases[i].nodes;
        ok &= pass;
        CHECK_MSG(pass, std::string(cases[i].fen));
        std::printf("  %-4s d%d %12llu (expected %12llu) %6.2fs  %s\n", pass ? "ok" : "FAIL", cases[i].depth,
                    (unsigned long long)got, (unsigned long long)cases[i].nodes, sec, cases[i].fen);
    }
    return ok;
}

// Collect positions from random legal playouts.
std::vector<std::string> random_positions(int games, uint32_t seed, int maxPlies = 160) {
    static const char* starts[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        "4k3/pppppppp/8/8/8/8/PPPPPPPP/4K3 w - - 0 1",
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
    };
    std::mt19937 rng(seed);
    std::vector<std::string> out;
    for (int g = 0; g < games; ++g) {
        std::deque<StateInfo> states(1);
        Position pos;
        pos.set(starts[g % (sizeof(starts) / sizeof(starts[0]))], &states.back());
        for (int ply = 0; ply < maxPlies; ++ply) {
            MoveList<LEGAL> ml(pos);
            if (!ml.size()) break;
            // Prefer captures and promotions sometimes to reach varied material
            std::vector<Move> moves;
            for (const ExtMove& m : ml) moves.push_back(m.move);
            Move m = moves[rng() % moves.size()];
            states.emplace_back();
            pos.do_move(m, states.back());
            out.push_back(pos.fen());
            if (pos.rule50_count() > 100) break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------

void test_make_unmake_and_incremental_state() {
    std::printf("make/unmake, incremental keys, PSQT and material\n");
    std::mt19937 rng(12345);
    auto fens = random_positions(60, 7);
    int positions = 0;
    for (const std::string& fen : fens) {
        Position pos;
        StateInfo si;
        CHECK(pos.set(fen, &si));
        std::string why;
        CHECK_MSG(pos.is_consistent(&why), fen + " : " + why);
        CHECK_MSG(pos.fen() == fen, fen + " vs " + pos.fen());
        const Key key0 = pos.key();
        for (const ExtMove& m : MoveList<LEGAL>(pos)) {
            StateInfo st;
            pos.do_move(m.move, st);
            bool ok = pos.is_consistent(&why);
            CHECK_MSG(ok, fen + " after " + move_to_uci(m.move) + ": " + why);
            pos.undo_move(m.move);
            CHECK_MSG(pos.fen() == fen && pos.key() == key0, fen + " undo " + move_to_uci(m.move));
        }
        if (!pos.checkers()) {
            StateInfo st;
            pos.do_null_move(st);
            CHECK(pos.key() == pos.compute_key());
            pos.undo_null_move();
            CHECK(pos.fen() == fen && pos.key() == key0);
        }
        ++positions;
    }
    std::printf("  %d positions checked\n", positions);
}

void test_move_properties() {
    std::printf("pseudo_legal / legal / gives_check consistency\n");
    std::mt19937 rng(99);
    auto fens = random_positions(60, 3);
    for (const std::string& fen : fens) {
        Position pos;
        StateInfo si;
        pos.set(fen, &si);
        ExtMove buf[MAX_MOVES];
        ExtMove* end = pos.checkers() ? generate<EVASIONS>(pos, buf) : generate<NON_EVASIONS>(pos, buf);
        std::vector<Move> pseudo;
        for (ExtMove* p = buf; p != end; ++p) pseudo.push_back(p->move);

        MoveList<LEGAL> legalList(pos);
        size_t legalCount = 0;
        for (Move m : pseudo) {
            CHECK_MSG(pos.pseudo_legal(m), fen + " " + move_to_uci(m));
            // Independent legality: make the move and test whether our king is attacked
            StateInfo st;
            int us = pos.side_to_move();
            pos.do_move(m, st);
            bool reallyLegal = !(pos.attackers_to(pos.king_sq(us)) & pos.pieces_c(us ^ 1));
            bool inCheckAfter = pos.checkers() != 0;
            pos.undo_move(m);
            // Castling legality also forbids passing through attacked squares
            if (type_of_move(m) == CASTLING) {
                int step = to_sq(m) > from_sq(m) ? 1 : -1;
                for (int s = from_sq(m); s != to_sq(m) + step; s += step)
                    if (pos.attackers_to(s) & pos.pieces_c(us ^ 1)) reallyLegal = false;
            }
            CHECK_MSG(pos.legal(m) == reallyLegal, fen + " legal " + move_to_uci(m));
            CHECK_MSG(legalList.contains(m) == reallyLegal, fen + " list " + move_to_uci(m));
            if (reallyLegal) {
                ++legalCount;
                CHECK_MSG(pos.gives_check(m) == inCheckAfter, fen + " gives_check " + move_to_uci(m));
            }
        }
        CHECK(legalCount == legalList.size());
        // Random move encodings must be accepted exactly when they are in the pseudo-legal list
        for (int i = 0; i < 300; ++i) {
            Move m = Move(rng() & 0xFFFF);
            bool inList = std::find(pseudo.begin(), pseudo.end(), m) != pseudo.end();
            CHECK_MSG(pos.pseudo_legal(m) == inList, fen + " random " + std::to_string(m));
        }
    }
}

void test_see() {
    std::printf("static exchange evaluation\n");
    struct Case {
        const char* fen;
        const char* move;
        Value threshold;
        bool expected;
    } cases[] = {
        // Rook takes an undefended pawn
        {"1k1r4/1pp4p/p7/4p3/8/P5P1/1PP4P/2K1R3 w - - 0 1", "e1e5", 100, true},
        {"1k1r4/1pp4p/p7/4p3/8/P5P1/1PP4P/2K1R3 w - - 0 1", "e1e5", 101, false},
        // Knight takes a pawn defended by a pawn: loses the knight for a pawn
        {"4k3/8/3p4/4p3/8/5N2/8/4K3 w - - 0 1", "f3e5", 0, false},
        {"4k3/8/3p4/4p3/8/5N2/8/4K3 w - - 0 1", "f3e5", PawnValue - KnightValue, true},
        // Queen takes a defended rook: Q for R
        {"4k3/8/8/3r4/2p5/8/8/3QK3 w - - 0 1", "d1d5", 0, true},
        {"4k3/4p3/8/3r4/8/8/8/3QK3 w - - 0 1", "d1d5", 0, true},
        {"4k3/8/2p5/3r4/8/8/8/3QK3 w - - 0 1", "d1d5", 0, false},
        {"4k3/8/2p5/3r4/8/8/8/3QK3 w - - 0 1", "d1d5", RookValue - QueenValue, true},
        // X-ray: rook behind rook recaptures
        {"4k3/3r4/3r4/8/8/3R4/3R4/4K3 w - - 0 1", "d3d6", 0, true},
        // Pinned defender cannot recapture
        {"3rk3/8/8/3p4/4P3/8/8/3QK3 w - - 0 1", "e4d5", 0, true},
        {"4k3/8/8/3n4/8/8/8/3RK2b w - - 0 1", "d1d5", 0, false},
        // The c6 pawn is pinned to its king by the a4 bishop and cannot recapture
        {"4k3/8/2p5/3n4/B7/8/8/3RK3 w - - 0 1", "d1d5", KnightValue, true},
        {"4k3/8/2p5/3n4/B7/8/8/3RK3 w - - 0 1", "d1d5", KnightValue + 1, false},
    };
    for (auto& c : cases) {
        Position pos;
        StateInfo si;
        CHECK(pos.set(c.fen, &si));
        Move m = uci_to_move(pos, c.move);
        CHECK_MSG(m != MOVE_NONE, std::string(c.fen) + " " + c.move);
        if (m != MOVE_NONE) CHECK_MSG(pos.see_ge(m, c.threshold) == c.expected, std::string(c.fen) + " " + c.move);
    }
}

void test_special_moves() {
    std::printf("castling, en passant, promotions\n");
    {
        // Capturing a rook removes the corresponding castling right
        Position pos;
        StateInfo si, st;
        pos.set("r3k2r/8/8/8/8/8/6b1/R3K2R b KQkq - 0 1", &si);
        Move m = uci_to_move(pos, "g2h1");
        CHECK(m != MOVE_NONE);
        pos.do_move(m, st);
        CHECK(!pos.can_castle(WHITE_OO) && pos.can_castle(WHITE_OOO));
        CHECK(pos.can_castle(BLACK_OO) && pos.can_castle(BLACK_OOO));
        CHECK(pos.fen() == "r3k2r/8/8/8/8/8/8/R3K2b w Qkq - 0 2");
    }
    {
        // Cannot castle out of, through or into check
        Position pos;
        StateInfo si;
        pos.set("4k3/8/8/8/8/8/5r2/R3K2R w KQ - 0 1", &si);  // f2 rook attacks f1 -> no O-O, and e1 in check? no
        CHECK(uci_to_move(pos, "e1g1") == MOVE_NONE);
        CHECK(uci_to_move(pos, "e1c1") != MOVE_NONE);
        pos.set("4k3/8/8/8/8/8/3r4/R3K2R w KQ - 0 1", &si);  // d2 rook attacks d1
        CHECK(uci_to_move(pos, "e1c1") == MOVE_NONE);
        CHECK(uci_to_move(pos, "e1g1") != MOVE_NONE);
        pos.set("4k3/8/8/8/8/8/4r3/R3K2R w KQ - 0 1", &si);  // in check
        CHECK(uci_to_move(pos, "e1c1") == MOVE_NONE && uci_to_move(pos, "e1g1") == MOVE_NONE);
        pos.set("4k3/8/8/8/8/8/1r6/R3K2R w KQ - 0 1", &si);  // b1 attacked only: O-O-O allowed
        CHECK(uci_to_move(pos, "e1c1") != MOVE_NONE);
        // Castling moves the rook
        StateInfo st;
        Move m = uci_to_move(pos, "e1c1");
        pos.do_move(m, st);
        CHECK(pos.piece_on(SQ_D1) == W_ROOK && pos.piece_on(SQ_C1) == W_KING && pos.empty(SQ_A1));
        pos.undo_move(m);
        CHECK(pos.piece_on(SQ_A1) == W_ROOK && pos.piece_on(SQ_E1) == W_KING);
    }
    {
        // En passant square only recorded when a capture is possible
        Position pos;
        StateInfo si, st;
        pos.set("4k3/8/8/8/8/8/4P3/4K3 w - - 0 1", &si);
        pos.do_move(uci_to_move(pos, "e2e4"), st);
        CHECK(pos.ep_square() == SQ_NONE);
        pos.set("4k3/8/8/8/3p4/8/4P3/4K3 w - - 0 1", &si);
        pos.do_move(uci_to_move(pos, "e2e4"), st);
        CHECK(pos.ep_square() == SQ_E3);
        Move ep = uci_to_move(pos, "d4e3");
        CHECK(ep != MOVE_NONE && type_of_move(ep) == EN_PASSANT);
        // En passant that would expose the king along the rank is illegal
        pos.set("8/8/8/K2pP2r/8/8/8/4k3 w - d6 0 1", &si);
        CHECK(uci_to_move(pos, "e5d6") == MOVE_NONE);
    }
    {
        // All four promotions, including capturing ones
        Position pos;
        StateInfo si;
        pos.set("1n2k3/P7/8/8/8/8/8/4K3 w - - 0 1", &si);
        int promos = 0;
        for (const ExtMove& m : MoveList<LEGAL>(pos))
            if (type_of_move(m.move) == PROMOTION) ++promos;
        CHECK(promos == 8);
        CHECK(uci_to_move(pos, "a7b8n") != MOVE_NONE);
        CHECK(uci_to_move(pos, "a7a8r") != MOVE_NONE);
        StateInfo st;
        Move m = uci_to_move(pos, "a7b8q");
        pos.do_move(m, st);
        CHECK(pos.piece_on(SQ_B8) == W_QUEEN && pos.count(WHITE, PAWN) == 0 && pos.count(BLACK, KNIGHT) == 0);
        std::string why;
        CHECK_MSG(pos.is_consistent(&why), why);
    }
}

void test_terminal_and_draw_rules() {
    std::printf("check, checkmate, stalemate, repetition, fifty-move rule, insufficient material\n");
    Position pos;
    StateInfo si;
    // Fool's mate
    pos.set("rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3", &si);
    CHECK(pos.checkers() && MoveList<LEGAL>(pos).size() == 0);
    // Stalemate
    pos.set("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", &si);
    CHECK(!pos.checkers() && MoveList<LEGAL>(pos).size() == 0);
    // Double check: only king moves
    pos.set("4k3/8/8/8/8/8/2n5/R3K2r w Q - 0 1", &si);
    for (const ExtMove& m : MoveList<LEGAL>(pos)) CHECK(type_of(pos.moved_piece(m.move)) == KING);

    // Repetition: Nf3 Nf6 Ng1 Ng8 twice
    {
        std::deque<StateInfo> states(1);
        Position p;
        p.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &states.back());
        const char* seq[] = {"g1f3", "g8f6", "f3g1", "f6g8", "g1f3", "g8f6", "f3g1", "f6g8"};
        int i = 0;
        for (const char* mv : seq) {
            states.emplace_back();
            p.do_move(uci_to_move(p, mv), states.back());
            ++i;
            if (i == 4) {
                CHECK(p.has_repeated());
                CHECK(p.is_draw(5));   // repetition inside the search tree
                CHECK(!p.is_draw(3));  // occurred before the root: needs threefold
            }
        }
        CHECK(p.is_draw(0));  // threefold
    }
    // Fifty-move rule
    {
        std::deque<StateInfo> states(1);
        Position p;
        p.set("4k3/8/8/8/8/8/8/R3K3 w - - 99 80", &states.back());
        CHECK(!p.is_draw(0));
        states.emplace_back();
        p.do_move(uci_to_move(p, "a1a2"), states.back());
        CHECK(p.is_draw(0));
        // A checkmate delivered on the hundredth half-move is still a mate
        Position q;
        std::deque<StateInfo> s2(1);
        q.set("7k/8/6K1/8/8/8/8/R7 w - - 99 80", &s2.back());
        s2.emplace_back();
        q.do_move(uci_to_move(q, "a1a8"), s2.back());
        CHECK(q.checkers() && MoveList<LEGAL>(q).size() == 0);
        CHECK(!q.is_draw(0));
    }
    // Insufficient material
    const char* drawn[] = {"8/8/4k3/8/8/3K4/8/8 w - - 0 1", "8/8/4k3/8/8/3K4/5N2/8 w - - 0 1",
                           "8/8/4kb2/8/8/3K4/5B2/8 w - - 0 1", "8/8/4k3/8/8/3KB3/8/8 b - - 0 1"};
    for (const char* f : drawn) {
        pos.set(f, &si);
        CHECK_MSG(pos.is_insufficient_material(), std::string(f));
    }
    const char* notDrawn[] = {"8/8/4k3/8/8/3K4/5P2/8 w - - 0 1", "8/8/4kb2/8/8/3K4/4B3/8 w - - 0 1",
                              "8/8/4kn2/8/8/3K4/5B2/8 w - - 0 1"};
    for (const char* f : notDrawn) {
        pos.set(f, &si);
        CHECK_MSG(!pos.is_insufficient_material(), std::string(f));
    }
}

void test_zobrist() {
    std::printf("zobrist hashing\n");
    auto play = [](const std::vector<std::string>& moves) {
        std::deque<StateInfo> states(1);
        Position p;
        p.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &states.back());
        for (auto& mv : moves) {
            states.emplace_back();
            p.do_move(uci_to_move(p, mv), states.back());
        }
        return std::make_pair(p.key(), p.fen());
    };
    auto a = play({"e2e4", "e7e5", "g1f3", "b8c6"});
    auto b = play({"g1f3", "b8c6", "e2e4", "e7e5"});
    CHECK(a.first == b.first);  // transposition
    auto c = play({"e2e3", "e7e6", "e3e4", "e6e5", "g1f3", "b8c6"});
    CHECK(c.first == a.first);  // same position reached in a different number of moves
    {
        Position w, bl;
        StateInfo sw, sb;
        w.set("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3", &sw);
        bl.set("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 2 3", &sb);
        CHECK(w.key() == a.first && w.key() != bl.key());  // side to move is part of the key
    }
    // Castling rights and en passant change the key
    Position p1, p2;
    StateInfo s1, s2;
    p1.set("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", &s1);
    p2.set("r3k2r/8/8/8/8/8/8/R3K2R w KQk - 0 1", &s2);
    CHECK(p1.key() != p2.key());
    p1.set("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", &s1);
    p2.set("4k3/8/8/3pP3/8/8/8/4K3 w - - 0 1", &s2);
    CHECK(p1.key() != p2.key());
    // Key recomputed from FEN matches the incremental key along random games
    for (const std::string& fen : random_positions(20, 11)) {
        Position p;
        StateInfo s;
        p.set(fen, &s);
        CHECK(p.key() == p.compute_key());
    }
}

void test_eval() {
    std::printf("evaluation symmetry, cache consistency and endgame knowledge\n");
    auto cache = std::make_unique<Eval::Cache>();
    cache->clear();
    auto trace = std::make_unique<EvalTrace>();
    int n = 0;
    for (const std::string& fen : random_positions(80, 5)) {
        Position pos;
        StateInfo si;
        pos.set(fen, &si);
        if (pos.checkers()) continue;
        Value v = Eval::evaluate(pos, *cache);
        Value vt = Eval::evaluate_trace(pos, *trace);
        CHECK_MSG(v == vt || std::abs(v) > Eval::LazyThreshold, fen + " cached " + std::to_string(v) + " traced " + std::to_string(vt));
        Value again = Eval::evaluate(pos, *cache);  // now served from the pawn/material caches
        CHECK(again == v);
        Position flipped;
        StateInfo sf;
        flipped = pos;
        flipped.flip(&sf);
        Value vf = Eval::evaluate(flipped, *cache);
        CHECK_MSG(v == vf, fen + " " + std::to_string(v) + " vs flipped " + std::to_string(vf));
        ++n;
    }
    std::printf("  %d positions\n", n);

    // Endgames
    struct EgCase {
        const char* fen;
        int expectSign;  // +1: side to move clearly winning, 0: draw score, -1 losing
    } cases[] = {
        {"8/8/8/4k3/8/8/8/3QK3 w - - 0 1", +1},      // KQK
        {"8/8/8/4k3/8/8/8/3RK3 b - - 0 1", -1},      // KRK
        {"8/8/8/4k3/8/8/8/2BNK3 w - - 0 1", +1},     // KBNK
        {"8/8/8/4k3/8/8/8/3NK3 w - - 0 1", 0},       // KNK
        {"8/8/8/4k3/8/8/8/2NNK3 w - - 0 1", 0},      // KNNK
        {"8/8/8/4k3/8/8/8/3BK3 w - - 0 1", 0},       // KBK
        {"4k3/8/4K3/4P3/8/8/8/8 w - - 0 1", +1},     // KPK, king in front on the 6th: win
        {"4k3/8/4K3/4P3/8/8/8/8 b - - 0 1", -1},
        {"8/4k3/8/4K3/4P3/8/8/8 w - - 0 1", 0},      // KPK, defender has the opposition
        {"8/4k3/8/4K3/4P3/8/8/8 b - - 0 1", -1},     // ... but not with black to move
        {"k7/8/1K6/P7/8/8/8/8 w - - 0 1", 0},        // rook pawn, defender in the corner
    };
    for (auto& c : cases) {
        Position pos;
        StateInfo si;
        pos.set(c.fen, &si);
        Value v = Eval::evaluate(pos, *cache);
        bool ok = c.expectSign > 0 ? v > 2 * PawnValue : c.expectSign < 0 ? v < -2 * PawnValue : v == 0;
        CHECK_MSG(ok, std::string(c.fen) + " eval " + std::to_string(v));
    }
    // KPK bitbase spot checks (strong side white, pawn on files a-d)
    CHECK(Bitbases::probe_kpk(SQ_E6, SQ_E5, SQ_E8, WHITE));
    CHECK(Bitbases::probe_kpk(SQ_E6, SQ_E5, SQ_E8, BLACK));
    CHECK(!Bitbases::probe_kpk(SQ_B6, SQ_A5, SQ_A8, WHITE));
    CHECK(!Bitbases::probe_kpk(SQ_E5, SQ_E4, SQ_E7, WHITE));
    CHECK(Bitbases::probe_kpk(SQ_E5, SQ_E4, SQ_E7, BLACK));
}

void test_tt() {
    std::printf("transposition table\n");
    TranspositionTable tt;
    tt.resize(1);
    TTData d;
    const Key k = 0x123456789ABCDEFULL;
    CHECK(!tt.probe(k, d));
    tt.store(k, 123, true, BOUND_EXACT, 10, make_move(SQ_E2, SQ_E4), -45);
    CHECK(tt.probe(k, d));
    CHECK(d.value == 123 && d.eval == -45 && d.depth == 10 && d.bound == BOUND_EXACT && d.pv &&
          d.move == make_move(SQ_E2, SQ_E4));
    // Shallower non-exact result for the same key does not overwrite a deep entry, but keeps a new move
    tt.store(k, 50, false, BOUND_LOWER, 2, make_move(SQ_D2, SQ_D4), 0);
    CHECK(tt.probe(k, d) && d.depth == 10 && d.value == 123 && d.move == make_move(SQ_D2, SQ_D4));
    // Qsearch and "eval only" depths round trip
    tt.store(k ^ 1, -7, false, BOUND_UPPER, DEPTH_QS, MOVE_NONE, 3);
    CHECK(tt.probe(k ^ 1, d) && d.depth == DEPTH_QS && d.bound == BOUND_UPPER && d.value == -7);
    tt.store(k ^ 2, VALUE_NONE, false, BOUND_NONE, DEPTH_NONE, MOVE_NONE, 77);
    CHECK(tt.probe(k ^ 2, d) && d.depth == DEPTH_NONE && d.value == VALUE_NONE && d.eval == 77);
    // Mate scores are stored relative to the node
    for (int ply : {0, 3, 17}) {
        for (Value v : {mate_in(5 + ply), mated_in(8 + ply), Value(250), Value(-31)}) {
            Value stored = value_to_tt(v, ply);
            CHECK(value_from_tt(stored, ply, 0) == v);
        }
    }
    CHECK(value_to_tt(mate_in(10), 4) == mate_in(6));
    // A mate that cannot be delivered before the fifty-move rule is not reported as mate
    CHECK(value_from_tt(mate_in(20), 0, 90) < VALUE_MATE_IN_MAX_PLY);
    // Hashfull grows with the number of entries of the current generation
    tt.clear();
    CHECK(tt.hashfull() == 0);
    for (Key x = 1; x < 200000; ++x) tt.store(x * 0x9E3779B97F4A7C15ULL, 1, false, BOUND_EXACT, 5, MOVE_NONE, 0);
    CHECK(tt.hashfull() > 900);
    tt.new_search();
    CHECK(tt.hashfull() == 0);
    // Resize and clear
    tt.resize(4);
    CHECK(tt.size_mb() == 4 && tt.hashfull() == 0);

    // Concurrent stress test: every probe hit must carry data derived from its own key
    tt.resize(1);
    std::atomic<int> bad{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t)
        workers.emplace_back([&, t]() {
            std::mt19937_64 rng(t);
            for (int i = 0; i < 300000; ++i) {
                Key key = rng() % 5000 + 1;
                key *= 0xD6E8FEB86659FD93ULL;
                Value v = Value(key % 2000) - 1000;
                Move m = Move((key >> 20) & 0xFFF);
                if (i & 1)
                    tt.store(key, v, false, BOUND_EXACT, int(key % 50), m, v / 2);
                else {
                    TTData dd;
                    if (tt.probe(key, dd) && (dd.value != v || dd.move != m || dd.eval != v / 2)) bad++;
                }
            }
        });
    for (auto& w : workers) w.join();
    CHECK(bad == 0);
}

struct SearchResult {
    Move best;
    Value score;
    Depth depth;
};

SearchResult search_position(const std::string& fen, Search::LimitsType limits) {
    StateListPtr states(new std::deque<StateInfo>(1));
    Position pos;
    pos.set(fen, &states->back());
    limits.startTime = now_ms();
    Threads.start_thinking(pos, states, limits, false);
    Threads.wait_for_search_finished();
    const Search::Thread* th = Threads.main();
    return {th->best_move(), th->best_score(), th->completedDepth};
}

void test_search() {
    std::printf("search: mates, stalemate, interruption, threads, node limits\n");
    Threads.shared.silent = true;
    TT.clear();
    Threads.clear();
    Search::LimitsType lim;
    lim.depth = 8;

    auto r = search_position("k7/8/1K6/8/8/8/8/7R w - - 0 1", lim);
    CHECK(move_to_uci(r.best) == "h1h8" && r.score == mate_in(1));
    r = search_position("k7/8/2K5/8/8/8/8/7R w - - 0 1", lim);
    CHECK(r.score == mate_in(3));
    // Avoid stalemate in KQK: must find the mate, not Qb6 stalemate
    r = search_position("k7/8/1K6/8/8/8/8/2Q5 w - - 0 1", lim);
    CHECK(r.score >= VALUE_MATE_IN_MAX_PLY);
    // Being mated is reported as a negative mate score
    r = search_position("k7/8/1K6/8/8/8/8/7R b - - 0 1", lim);
    CHECK(r.score <= VALUE_MATED_IN_MAX_PLY || r.score < -VALUE_KNOWN_WIN);
    // No legal moves
    r = search_position("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", lim);
    CHECK(r.best == MOVE_NONE);
    // A mate in 2 with a quiet first move (back-rank)
    lim.depth = 10;
    r = search_position("6k1/5ppp/8/8/8/8/1Q3PPP/1R4K1 w - - 0 1", lim);
    CHECK(r.score >= mate_in(5));
    // Tactics: win the queen with a knight fork (Nc7+)
    r = search_position("r3k3/ppq2ppp/8/1N6/8/8/PPP2PPP/4K3 w - - 0 1", lim);
    CHECK(move_to_uci(r.best) == "b5c7");
    // Draw by repetition is recognised: perpetual check saves a lost position
    r = search_position("6k1/5ppp/8/8/8/8/q4PPP/1Q4K1 w - - 0 1", lim);
    CHECK(r.best != MOVE_NONE);

    // searchmoves restricts the root
    lim = Search::LimitsType();
    lim.depth = 6;
    {
        StateListPtr states(new std::deque<StateInfo>(1));
        Position pos;
        pos.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &states->back());
        lim.searchmoves.push_back(uci_to_move(pos, "a2a3"));
        lim.startTime = now_ms();
        Threads.start_thinking(pos, states, lim, false);
        Threads.wait_for_search_finished();
        CHECK(move_to_uci(Threads.main()->best_move()) == "a2a3");
    }

    // Node limit is honoured closely with one thread
    lim = Search::LimitsType();
    lim.nodes = 20000;
    r = search_position("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4", lim);
    CHECK(Threads.nodes_searched() >= 20000 && Threads.nodes_searched() < 20000 + 2048);
    CHECK(r.best != MOVE_NONE);

    // Interrupting an infinite search leaves positions intact and returns a legal move
    {
        const std::string fen = "r1bq1rk1/pp2bppp/2n1pn2/3p4/2PP4/2N1PN2/PP3PPP/R2QKB1R w KQ - 1 8";
        StateListPtr states(new std::deque<StateInfo>(1));
        Position pos;
        pos.set(fen, &states->back());
        Position copy = pos;
        Search::LimitsType inf;
        inf.infinite = true;
        inf.startTime = now_ms();
        Threads.start_thinking(pos, states, inf, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        Threads.stop();
        Threads.wait_for_search_finished();
        CHECK(pos.fen() == fen && pos.key() == copy.key());
        CHECK(MoveList<LEGAL>(pos).contains(Threads.main()->best_move()));
        CHECK(Threads.main()->rootPos.fen() == fen);
    }

    // Multi-threaded search completes and agrees on a legal move
    Threads.set(4);
    Threads.shared.silent = true;
    for (const char* fen : {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
                            "4rrk1/pp1n3p/3q2pQ/2p1pb2/2PP4/2P3N1/P2B2PP/4RRK1 b - - 7 19"}) {
        Search::LimitsType l;
        l.movetime = 300;
        StateListPtr states(new std::deque<StateInfo>(1));
        Position pos;
        pos.set(fen, &states->back());
        Position copy = pos;
        l.startTime = now_ms();
        Threads.start_thinking(pos, states, l, false);
        Threads.wait_for_search_finished();
        CHECK(MoveList<LEGAL>(copy).contains(Threads.main()->best_move()));
        CHECK(Threads.nodes_searched() > 0);
    }
    // Repeated start/stop cycles with several threads
    for (int i = 0; i < 20; ++i) {
        Search::LimitsType l;
        l.infinite = true;
        StateListPtr states(new std::deque<StateInfo>(1));
        Position pos;
        pos.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &states->back());
        l.startTime = now_ms();
        Threads.start_thinking(pos, states, l, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(5 + i));
        Threads.stop();
        Threads.wait_for_search_finished();
        CHECK(Threads.main()->best_move() != MOVE_NONE);
    }
    Threads.set(1);
    Threads.shared.silent = true;
}

}  // namespace

int main(int argc, char* argv[]) {
    Bitboards::init();
    Position::init();
    Eval::init();
    Search::init();
    TT.resize(16);
    Threads.set(1);
    Threads.shared.silent = true;

    std::string mode = argc > 1 ? argv[1] : "unit";
    if (mode == "perftfen" && argc >= 4) {
        Position pos;
        StateInfo si;
        if (!pos.set(argv[2], &si)) return 2;
        std::printf("%llu\n", (unsigned long long)perft(pos, std::atoi(argv[3])));
        Threads.set(0);
        return 0;
    }
    if (mode == "perftbatch" && argc >= 3) {
        // One FEN per line on stdin, one perft count per line on stdout
        std::string line;
        int depth = std::atoi(argv[2]);
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            Position pos;
            StateInfo si;
            if (!pos.set(line, &si)) {
                std::printf("invalid\n");
                continue;
            }
            std::printf("%llu\n", (unsigned long long)perft(pos, depth));
        }
        Threads.set(0);
        return 0;
    }
    if (mode == "playout" && argc >= 4) {
        for (const std::string& f : random_positions(std::atoi(argv[2]), uint32_t(std::atoi(argv[3])), 120))
            std::printf("%s\n", f.c_str());
        Threads.set(0);
        return 0;
    }

    auto t0 = std::chrono::steady_clock::now();
    if (mode == "perft") {
        std::printf("perft reference suite\n");
        run_perft(FullPerft, sizeof(FullPerft) / sizeof(FullPerft[0]));
    } else {
        std::printf("quick perft\n");
        run_perft(QuickPerft, sizeof(QuickPerft) / sizeof(QuickPerft[0]));
        test_make_unmake_and_incremental_state();
        test_move_properties();
        test_see();
        test_special_moves();
        test_terminal_and_draw_rules();
        test_zobrist();
        test_eval();
        test_tt();
        test_search();
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n%d checks, %d failures (%.1fs)\n", checks, failures, sec);
    Threads.set(0);
    return failures ? 1 : 0;
}
