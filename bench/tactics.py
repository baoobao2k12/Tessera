#!/usr/bin/env python3
"""Runs a UCI engine over an EPD test suite with 'bm' (best move) operations
and reports how many positions it solves.

usage: tactics.py <engine> [epd=bench/tactics.epd] [movetime_ms=1000] [threads=1]

Best moves are given in SAN; they are converted to UCI with the move
generator in tools/pychess_lite.py, so no external chess library is needed.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import pychess_lite as pc  # noqa: E402

FILES = "abcdefgh"


def piece_at(board, sq):
    return board.b[pc.parse_sq(sq)].lower()


def san_matches(board, uci, san):
    """True if `uci` is written `san` in this position."""
    san = san.rstrip("+#!?")
    if san in ("O-O", "0-0"):
        return uci in ("e1g1", "e8g8") and piece_at(board, uci[:2]) == "k"
    if san in ("O-O-O", "0-0-0"):
        return uci in ("e1c1", "e8c8") and piece_at(board, uci[:2]) == "k"
    promo = ""
    if "=" in san:
        san, promo = san.split("=")
        promo = promo.lower()
    piece = san[0] if san[0] in "KQRBN" else "P"
    dest = san[-2:]
    if uci[2:4] != dest or (promo and uci[4:] != promo):
        return False
    if piece_at(board, uci[:2]) != piece.lower():
        return False
    disamb = san[1:-2].replace("x", "") if piece != "P" else san[:-2].replace("x", "")
    for ch in disamb:
        if ch in FILES and uci[0] != ch:
            return False
        if ch.isdigit() and uci[1] != ch:
            return False
    return True


def san_to_uci(fen, san):
    board = pc.Board(fen)
    hits = [m for m in board.legal_moves() if san_matches(board, m, san)]
    return hits[0] if len(hits) == 1 else None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    engine = sys.argv[1]
    epd = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "tactics.epd")
    movetime = int(sys.argv[3]) if len(sys.argv) > 3 else 1000
    threads = int(sys.argv[4]) if len(sys.argv) > 4 else 1

    p = subprocess.Popen([engine], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def send(s):
        p.stdin.write(s + "\n")

    def wait_for(prefix):
        lines = []
        while True:
            line = p.stdout.readline()
            if not line:
                raise RuntimeError("engine exited")
            lines.append(line.strip())
            if line.startswith(prefix):
                return lines

    send("uci")
    wait_for("uciok")
    send(f"setoption name Threads value {threads}")
    send("isready")
    wait_for("readyok")

    solved = total = 0
    for raw in open(epd):
        raw = raw.strip()
        if not raw or raw.startswith("#"):
            continue
        fields = raw.split()
        fen = " ".join(fields[:4]) + " 0 1"
        ops = " ".join(fields[4:])
        bm = ops.split("bm ", 1)[1].split(";", 1)[0].split()
        ident = ops.split('id "', 1)[1].split('"', 1)[0] if 'id "' in ops else f"#{total + 1}"
        want = {san_to_uci(fen, s) for s in bm}
        if None in want:
            print(f"{ident:16s} bad bm {bm}")
            continue
        send("ucinewgame")
        send(f"position fen {fen}")
        send(f"go movetime {movetime}")
        lines = wait_for("bestmove")
        got = lines[-1].split()[1]
        total += 1
        ok = got in want
        solved += ok
        score = next((l.split(" score ")[1].split(" nodes")[0] for l in reversed(lines) if " score " in l), "?")
        print(f"{ident:16s} {'ok  ' if ok else 'FAIL'} best {got:6s} expected {'/'.join(sorted(want)):11s} score {score}")
    send("quit")
    print(f"solved {solved}/{total} at {movetime} ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
