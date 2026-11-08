// Alpha-beta search, Lazy SMP thread pool and time management.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "eval.h"
#include "movepick.h"
#include "position.h"
#include "tt.h"

inline TimePoint now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

namespace Search {

struct LimitsType {
    std::vector<Move> searchmoves;
    TimePoint time[COLOR_NB] = {0, 0};
    TimePoint inc[COLOR_NB] = {0, 0};
    TimePoint movetime = 0;
    TimePoint startTime = 0;
    int movestogo = 0;
    int depth = 0;
    int mate = 0;
    uint64_t nodes = 0;
    bool infinite = false;
    bool perft = false;
    bool use_time_management() const { return time[WHITE] || time[BLACK]; }
};

class TimeManager {
public:
    void init(const LimitsType& limits, int us, int ply, int moveOverhead);
    TimePoint optimum() const { return optimumTime; }
    TimePoint maximum() const { return maximumTime; }
    TimePoint elapsed() const { return now_ms() - startTime; }

private:
    TimePoint startTime = 0;
    TimePoint optimumTime = 0;
    TimePoint maximumTime = 0;
};

struct Stack {
    PieceToHistory* contHist;
    int ply;
    Move currentMove;
    Move excludedMove;
    Move killers[2];
    Value staticEval;
    int statScore;
    int moveCount;
    bool inCheck;
    bool ttPv;
    bool ttHit;
    int doubleExtensions;
    int cutoffCnt;
};

struct RootMove {
    explicit RootMove(Move m) : pv(1, m) {}
    bool operator==(Move m) const { return pv[0] == m; }
    // Sort in descending order of score, then previous score
    bool operator<(const RootMove& m) const {
        return m.score != score ? m.score < score : m.previousScore < previousScore;
    }
    Value score = -VALUE_INFINITE;
    Value previousScore = -VALUE_INFINITE;
    Value averageScore = -VALUE_INFINITE;
    Value uciScore = -VALUE_INFINITE;
    bool scoreLowerbound = false;
    bool scoreUpperbound = false;
    int selDepth = 0;
    uint64_t effort = 0;
    std::vector<Move> pv;
};

using RootMoves = std::vector<RootMove>;

// State shared by all threads taking part in one search.
struct SharedState {
    LimitsType limits;
    std::atomic<bool> stop{false};
    std::atomic<bool> ponder{false};
    std::atomic<bool> stopOnPonderhit{false};
    TranspositionTable* tt = &TT;
    TimeManager tm;
    int moveOverhead = 30;
    int contempt = 0;
    int rootColor = WHITE;
    bool silent = false;
};

class ThreadPool;

class Thread {
public:
    // spawn = false creates a searcher driven synchronously by the caller (datagen, tests)
    Thread(int index, SharedState* shared, ThreadPool* pool, bool spawn = true);
    ~Thread();
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    void start_searching();
    void wait_for_search_finished();
    void clear();

    // Prepare the root position and root moves for a new search.
    void setup(const Position& pos, const RootMoves& rms);
    // Runs a complete search on the calling thread (main thread logic).
    void search_main();
    void iterative_deepening();

    bool is_main() const { return idx == 0; }
    uint64_t node_count() const { return nodes.load(std::memory_order_relaxed); }
    Move best_move() const { return rootMoves.empty() ? MOVE_NONE : rootMoves[0].pv[0]; }
    Value best_score() const { return rootMoves.empty() ? VALUE_NONE : rootMoves[0].score; }

    int idx;
    SharedState* sh;
    ThreadPool* pool;
    Position rootPos;
    StateInfo rootState;
    RootMoves rootMoves;
    std::atomic<uint64_t> nodes{0};
    Depth rootDepth = 0;
    Depth completedDepth = 0;
    int selDepth = 0;
    int pvIdx = 0;
    int nmpMinPly = 0;
    double bestMoveChanges = 0;
    Value previousScore = VALUE_INFINITE;  // main thread: score of the previous move's search
    Value iterValue[4] = {};

    ButterflyHistory mainHistory;
    CaptureHistory captureHistory;
    ContinuationHistory continuationHistory[2][2];
    CounterMoveTable counterMoves;
    std::unique_ptr<Eval::Cache> evalCache;

private:
    void idle_loop();
    void check_time();
    template <int NT> Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode);
    template <int NT> Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth = DEPTH_QS);
    Value evaluate(const Position& pos) { return Eval::evaluate(pos, *evalCache); }
    Value value_draw(const Position& pos) const {
        int c = sh->contempt;
        Value v = (pos.side_to_move() == sh->rootColor) ? Value(-c) : Value(c);
        return VALUE_DRAW + v - 1 + Value(nodes.load(std::memory_order_relaxed) & 2);
    }
    void update_pv(int ply, Move m);
    void update_quiet_stats(const Position& pos, Stack* ss, Move move, int bonus);
    void update_continuation_histories(Stack* ss, int pc, int to, int bonus);
    void update_all_stats(const Position& pos, Stack* ss, Move bestMove, Value bestValue, Value beta, int prevSq,
                          Move* quiets, int quietCount, Move* captures, int captureCount, Depth depth);
    std::string pv_info(Depth depth) const;
    void inc_nodes() { nodes.fetch_add(1, std::memory_order_relaxed); }

    int callsCnt = 0;
    Move pvTable[MAX_PLY + 2][MAX_PLY + 2];
    int pvLength[MAX_PLY + 2];

    std::mutex mutex;
    std::condition_variable cv;
    bool exitFlag = false;
    bool searching = true;
    std::thread stdThread;
};

class ThreadPool {
public:
    ~ThreadPool() { set(0); }
    void set(size_t n);
    void clear();
    void start_thinking(const Position& pos, StateListPtr& states, const LimitsType& limits, bool ponderMode);
    void stop() { shared.stop = true; }
    void ponderhit();
    void wait_for_search_finished();
    uint64_t nodes_searched() const;
    Thread* main() const { return threads.front().get(); }
    Thread* best_thread() const;
    size_t size() const { return threads.size(); }
    Thread* at(size_t i) const { return threads[i].get(); }

    SharedState shared;

private:
    std::vector<std::unique_ptr<Thread>> threads;
    StateListPtr setupStates;
};

void init();
std::string score_to_uci(Value v);
uint64_t perft(Position& pos, Depth depth, bool root = true);

}  // namespace Search

extern Search::ThreadPool Threads;

// Thread-safe console output
std::mutex& io_mutex();
void sync_print(const std::string& s);
