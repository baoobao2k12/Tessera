# Tessera

Tessera is a UCI chess engine written in C++20. Its evaluation is entirely
handcrafted (no NNUE, no neural networks, no machine-learning inference); the
evaluation *weights* are fitted with Texel tuning on positions from the
engine's own self-play games, using tools that ship with the project.

## Building

Requirements: CMake 3.16+, a C++20 compiler (GCC 10+, Clang 12+, or MSVC 2019+),
Python 3 for the UCI and cross-check tests (optional).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/tessera            # UCI engine
```

Build options:

| Option | Values | Default |
|---|---|---|
| `TESSERA_ARCH` | `auto` (detect at configure time), `native`, `bmi2` (POPCNT+PEXT), `popcnt`, `generic` | `auto` |
| `TESSERA_LTO` | link-time optimisation in Release | `ON` |
| `TESSERA_PGO` | `off`, `generate`, `use` (see `scripts/build_pgo.sh`) | `off` |
| `TESSERA_SANITIZE` | e.g. `address,undefined` or `thread` | empty |

A binary built for `bmi2` or `popcnt` checks the CPU at startup and exits with
a clear message instead of crashing on an unsupported machine. Use
`-DTESSERA_ARCH=generic` for a portable build. On Windows, configure with the
Visual Studio generator (MSVC) or with Clang; the match runner
(`tessera_match`) is POSIX-only and is skipped there.

Profile-guided build: `scripts/build_pgo.sh` (produces `build-pgo/tessera`).

## UCI

Supported commands: `uci`, `isready`, `ucinewgame`, `position startpos|fen ... [moves ...]`,
`go` with `depth`, `nodes`, `movetime`, `wtime/btime/winc/binc/movestogo`, `infinite`,
`mate`, `ponder`, `searchmoves`, plus `stop`, `ponderhit`, `setoption`, `quit`.

Options: `Hash` (MB, 1-65536), `Threads` (1-512), `Clear Hash`, `Ponder`,
`Move Overhead` (ms), `TunedEval` (use the tuned weights; off = hand-written defaults).

Non-UCI debugging commands: `d` (show the board), `eval` (evaluation breakdown
by term), `flip`, `perft N` / `go perft N`, `bench [depth] [threads] [hash]`.

`info` lines report depth, seldepth, score (cp or mate), lower/upper bound
flags, nodes, nps, hashfull, time and the PV. `bestmove 0000` is printed only
when the side to move has no legal move.

## Contributors

- Tessera developers
- (add your collaborator here)

| File | Contents |
|---|---|
| `src/bitboard.*` | Bitboards, attack tables, magic bitboards with an optional PEXT path |
| `src/position.*` | Position, make/unmake with incremental Zobrist keys, PSQ score, pins/checks, SEE, repetition and draw rules |
| `src/movegen.*` | Pseudo-legal generators (captures, quiets, evasions) and a legal-move filter |
| `src/movepick.*` | Staged move picker and history tables |
| `src/search.*` | Iterative deepening, PVS, quiescence, Lazy SMP thread pool, time management |
| `src/tt.*` | Lock-free shared transposition table |
| `src/eval.*`, `src/evalparams.h`, `src/params*.cpp` | Tapered handcrafted evaluation; all weights in one tunable parameter table |
| `src/endgame.*` | KPK bitbase, specialised endgame evaluators and scale factors |
| `src/uci.*` | UCI protocol, bench |
| `tools/datagen.cpp`, `tools/tune.cpp` | Self-play data generation and Texel tuner (`tessera_tune`) |
| `tools/match.cpp` | Engine-vs-engine match runner with Elo, pentanomial error bars and SPRT (`tessera_match`) |
| `tests/` | Unit tests, perft suite, UCI protocol test, move-generator cross-check |
| `bench/` | Benchmark script and tactical test suite |

### Board and move generation

- Bitboards per piece type and colour; magic bitboards for sliders, with PEXT when built for BMI2.
- Hardware POPCNT/LSB when available, portable fallbacks otherwise.
- Pseudo-legal generation in stages for the search, with a fast legality test
  (only pinned pieces, king moves and en passant need the full check); a `LEGAL` generator for the root and perft.
- Castling, en passant (including the discovered-check case), all four promotions, check evasions and double check.
- Incremental Zobrist, pawn and material keys; threefold repetition (twofold inside the search tree),
  fifty-move rule, insufficient material, checkmate and stalemate.
- Standard chess only (no Chess960).

### Search

- Iterative deepening with aspiration windows (widening on fail-high/low, depth reduced on repeated fail-high).
- Negamax principal variation search; mate distance pruning; repetition and fifty-move detection.
- Transposition table: 64-byte buckets of four 16-byte entries, depth/bound/move/static eval/PV flag,
  generation-based ageing and a depth-minus-age replacement policy, mate scores stored relative to the node
  (and downgraded when they could not be delivered before the fifty-move rule).
- Quiescence search with stand-pat, delta pruning, SEE pruning, check evasions, and quiet checks at the first ply.
- Move ordering: TT move, good captures (MVV + capture history, SEE-filtered), killers, countermove,
  quiets by butterfly + continuation histories (1, 2, 4 and 6 plies back), then losing captures.
  Staged generation with partial insertion sort.
- Pruning: reverse futility, razoring, null move (adaptive reduction, verification search at high depth,
  disabled without non-pawn material), ProbCut, futility, late move (move-count) pruning,
  continuation-history pruning, SEE pruning of captures and quiets.
- Reductions/extensions: late move reductions driven by history, node type, TT-PV status and improvement;
  internal iterative reduction when there is no TT move; singular extensions with double extensions and
  multi-cut; check extensions.
- Time management: optimum/maximum budgets, scaled by best-move stability, score trend and the share of
  nodes spent on the best move; hard stop at the maximum.
- Lazy SMP: one shared TT, per-thread histories and stacks; the final move is chosen by a depth- and
  score-weighted vote across threads.

### Evaluation

Tapered between middlegame and endgame by a 24-point game phase. Every weight is a packed (mg, eg) score in
`EvalParams`, so the same code path serves the engine and the tuner.

- Material, bishop pair and simple imbalance terms; piece-square tables (mirrored, half-board for pieces).
- Pawns (cached in a pawn hash table): isolated, doubled, backward, weak unopposed, weak levers, blocked,
  connected/phalanx/supported chains, candidate passers, islands, majorities and minority attacks, weak squares.
- Passed pawns: rank, king distances, free/safe path, defended, blocked, file, connected and protected passers,
  rook behind the passer, unstoppable passers in pawn endings.
- Pieces: mobility tables per piece type over a safe mobility area, knight/bishop outposts and reachable outposts,
  minor behind pawn, king protectors, bad bishops (own pawns on its colour), long diagonal, trapped bishops and rooks,
  rooks on open/semi-open/closed files, on the queen file, on the 7th rank, connected rooks, weak/infiltrating queens.
- King safety: pawn shelter and storms (including castling targets), king on open files, king-pawn distance,
  pawnless flank, flank attacks, and a king-danger sum (attacker weights, safe and unsafe checks, weak ring squares,
  pinned blockers, defenders) turned into a quadratic penalty.
- Threats: by minors, rooks and king, hanging pieces, restricted pieces, safe pawn threats and pawn-push threats,
  knight/slider attacks on the queen.
- Space and tempo.
- Endgames: KPK bitbase (generated at startup), dedicated evaluators for KXK, KBNK, KPK, KRKP, KRKB, KRKN,
  KQKP, KQKR, scale factors for KBPsK, KPsK, KRPKR, KBPKB, KBPKN, KPKP, opposite-coloured bishops and other
  drawish material; an endgame "complexity" term; insufficient material is a draw; evaluation is damped as the
  fifty-move counter grows.

## Tuning workflow

All evaluation weights can be refitted with the bundled tools; no external data is required.

```sh
# 1. generate labelled positions from self-play (fixed nodes per move)
./build/tessera_tune datagen out=train.txt games=16000 threads=2 nodes=6000 random=6 seed=1 openings=data/openings.epd
# 2. check that the tuner's model reproduces the real evaluation
./build/tessera_tune check data=train.txt
# 3. tune (writes src/params_tuned.cpp), then rebuild
./build/tessera_tune tune data=train.txt epochs=800 lambda=0.7 threads=2
# 4. verify with a match against the previous binary
./build/tessera_match match -e1 build/tessera -e2 old/tessera -tc nodes=25000 -openings data/book.epd -pairs 200 -concurrency 2
```

`datagen` keeps quiet positions (not in check, best move not a capture or promotion, no mate score) and labels
them with the game result and the search score. The tuner records each position's parameter usage with
`Eval::evaluate_trace`, recomputes the evaluation for any weight vector with a model of the full formula
(including the quadratic king-danger transform, complexity, scale factor and phase), fits the sigmoid constant K,
and minimises the mean squared error against `lambda * result + (1 - lambda) * sigmoid(K * score)` with
full-batch Adam, keeping the weights with the best held-out (validation) error. Openings for data generation
(`data/openings.epd`) are separate from the match book (`data/book.epd`).

## Testing

```sh
cd build && ctest --output-on-failure
```

| Test | What it covers |
|---|---|
| `unit` | Make/unmake and incremental state, move properties, SEE, castling, en passant, promotions, check/mate/stalemate, repetition, fifty-move rule, insufficient material, Zobrist consistency, evaluation symmetry and incremental PSQ, TT packing/replacement/mate scores, search sanity, multithreaded search |
| `perft` | Reference perft counts (start position, Kiwipete and the other standard Chess Programming Wiki positions) |
| `uci` | Full UCI session: handshake, every `go` mode, stop/ponderhit, isready while searching, options, searchmoves, mates, no-move positions, repetition, garbage input, stdout hygiene, quit |
| `movegen_crosscheck` | Perft of random positions compared with an independent Python move generator (`tools/pychess_lite.py`) |

Sanitizer builds: `-DTESSERA_SANITIZE=address,undefined` and `-DTESSERA_SANITIZE=thread`.

Benchmarks: `bench/run_bench.sh [engine] [depth] [movetime]` runs the fixed-depth bench (its node count is a
deterministic signature of the search), perft speed and the tactical suite in `bench/tactics.epd`
(Win At Chess 1-10 and two short mates).

## Measured results

Development machine: 2-core Intel Xeon (cloud VM), Linux, GCC 13.3 (BMI2 build) and Clang 18.1.

**Correctness**

- `ctest`: 4/4 suites pass (unit: 4.2M checks; perft reference suite: 42 positions/depths; UCI protocol test;
  random-position cross-check against the Python move generator). Same result with Clang and
  `TESSERA_ARCH=generic`.
- AddressSanitizer + UndefinedBehaviorSanitizer: all test suites and a 1- and 4-thread bench, no reports.
- ThreadSanitizer: unit tests (including a 4-thread search), a 4-thread bench, a UCI session that changes
  the thread count and interrupts `go infinite`, and 2-thread data generation: no reports.
- `tessera_tune check`: the tuner's model matches the engine evaluation with a mean absolute difference of
  0.5 cp (integer rounding).

**Speed** (single thread, `bench` at depth 13)

| Build | Nodes | NPS |
|---|---|---|
| GCC, BMI2, LTO | 4,733,338 | ~2.19M |
| GCC, BMI2, LTO + PGO | 4,733,338 | ~2.24M |
| Clang, generic | identical node count to GCC (the search is deterministic) | |

Two threads reach ~2.07x the node rate of one thread from the start position (1.81M vs 3.75M nps,
5 s search). Perft 6 from the start position (bulk counting at the leaves): 119,060,324 nodes in 432 ms.

**Tactics**: 11/12 in `bench/tactics.epd` at 1 s per position (WAC.002, an exchange sacrifice for a pawn
breakthrough, is missed by the tuned evaluation; the untuned evaluation found it).

**Playing strength** (self-play with `tessera_match`, paired openings, 95% intervals)

| Change | Conditions | Games | Elo |
|---|---|---|---|
| Quiet checks at the first quiescence ply | 25k nodes/move | 400 | -3.5 [-30.8, +23.8] (neutral; kept because it finds short mates the old search missed) |
| Also letting quiet checks bypass move-count pruning (rejected) | 25k nodes/move | 400 | -34.9 [-63.2, -7.0] |
| Texel tuning, round 1 (824k positions) | 25k nodes/move | 400 | +146.2 [+115.3, +179.3] |
| Texel tuning, round 1 | 3s + 0.03s | 200 | +186.2 [+146.0, +231.7] |
| Texel tuning, round 2 (1.49M positions) | 3s + 0.03s | 300 | +24.4 [+2.1, +46.8] |
| **Final vs. the engine before this work** | 3s + 0.03s | 300 | **+155.5 [+123.0, +190.9]** |

## Known limitations

- No Chess960, no Syzygy tablebases, no MultiPV.
- All Elo figures above come from self-play against earlier Tessera versions; self-play exaggerates
  differences and says nothing about strength against other engines. No rating against other engines
  has been measured.
- The tuning data comes from short self-play games (6,000 nodes per move); deeper data would likely give
  better weights.
- The MSVC and Windows code paths are written but were not compiled or tested in the development
  environment (Linux, GCC 13 and Clang).
- `tessera_match` uses fork/pipes and is POSIX-only.
