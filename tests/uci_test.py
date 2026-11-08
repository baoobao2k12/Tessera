#!/usr/bin/env python3
"""End-to-end UCI protocol tests: drives the engine binary through a session
and checks responses, legality of best moves, timing and termination.

usage: uci_test.py <path to tessera>
"""
import os
import queue
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import pychess_lite as pc  # noqa: E402

ALLOWED_PREFIXES = ("id ", "option ", "uciok", "readyok", "info ", "bestmove ")


class Engine:
    def __init__(self, path):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.q = queue.Queue()
        self.all_lines = []
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in self.p.stdout:
            line = line.rstrip("\n")
            self.all_lines.append(line)
            self.q.put(line)
        self.q.put(None)

    def send(self, cmd):
        self.p.stdin.write(cmd + "\n")
        self.p.stdin.flush()

    def read_until(self, prefix, timeout):
        lines = []
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError("no '%s' within %.1fs; got %s" % (prefix, timeout, lines[-3:]))
            try:
                line = self.q.get(timeout=left)
            except queue.Empty:
                continue
            if line is None:
                raise EOFError("engine exited; last lines %s" % lines[-3:])
            lines.append(line)
            if line.startswith(prefix):
                return lines

    def expect_silence(self, seconds):
        try:
            line = self.q.get(timeout=seconds)
        except queue.Empty:
            return None
        return line


failures = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures.append(what)


def bestmove_of(lines):
    return lines[-1].split()[1]


def legal_in(fen_or_moves, move):
    b = pc.Board()
    if fen_or_moves.startswith("fen "):
        b = pc.Board(fen_or_moves[4:].split(" moves ")[0])
        moves = fen_or_moves.split(" moves ")[1].split() if " moves " in fen_or_moves else []
    else:
        moves = fen_or_moves.split()
    for m in moves:
        b = b.push_uci(m)
    return move in b.legal_moves()


def main():
    eng = Engine(sys.argv[1])

    print("handshake")
    eng.send("uci")
    lines = eng.read_until("uciok", 5)
    check(any(l.startswith("id name Tessera") for l in lines), "id name")
    check(any(l.startswith("id author") for l in lines), "id author")
    for opt in ("Hash", "Threads", "Clear Hash", "Ponder", "Move Overhead"):
        check(any(l.startswith("option name %s type" % opt) for l in lines), "option " + opt)
    eng.send("isready")
    eng.read_until("readyok", 5)
    check(True, "isready -> readyok")

    print("go depth")
    eng.send("ucinewgame")
    eng.send("position startpos")
    eng.send("go depth 8")
    lines = eng.read_until("bestmove", 30)
    infos = [l for l in lines if l.startswith("info depth")]
    check(len(infos) >= 8, "one info line per iteration")
    last = infos[-1].split()
    for field in ("depth", "seldepth", "score", "nodes", "nps", "hashfull", "time", "pv"):
        check(field in last, "info contains " + field)
    check(last[last.index("depth") + 1] == "8", "reached depth 8")
    check(legal_in("", bestmove_of(lines)), "bestmove legal from startpos")
    check("ponder" in lines[-1], "bestmove carries a ponder move")
    pv = last[last.index("pv") + 1:]
    b = pc.Board()
    ok = True
    for m in pv:
        nb = b.push_uci(m)
        if nb is None:
            ok = False
            break
        b = nb
    check(ok and pv[0] == bestmove_of(lines), "PV is a legal line starting with bestmove")

    print("position ... moves")
    eng.send("position startpos moves e2e4 e7e5 g1f3 b8c6 f1b5")
    eng.send("go depth 6")
    lines = eng.read_until("bestmove", 30)
    check(legal_in("e2e4 e7e5 g1f3 b8c6 f1b5", bestmove_of(lines)), "bestmove legal after moves")

    print("go movetime")
    fen = "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4"
    eng.send("position fen " + fen)
    t0 = time.time()
    eng.send("go movetime 400")
    lines = eng.read_until("bestmove", 5)
    dt = time.time() - t0
    check(0.3 <= dt < 0.9, "movetime 400 respected (%.0f ms)" % (dt * 1000))
    check(legal_in("fen " + fen, bestmove_of(lines)), "bestmove legal (fen)")

    print("go wtime/btime")
    eng.send("position startpos moves d2d4")
    t0 = time.time()
    eng.send("go wtime 3000 btime 3000 winc 0 binc 0")
    lines = eng.read_until("bestmove", 5)
    dt = time.time() - t0
    check(dt < 1.0, "sudden death 3s uses a small slice (%.0f ms)" % (dt * 1000))
    check(legal_in("d2d4", bestmove_of(lines)), "bestmove legal (black)")
    t0 = time.time()
    eng.send("go wtime 200 btime 200 winc 0 binc 0")
    lines = eng.read_until("bestmove", 5)
    check(time.time() - t0 < 0.2, "low time 200ms answered in time")
    t0 = time.time()
    eng.send("go wtime 5000 btime 5000 movestogo 1")
    lines = eng.read_until("bestmove", 6)
    check(time.time() - t0 < 4.6, "movestogo 1 keeps a reserve")
    eng.send("go wtime 10000 btime 10000 winc 100 binc 100 movestogo 20")
    lines = eng.read_until("bestmove", 10)
    check(legal_in("d2d4", bestmove_of(lines)), "movestogo 20")

    print("go nodes")
    eng.send("position startpos")
    eng.send("go nodes 5000")
    lines = eng.read_until("bestmove", 10)
    nodes = [int(l.split()[l.split().index("nodes") + 1]) for l in lines if " nodes " in l]
    check(nodes and nodes[-1] <= 5000 + 1100, "node limit respected (%s)" % (nodes[-1] if nodes else None))

    print("go infinite + stop")
    n0 = len(eng.all_lines)
    eng.send("go infinite")
    time.sleep(0.6)
    check(not any(l.startswith("bestmove") for l in eng.all_lines[n0:]), "no bestmove before stop")
    t0 = time.time()
    eng.send("stop")
    lines = eng.read_until("bestmove", 3)
    check(time.time() - t0 < 0.5, "stop is prompt")
    check(legal_in("", bestmove_of(lines)), "bestmove after stop is legal")

    print("isready while searching")
    eng.send("go infinite")
    time.sleep(0.1)
    eng.send("isready")
    lines = eng.read_until("readyok", 2)
    check(not any(l.startswith("bestmove") for l in lines), "readyok without ending the search")
    eng.send("stop")
    eng.read_until("bestmove", 3)

    print("ponder / ponderhit")
    eng.send("position startpos moves e2e4")
    n0 = len(eng.all_lines)
    eng.send("go ponder wtime 2000 btime 2000")
    time.sleep(0.6)
    check(not any(l.startswith("bestmove") for l in eng.all_lines[n0:]), "no bestmove while pondering")
    t0 = time.time()
    eng.send("ponderhit")
    lines = eng.read_until("bestmove", 4)
    check(time.time() - t0 < 2.0, "ponderhit then bestmove in time")
    check(legal_in("e2e4", bestmove_of(lines)), "bestmove after ponderhit legal")
    eng.send("go ponder wtime 2000 btime 2000")
    time.sleep(0.2)
    eng.send("stop")
    lines = eng.read_until("bestmove", 3)
    check(legal_in("e2e4", bestmove_of(lines)), "stop during ponder")

    print("options")
    eng.send("setoption name Hash value 32")
    eng.send("setoption name Threads value 2")
    eng.send("setoption name Clear Hash")
    eng.send("setoption name Move Overhead value 50")
    eng.send("isready")
    eng.read_until("readyok", 10)
    eng.send("position startpos")
    eng.send("go depth 10")
    lines = eng.read_until("bestmove", 30)
    check(legal_in("", bestmove_of(lines)), "two threads: legal bestmove")
    eng.send("go movetime 300")
    lines = eng.read_until("bestmove", 5)
    check(legal_in("", bestmove_of(lines)), "two threads: movetime")
    eng.send("setoption name Threads value 1")
    eng.send("setoption name Hash value 16")
    eng.send("isready")
    eng.read_until("readyok", 10)

    print("searchmoves")
    eng.send("position startpos")
    eng.send("go depth 6 searchmoves a2a3 h2h3")
    lines = eng.read_until("bestmove", 20)
    check(bestmove_of(lines) in ("a2a3", "h2h3"), "searchmoves restricts the root")

    print("mate and no-move positions")
    eng.send("position fen k7/8/2K5/8/8/8/8/7R w - - 0 1")
    eng.send("go depth 8")
    lines = eng.read_until("bestmove", 10)
    check(any("score mate 2" in l for l in lines), "mate in 2 found")
    eng.send("position fen 6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1")
    eng.send("go mate 1")
    lines = eng.read_until("bestmove", 10)
    check(any("score mate 1" in l for l in lines) and bestmove_of(lines) == "a1a8", "go mate 1 finds Ra8#")
    eng.send("position fen rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3")
    eng.send("go depth 5")
    lines = eng.read_until("bestmove", 5)
    check(lines[-1].strip() == "bestmove 0000", "checkmated side answers bestmove 0000")
    check(any("score mate 0" in l for l in lines), "checkmated: score mate 0")
    eng.send("position fen 7k/5Q2/6K1/8/8/8/8/8 b - - 0 1")
    eng.send("go movetime 100")
    lines = eng.read_until("bestmove", 5)
    check(lines[-1].strip() == "bestmove 0000", "stalemated side answers bestmove 0000")
    eng.send("position fen 8/8/8/8/8/8/8/K6k w - - 0 1")
    eng.send("go depth 3")
    lines = eng.read_until("bestmove", 5)
    check(legal_in("fen 8/8/8/8/8/8/8/K6k w - - 0 1", bestmove_of(lines)), "bare kings: legal move")

    print("draw by repetition is scored as a draw")
    eng.send("position startpos moves g1f3 g8f6 f3g1 f6g8 g1f3 g8f6 f3g1 f6g8")
    eng.send("go depth 6")
    lines = eng.read_until("bestmove", 10)
    check(legal_in("g1f3 g8f6 f3g1 f6g8 g1f3 g8f6 f3g1 f6g8", bestmove_of(lines)), "legal after repetitions")

    print("robustness")
    eng.send("foo bar")
    eng.send("position fen this is not a fen")
    eng.send("position startpos moves e2e5")
    eng.send("go depth 2 searchmoves zzzz")
    lines = eng.read_until("bestmove", 5)
    check(lines[-1].startswith("bestmove"), "garbage tolerated, still searching")
    eng.send("stop")
    eng.send("isready")
    eng.read_until("readyok", 3)
    check(True, "responsive after stop with no search running")
    count = 0
    for _ in range(25):
        eng.send("go infinite")
        time.sleep(0.01)
        eng.send("stop")
        eng.read_until("bestmove", 3)
        count += 1
    eng.send("isready")
    lines = eng.read_until("readyok", 3)
    check(count == 25 and not any(l.startswith("bestmove") for l in lines), "exactly one bestmove per go")

    print("output hygiene")
    bad = [l for l in eng.all_lines if l and not l.startswith(ALLOWED_PREFIXES)]
    check(not bad, "only UCI lines on stdout (%d other)" % len(bad))
    if bad:
        print("     e.g. %r" % bad[:3])

    print("quit")
    t0 = time.time()
    eng.send("quit")
    try:
        rc = eng.p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        rc = None
        eng.p.kill()
    check(rc == 0 and time.time() - t0 < 2, "quit exits cleanly")

    print("\n%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
