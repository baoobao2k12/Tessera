#!/usr/bin/env python3
"""Cross-checks the engine's move generator against the independent Python
implementation in tools/pychess_lite.py on random positions.

usage: crosscheck.py <path to tessera_tests> [games] [depth]
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import pychess_lite as pc  # noqa: E402

REFERENCE = [
    ("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 3, 8902),
    ("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 2, 2039),
    ("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 3, 2812),
    ("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 3, 9467),
    ("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 2, 1486),
]


def main():
    exe = sys.argv[1]
    games = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    depth = int(sys.argv[3]) if len(sys.argv) > 3 else 2

    # The Python implementation must itself agree with published perft numbers
    for fen, d, n in REFERENCE:
        got = pc.perft(pc.Board(fen), d)
        if got != n:
            print("python reference mismatch on %s: %d != %d" % (fen, got, n))
            return 1

    fens = subprocess.run([exe, "playout", str(games), "4242"], capture_output=True, text=True,
                          check=True).stdout.split("\n")
    fens = [f for f in fens if f.strip()]
    mismatches = 0
    out = subprocess.run([exe, "perftbatch", str(depth)], input="\n".join(fens) + "\n", capture_output=True,
                         text=True, check=True).stdout.split()
    if len(out) != len(fens):
        print("engine returned %d results for %d positions" % (len(out), len(fens)))
        return 1
    for fen, res in zip(fens, out):
        b = pc.Board(fen)
        py = pc.perft(b, depth) if b.legal_moves_raw() else 0
        cpp = int(res)
        if py != cpp:
            mismatches += 1
            print("MISMATCH depth %d: %s python=%d engine=%d" % (depth, fen, py, cpp))
    print("crosscheck: %d positions, depth %d, %d mismatches" % (len(fens), depth, mismatches))
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
