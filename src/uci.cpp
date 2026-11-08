#include "uci.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <vector>

#include "eval.h"
#include "movegen.h"
#include "search.h"
#include "tt.h"

namespace {

const char* StartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

struct Options {
    int hash = 16;
    int threads = 1;
    int moveOverhead = 30;
    int contempt = 0;
    bool tunedEval = true;
} opts;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void print_options() {
    std::ostringstream ss;
    ss << "id name " << ENGINE_NAME << ' ' << ENGINE_VERSION << "\n";
    ss << "id author " << ENGINE_AUTHOR << "\n";
    ss << "option name Hash type spin default 16 min 1 max 65536\n";
    ss << "option name Threads type spin default 1 min 1 max 512\n";
    ss << "option name Clear Hash type button\n";
    ss << "option name Ponder type check default false\n";
    ss << "option name Move Overhead type spin default 30 min 0 max 5000\n";
    ss << "option name Contempt type spin default 0 min -100 max 100\n";
    ss << "option name TunedEval type check default true\n";
    ss << "uciok";
    sync_print(ss.str());
}

void set_option(std::istringstream& is) {
    std::string token, name, value;
    is >> token;  // "name"
    while (is >> token && token != "value") name += (name.empty() ? "" : " ") + token;
    while (is >> token) value += (value.empty() ? "" : " ") + token;
    const std::string n = lower(name);

    Threads.wait_for_search_finished();
    auto to_int = [&](int lo, int hi, int def) {
        try {
            return std::clamp(std::stoi(value), lo, hi);
        } catch (...) {
            return def;
        }
    };

    if (n == "hash") {
        opts.hash = to_int(1, 65536, opts.hash);
        TT.resize(size_t(opts.hash), opts.threads);
    } else if (n == "threads") {
        opts.threads = to_int(1, 512, opts.threads);
        Threads.set(size_t(opts.threads));
    } else if (n == "clear hash") {
        TT.clear(opts.threads);
        Threads.clear();
    } else if (n == "move overhead") {
        opts.moveOverhead = to_int(0, 5000, opts.moveOverhead);
        Threads.shared.moveOverhead = opts.moveOverhead;
    } else if (n == "contempt") {
        opts.contempt = to_int(-100, 100, opts.contempt);
        Threads.shared.contempt = opts.contempt;
    } else if (n == "ponder") {
        // Nothing to do: pondering is driven by "go ponder" / "ponderhit"
    } else if (n == "tunedeval") {
        opts.tunedEval = lower(value) != "false";
        set_params(P, opts.tunedEval);
        Eval::init_psqt();
        Threads.clear();
    } else
        sync_print("info string unknown option " + name);
}

// Returns false (and leaves pos untouched) on malformed input.
bool position_cmd(Position& pos, std::istringstream& is, StateListPtr& states) {
    std::string token, fen;
    is >> token;
    if (token == "startpos") {
        fen = StartFEN;
        is >> token;  // optional "moves"
    } else if (token == "fen") {
        while (is >> token && token != "moves") fen += token + " ";
    } else
        return false;

    StateListPtr newStates(new std::deque<StateInfo>(1));
    Position tmp;
    if (!tmp.set(fen, &newStates->back())) {
        sync_print("info string invalid fen");
        return false;
    }
    while (is >> token) {
        Move m = uci_to_move(tmp, token);
        if (m == MOVE_NONE) {
            sync_print("info string illegal move " + token);
            break;
        }
        newStates->emplace_back();
        tmp.do_move(m, newStates->back());
    }
    pos = tmp;
    states = std::move(newStates);
    return true;
}

void go_cmd(Position& pos, std::istringstream& is, StateListPtr& states) {
    Search::LimitsType limits;
    limits.startTime = now_ms();
    std::string token;
    bool ponderMode = false;

    while (is >> token) {
        if (token == "searchmoves") {
            while (is >> token) {
                Move m = uci_to_move(pos, token);
                if (m != MOVE_NONE) limits.searchmoves.push_back(m);
            }
        } else if (token == "wtime" || token == "btime") {
            TimePoint t = 0;
            is >> t;
            limits.time[token == "wtime" ? WHITE : BLACK] = std::max<TimePoint>(1, t);
        } else if (token == "winc") {
            is >> limits.inc[WHITE];
            limits.inc[WHITE] = std::max<TimePoint>(0, limits.inc[WHITE]);
        } else if (token == "binc") {
            is >> limits.inc[BLACK];
            limits.inc[BLACK] = std::max<TimePoint>(0, limits.inc[BLACK]);
        } else if (token == "movestogo")
            is >> limits.movestogo;
        else if (token == "depth")
            is >> limits.depth;
        else if (token == "nodes")
            is >> limits.nodes;
        else if (token == "movetime")
            is >> limits.movetime;
        else if (token == "mate")
            is >> limits.mate;
        else if (token == "infinite")
            limits.infinite = true;
        else if (token == "ponder")
            ponderMode = true;
        else if (token == "perft") {
            int d = 1;
            is >> d;
            TimePoint t0 = now_ms();
            uint64_t n = Search::perft(pos, std::max(1, d));
            TimePoint dt = std::max<TimePoint>(1, now_ms() - t0);
            sync_print("\nNodes searched: " + std::to_string(n) + "\nTime (ms): " + std::to_string(dt) +
                       "\nNodes/second: " + std::to_string(n * 1000 / uint64_t(dt)));
            return;
        }
    }
    Threads.start_thinking(pos, states, limits, ponderMode);
}

const std::vector<std::string> BenchFens = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 11",
    "4rrk1/pp1n3p/3q2pQ/2p1pb2/2PP4/2P3N1/P2B2PP/4RRK1 b - - 7 19",
    "rq3rk1/ppp2ppp/1bnpb3/3N2B1/3NP3/7P/PPPQ1PP1/2KR3R w - - 7 14",
    "r1bq1r1k/1pp1n1pp/1p1p4/4p2Q/4Pp2/1BNP4/PPP2PPP/3R1RK1 w - - 2 14",
    "r3r1k1/2p2ppp/p1p1bn2/8/1q2P3/2NPQN2/PPP3PP/R4RK1 b - - 2 15",
    "r1bbk1nr/pp3p1p/2n5/1N4p1/2Np1B2/8/PPP2PPP/2KR1B1R w kq - 0 13",
    "r1bq1rk1/ppp1nppp/4n3/3p3Q/3P4/1BP1B3/PP1N2PP/R4RK1 w - - 1 16",
    "4r1k1/r1q2ppp/ppp2n2/4P3/5Rb1/1N1BQ3/PPP3PP/R5K1 w - - 1 17",
    "2rqkb1r/ppp2p2/2npb1p1/1N1Nn2p/2P1PP2/8/PP2B1PP/R1BQK2R b KQ - 0 11",
    "r1bq1r1k/b1p1npp1/p2p3p/1p6/3PP3/1B2NN2/PP3PPP/R2Q1RK1 w - - 1 16",
    "3r1rk1/p5pp/bpp1pp2/8/q1PP1P2/b3P3/P2NQRPP/1R2B1K1 b - - 6 22",
    "r1q2rk1/2p1bppp/2Pp4/p6b/Q1PNp3/4B3/PP1R1PPP/2K4R w - - 2 18",
    "4k2r/1pb2ppp/1p2p3/1R1p4/3P4/2r1PN2/P4PPP/1R4K1 b - - 3 22",
    "3q2k1/pb3p1p/4pbp1/2r5/PpN2N2/1P2P2P/5PP1/Q2R2K1 b - - 4 26",
    "6k1/6p1/6Pp/ppp5/3pn2P/1P3K2/1PP2P2/3N4 b - - 0 1",
    "3b4/5kp1/1p1p1p1p/pP1PpP1P/P1P1P3/3KN3/8/8 w - - 0 1",
    "2K5/p7/7P/5pR1/8/5k2/r7/8 w - - 0 1",
    "8/6pk/1p6/8/PP3p1p/5P2/4KP1q/3Q4 w - - 0 1",
    "7k/3p2pp/4q3/8/4Q3/5Kp1/P6b/8 w - - 0 1",
    "8/2p5/8/2kPKp1p/2p4P/2P5/3P4/8 w - - 0 1",
    "8/1p3pp1/7p/5P1P/2k3P1/8/2K2P2/8 w - - 0 1",
    "8/pp2r1k1/2p1p3/3pP2p/1P1P1P1P/P5KR/8/8 w - - 0 1",
    "8/3p4/p1bk3p/Pp6/1Kp1PpPp/2P2P1P/2P5/5B2 b - - 0 1",
    "5k2/7R/4P2p/5K2/p1r2P1p/8/8/8 b - - 0 1",
    "6k1/6p1/P6p/r1N5/5p2/7P/1b3PP1/4R1K1 w - - 0 1",
    "1r3k2/4q3/2Pp3b/3Bp3/2Q2p2/1p1P2P1/1P2KP2/3N4 w - - 0 1",
    "6k1/4pp1p/3p2p1/P1pPb3/R7/1r2P1PP/3B1P2/6K1 w - - 0 1",
    "8/3p3B/5p2/5P2/p7/PP5b/k7/6K1 w - - 0 1",
    "8/8/8/8/5kp1/P7/8/1K1N4 w - - 0 1",
    "8/8/8/5N2/8/p7/8/2NK3k w - - 0 1",
    "8/3k4/8/8/8/4B3/4KB2/2B5 w - - 0 1",
    "8/8/1P6/5pr1/8/4R3/7k/2K5 w - - 0 1",
    "8/2p4P/8/kr6/6R1/8/8/1K6 w - - 0 1",
    "8/8/3P3k/8/1p6/8/1P6/1K3n2 b - - 0 1",
    "8/R7/2q5/8/6k1/8/1P5p/K6R w - - 0 124",
    "6k1/3b3r/1p1p4/p1n2p2/1PPNpP1q/P3Q1p1/1R1RB1P1/5K2 b - - 0 1",
    "r2r1n2/pp2bk2/2p1p2p/3q4/3PN1QP/2P3R1/P4PP1/5RK1 w - - 0 1",
    "8/8/8/8/8/6k1/6p1/6K1 w - - 0 1",
    "7k/7P/6K1/8/3B4/8/8/8 b - - 0 1",
    "r1bqkbnr/pp1ppppp/2n5/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
    "rnbqkb1r/pp3ppp/4pn2/2pp4/2PP4/2N2N2/PP2PPPP/R1BQKB1R w KQkq - 0 5",
    "r2qkb1r/pp2pppp/2n2n2/3p1b2/3P4/2N1PN2/PP3PPP/R1BQKB1R w KQkq - 3 7",
    "r1bq1rk1/pp2bppp/2n1pn2/3p4/2PP4/2N1PN2/PP3PPP/R2QKB1R w KQ - 1 8",
    "2r2rk1/1bqnbppp/p2ppn2/1p6/3NP3/1BN1BP2/PPPQ2PP/2KR3R w - - 4 13",
};

void bench_cmd(std::istringstream& is) {
    int depth = 13, threads = 1, hash = 16;
    std::string tok;
    if (is >> tok) depth = std::max(1, std::atoi(tok.c_str()));
    if (is >> tok) threads = std::max(1, std::atoi(tok.c_str()));
    if (is >> tok) hash = std::max(1, std::atoi(tok.c_str()));
    UCI::bench(depth, threads, hash, true);
}

}  // namespace

uint64_t UCI::bench(int depth, int threads, int hashMb, bool verbose) {
    Threads.wait_for_search_finished();
    const size_t oldThreads = Threads.size();
    const size_t oldHash = TT.size_mb();
    TT.resize(size_t(hashMb), threads);
    Threads.set(size_t(threads));
    Threads.shared.silent = true;

    uint64_t totalNodes = 0;
    TimePoint totalTime = 0;
    int idx = 0;
    for (const std::string& fen : BenchFens) {
        ++idx;
        StateListPtr states(new std::deque<StateInfo>(1));
        Position pos;
        if (!pos.set(fen, &states->back())) {
            std::cerr << "bench: invalid fen " << fen << std::endl;
            continue;
        }
        TT.clear(threads);
        Threads.clear();
        Search::LimitsType limits;
        limits.depth = depth;
        limits.startTime = now_ms();
        Threads.start_thinking(pos, states, limits, false);
        Threads.wait_for_search_finished();
        TimePoint dt = now_ms() - limits.startTime;
        uint64_t n = Threads.nodes_searched();
        totalNodes += n;
        totalTime += dt;
        if (verbose) {
            const Search::Thread* th = Threads.main();
            std::cerr << "Position " << idx << "/" << BenchFens.size() << ": " << fen << "\n  bestmove "
                      << move_to_uci(th->best_move()) << " score " << Search::score_to_uci(th->best_score())
                      << " depth " << th->completedDepth << " nodes " << n << " time " << dt << "ms" << std::endl;
        }
    }
    Threads.shared.silent = false;
    totalTime = std::max<TimePoint>(1, totalTime);
    std::cerr << "\n===========================\nTotal time (ms) : " << totalTime << "\nNodes searched  : " << totalNodes
              << "\nNodes/second    : " << totalNodes * 1000 / uint64_t(totalTime) << std::endl;
    // Machine-readable summary on stdout (used by scripts)
    sync_print(std::to_string(totalNodes) + " nodes " + std::to_string(totalNodes * 1000 / uint64_t(totalTime)) + " nps");

    TT.resize(oldHash ? oldHash : 16, int(oldThreads));
    Threads.set(oldThreads ? oldThreads : 1);
    return totalNodes;
}

void UCI::loop(int argc, char* argv[]) {
    Position pos;
    StateListPtr states(new std::deque<StateInfo>(1));
    pos.set(StartFEN, &states->back());

    std::string cmd;
    for (int i = 1; i < argc; ++i) cmd += std::string(argv[i]) + " ";
    const bool oneShot = argc > 1;

    do {
        if (!oneShot && !std::getline(std::cin, cmd)) cmd = "quit";
        std::istringstream is(cmd);
        std::string token;
        is >> std::skipws >> token;

        if (token == "quit" || token == "stop") {
            Threads.stop();
            if (token == "quit") Threads.wait_for_search_finished();
        } else if (token == "ponderhit")
            Threads.ponderhit();
        else if (token == "uci")
            print_options();
        else if (token == "isready")
            sync_print("readyok");
        else if (token == "setoption")
            set_option(is);
        else if (token == "ucinewgame") {
            Threads.wait_for_search_finished();
            TT.clear(opts.threads);
            Threads.clear();
        } else if (token == "position") {
            Threads.wait_for_search_finished();
            position_cmd(pos, is, states);
        } else if (token == "go") {
            Threads.wait_for_search_finished();
            go_cmd(pos, is, states);
        } else if (token == "bench")
            bench_cmd(is);
        else if (token == "perft") {
            int d = 1;
            is >> d;
            std::istringstream pi("perft " + std::to_string(d));
            go_cmd(pos, pi, states);
        } else if (token == "d")
            sync_print(pos.pretty());
        else if (token == "eval") {
            if (pos.checkers())
                sync_print("info string position is in check, no static evaluation");
            else {
                Eval::Cache* cache = new Eval::Cache();
                cache->clear();
                std::string s = Eval::describe(pos);
                s += "Static eval (side to move): " + std::to_string(Eval::evaluate(pos, *cache));
                delete cache;
                sync_print(s);
            }
        } else if (token == "flip") {
            Threads.wait_for_search_finished();
            StateListPtr ns(new std::deque<StateInfo>(1));
            pos.flip(&ns->back());
            states = std::move(ns);
        } else if (!token.empty())
            sync_print("info string unknown command " + token);

        if (oneShot) {
            Threads.wait_for_search_finished();
            break;
        }
    } while (cmd != "quit");
}
