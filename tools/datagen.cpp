// Self-play data generation for Texel tuning.
//
// Plays engine-vs-itself games at a fixed node budget per move, starting from
// an opening file plus a few random plies, and writes quiet positions labelled
// with the game result and the search score:
//
//     <fen> | <score, white POV, centipawns> | <result: 1.0 / 0.5 / 0.0, white POV>
//
// Each worker owns a private transposition table and a synchronous search
// thread, so workers share nothing but the output file.
#include <atomic>
#include <cmath>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "movegen.h"
#include "position.h"
#include "search.h"
#include "tt.h"

namespace {

struct DatagenConfig {
    std::string out = "data/train.txt";
    std::string openings = "data/openings.epd";
    int games = 1000;
    int threads = 1;
    uint64_t nodes = 6000;
    int randomPlies = 6;
    uint64_t seed = 1;
};

std::vector<std::string> read_openings(const std::string& path) {
    std::vector<std::string> v;
    std::ifstream in(path);
    std::string line;
    StateInfo st;
    Position pos;
    while (std::getline(in, line)) {
        // EPD lines may carry operations after the four FEN fields
        std::istringstream ss(line);
        std::string f[4], fen;
        if (!(ss >> f[0] >> f[1] >> f[2] >> f[3])) continue;
        fen = f[0] + " " + f[1] + " " + f[2] + " " + f[3] + " 0 1";
        if (pos.set(fen, &st)) v.push_back(fen);
    }
    return v;
}

class Worker {
public:
    Worker(const DatagenConfig& c, uint64_t seed) : cfg(c), rng(seed) {
        tt.resize(16);
        shared.tt = &tt;
        shared.silent = true;
        thread = std::make_unique<Search::Thread>(0, &shared, nullptr, false);
    }

    // Search the current position; returns false if there is no legal move.
    bool search(const Position& pos, uint64_t nodes, Move& best, Value& score) {
        Search::RootMoves rms;
        for (const ExtMove& m : MoveList<LEGAL>(pos)) rms.emplace_back(m.move);
        if (rms.empty()) return false;
        shared.limits = Search::LimitsType{};
        shared.limits.nodes = nodes;
        shared.limits.startTime = now_ms();
        shared.stop = false;
        shared.ponder = false;
        thread->setup(pos, rms);
        thread->search_main();
        best = thread->best_move();
        score = thread->best_score();
        return true;
    }

    // Plays one game and appends its labelled positions to `lines`.
    void play(const std::string& startFen, std::vector<std::string>& lines) {
        StateListPtr states(new std::deque<StateInfo>(1));
        Position pos;
        pos.set(startFen, &states->back());
        tt.clear();
        thread->clear();

        // Random opening plies for diversity
        for (int i = 0; i < cfg.randomPlies; ++i) {
            MoveList<LEGAL> ml(pos);
            if (!ml.size()) return;
            Move m = ml.begin()[std::uniform_int_distribution<size_t>(0, ml.size() - 1)(rng)].move;
            states->emplace_back();
            pos.do_move(m, states->back());
        }
        // Reject openings that are already decided
        Move best;
        Value score;
        if (!search(pos, 4000, best, score) || std::abs(score) > 300) return;

        struct Rec {
            std::string fen;
            Value score;  // white POV
        };
        std::vector<Rec> recs;
        double result = 0.5;
        int winPlies = 0, drawPlies = 0;

        for (int ply = 0;; ++ply) {
            if (pos.is_draw(0) || pos.is_insufficient_material() || ply >= 400) break;
            if (!search(pos, cfg.nodes, best, score)) {
                // Checkmate or stalemate
                if (pos.checkers()) result = pos.side_to_move() == WHITE ? 0.0 : 1.0;
                break;
            }
            const Value whiteScore = pos.side_to_move() == WHITE ? score : -score;

            // Adjudication: a decisive score held for 6 plies, or a dead draw for 12 plies late in the game
            winPlies = std::abs(score) >= 1200 ? winPlies + 1 : 0;
            drawPlies = (std::abs(score) <= 8 && ply >= 80) ? drawPlies + 1 : 0;
            if (winPlies >= 6) {
                result = whiteScore > 0 ? 1.0 : 0.0;
                break;
            }
            if (drawPlies >= 12) break;

            // Keep quiet positions only: not in check, best move not tactical, no mate scores
            if (!pos.checkers() && !pos.capture_or_promotion(best) && !is_mate_score(score) &&
                std::abs(score) < 2500)
                recs.push_back({pos.fen(), whiteScore});

            states->emplace_back();
            pos.do_move(best, states->back());
        }

        const char* res = result == 1.0 ? "1.0" : result == 0.0 ? "0.0" : "0.5";
        for (const Rec& r : recs) lines.push_back(r.fen + " | " + std::to_string(r.score) + " | " + res);
    }

private:
    const DatagenConfig& cfg;
    std::mt19937_64 rng;
    TranspositionTable tt;
    Search::SharedState shared;
    std::unique_ptr<Search::Thread> thread;
};

}  // namespace

int run_datagen(int argc, char* argv[]) {
    DatagenConfig cfg;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto eq = a.find('=');
        if (eq == std::string::npos) continue;
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "out") cfg.out = v;
        else if (k == "openings") cfg.openings = v;
        else if (k == "games") cfg.games = std::stoi(v);
        else if (k == "threads") cfg.threads = std::max(1, std::stoi(v));
        else if (k == "nodes") cfg.nodes = std::stoull(v);
        else if (k == "random") cfg.randomPlies = std::stoi(v);
        else if (k == "seed") cfg.seed = std::stoull(v);
        else {
            std::cerr << "datagen: unknown option " << k << std::endl;
            return 1;
        }
    }
    const std::vector<std::string> openings = read_openings(cfg.openings);
    if (openings.empty()) {
        std::cerr << "datagen: no openings in " << cfg.openings << std::endl;
        return 1;
    }
    std::ofstream out(cfg.out, std::ios::app);
    if (!out) {
        std::cerr << "datagen: cannot open " << cfg.out << std::endl;
        return 1;
    }

    std::atomic<int> next{0}, done{0};
    std::atomic<uint64_t> positions{0};
    std::mutex outMutex;
    const TimePoint start = now_ms();

    auto work = [&](int id) {
        Worker w(cfg, cfg.seed * 1000003 + uint64_t(id));
        std::mt19937_64 pick(cfg.seed * 7919 + uint64_t(id));
        std::vector<std::string> lines;
        while (next.fetch_add(1) < cfg.games) {
            lines.clear();
            w.play(openings[std::uniform_int_distribution<size_t>(0, openings.size() - 1)(pick)], lines);
            {
                std::lock_guard<std::mutex> lk(outMutex);
                for (const std::string& l : lines) out << l << '\n';
                out.flush();
            }
            positions += lines.size();
            int d = ++done;
            if (d % 100 == 0) {
                std::lock_guard<std::mutex> lk(outMutex);
                std::cerr << "datagen: " << d << "/" << cfg.games << " games, " << positions.load() << " positions, "
                          << (now_ms() - start) / 1000 << "s" << std::endl;
            }
        }
    };
    std::vector<std::thread> pool;
    for (int i = 0; i < cfg.threads; ++i) pool.emplace_back(work, i);
    for (auto& t : pool) t.join();
    std::cerr << "datagen: finished " << done.load() << " games, " << positions.load() << " positions" << std::endl;
    return 0;
}
