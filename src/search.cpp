#include "search.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>

Search::ThreadPool Threads;

std::mutex& io_mutex() {
    static std::mutex m;
    return m;
}

void sync_print(const std::string& s) {
    std::lock_guard<std::mutex> lk(io_mutex());
    std::cout << s << std::endl;
}

namespace Search {

namespace {

enum NodeType { NonPV, PV, Root };

int Reductions[MAX_MOVES];

Depth reduction(bool improving, Depth d, int mn) {
    int r = Reductions[std::min(d, MAX_MOVES - 1)] * Reductions[std::min(mn, MAX_MOVES - 1)];
    return (r + 600) / 1024 + (!improving && r > 900);
}

constexpr int futility_move_count(bool improving, Depth depth) { return (3 + depth * depth) / (2 - improving); }

Value futility_margin(Depth d, bool improving) { return Value(80 * (d - improving)); }

int stat_bonus(Depth d) { return std::min(12 * d * d + 120 * d - 100, 2000); }

}  // namespace

void init() {
    Reductions[0] = 0;
    for (int i = 1; i < MAX_MOVES; ++i) Reductions[i] = int(21.0 * std::log(double(i)));
}

std::string score_to_uci(Value v) {
    std::ostringstream ss;
    if (std::abs(v) < VALUE_MATE_IN_MAX_PLY)
        ss << "cp " << v;
    else
        ss << "mate " << (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2;
    return ss.str();
}

uint64_t perft(Position& pos, Depth depth, bool root) {
    StateInfo st;
    uint64_t total = 0;
    MoveList<LEGAL> ml(pos);
    if (depth <= 1) {
        if (root)
            for (const ExtMove& m : ml) sync_print(move_to_uci(m.move) + ": 1");
        return ml.size();
    }
    for (const ExtMove& m : ml) {
        pos.do_move(m.move, st);
        uint64_t cnt = perft(pos, depth - 1, false);
        pos.undo_move(m.move);
        total += cnt;
        if (root) sync_print(move_to_uci(m.move) + ": " + std::to_string(cnt));
    }
    return total;
}

// ---------------------------------------------------------------------------
// Time management

void TimeManager::init(const LimitsType& limits, int us, int ply, int moveOverhead) {
    startTime = limits.startTime;
    optimumTime = maximumTime = 0;
    if (limits.movetime) {
        optimumTime = maximumTime = std::max<TimePoint>(1, limits.movetime - std::min<TimePoint>(moveOverhead, 10));
        return;
    }
    if (!limits.use_time_management()) return;

    const TimePoint time = limits.time[us];
    const TimePoint inc = limits.inc[us];
    const TimePoint safeTime = std::max<TimePoint>(1, time - moveOverhead);
    double opt;
    if (limits.movestogo) {
        int mtg = std::min(limits.movestogo, 50);
        opt = double(safeTime) / (mtg + 0.5) + 0.8 * double(inc);
        maximumTime = TimePoint(std::min(opt * 3.0, safeTime * (mtg == 1 ? 0.9 : 0.7)));
    } else {
        // Spend a little less early in the game, when the opening is cheap
        double movesLeft = std::max(18.0, 40.0 - ply / 3.0);
        opt = double(safeTime) / movesLeft + 0.8 * double(inc);
        maximumTime = TimePoint(std::min(opt * 4.5, safeTime * 0.75));
    }
    optimumTime = TimePoint(std::min(opt, safeTime * 0.5));
    optimumTime = std::max<TimePoint>(1, optimumTime);
    maximumTime = std::max(optimumTime, maximumTime);
}

// ---------------------------------------------------------------------------
// Threads

Thread::Thread(int index, SharedState* shared, ThreadPool* p, bool spawn)
    : idx(index), sh(shared), pool(p), evalCache(std::make_unique<Eval::Cache>()) {
    clear();
    if (spawn) {
        stdThread = std::thread(&Thread::idle_loop, this);
        wait_for_search_finished();
    } else
        searching = false;
}

Thread::~Thread() {
    if (stdThread.joinable()) {
        {
            std::lock_guard<std::mutex> lk(mutex);
            exitFlag = true;
            searching = true;
        }
        cv.notify_all();
        stdThread.join();
    }
}

void Thread::clear() {
    std::memset(mainHistory, 0, sizeof(mainHistory));
    std::memset(captureHistory, 0, sizeof(captureHistory));
    std::memset(counterMoves, 0, sizeof(counterMoves));
    for (auto& a : continuationHistory)
        for (auto& b : a) std::memset(b, 0, sizeof(b));
    evalCache->clear();
    previousScore = VALUE_INFINITE;
}

void Thread::start_searching() {
    {
        std::lock_guard<std::mutex> lk(mutex);
        searching = true;
    }
    cv.notify_all();
}

void Thread::wait_for_search_finished() {
    std::unique_lock<std::mutex> lk(mutex);
    cv.wait(lk, [&] { return !searching; });
}

void Thread::idle_loop() {
    while (true) {
        std::unique_lock<std::mutex> lk(mutex);
        searching = false;
        cv.notify_all();
        cv.wait(lk, [&] { return searching; });
        if (exitFlag) return;
        lk.unlock();
        if (is_main())
            search_main();
        else
            iterative_deepening();
    }
}

void Thread::setup(const Position& pos, const RootMoves& rms) {
    rootPos = pos;
    rootState = *pos.state();
    rootPos.set_state(&rootState);
    rootMoves = rms;
    nodes = 0;
    rootDepth = completedDepth = 0;
    selDepth = 0;
    nmpMinPly = 0;
    bestMoveChanges = 0;
}

void ThreadPool::set(size_t n) {
    if (!threads.empty()) {
        main()->wait_for_search_finished();
        threads.clear();
    }
    for (size_t i = 0; i < n; ++i) threads.push_back(std::make_unique<Thread>(int(i), &shared, this, true));
}

void ThreadPool::clear() {
    for (auto& th : threads) th->clear();
}

uint64_t ThreadPool::nodes_searched() const {
    uint64_t n = 0;
    for (auto& th : threads) n += th->node_count();
    return n;
}

void ThreadPool::wait_for_search_finished() { main()->wait_for_search_finished(); }

void ThreadPool::ponderhit() {
    shared.ponder = false;
    if (shared.stopOnPonderhit) shared.stop = true;
}

void ThreadPool::start_thinking(const Position& pos, StateListPtr& states, const LimitsType& limits, bool ponderMode) {
    main()->wait_for_search_finished();
    shared.stop = false;
    shared.stopOnPonderhit = false;
    shared.ponder = ponderMode;
    shared.limits = limits;
    shared.rootColor = pos.side_to_move();

    RootMoves rootMoves;
    for (const ExtMove& m : MoveList<LEGAL>(pos))
        if (limits.searchmoves.empty() ||
            std::find(limits.searchmoves.begin(), limits.searchmoves.end(), m.move) != limits.searchmoves.end())
            rootMoves.emplace_back(m.move);

    // The game history must outlive the search (repetition detection walks it)
    if (states) setupStates = std::move(states);

    for (auto& th : threads) th->setup(pos, rootMoves);
    main()->start_searching();
}

Thread* ThreadPool::best_thread() const {
    Thread* best = main();
    if (threads.size() == 1) return best;
    std::map<Move, int64_t> votes;
    Value minScore = VALUE_NONE;
    for (auto& th : threads)
        if (!th->rootMoves.empty()) minScore = std::min(minScore, th->rootMoves[0].score);
    for (auto& th : threads)
        if (!th->rootMoves.empty() && th->completedDepth > 0)
            votes[th->rootMoves[0].pv[0]] += int64_t(th->rootMoves[0].score - minScore + 14) * th->completedDepth;

    for (auto& th : threads) {
        Thread* t = th.get();
        if (t->rootMoves.empty() || t->completedDepth == 0) continue;
        const Value bs = best->rootMoves[0].score, ts = t->rootMoves[0].score;
        if (std::abs(bs) >= VALUE_MATE_IN_MAX_PLY) {
            if (ts > bs) best = t;  // shortest mate (or longest defence)
        } else if (ts >= VALUE_MATE_IN_MAX_PLY ||
                   (ts > VALUE_MATED_IN_MAX_PLY && votes[t->rootMoves[0].pv[0]] > votes[best->rootMoves[0].pv[0]]))
            best = t;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Main thread: drives the search, waits for helpers, reports the best move

void Thread::search_main() {
    SharedState& s = *sh;
    s.tm.init(s.limits, rootPos.side_to_move(), rootPos.game_ply(), s.moveOverhead);
    s.tt->new_search();

    if (rootMoves.empty()) {
        rootMoves.emplace_back(MOVE_NONE);
        if (!s.silent)
            sync_print(std::string("info depth 0 score ") + (rootPos.checkers() ? "mate 0" : "cp 0"));
    } else {
        if (pool)
            for (size_t i = 1; i < pool->size(); ++i) pool->at(i)->start_searching();
        iterative_deepening();
    }

    // In ponder or infinite mode we may not output bestmove before being told to
    while (!s.stop && (s.ponder || s.limits.infinite)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    s.stop = true;

    if (pool)
        for (size_t i = 1; i < pool->size(); ++i) pool->at(i)->wait_for_search_finished();

    Thread* best = this;
    if (pool && pool->size() > 1 && rootMoves[0].pv[0] != MOVE_NONE && !s.limits.depth) best = pool->best_thread();
    previousScore = best->rootMoves[0].score;

    if (s.silent) {
        if (best != this) rootMoves = best->rootMoves;
        return;
    }
    if (best != this) sync_print(best->pv_info(best->completedDepth));

    std::string out = "bestmove " + move_to_uci(best->rootMoves[0].pv[0]);
    Move ponderMove = MOVE_NONE;
    if (best->rootMoves[0].pv.size() > 1)
        ponderMove = best->rootMoves[0].pv[1];
    else if (best->rootMoves[0].pv[0] != MOVE_NONE) {
        // Try to find a ponder move in the transposition table
        StateInfo st;
        Position p = rootPos;
        StateInfo rs = *rootPos.state();
        p.set_state(&rs);
        Move bm = best->rootMoves[0].pv[0];
        p.do_move(bm, st);
        TTData td;
        if (s.tt->probe(p.key(), td) && td.move && MoveList<LEGAL>(p).contains(td.move)) ponderMove = td.move;
    }
    if (ponderMove != MOVE_NONE) out += " ponder " + move_to_uci(ponderMove);
    sync_print(out);
}

std::string Thread::pv_info(Depth depth) const {
    std::ostringstream ss;
    const TimePoint elapsed = std::max<TimePoint>(1, sh->tm.elapsed());
    const uint64_t nodesSearched = pool ? pool->nodes_searched() : node_count();
    const RootMove& rm = rootMoves[0];
    bool updated = rm.score != -VALUE_INFINITE;
    Depth d = updated ? depth : std::max(1, depth - 1);
    Value v = updated ? rm.uciScore : rm.previousScore;
    if (v == -VALUE_INFINITE) v = VALUE_ZERO;
    ss << "info depth " << std::max(1, d) << " seldepth " << rm.selDepth << " multipv 1 score " << score_to_uci(v);
    if (updated && rm.scoreLowerbound) ss << " lowerbound";
    if (updated && rm.scoreUpperbound) ss << " upperbound";
    ss << " nodes " << nodesSearched << " nps " << nodesSearched * 1000 / uint64_t(elapsed) << " hashfull "
       << sh->tt->hashfull() << " tbhits 0 time " << elapsed << " pv";
    for (Move m : rm.pv) ss << ' ' << move_to_uci(m);
    return ss.str();
}

void Thread::check_time() {
    if (--callsCnt > 0) return;
    const LimitsType& L = sh->limits;
    callsCnt = L.nodes ? int(std::min<uint64_t>(1024, L.nodes / 1024 + 1)) : 1024;
    if (sh->ponder) return;
    const TimePoint elapsed = sh->tm.elapsed();
    if (((L.use_time_management() || L.movetime) && elapsed >= sh->tm.maximum()) ||
        (L.nodes && (pool ? pool->nodes_searched() : node_count()) >= L.nodes))
        sh->stop = true;
}

// ---------------------------------------------------------------------------
// Iterative deepening

void Thread::iterative_deepening() {
    Stack stack[MAX_PLY + 10];
    Stack* ss = stack + 7;
    std::memset(static_cast<void*>(stack), 0, sizeof(stack));
    for (int i = 7; i > 0; --i) {
        (ss - i)->contHist = &continuationHistory[0][0][NO_PIECE][0];
        (ss - i)->staticEval = VALUE_NONE;
    }
    for (int i = 0; i <= MAX_PLY + 2; ++i) (ss + i)->ply = i;

    Value bestValue = -VALUE_INFINITE, alpha = -VALUE_INFINITE, beta = VALUE_INFINITE, delta = VALUE_INFINITE;
    Move lastBestMove = MOVE_NONE;
    int stableIterations = 0;
    double totBestMoveChanges = 0;

    if (is_main()) {
        Value init = previousScore == VALUE_INFINITE ? VALUE_ZERO : previousScore;
        for (Value& v : iterValue) v = init;
    }
    int iterIdx = 0;

    while (++rootDepth < MAX_PLY && !sh->stop && !(sh->limits.depth && is_main() && rootDepth > sh->limits.depth)) {
        if (is_main()) totBestMoveChanges /= 2;
        for (RootMove& rm : rootMoves) rm.previousScore = rm.score;
        pvIdx = 0;
        selDepth = 0;

        // Aspiration window around the previous score
        if (rootDepth >= 4) {
            Value prev = rootMoves[0].averageScore;
            delta = 14 + std::abs(prev) / 50;
            alpha = std::max(prev - delta, -VALUE_INFINITE);
            beta = std::min(prev + delta, VALUE_INFINITE);
        } else {
            alpha = -VALUE_INFINITE;
            beta = VALUE_INFINITE;
            delta = VALUE_INFINITE;
        }

        int failedHighCnt = 0;
        while (true) {
            Depth adjustedDepth = std::max(1, rootDepth - failedHighCnt);
            bestValue = search<Root>(rootPos, ss, alpha, beta, adjustedDepth, false);
            std::stable_sort(rootMoves.begin(), rootMoves.end());
            if (sh->stop) break;

            if (is_main() && !sh->silent && (bestValue <= alpha || bestValue >= beta) && sh->tm.elapsed() > 3000)
                sync_print(pv_info(rootDepth));

            if (bestValue <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = std::max(bestValue - delta, -VALUE_INFINITE);
                failedHighCnt = 0;
                if (is_main()) sh->stopOnPonderhit = false;
            } else if (bestValue >= beta) {
                beta = std::min(bestValue + delta, VALUE_INFINITE);
                ++failedHighCnt;
            } else
                break;
            delta += delta / 3 + 5;
        }

        if (!sh->stop) completedDepth = rootDepth;
        if (is_main() && !sh->silent && (!sh->stop || rootDepth == 1 || completedDepth == rootDepth - 1))
            sync_print(pv_info(rootDepth));

        if (rootMoves[0].pv[0] != lastBestMove) {
            lastBestMove = rootMoves[0].pv[0];
            stableIterations = 0;
        } else
            ++stableIterations;

        if (sh->limits.mate && bestValue >= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - bestValue <= 2 * sh->limits.mate)
            sh->stop = true;

        if (!is_main()) continue;

        totBestMoveChanges += bestMoveChanges;
        bestMoveChanges = 0;

        // Decide whether to start another iteration
        if (sh->limits.use_time_management() && !sh->stop && !sh->stopOnPonderhit && completedDepth >= 4) {
            const Value score = rootMoves[0].score;
            double fallingEval = (12 + 2 * (previousScore == VALUE_INFINITE ? 0 : previousScore - score) +
                                  (iterValue[iterIdx] - score)) / 100.0;
            fallingEval = std::clamp(fallingEval, 0.6, 1.6);
            static constexpr double stability[] = {1.6, 1.25, 1.0, 0.88, 0.8, 0.75};
            double stab = stability[std::min(stableIterations, 5)];
            double instability = 1.0 + totBestMoveChanges / 2.0;
            uint64_t total = std::max<uint64_t>(1, node_count());
            double effort = double(rootMoves[0].effort) / double(total);
            double effortFactor = std::clamp(1.5 - effort, 0.65, 1.3);
            double soft = sh->tm.optimum() * fallingEval * stab * std::min(instability, 2.0) * effortFactor;
            if (rootMoves.size() == 1) soft = std::min(soft, sh->tm.optimum() * 0.25);
            if (double(sh->tm.elapsed()) > soft) {
                if (sh->ponder)
                    sh->stopOnPonderhit = true;
                else
                    sh->stop = true;
            }
        }
        iterValue[iterIdx] = bestValue;
        iterIdx = (iterIdx + 1) & 3;
    }
}

// ---------------------------------------------------------------------------
// History helpers

void Thread::update_pv(int ply, Move m) {
    pvTable[ply][ply] = m;
    for (int i = ply + 1; i < pvLength[ply + 1]; ++i) pvTable[ply][i] = pvTable[ply + 1][i];
    pvLength[ply] = std::max(pvLength[ply + 1], ply + 1);
}

void Thread::update_continuation_histories(Stack* ss, int pc, int to, int bonus) {
    for (int i : {1, 2, 4, 6}) {
        if (ss->inCheck && i > 2) break;
        if (is_ok_move((ss - i)->currentMove)) update_history<CONT_HISTORY_MAX>((*(ss - i)->contHist)[pc][to], bonus);
    }
}

void Thread::update_quiet_stats(const Position& pos, Stack* ss, Move move, int bonus) {
    if (ss->killers[0] != move) {
        ss->killers[1] = ss->killers[0];
        ss->killers[0] = move;
    }
    const int us = pos.side_to_move();
    update_history<HISTORY_MAX>(mainHistory[us][from_to(move)], bonus);
    update_continuation_histories(ss, pos.moved_piece(move), to_sq(move), bonus);
    if (is_ok_move((ss - 1)->currentMove)) {
        const int prevSq = to_sq((ss - 1)->currentMove);
        counterMoves[pos.piece_on(prevSq)][prevSq] = move;
    }
}

void Thread::update_all_stats(const Position& pos, Stack* ss, Move bestMove, Value bestValue, Value beta, int prevSq,
                              Move* quiets, int quietCount, Move* captures, int captureCount, Depth depth) {
    const int us = pos.side_to_move();
    const int bonus1 = stat_bonus(depth + 1);

    if (!pos.capture_or_promotion(bestMove)) {
        const int bonus2 = bestValue > beta + PawnValue ? bonus1 : stat_bonus(depth);
        update_quiet_stats(pos, ss, bestMove, bonus2);
        for (int i = 0; i < quietCount; ++i) {
            update_history<HISTORY_MAX>(mainHistory[us][from_to(quiets[i])], -bonus2);
            update_continuation_histories(ss, pos.moved_piece(quiets[i]), to_sq(quiets[i]), -bonus2);
        }
    } else {
        const int captured = type_of_move(bestMove) == EN_PASSANT ? PAWN : type_of(pos.piece_on(to_sq(bestMove)));
        update_history<HISTORY_MAX>(captureHistory[pos.moved_piece(bestMove)][to_sq(bestMove)][captured], bonus1);
    }

    // Penalise the refuted early quiet move of the previous ply
    if (prevSq != SQ_NONE && ((ss - 1)->moveCount == 1 + (ss - 1)->ttHit || (ss - 1)->currentMove == (ss - 1)->killers[0]) &&
        !pos.captured_piece())
        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -bonus1);

    for (int i = 0; i < captureCount; ++i) {
        const Move m = captures[i];
        const int captured = type_of_move(m) == EN_PASSANT ? PAWN : type_of(pos.piece_on(to_sq(m)));
        update_history<HISTORY_MAX>(captureHistory[pos.moved_piece(m)][to_sq(m)][captured], -bonus1);
    }
}

// ---------------------------------------------------------------------------
// Principal variation search

template <int NT>
Value Thread::search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {
    constexpr bool PvNode = NT != NonPV;
    constexpr bool rootNode = NT == Root;

    if (depth <= 0) return qsearch<PvNode ? PV : NonPV>(pos, ss, alpha, beta);
    depth = std::min(depth, MAX_PLY - 1);

    const int ply = ss->ply;
    if (PvNode) {
        pvLength[ply] = ply;
        if (selDepth < ply + 1) selDepth = ply + 1;
    }
    inc_nodes();
    if (is_main()) check_time();

    const bool inCheck = pos.checkers();
    const int us = pos.side_to_move();
    StateInfo st;
    Move move;

    if (!rootNode) {
        if (sh->stop.load(std::memory_order_relaxed) || pos.is_draw(ply) || ply >= MAX_PLY)
            return (ply >= MAX_PLY && !inCheck) ? evaluate(pos) : value_draw(pos);
        // Mate distance pruning
        alpha = std::max(mated_in(ply), alpha);
        beta = std::min(mate_in(ply + 1), beta);
        if (alpha >= beta) return alpha;
    }

    ss->inCheck = inCheck;
    ss->moveCount = 0;
    (ss + 1)->excludedMove = MOVE_NONE;
    (ss + 2)->killers[0] = (ss + 2)->killers[1] = MOVE_NONE;
    (ss + 2)->cutoffCnt = 0;
    ss->doubleExtensions = (ss - 1)->doubleExtensions;
    ss->statScore = 0;
    const int prevSq = is_ok_move((ss - 1)->currentMove) ? to_sq((ss - 1)->currentMove) : SQ_NONE;

    // Transposition table lookup
    const Move excludedMove = ss->excludedMove;
    const Key posKey = pos.key();
    TTData tte{};
    const bool ttHit = !excludedMove && sh->tt->probe(posKey, tte);
    ss->ttHit = ttHit;
    const Value ttValue = ttHit ? value_from_tt(tte.value, ply, pos.rule50_count()) : VALUE_NONE;
    Move ttMove = rootNode ? rootMoves[pvIdx].pv[0] : ttHit ? tte.move : MOVE_NONE;
    if (!rootNode && ttMove && !pos.pseudo_legal(ttMove)) ttMove = MOVE_NONE;
    const bool ttCapture = ttMove && pos.capture_or_promotion(ttMove);
    if (!excludedMove) ss->ttPv = PvNode || (ttHit && tte.pv);

    // Cutoff on a sufficiently deep TT entry (non-PV nodes only)
    if (!PvNode && ttHit && tte.depth >= depth && ttValue != VALUE_NONE &&
        (ttValue >= beta ? (tte.bound & BOUND_LOWER) : (tte.bound & BOUND_UPPER))) {
        if (ttMove) {
            if (ttValue >= beta) {
                if (!ttCapture) update_quiet_stats(pos, ss, ttMove, stat_bonus(depth));
                if (prevSq != SQ_NONE && (ss - 1)->moveCount <= 2 && !pos.captured_piece())
                    update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + 1));
            } else if (!ttCapture) {
                const int penalty = -stat_bonus(depth);
                update_history<HISTORY_MAX>(mainHistory[us][from_to(ttMove)], penalty);
                update_continuation_histories(ss, pos.moved_piece(ttMove), to_sq(ttMove), penalty);
            }
        }
        if (pos.rule50_count() < 90) return ttValue;
    }

    Value eval = VALUE_NONE;
    bool improving = false;

    if (inCheck) {
        ss->staticEval = VALUE_NONE;
    } else {
        // Static evaluation, refined by the TT value when its bound allows
        if (excludedMove)
            eval = ss->staticEval;
        else if (ttHit) {
            ss->staticEval = eval = tte.eval != VALUE_NONE ? tte.eval : evaluate(pos);
            if (ttValue != VALUE_NONE && (tte.bound & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER))) eval = ttValue;
        } else {
            ss->staticEval = eval = evaluate(pos);
            sh->tt->store(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_NONE, MOVE_NONE, eval);
        }

        if ((ss - 2)->staticEval != VALUE_NONE)
            improving = ss->staticEval > (ss - 2)->staticEval;
        else if ((ss - 4)->staticEval != VALUE_NONE)
            improving = ss->staticEval > (ss - 4)->staticEval;
        else
            improving = true;

        // Razoring: hopeless positions at low depth go straight to quiescence
        if (!PvNode && depth <= 3 && eval + 250 + 220 * depth < alpha) {
            Value v = qsearch<NonPV>(pos, ss, alpha - 1, alpha);
            if (v < alpha) return v;
        }

        // Reverse futility pruning
        if (!ss->ttPv && depth < 9 && eval - futility_margin(depth, improving) >= beta && eval < VALUE_KNOWN_WIN &&
            !excludedMove)
            return eval;

        // Null move pruning, verified at high depth against zugzwang
        if (!PvNode && (ss - 1)->currentMove != MOVE_NULL && (ss - 1)->statScore < 23000 && eval >= beta &&
            eval >= ss->staticEval && ss->staticEval >= beta - 20 * depth + 120 && !excludedMove &&
            pos.non_pawn_material(us) && ply >= nmpMinPly && beta > VALUE_MATED_IN_MAX_PLY) {
            const Depth R = std::min(4 + depth / 4 + std::min(int(eval - beta) / 200, 3), depth);
            ss->currentMove = MOVE_NULL;
            ss->contHist = &continuationHistory[0][0][NO_PIECE][0];
            pos.do_null_move(st);
            Value nullValue = -search<NonPV>(pos, ss + 1, -beta, -beta + 1, depth - R, !cutNode);
            pos.undo_null_move();

            if (nullValue >= beta) {
                if (nullValue >= VALUE_MATE_IN_MAX_PLY) nullValue = beta;
                if (nmpMinPly || depth < 12) return nullValue;
                nmpMinPly = ply + 3 * (depth - R) / 4;
                Value v = search<NonPV>(pos, ss, beta - 1, beta, depth - R, false);
                nmpMinPly = 0;
                if (v >= beta) return nullValue;
            }
        }

        // ProbCut: a good capture that beats beta by a margin at reduced depth
        const Value probCutBeta = beta + 130 - 40 * improving;
        if (!PvNode && depth > 4 && !excludedMove && std::abs(beta) < VALUE_MATE_IN_MAX_PLY &&
            !(ttHit && tte.depth >= depth - 3 && ttValue != VALUE_NONE && ttValue < probCutBeta)) {
            MovePicker mp(pos, ttMove, probCutBeta - ss->staticEval, &captureHistory);
            while ((move = mp.next_move()) != MOVE_NONE) {
                if (!pos.legal(move)) continue;
                sh->tt->prefetch(pos.key_after(move));
                ss->currentMove = move;
                ss->contHist = &continuationHistory[0][1][pos.moved_piece(move)][to_sq(move)];
                pos.do_move(move, st);
                Value value = -qsearch<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1);
                if (value >= probCutBeta)
                    value = -search<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1, depth - 4, !cutNode);
                pos.undo_move(move);
                if (sh->stop.load(std::memory_order_relaxed)) return VALUE_ZERO;
                if (value >= probCutBeta) {
                    sh->tt->store(posKey, value_to_tt(value, ply), ss->ttPv, BOUND_LOWER, depth - 3, move, ss->staticEval);
                    return value;
                }
            }
        }
    }

    // Internal iterative reduction: without a TT move, search shallower
    if ((PvNode || cutNode) && depth >= 4 && !ttMove) depth -= 1;

    // ----- Move loop
    const PieceToHistory* contHist[] = {(ss - 1)->contHist, (ss - 2)->contHist, nullptr,
                                        (ss - 4)->contHist, nullptr,            (ss - 6)->contHist};
    const Move countermove = prevSq != SQ_NONE ? counterMoves[pos.piece_on(prevSq)][prevSq] : MOVE_NONE;
    MovePicker mp(pos, ttMove, depth, &mainHistory, &captureHistory, contHist, countermove, ss->killers);

    Value bestValue = -VALUE_INFINITE, value = -VALUE_INFINITE;
    Move bestMove = MOVE_NONE;
    Move quietsSearched[64], capturesSearched[32];
    int quietCount = 0, captureCount = 0, moveCount = 0;
    bool moveCountPruning = false, singularQuietLMR = false;

    while ((move = mp.next_move(moveCountPruning)) != MOVE_NONE) {
        if (move == excludedMove) continue;
        if (rootNode && !std::count(rootMoves.begin() + pvIdx, rootMoves.end(), move)) continue;
        if (!rootNode && !pos.legal(move)) continue;

        ss->moveCount = ++moveCount;
        if (rootNode && is_main() && !sh->silent && sh->tm.elapsed() > 3000)
            sync_print("info depth " + std::to_string(depth) + " currmove " + move_to_uci(move) + " currmovenumber " +
                       std::to_string(moveCount + pvIdx));

        const bool capture = pos.capture_or_promotion(move);
        const int movedPiece = pos.moved_piece(move);
        const int to = to_sq(move);
        const bool givesCheck = pos.gives_check(move);
        int extension = 0;
        Depth newDepth = depth - 1;

        // Shallow depth pruning
        if (!rootNode && pos.non_pawn_material(us) && bestValue > VALUE_MATED_IN_MAX_PLY) {
            moveCountPruning = moveCount >= futility_move_count(improving, depth);
            int lmrDepth = std::max(newDepth - reduction(improving, depth, moveCount), 0);

            if (capture || givesCheck) {
                if (!givesCheck && !inCheck && lmrDepth < 6 && type_of_move(move) == NORMAL) {
                    const int captured = type_of(pos.piece_on(to));
                    if (ss->staticEval + 200 + 180 * lmrDepth + PieceValue[captured] +
                            captureHistory[movedPiece][to][captured] / 8 <= alpha)
                        continue;
                }
                if (!pos.see_ge(move, Value(-100 * depth))) continue;
            } else {
                int history = (*contHist[0])[movedPiece][to] + (*contHist[1])[movedPiece][to] +
                              (*contHist[3])[movedPiece][to];
                // Continuation history pruning
                if (lmrDepth < 4 && history < -2500 * (depth - 1)) continue;
                history += 2 * mainHistory[us][from_to(move)];
                lmrDepth = std::max(0, lmrDepth + history / 8000);
                // Futility pruning for quiet moves
                if (!inCheck && lmrDepth < 8 && ss->staticEval + 120 + 130 * lmrDepth <= alpha) continue;
                // SEE pruning for quiet moves
                if (!pos.see_ge(move, Value(-(28 - std::min(lmrDepth, 16)) * lmrDepth * lmrDepth))) continue;
            }
        }

        // Singular extension: is the TT move much better than all alternatives?
        if (!rootNode && depth >= 7 && move == ttMove && !excludedMove && std::abs(ttValue) < VALUE_KNOWN_WIN &&
            (tte.bound & BOUND_LOWER) && tte.depth >= depth - 3) {
            const Value singularBeta = ttValue - 2 * depth;
            const Depth singularDepth = (depth - 1) / 2;
            ss->excludedMove = move;
            value = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
            ss->excludedMove = MOVE_NONE;
            ss->moveCount = moveCount;

            if (value < singularBeta) {
                extension = 1;
                singularQuietLMR = !ttCapture;
                if (!PvNode && value < singularBeta - 25 && ss->doubleExtensions <= 6) extension = 2;
            } else if (singularBeta >= beta)
                return singularBeta;  // multi-cut: several moves fail high
            else if (ttValue >= beta)
                extension = -1;
        } else if (givesCheck && depth > 2 && pos.see_ge(move, 0))
            extension = 1;  // check extension

        newDepth += extension;
        ss->doubleExtensions = (ss - 1)->doubleExtensions + (extension == 2);

        sh->tt->prefetch(pos.key_after(move));
        ss->currentMove = move;
        ss->contHist = &continuationHistory[inCheck][capture][movedPiece][to];
        const uint64_t nodesBefore = node_count();
        pos.do_move(move, st, givesCheck);

        bool doFullDepthSearch;
        bool didLMR = false;

        // Late move reductions
        if (depth >= 2 && moveCount > 1 + 2 * rootNode && (!capture || cutNode || !ss->ttPv)) {
            Depth r = reduction(improving, depth, moveCount);
            if (ss->ttPv) r -= 1 + (PvNode && !rootNode);
            if ((ss - 1)->moveCount > 13) r--;
            if (singularQuietLMR) r--;

            if (!capture) {
                if (ttCapture) r++;
                if (cutNode) r += 2;
                else if (type_of_move(move) == NORMAL && !pos.see_ge(make_move(to, from_sq(move))))
                    r -= 2;  // the move escapes a capture
                ss->statScore = 2 * mainHistory[us][from_to(move)] + (*contHist[0])[movedPiece][to] +
                                (*contHist[1])[movedPiece][to] + (*contHist[3])[movedPiece][to] - 4500;
                if (ss->statScore >= -100 && (ss - 1)->statScore < -110) r--;
                else if ((ss - 1)->statScore >= -100 && ss->statScore < -110) r++;
                r -= ss->statScore / 14000;
            } else if (depth < 8 && moveCount > 2)
                r++;

            const Depth d = std::clamp(newDepth - r, 1, std::max(newDepth, 1));
            value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, d, true);
            doFullDepthSearch = value > alpha && d < newDepth;
            didLMR = true;
        } else
            doFullDepthSearch = !PvNode || moveCount > 1;

        if (doFullDepthSearch) {
            value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);
            if (didLMR && !capture) {
                const int bonus = value > alpha ? stat_bonus(newDepth) : -stat_bonus(newDepth);
                update_continuation_histories(ss, movedPiece, to, bonus);
            }
        }

        if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta))))
            value = -search<PV>(pos, ss + 1, -beta, -alpha, newDepth, false);

        pos.undo_move(move);

        if (sh->stop.load(std::memory_order_relaxed)) return VALUE_ZERO;

        if (rootNode) {
            RootMove& rm = *std::find(rootMoves.begin(), rootMoves.end(), move);
            rm.effort += node_count() - nodesBefore;
            rm.averageScore = rm.averageScore != -VALUE_INFINITE ? (2 * value + rm.averageScore) / 3 : value;
            if (moveCount == 1 || value > alpha) {
                rm.score = rm.uciScore = value;
                rm.selDepth = selDepth;
                rm.scoreLowerbound = rm.scoreUpperbound = false;
                if (value >= beta) {
                    rm.scoreLowerbound = true;
                    rm.uciScore = beta;
                } else if (value <= alpha) {
                    rm.scoreUpperbound = true;
                    rm.uciScore = alpha;
                }
                rm.pv.resize(1);
                for (int i = ply + 1; i < pvLength[ply + 1]; ++i) rm.pv.push_back(pvTable[ply + 1][i]);
                if (moveCount > 1 && is_main()) ++bestMoveChanges;
            } else
                rm.score = -VALUE_INFINITE;
        }

        if (value > bestValue) {
            bestValue = value;
            if (value > alpha) {
                bestMove = move;
                if (PvNode && !rootNode) update_pv(ply, move);
                if (PvNode && value < beta)
                    alpha = value;
                else {
                    ss->cutoffCnt++;
                    break;
                }
            }
        }

        if (move != bestMove) {
            if (capture && captureCount < 32) capturesSearched[captureCount++] = move;
            else if (!capture && quietCount < 64) quietsSearched[quietCount++] = move;
        }
    }

    if (!moveCount)
        bestValue = excludedMove ? alpha : inCheck ? mated_in(ply) : VALUE_DRAW;
    else if (bestMove)
        update_all_stats(pos, ss, bestMove, bestValue, beta, prevSq, quietsSearched, quietCount, capturesSearched,
                         captureCount, depth);
    else if ((depth >= 3 || PvNode) && prevSq != SQ_NONE && !pos.captured_piece())
        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, stat_bonus(depth));  // prior move refuted us

    if (!excludedMove && !(rootNode && pvIdx))
        sh->tt->store(posKey, value_to_tt(bestValue, ply), ss->ttPv,
                      bestValue >= beta ? BOUND_LOWER : PvNode && bestMove ? BOUND_EXACT : BOUND_UPPER, depth, bestMove,
                      ss->staticEval);
    return bestValue;
}

// ---------------------------------------------------------------------------
// Quiescence search

template <int NT>
Value Thread::qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth) {
    constexpr bool PvNode = NT == PV;
    const int ply = ss->ply;
    if (PvNode) {
        pvLength[ply] = ply;
        if (selDepth < ply + 1) selDepth = ply + 1;
    }
    inc_nodes();
    if (is_main()) check_time();

    const bool inCheck = pos.checkers();
    ss->inCheck = inCheck;
    if (pos.is_draw(ply) || ply >= MAX_PLY) return (ply >= MAX_PLY && !inCheck) ? evaluate(pos) : value_draw(pos);

    const Value oldAlpha = alpha;
    // TT depth: positions where quiet checks were tried count as deeper
    const Depth ttDepth = inCheck || depth >= DEPTH_QS ? DEPTH_QS : DEPTH_QS_NO_CHECKS;
    const Key posKey = pos.key();
    TTData tte{};
    const bool ttHit = sh->tt->probe(posKey, tte);
    const Value ttValue = ttHit ? value_from_tt(tte.value, ply, pos.rule50_count()) : VALUE_NONE;
    const Move ttMove = ttHit ? tte.move : MOVE_NONE;
    const bool pvHit = ttHit && tte.pv;

    if (!PvNode && ttHit && tte.depth >= ttDepth && ttValue != VALUE_NONE &&
        (ttValue >= beta ? (tte.bound & BOUND_LOWER) : (tte.bound & BOUND_UPPER)))
        return ttValue;

    Value bestValue, futilityBase;
    if (inCheck) {
        ss->staticEval = VALUE_NONE;
        bestValue = futilityBase = -VALUE_INFINITE;
    } else {
        if (ttHit) {
            ss->staticEval = bestValue = tte.eval != VALUE_NONE ? tte.eval : evaluate(pos);
            if (ttValue != VALUE_NONE && (tte.bound & (ttValue > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttValue;
        } else
            ss->staticEval = bestValue = evaluate(pos);

        // Stand pat
        if (bestValue >= beta) {
            if (!ttHit)
                sh->tt->store(posKey, value_to_tt(bestValue, ply), false, BOUND_LOWER, DEPTH_NONE, MOVE_NONE,
                              ss->staticEval);
            return bestValue;
        }
        if (PvNode && bestValue > alpha) alpha = bestValue;
        futilityBase = bestValue + 120;
    }

    const PieceToHistory* contHist[] = {(ss - 1)->contHist, (ss - 2)->contHist, nullptr,
                                        (ss - 4)->contHist, nullptr,            (ss - 6)->contHist};
    MovePicker mp(pos, ttMove, ttDepth, &mainHistory, &captureHistory, contHist);

    StateInfo st;
    Move move, bestMove = MOVE_NONE;
    int moveCount = 0;

    while ((move = mp.next_move()) != MOVE_NONE) {
        if (!pos.legal(move)) continue;
        const bool givesCheck = pos.gives_check(move);
        const bool capture = pos.capture_or_promotion(move);
        moveCount++;

        if (bestValue > VALUE_MATED_IN_MAX_PLY) {
            // Futility (delta) pruning
            if (!givesCheck && futilityBase > -VALUE_KNOWN_WIN && type_of_move(move) != PROMOTION) {
                const int captured = type_of_move(move) == EN_PASSANT ? PAWN : type_of(pos.piece_on(to_sq(move)));
                const Value futilityValue = futilityBase + PieceValue[captured];
                if (futilityValue <= alpha) {
                    bestValue = std::max(bestValue, futilityValue);
                    continue;
                }
                if (futilityBase <= alpha && !pos.see_ge(move, 1)) {
                    bestValue = std::max(bestValue, futilityBase);
                    continue;
                }
            }
            // Losing captures are not worth searching
            if (!pos.see_ge(move, 0)) continue;
            // Once an evasion is found, quiet evasions late in the list can be skipped
            if (inCheck && !capture && moveCount > 3) continue;
        }

        sh->tt->prefetch(pos.key_after(move));
        ss->currentMove = move;
        ss->contHist = &continuationHistory[inCheck][capture][pos.moved_piece(move)][to_sq(move)];
        pos.do_move(move, st, givesCheck);
        const Value value = -qsearch<NT>(pos, ss + 1, -beta, -alpha, depth - 1);
        pos.undo_move(move);

        if (value > bestValue) {
            bestValue = value;
            if (value > alpha) {
                bestMove = move;
                if (PvNode) update_pv(ply, move);
                if (PvNode && value < beta)
                    alpha = value;
                else
                    break;
            }
        }
    }

    if (inCheck && bestValue == -VALUE_INFINITE) return mated_in(ply);

    sh->tt->store(posKey, value_to_tt(bestValue, ply), pvHit,
                  bestValue >= beta ? BOUND_LOWER : PvNode && bestValue > oldAlpha ? BOUND_EXACT : BOUND_UPPER,
                  ttDepth, bestMove, ss->staticEval);
    return bestValue;
}

}  // namespace Search
