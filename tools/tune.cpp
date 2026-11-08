// tessera_tune - training data generation and Texel tuning of the evaluation.
//
//   tessera_tune datagen out=FILE games=N threads=T nodes=N random=P seed=S openings=FILE
//   tessera_tune check  data=FILE [limit=N]
//   tessera_tune tune   data=FILE [epochs=N] [lr=X] [lambda=X] [reg=X] [threads=T] [limit=N]
//                       [val=FRACTION] [out=src/params_tuned.cpp]
//
// The tuner works on EvalTrace: for every position the evaluation records how
// often each parameter contributed, so the evaluation can be recomputed for any
// parameter vector without running the engine. The model reproduces the full
// evaluation formula, including the quadratic king-danger transform, the
// complexity adjustment, the scale factor and the game phase. `check` verifies
// that the model matches the real evaluation before any tuning is trusted.
//
// Loss: mean squared error between sigmoid(K * eval) and a target that blends
// the game result with the search score (weight `lambda` on the result).
// K is fitted to the starting parameters. Optimiser: full-batch Adam.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bitboard.h"
#include "eval.h"
#include "evalparams.h"
#include "position.h"
#include "search.h"
#include "tt.h"

int run_datagen(int argc, char* argv[]);

namespace {

struct Coef {
    uint16_t slot;
    int16_t coef;
};

struct TunePos {
    uint32_t lin0, lin1;          // linear coefficients (white minus black)
    uint32_t kd0[2], kd1[2];      // king danger coefficients per defending king
    uint32_t cx0, cx1;            // complexity coefficients
    float kdConst[2];
    float phase;                  // 0..24
    float scale;                  // 0..64
    float r50;                    // fifty-move damping factor
    float result;                 // game result, white POV
    float score;                  // search score, white POV (centipawns)
    float target;                 // blended target, filled in after K is known
};

struct Dataset {
    std::vector<TunePos> pos;
    std::vector<Coef> coefs;
};

struct Args {
    std::string data, out = "src/params_tuned.cpp";
    int epochs = 400, threads = 2;
    size_t limit = 0;
    double lr = 1.0, lambda = 0.7, reg = 0.0, val = 0.05;
};

bool parse_args(int argc, char* argv[], Args& a) {
    for (int i = 0; i < argc; ++i) {
        std::string s = argv[i];
        auto eq = s.find('=');
        if (eq == std::string::npos) return false;
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        if (k == "data") a.data = v;
        else if (k == "out") a.out = v;
        else if (k == "epochs") a.epochs = std::stoi(v);
        else if (k == "threads") a.threads = std::max(1, std::stoi(v));
        else if (k == "limit") a.limit = std::stoull(v);
        else if (k == "lr") a.lr = std::stod(v);
        else if (k == "lambda") a.lambda = std::stod(v);
        else if (k == "reg") a.reg = std::stod(v);
        else if (k == "val") a.val = std::stod(v);
        else {
            std::cerr << "unknown option " << k << std::endl;
            return false;
        }
    }
    return true;
}

// Reads "<fen> | <score> | <result>" lines and records each position's trace.
bool load(const Args& a, Dataset& ds, size_t& skipped) {
    std::ifstream in(a.data);
    if (!in) {
        std::cerr << "cannot open " << a.data << std::endl;
        return false;
    }
    auto tr = std::make_unique<EvalTrace>();
    std::string line;
    skipped = 0;
    StateInfo st;
    Position pos;
    while (std::getline(in, line)) {
        if (a.limit && ds.pos.size() >= a.limit) break;
        auto p1 = line.find('|'), p2 = line.rfind('|');
        if (p1 == std::string::npos || p2 == p1) continue;
        std::string fen = line.substr(0, p1);
        if (!pos.set(fen, &st) || pos.checkers()) {
            ++skipped;
            continue;
        }
        Eval::evaluate_trace(pos, *tr);
        if (tr->special) {  // specialised endgame code ignores the parameters
            ++skipped;
            continue;
        }
        TunePos tp{};
        tp.score = std::stof(line.substr(p1 + 1, p2 - p1 - 1));
        tp.result = std::stof(line.substr(p2 + 1));
        tp.phase = float(tr->phase);
        tp.scale = float(tr->scale);
        tp.r50 = float(200 - std::min(pos.rule50_count(), 100)) / 200.0f;
        tp.lin0 = uint32_t(ds.coefs.size());
        for (int s = 0; s < NUM_PARAM_SLOTS; ++s) {
            int c = tr->lin[s][WHITE] - tr->lin[s][BLACK];
            if (c) ds.coefs.push_back({uint16_t(s), int16_t(c)});
        }
        tp.lin1 = uint32_t(ds.coefs.size());
        for (int c = 0; c < 2; ++c) {
            tp.kd0[c] = uint32_t(ds.coefs.size());
            for (int s = 0; s < NUM_PARAM_SLOTS; ++s)
                if (tr->kd[c][s]) ds.coefs.push_back({uint16_t(s), int16_t(tr->kd[c][s])});
            tp.kd1[c] = uint32_t(ds.coefs.size());
            tp.kdConst[c] = float(tr->kdConst[c]);
        }
        tp.cx0 = uint32_t(ds.coefs.size());
        for (int s = 0; s < NUM_PARAM_SLOTS; ++s)
            if (tr->cx[s]) ds.coefs.push_back({uint16_t(s), int16_t(tr->cx[s])});
        tp.cx1 = uint32_t(ds.coefs.size());
        ds.pos.push_back(tp);
    }
    return true;
}

// Evaluation model: white POV, before fifty-move damping. Fills the partial
// derivatives needed for the gradient when `grad` is true.
struct ModelOut {
    double v;
    double dmg, deg;   // dv/dmg, dv/deg (eg before complexity)
    double dcx;        // dv/dcomplexity
    double dkd[2];     // dv/dkd for each king (0 when inactive)
};

inline ModelOut model(const TunePos& t, const Coef* coefs, const double* mgp, const double* egp) {
    double mg = 0, eg = 0;
    for (uint32_t i = t.lin0; i < t.lin1; ++i) {
        mg += coefs[i].coef * mgp[coefs[i].slot];
        eg += coefs[i].coef * egp[coefs[i].slot];
    }
    ModelOut o{};
    double kdMg[2] = {0, 0}, kdEg[2] = {0, 0};
    for (int c = 0; c < 2; ++c) {
        double kd = t.kdConst[c];
        for (uint32_t i = t.kd0[c]; i < t.kd1[c]; ++i) kd += coefs[i].coef * mgp[coefs[i].slot];
        if (kd > 100) {
            const double sign = c == WHITE ? -1.0 : 1.0;
            const double k = std::min(kd, 15000.0);
            mg += sign * k * k / KD_MG_DIV;
            eg += sign * k / KD_EG_DIV;
            if (kd < 15000) {
                kdMg[c] = sign * 2.0 * k / KD_MG_DIV;
                kdEg[c] = sign / KD_EG_DIV;
            }
        }
    }
    double cx = 0;
    for (uint32_t i = t.cx0; i < t.cx1; ++i) cx += coefs[i].coef * mgp[coefs[i].slot];
    const double s = eg > 0 ? 1.0 : eg < 0 ? -1.0 : 0.0;
    double eg2 = eg, degFactor = 1.0, dcxFactor = 0.0;
    if (s != 0) {
        if (cx > -std::abs(eg)) {
            eg2 = eg + s * cx;
            dcxFactor = s;
        } else {
            eg2 = 0;
            degFactor = 0;
        }
    }
    const double wMg = t.phase / 24.0, wEg = (24.0 - t.phase) * t.scale / (64.0 * 24.0);
    o.v = (mg * wMg + eg2 * wEg) * t.r50;
    o.dmg = wMg * t.r50;
    o.deg = wEg * degFactor * t.r50;
    o.dcx = wEg * dcxFactor * t.r50;
    for (int c = 0; c < 2; ++c) o.dkd[c] = o.dmg * kdMg[c] + o.deg * kdEg[c];
    return o;
}

inline double sigmoid(double k, double v) { return 1.0 / (1.0 + std::exp(-k * v)); }

struct Tuner {
    Dataset& ds;
    const Args& a;
    std::vector<size_t> train, valid;
    std::vector<double> mg, eg;  // parameters, indexed by slot

    Tuner(Dataset& d, const Args& args) : ds(d), a(args), mg(NUM_PARAM_SLOTS), eg(NUM_PARAM_SLOTS) {
        const Score* p = reinterpret_cast<const Score*>(&P);
        for (int s = 0; s < NUM_PARAM_SLOTS; ++s) {
            mg[s] = mg_value(p[s]);
            eg[s] = eg_value(p[s]);
        }
        std::vector<size_t> idx(ds.pos.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::mt19937_64 rng(12345);
        std::shuffle(idx.begin(), idx.end(), rng);
        const size_t nv = size_t(double(idx.size()) * a.val);
        valid.assign(idx.begin(), idx.begin() + nv);
        train.assign(idx.begin() + nv, idx.end());
    }

    template <typename F>
    void parallel(const std::vector<size_t>& set, F f) {
        std::vector<std::thread> th;
        const size_t n = set.size(), chunk = (n + a.threads - 1) / a.threads;
        for (int t = 0; t < a.threads; ++t)
            th.emplace_back([&, t] {
                const size_t b = std::min(n, t * chunk), e = std::min(n, b + chunk);
                f(t, b, e);
            });
        for (auto& x : th) x.join();
    }

    // Error against the game result (used to fit K) or against the blended target.
    double error(const std::vector<size_t>& set, double k, bool vsResult) {
        std::vector<double> part(a.threads, 0.0);
        parallel(set, [&](int t, size_t b, size_t e) {
            double sum = 0;
            for (size_t i = b; i < e; ++i) {
                const TunePos& tp = ds.pos[set[i]];
                const double d = sigmoid(k, model(tp, ds.coefs.data(), mg.data(), eg.data()).v) -
                                 (vsResult ? tp.result : tp.target);
                sum += d * d;
            }
            part[t] = sum;
        });
        double s = 0;
        for (double x : part) s += x;
        return s / double(std::max<size_t>(1, set.size()));
    }

    double fit_k() {
        double lo = 0.0005, hi = 0.03;
        for (int it = 0; it < 60; ++it) {
            const double m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
            if (error(train, m1, true) < error(train, m2, true)) hi = m2;
            else lo = m1;
        }
        return (lo + hi) / 2;
    }

    void gradient(double k, std::vector<double>& gmg, std::vector<double>& geg) {
        std::vector<std::vector<double>> pm(a.threads, std::vector<double>(NUM_PARAM_SLOTS, 0.0)),
            pe(a.threads, std::vector<double>(NUM_PARAM_SLOTS, 0.0));
        parallel(train, [&](int t, size_t b, size_t e) {
            double* gm = pm[t].data();
            double* ge = pe[t].data();
            const Coef* cf = ds.coefs.data();
            for (size_t i = b; i < e; ++i) {
                const TunePos& tp = ds.pos[train[i]];
                const ModelOut o = model(tp, cf, mg.data(), eg.data());
                const double sg = sigmoid(k, o.v);
                const double dE = 2.0 * (sg - tp.target) * sg * (1.0 - sg) * k;
                const double am = dE * o.dmg, ae = dE * o.deg;
                for (uint32_t j = tp.lin0; j < tp.lin1; ++j) {
                    gm[cf[j].slot] += am * cf[j].coef;
                    ge[cf[j].slot] += ae * cf[j].coef;
                }
                for (int c = 0; c < 2; ++c)
                    if (o.dkd[c] != 0)
                        for (uint32_t j = tp.kd0[c]; j < tp.kd1[c]; ++j) gm[cf[j].slot] += dE * o.dkd[c] * cf[j].coef;
                if (o.dcx != 0)
                    for (uint32_t j = tp.cx0; j < tp.cx1; ++j) gm[cf[j].slot] += dE * o.dcx * cf[j].coef;
            }
        });
        const double inv = 1.0 / double(std::max<size_t>(1, train.size()));
        for (int s = 0; s < NUM_PARAM_SLOTS; ++s) {
            double m = 0, e = 0;
            for (int t = 0; t < a.threads; ++t) {
                m += pm[t][s];
                e += pe[t][s];
            }
            gmg[s] = m * inv;
            geg[s] = e * inv;
        }
    }
};

void init_engine() {
    Bitboards::init();
    Position::init();
    Eval::init();
    Search::init();
}

int run_check(int argc, char* argv[]) {
    Args a;
    a.limit = 200000;
    if (!parse_args(argc, argv, a) || a.data.empty()) return 2;
    Dataset ds;
    size_t skipped;
    if (!load(a, ds, skipped)) return 1;
    // Recompute the trace eval for the same positions and compare with the model.
    std::ifstream in(a.data);
    std::string line;
    auto tr = std::make_unique<EvalTrace>();
    std::vector<double> mg(NUM_PARAM_SLOTS), eg(NUM_PARAM_SLOTS);
    const Score* p = reinterpret_cast<const Score*>(&P);
    for (int s = 0; s < NUM_PARAM_SLOTS; ++s) mg[s] = mg_value(p[s]), eg[s] = eg_value(p[s]);
    size_t i = 0, n = 0, over2 = 0;
    double sumAbs = 0, maxAbs = 0;
    StateInfo st;
    Position pos;
    while (i < ds.pos.size() && std::getline(in, line)) {
        std::string fen = line.substr(0, line.find('|'));
        if (!pos.set(fen, &st) || pos.checkers()) continue;
        Eval::evaluate_trace(pos, *tr);
        if (tr->special) continue;
        TunePos tp = ds.pos[i++];
        tp.r50 = 1.0f;  // trace eval is before fifty-move damping
        const double d = std::abs(model(tp, ds.coefs.data(), mg.data(), eg.data()).v - tr->eval);
        sumAbs += d;
        maxAbs = std::max(maxAbs, d);
        over2 += d > 2.0;
        ++n;
    }
    std::cout << "positions " << n << " skipped " << skipped << "\nmodel vs evaluation: mean |diff| " << sumAbs / double(n)
              << " cp, max " << maxAbs << " cp, over 2cp: " << over2 << std::endl;
    // The integer evaluation rounds at each stage (~1cp). Rarely the endgame score
    // sits at zero after rounding, which flips the sign used by the complexity term;
    // allow a tiny fraction of such positions but no large systematic mismatch.
    return (maxAbs <= 50.0 && double(over2) <= 0.001 * double(n)) ? 0 : 1;
}

void write_params(const Args& a, const Tuner& t) {
    EvalParams def;
    set_default_params(def);
    const Score* d = reinterpret_cast<const Score*>(&def);
    std::ofstream out(a.out);
    out << "// Tuned parameter overrides, generated by `tessera_tune tune`.\n"
           "// Values not listed here keep the hand-written defaults from params.cpp.\n"
           "#include \"evalparams.h\"\n\n"
           "void apply_tuned_params(EvalParams& p) {\n";
    int changed = 0;
    for (const ParamInfo& pi : param_table())
        for (int i = 0; i < pi.count; ++i) {
            const int s = pi.offset + i;
            const int m = int(std::lround(t.mg[s]));
            const int e = pi.kind == PK_LINEAR ? int(std::lround(t.eg[s])) : eg_value(d[s]);
            if (S(m, e) == d[s]) continue;
            out << "    p." << pi.name << "[" << i << "] = S(" << m << ", " << e << ");\n";
            ++changed;
        }
    if (!changed) out << "    (void)p;\n";
    out << "}\n";
    std::cerr << "wrote " << a.out << " (" << changed << " values differ from the defaults)" << std::endl;
}

int run_tune(int argc, char* argv[]) {
    Args a;
    if (!parse_args(argc, argv, a) || a.data.empty()) return 2;
    Dataset ds;
    size_t skipped;
    const TimePoint t0 = now_ms();
    if (!load(a, ds, skipped)) return 1;
    std::cerr << "loaded " << ds.pos.size() << " positions (" << skipped << " skipped), " << ds.coefs.size()
              << " coefficients, " << (now_ms() - t0) / 1000 << "s" << std::endl;
    if (ds.pos.size() < 1000) {
        std::cerr << "not enough positions" << std::endl;
        return 1;
    }
    Tuner t(ds, a);
    const double k = t.fit_k();
    for (TunePos& tp : ds.pos) tp.target = float(a.lambda * tp.result + (1 - a.lambda) * sigmoid(k, tp.score));
    std::cerr << std::setprecision(6) << "K = " << k << "  start: train " << t.error(t.train, k, false) << " valid "
              << t.error(t.valid, k, false) << std::endl;

    const std::vector<double> mg0 = t.mg, eg0 = t.eg;
    std::vector<double> gm(NUM_PARAM_SLOTS), ge(NUM_PARAM_SLOTS), m1(2 * NUM_PARAM_SLOTS, 0.0),
        m2(2 * NUM_PARAM_SLOTS, 0.0);
    std::vector<double> bestMg = t.mg, bestEg = t.eg;
    double bestValid = t.error(t.valid, k, false);
    const double b1 = 0.9, b2 = 0.999, epsAdam = 1e-8;
    for (int ep = 1; ep <= a.epochs; ++ep) {
        t.gradient(k, gm, ge);
        for (int s = 0; s < NUM_PARAM_SLOTS; ++s) {
            const bool linear = param_kind_of_slot(s) == PK_LINEAR;
            double g[2] = {gm[s] + a.reg * (t.mg[s] - mg0[s]), linear ? ge[s] + a.reg * (t.eg[s] - eg0[s]) : 0.0};
            double* val[2] = {&t.mg[s], &t.eg[s]};
            for (int h = 0; h < 2; ++h) {
                const int j = 2 * s + h;
                m1[j] = b1 * m1[j] + (1 - b1) * g[h];
                m2[j] = b2 * m2[j] + (1 - b2) * g[h] * g[h];
                const double mh = m1[j] / (1 - std::pow(b1, ep)), vh = m2[j] / (1 - std::pow(b2, ep));
                *val[h] -= a.lr * mh / (std::sqrt(vh) + epsAdam);
            }
        }
        if (ep % 25 == 0 || ep == a.epochs) {
            const double ev = t.error(t.valid, k, false);
            std::cerr << "epoch " << ep << "  train " << t.error(t.train, k, false) << "  valid " << ev
                      << (ev < bestValid ? "  *" : "") << "  " << (now_ms() - t0) / 1000 << "s" << std::endl;
            if (ev < bestValid) {
                bestValid = ev;
                bestMg = t.mg;
                bestEg = t.eg;
            }
        }
    }
    // Keep the parameters with the best held-out error (early stopping)
    t.mg = bestMg;
    t.eg = bestEg;
    write_params(a, t);
    return 0;
}

int usage() {
    std::cerr << "usage:\n"
                 "  tessera_tune datagen out=FILE games=N threads=T nodes=N random=P seed=S openings=FILE\n"
                 "  tessera_tune check data=FILE [limit=N]\n"
                 "  tessera_tune tune data=FILE [epochs=N] [lr=X] [lambda=X] [reg=X] [threads=T] [limit=N] [val=F]"
                 " [out=FILE]\n";
    return 2;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) return usage();
    init_engine();
    const std::string mode = argv[1];
    int rc;
    if (mode == "datagen") rc = run_datagen(argc - 2, argv + 2);
    else if (mode == "check") rc = run_check(argc - 2, argv + 2);
    else if (mode == "tune") rc = run_tune(argc - 2, argv + 2);
    else rc = usage();
    return rc == 2 ? usage() : rc;
}
