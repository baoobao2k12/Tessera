// tessera_match - engine-vs-engine match runner and opening generator.
//
// Plays paired games (every opening once with each colour) between two UCI
// engines, adjudicates with Tessera's own rules code, and reports the score,
// an Elo estimate with a 95% interval from the pentanomial distribution of
// game pairs, and an optional SPRT log-likelihood ratio.
//
//   tessera_match match -e1 NEW -e2 OLD [-o1 Name=Value ...] [-o2 ...]
//                       -tc nodes=20000 | depth=8 | movetime=100 | 10+0.1
//                       -openings book.epd [-pairs N] [-concurrency C]
//                       [-sprt elo0 elo1 alpha beta] [-log games.txt] [-noadj]
//   tessera_match openings -n N -plies P -seed S -out book.epd
//
// POSIX only (fork/pipe); it is a development tool, not part of the engine.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bitboard.h"
#include "eval.h"
#include "movegen.h"
#include "position.h"
#include "search.h"
#include "tt.h"

namespace {

// ---------------------------------------------------------------------------
// UCI engine process

class Engine {
public:
    bool start(const std::string& path, const std::vector<std::string>& options) {
        path_ = path;
        options_ = options;
        int toChild[2], fromChild[2];
        if (pipe(toChild) || pipe(fromChild)) return false;
        pid_ = fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {
            dup2(toChild[0], STDIN_FILENO);
            dup2(fromChild[1], STDOUT_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, STDERR_FILENO);
            close(toChild[0]);
            close(toChild[1]);
            close(fromChild[0]);
            close(fromChild[1]);
            execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        close(toChild[0]);
        close(fromChild[1]);
        in_ = toChild[1];
        out_ = fromChild[0];
        buf_.clear();
        send("uci");
        std::string line;
        while (read_line(line, 10000))
            if (line.rfind("id name", 0) == 0)
                name_ = line.substr(8);
            else if (line == "uciok")
                break;
        for (const std::string& o : options) {
            size_t eq = o.find('=');
            if (eq == std::string::npos) continue;
            send("setoption name " + o.substr(0, eq) + " value " + o.substr(eq + 1));
        }
        return sync();
    }

    bool restart() {
        kill_process();
        return start(path_, options_);
    }

    bool sync() {
        send("isready");
        std::string line;
        while (read_line(line, 30000))
            if (line == "readyok") return true;
        return false;
    }

    void send(const std::string& s) {
        std::string msg = s + "\n";
        const char* p = msg.data();
        size_t left = msg.size();
        while (left) {
            ssize_t n = write(in_, p, left);
            if (n <= 0) return;
            p += n;
            left -= size_t(n);
        }
    }

    // Reads one line; timeoutMs < 0 waits forever. Returns false on timeout or EOF.
    bool read_line(std::string& line, int64_t timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max<int64_t>(0, timeoutMs));
        while (true) {
            size_t nl = buf_.find('\n');
            if (nl != std::string::npos) {
                line = buf_.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buf_.erase(0, nl + 1);
                return true;
            }
            int wait = -1;
            if (timeoutMs >= 0) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0) return false;
                wait = int(std::min<int64_t>(left, 1 << 30));
            }
            pollfd pfd{out_, POLLIN, 0};
            int r = poll(&pfd, 1, wait);
            if (r == 0) return false;
            if (r < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            char tmp[4096];
            ssize_t n = read(out_, tmp, sizeof(tmp));
            if (n <= 0) return false;
            buf_.append(tmp, size_t(n));
        }
    }

    void kill_process() {
        if (pid_ > 0) {
            send("quit");
            close(in_);
            close(out_);
            for (int i = 0; i < 50; ++i) {
                if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
                    pid_ = -1;
                    return;
                }
                usleep(2000);
            }
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
    }

    ~Engine() { kill_process(); }
    const std::string& name() const { return name_; }

private:
    std::string path_, name_;
    std::vector<std::string> options_;
    pid_t pid_ = -1;
    int in_ = -1, out_ = -1;
    std::string buf_;
};

// ---------------------------------------------------------------------------
// Time control

struct TimeControl {
    enum Kind { NODES, DEPTH, MOVETIME, CLOCK } kind = NODES;
    int64_t nodes = 20000, depth = 8, movetime = 100, base = 10000, inc = 100;

    static bool parse(const std::string& s, TimeControl& tc) {
        auto val = [&](const std::string& prefix) { return std::stoll(s.substr(prefix.size())); };
        try {
            if (s.rfind("nodes=", 0) == 0) tc.kind = NODES, tc.nodes = val("nodes=");
            else if (s.rfind("depth=", 0) == 0) tc.kind = DEPTH, tc.depth = val("depth=");
            else if (s.rfind("movetime=", 0) == 0) tc.kind = MOVETIME, tc.movetime = val("movetime=");
            else {
                size_t plus = s.find('+');
                tc.kind = CLOCK;
                tc.base = int64_t(std::stod(s.substr(0, plus)) * 1000);
                tc.inc = plus == std::string::npos ? 0 : int64_t(std::stod(s.substr(plus + 1)) * 1000);
            }
        } catch (...) {
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// One game

enum Result { WHITE_WIN, BLACK_WIN, DRAW, NO_RESULT };

struct GameRecord {
    Result result = NO_RESULT;
    std::string reason;
    std::vector<std::string> moves;
    int timeLosses[2] = {0, 0};   // indexed by engine slot
    int illegalMoves[2] = {0, 0};
    int crashes[2] = {0, 0};
};

struct MatchConfig {
    std::string path[2];
    std::vector<std::string> opts[2];
    TimeControl tc;
    std::vector<std::string> openings;
    int pairs = 0;
    int concurrency = 1;
    bool sprt = false;
    double elo0 = 0, elo1 = 5, alpha = 0.05, beta = 0.05;
    bool adjudicate = true;
    std::string logPath;
};

int parse_score(const std::string& line, bool& found) {
    std::istringstream is(line);
    std::string tok;
    while (is >> tok)
        if (tok == "score") {
            std::string kind;
            int v = 0;
            is >> kind >> v;
            found = true;
            if (kind == "mate") return v > 0 ? 30000 - v : -30000 - v;
            return v;
        }
    return 0;
}

// Plays one game; slot[c] is the engine index playing colour c.
GameRecord play_game(Engine* eng[2], const int slot[2], const std::string& fen, const MatchConfig& cfg) {
    GameRecord g;
    StateListPtr states(new std::deque<StateInfo>(1));
    Position pos;
    pos.set(fen, &states->back());

    for (int i = 0; i < 2; ++i) {
        eng[i]->send("ucinewgame");
        if (!eng[i]->sync()) {
            eng[i]->restart();
        }
    }

    int64_t clock[2] = {cfg.tc.base, cfg.tc.base};
    int lastScore[2] = {0, 0};  // white point of view, per colour
    bool haveScore[2] = {false, false};
    int winStreak = 0, drawStreak = 0;
    std::string movesStr;

    auto finish = [&](Result r, const std::string& why) {
        g.result = r;
        g.reason = why;
    };

    for (int ply = 0;; ++ply) {
        // Terminal positions
        MoveList<LEGAL> legal(pos);
        if (legal.size() == 0) {
            if (pos.checkers()) finish(pos.side_to_move() == WHITE ? BLACK_WIN : WHITE_WIN, "checkmate");
            else finish(DRAW, "stalemate");
            break;
        }
        if (pos.rule50_count() >= 100) { finish(DRAW, "fifty-move rule"); break; }
        if (pos.state()->repetition < 0) { finish(DRAW, "threefold repetition"); break; }
        if (pos.is_insufficient_material()) { finish(DRAW, "insufficient material"); break; }
        if (ply >= 600) { finish(DRAW, "game too long"); break; }

        const int us = pos.side_to_move();
        Engine* e = eng[slot[us]];
        e->send("position fen " + fen + (movesStr.empty() ? "" : " moves" + movesStr));

        std::string go;
        int64_t timeout = -1;
        switch (cfg.tc.kind) {
        case TimeControl::NODES: go = "go nodes " + std::to_string(cfg.tc.nodes); timeout = 60000; break;
        case TimeControl::DEPTH: go = "go depth " + std::to_string(cfg.tc.depth); timeout = 600000; break;
        case TimeControl::MOVETIME: go = "go movetime " + std::to_string(cfg.tc.movetime); timeout = cfg.tc.movetime + 5000; break;
        case TimeControl::CLOCK:
            go = "go wtime " + std::to_string(std::max<int64_t>(1, clock[WHITE])) + " btime " +
                 std::to_string(std::max<int64_t>(1, clock[BLACK])) + " winc " + std::to_string(cfg.tc.inc) +
                 " binc " + std::to_string(cfg.tc.inc);
            timeout = clock[us] + 2000;
            break;
        }
        const auto t0 = std::chrono::steady_clock::now();
        e->send(go);

        std::string line, best;
        int score = 0;
        bool scored = false;
        bool ok = false;
        while (e->read_line(line, timeout)) {
            if (line.rfind("info", 0) == 0) {
                bool f = false;
                int s = parse_score(line, f);
                if (f && line.find(" pv ") != std::string::npos && line.find("bound") == std::string::npos) {
                    score = s;
                    scored = true;
                }
            } else if (line.rfind("bestmove", 0) == 0) {
                std::istringstream is(line);
                std::string tok;
                is >> tok >> best;
                ok = true;
                break;
            }
        }
        const int64_t used =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

        if (!ok) {
            // Timeout or crash: the side to move loses
            if (cfg.tc.kind == TimeControl::CLOCK && used >= clock[us]) {
                g.timeLosses[slot[us]]++;
                finish(us == WHITE ? BLACK_WIN : WHITE_WIN, "time forfeit (no reply)");
            } else {
                g.crashes[slot[us]]++;
                finish(us == WHITE ? BLACK_WIN : WHITE_WIN, "engine crashed or hung");
            }
            e->restart();
            break;
        }
        if (cfg.tc.kind == TimeControl::CLOCK) {
            clock[us] -= used;
            if (clock[us] < 0) {
                g.timeLosses[slot[us]]++;
                finish(us == WHITE ? BLACK_WIN : WHITE_WIN, "time forfeit");
                break;
            }
            clock[us] += cfg.tc.inc;
        }

        Move m = uci_to_move(pos, best);
        if (m == MOVE_NONE || !legal.contains(m)) {
            g.illegalMoves[slot[us]]++;
            finish(us == WHITE ? BLACK_WIN : WHITE_WIN, "illegal move " + best);
            break;
        }
        g.moves.push_back(best);
        movesStr += " " + best;
        states->emplace_back();
        pos.do_move(m, states->back());

        // Adjudication on the engines' own scores (white point of view)
        if (scored) {
            lastScore[us] = us == WHITE ? score : -score;
            haveScore[us] = true;
        }
        if (cfg.adjudicate && haveScore[WHITE] && haveScore[BLACK]) {
            const int a = lastScore[WHITE], b = lastScore[BLACK];
            if ((a >= 1000 && b >= 1000) || (a <= -1000 && b <= -1000)) {
                if (++winStreak >= 8) {
                    finish(a > 0 ? WHITE_WIN : BLACK_WIN, "adjudication: score");
                    break;
                }
            } else
                winStreak = 0;
            if (ply >= 80 && std::abs(a) <= 10 && std::abs(b) <= 10) {
                if (++drawStreak >= 10) {
                    finish(DRAW, "adjudication: draw");
                    break;
                }
            } else
                drawStreak = 0;
        }
    }
    return g;
}

// ---------------------------------------------------------------------------
// Statistics

struct Stats {
    int wins = 0, losses = 0, draws = 0;  // from engine 1's point of view
    int penta[5] = {0, 0, 0, 0, 0};        // pair scores 0, 0.5, 1, 1.5, 2
    int timeLosses[2] = {0, 0}, illegal[2] = {0, 0}, crashes[2] = {0, 0};

    int games() const { return wins + losses + draws; }
    int pairs() const { return penta[0] + penta[1] + penta[2] + penta[3] + penta[4]; }

    // Mean and variance of the per-pair score (0..1)
    void pair_moments(double& mean, double& var) const {
        const int n = pairs();
        mean = var = 0;
        if (!n) return;
        for (int i = 0; i < 5; ++i) mean += penta[i] * (i / 4.0);
        mean /= n;
        for (int i = 0; i < 5; ++i) var += penta[i] * (i / 4.0 - mean) * (i / 4.0 - mean);
        var /= n;
    }

    static double elo_of(double s) {
        s = std::clamp(s, 1e-6, 1 - 1e-6);
        return -400.0 * std::log10(1.0 / s - 1.0);
    }
    static double score_of(double elo) { return 1.0 / (1.0 + std::pow(10.0, -elo / 400.0)); }

    // Generalised SPRT on pairs (normal approximation, logistic Elo bounds)
    double llr(double elo0, double elo1) const {
        double m, v;
        pair_moments(m, v);
        if (pairs() < 2 || v <= 0) return 0;
        const double s0 = score_of(elo0), s1 = score_of(elo1);
        return pairs() * (s1 - s0) * (2 * m - s0 - s1) / (2 * v);
    }

    std::string summary(const MatchConfig& cfg) const {
        std::ostringstream ss;
        double m, v;
        pair_moments(m, v);
        const double score = games() ? (wins + 0.5 * draws) / games() : 0.5;
        ss << std::fixed << std::setprecision(1);
        ss << "Games " << games() << " (+" << wins << " -" << losses << " =" << draws << ") score "
           << std::setprecision(3) << score;
        if (pairs() >= 2 && v > 0) {
            const double se = std::sqrt(v / pairs());
            const double elo = elo_of(m), lo = elo_of(m - 1.96 * se), hi = elo_of(m + 1.96 * se);
            ss << std::setprecision(1) << "  Elo " << elo << " [" << lo << ", " << hi << "]";
            const double los = 0.5 * (1 + std::erf((m - 0.5) / (se * std::sqrt(2.0))));
            ss << " LOS " << std::setprecision(1) << 100 * los << "%";
        }
        ss << "  penta [" << penta[0] << ' ' << penta[1] << ' ' << penta[2] << ' ' << penta[3] << ' ' << penta[4]
           << "]";
        if (cfg.sprt) {
            const double lo = std::log(cfg.beta / (1 - cfg.alpha)), hi = std::log((1 - cfg.beta) / cfg.alpha);
            ss << std::setprecision(2) << "  LLR " << llr(cfg.elo0, cfg.elo1) << " (" << lo << ", " << hi << ")";
        }
        if (timeLosses[0] + timeLosses[1] + illegal[0] + illegal[1] + crashes[0] + crashes[1])
            ss << "  [time losses " << timeLosses[0] << "/" << timeLosses[1] << ", illegal " << illegal[0] << "/"
               << illegal[1] << ", crashes " << crashes[0] << "/" << crashes[1] << "]";
        return ss.str();
    }
};

int run_match(const MatchConfig& cfg) {
    Stats stats;
    std::mutex mtx;
    std::atomic<int> nextPair{0};
    std::atomic<bool> done{false};
    std::ofstream log;
    if (!cfg.logPath.empty()) log.open(cfg.logPath);
    const double lowerBound = std::log(cfg.beta / (1 - cfg.alpha)), upperBound = std::log((1 - cfg.beta) / cfg.alpha);
    std::string engineNames[2];

    auto worker = [&](int id) {
        Engine e1, e2;
        if (!e1.start(cfg.path[0], cfg.opts[0]) || !e2.start(cfg.path[1], cfg.opts[1])) {
            std::lock_guard<std::mutex> lk(mtx);
            std::cerr << "worker " << id << ": failed to start engines" << std::endl;
            done = true;
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx);
            engineNames[0] = e1.name();
            engineNames[1] = e2.name();
        }
        Engine* eng[2] = {&e1, &e2};
        while (!done) {
            const int p = nextPair++;
            if (p >= cfg.pairs) break;
            const std::string& fen = cfg.openings[size_t(p) % cfg.openings.size()];
            double pairScore = 0;
            GameRecord recs[2];
            for (int k = 0; k < 2; ++k) {
                // k == 0: engine 1 has white
                const int slot[2] = {k, 1 - k};
                recs[k] = play_game(eng, slot, fen, cfg);
                const Result r = recs[k].result;
                if (r == DRAW) pairScore += 0.5;
                else if ((r == WHITE_WIN) == (k == 0)) pairScore += 1;
            }
            std::lock_guard<std::mutex> lk(mtx);
            for (int k = 0; k < 2; ++k) {
                const Result r = recs[k].result;
                if (r == DRAW) stats.draws++;
                else if ((r == WHITE_WIN) == (k == 0)) stats.wins++;
                else stats.losses++;
                for (int s = 0; s < 2; ++s) {
                    stats.timeLosses[s] += recs[k].timeLosses[s];
                    stats.illegal[s] += recs[k].illegalMoves[s];
                    stats.crashes[s] += recs[k].crashes[s];
                }
                if (log.is_open()) {
                    log << "game " << 2 * p + k + 1 << " white=" << (k == 0 ? "e1" : "e2") << " result="
                        << (r == WHITE_WIN ? "1-0" : r == BLACK_WIN ? "0-1" : "1/2-1/2") << " (" << recs[k].reason
                        << ") fen=\"" << fen << "\" moves=";
                    for (const auto& mv : recs[k].moves) log << mv << ' ';
                    log << '\n';
                    log.flush();
                }
            }
            stats.penta[int(pairScore * 2 + 0.01)]++;
            std::cout << stats.summary(cfg) << std::endl;
            if (cfg.sprt && stats.pairs() >= 10) {
                const double l = stats.llr(cfg.elo0, cfg.elo1);
                if (l >= upperBound || l <= lowerBound) done = true;
            }
        }
    };

    std::vector<std::thread> workers;
    for (int i = 0; i < cfg.concurrency; ++i) workers.emplace_back(worker, i);
    for (auto& w : workers) w.join();

    std::cout << "\nFinal: " << engineNames[0] << " (e1) vs " << engineNames[1] << " (e2)\n" << stats.summary(cfg) << std::endl;
    if (cfg.sprt) {
        const double l = stats.llr(cfg.elo0, cfg.elo1);
        std::cout << "SPRT [" << cfg.elo0 << ", " << cfg.elo1 << "]: "
                  << (l >= upperBound ? "H1 accepted (e1 is stronger)"
                      : l <= lowerBound ? "H0 accepted (no improvement)"
                                        : "inconclusive")
                  << std::endl;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Opening generation with Tessera's own search

struct Searcher {
    Search::SharedState sh;
    std::unique_ptr<Search::Thread> th;
    Searcher() {
        sh.silent = true;
        th = std::make_unique<Search::Thread>(0, &sh, nullptr, false);
    }
    // Score of the position for the side to move after a depth-limited search.
    Value search(const Position& pos, int depth, Move* best = nullptr) {
        Search::RootMoves rms;
        for (const ExtMove& m : MoveList<LEGAL>(pos)) rms.emplace_back(m.move);
        sh.limits = Search::LimitsType();
        sh.limits.depth = depth;
        sh.limits.startTime = now_ms();
        sh.stop = false;
        th->setup(pos, rms);
        th->search_main();
        if (best) *best = th->best_move();
        return th->rootMoves[0].score;
    }
};

int gen_openings(int n, int plies, uint64_t seed, const std::string& out) {
    std::mt19937_64 rng(seed);
    Searcher s;
    std::ofstream f(out);
    int made = 0, tries = 0;
    while (made < n) {
        ++tries;
        StateListPtr states(new std::deque<StateInfo>(1));
        Position pos;
        pos.set("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", &states->back());
        bool okLine = true;
        for (int ply = 0; ply < plies && okLine; ++ply) {
            // Score every move with a shallow search and pick randomly among the near-best ones
            std::vector<std::pair<Move, Value>> scored;
            Value bestV = -VALUE_INFINITE;
            for (const ExtMove& m : MoveList<LEGAL>(pos)) {
                StateInfo st;
                pos.do_move(m.move, st);
                Value v = MoveList<LEGAL>(pos).size() ? -s.search(pos, 3) : VALUE_DRAW;
                pos.undo_move(m.move);
                scored.push_back({m.move, v});
                bestV = std::max(bestV, v);
            }
            std::vector<Move> cands;
            const int margin = ply < 2 ? 60 : 35;
            for (auto& [m, v] : scored)
                if (v >= bestV - margin) cands.push_back(m);
            if (cands.empty()) { okLine = false; break; }
            Move m = cands[rng() % cands.size()];
            states->emplace_back();
            pos.do_move(m, states->back());
        }
        if (!okLine || !MoveList<LEGAL>(pos).size()) continue;
        TT.clear();
        const Value v = s.search(pos, 9);
        if (std::abs(v) > 50) continue;
        f << pos.fen() << '\n';
        ++made;
        if (made % 50 == 0) std::cerr << made << "/" << n << " openings (" << tries << " tries)" << std::endl;
    }
    return 0;
}

std::vector<std::string> load_openings(const std::string& path) {
    std::vector<std::string> v;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        // EPD lines carry 4 fields; FEN lines carry 6. Keep the first 6 tokens at most.
        std::istringstream is(line);
        std::vector<std::string> tok;
        std::string t;
        while (is >> t && tok.size() < 6) {
            if (t.find(';') != std::string::npos) break;
            tok.push_back(t);
        }
        if (tok.size() < 4) continue;
        if (tok.size() == 4 || tok[4].find_first_not_of("0123456789") != std::string::npos) {
            tok.resize(4);
            tok.push_back("0");
            tok.push_back("1");
        }
        std::string fen = tok[0] + ' ' + tok[1] + ' ' + tok[2] + ' ' + tok[3] + ' ' + tok[4] + ' ' + tok[5];
        StateInfo st;
        Position p;
        if (p.set(fen, &st)) v.push_back(fen);
    }
    return v;
}

int usage() {
    std::cerr << "usage:\n"
                 "  tessera_match match -e1 NEW -e2 OLD -tc nodes=N|depth=D|movetime=MS|BASE+INC -openings FILE\n"
                 "                      [-pairs N] [-concurrency C] [-o1 Name=Value]... [-o2 Name=Value]...\n"
                 "                      [-sprt elo0 elo1 alpha beta] [-log FILE] [-noadj]\n"
                 "  tessera_match openings -n N -plies P -seed S -out FILE\n";
    return 2;
}

}  // namespace

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2) return usage();
    const std::string mode = argv[1];
    std::vector<std::string> a(argv + 2, argv + argc);

    if (mode == "openings") {
        int n = 100, plies = 8;
        uint64_t seed = 1;
        std::string out = "openings.epd";
        for (size_t i = 0; i + 1 < a.size(); i += 2) {
            if (a[i] == "-n") n = std::stoi(a[i + 1]);
            else if (a[i] == "-plies") plies = std::stoi(a[i + 1]);
            else if (a[i] == "-seed") seed = std::stoull(a[i + 1]);
            else if (a[i] == "-out") out = a[i + 1];
            else return usage();
        }
        Bitboards::init();
        Position::init();
        Eval::init();
        Search::init();
        TT.resize(16);
        return gen_openings(n, plies, seed, out);
    }

    if (mode != "match") return usage();
    MatchConfig cfg;
    std::string openingsPath;
    for (size_t i = 0; i < a.size(); ++i) {
        auto next = [&]() -> std::string {
            if (i + 1 >= a.size()) {
                std::cerr << "missing value after " << a[i] << std::endl;
                std::exit(2);
            }
            return a[++i];
        };
        if (a[i] == "-e1") cfg.path[0] = next();
        else if (a[i] == "-e2") cfg.path[1] = next();
        else if (a[i] == "-o1") cfg.opts[0].push_back(next());
        else if (a[i] == "-o2") cfg.opts[1].push_back(next());
        else if (a[i] == "-tc") {
            if (!TimeControl::parse(next(), cfg.tc)) return usage();
        } else if (a[i] == "-openings") openingsPath = next();
        else if (a[i] == "-pairs") cfg.pairs = std::stoi(next());
        else if (a[i] == "-concurrency") cfg.concurrency = std::max(1, std::stoi(next()));
        else if (a[i] == "-log") cfg.logPath = next();
        else if (a[i] == "-noadj") cfg.adjudicate = false;
        else if (a[i] == "-sprt") {
            cfg.sprt = true;
            cfg.elo0 = std::stod(next());
            cfg.elo1 = std::stod(next());
            cfg.alpha = std::stod(next());
            cfg.beta = std::stod(next());
        } else
            return usage();
    }
    if (cfg.path[0].empty() || cfg.path[1].empty() || openingsPath.empty()) return usage();

    Bitboards::init();
    Position::init();
    cfg.openings = load_openings(openingsPath);
    if (cfg.openings.empty()) {
        std::cerr << "no valid openings in " << openingsPath << std::endl;
        return 1;
    }
    if (cfg.pairs <= 0) cfg.pairs = int(cfg.openings.size());
    std::cout << "Match: " << cfg.pairs << " game pairs, " << cfg.openings.size() << " openings, concurrency "
              << cfg.concurrency << std::endl;
    return run_match(cfg);
}
