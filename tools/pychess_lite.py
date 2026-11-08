"""Small, independent chess rules implementation (0x88 board).

Written separately from the C++ engine so it can serve as an independent
reference: tests/crosscheck.py compares perft counts against it, and
tools/match.py uses it to referee engine-vs-engine games.
"""

PIECES = "PNBRQKpnbrqk"
KNIGHT_D = (33, 31, 18, 14, -33, -31, -18, -14)
BISHOP_D = (15, 17, -15, -17)
ROOK_D = (1, -1, 16, -16)
KING_D = BISHOP_D + ROOK_D


def sq_name(s):
    return "abcdefgh"[s & 7] + str((s >> 4) + 1)


def parse_sq(t):
    return (int(t[1]) - 1) * 16 + "abcdefgh".index(t[0])


class Board:
    __slots__ = ("b", "white", "castle", "ep", "halfmove", "fullmove")

    def __init__(self, fen="rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"):
        parts = fen.split()
        while len(parts) < 6:
            parts.append(["w", "-", "-", "0", "1"][len(parts) - 1])
        self.b = ["."] * 128
        rank, file = 7, 0
        for ch in parts[0]:
            if ch == "/":
                rank -= 1
                file = 0
            elif ch.isdigit():
                file += int(ch)
            else:
                self.b[rank * 16 + file] = ch
                file += 1
        self.white = parts[1] == "w"
        self.castle = "" if parts[2] == "-" else parts[2]
        self.ep = None if parts[3] == "-" else parse_sq(parts[3])
        self.halfmove = int(parts[4])
        self.fullmove = int(parts[5])

    def copy(self):
        n = Board.__new__(Board)
        n.b = self.b[:]
        n.white, n.castle, n.ep = self.white, self.castle, self.ep
        n.halfmove, n.fullmove = self.halfmove, self.fullmove
        return n

    # ------------------------------------------------------------------
    def fen(self):
        rows = []
        for r in range(7, -1, -1):
            row, empty = "", 0
            for f in range(8):
                p = self.b[r * 16 + f]
                if p == ".":
                    empty += 1
                else:
                    if empty:
                        row += str(empty)
                        empty = 0
                    row += p
            if empty:
                row += str(empty)
            rows.append(row)
        ep = sq_name(self.ep) if self.ep is not None else "-"
        return "%s %s %s %s %d %d" % ("/".join(rows), "w" if self.white else "b", self.castle or "-", ep,
                                      self.halfmove, self.fullmove)

    def position_key(self):
        """Identity for repetition: placement, side, castling, and en passant only if capturable."""
        ep = "-"
        if self.ep is not None and any(m[1] == self.ep and self.b[m[0]].upper() == "P" for m in self.legal_moves_raw()):
            ep = sq_name(self.ep)
        return "%s %s %s %s" % (self.fen().split()[0], "w" if self.white else "b", self.castle or "-", ep)

    # ------------------------------------------------------------------
    def attacked(self, s, by_white):
        b = self.b
        # pawns
        if by_white:
            for d in (-15, -17):
                t = s + d
                if not t & 0x88 and b[t] == "P":
                    return True
        else:
            for d in (15, 17):
                t = s + d
                if not t & 0x88 and b[t] == "p":
                    return True
        n, k = ("N", "K") if by_white else ("n", "k")
        for d in KNIGHT_D:
            t = s + d
            if not t & 0x88 and b[t] == n:
                return True
        for d in KING_D:
            t = s + d
            if not t & 0x88 and b[t] == k:
                return True
        bq = ("B", "Q") if by_white else ("b", "q")
        rq = ("R", "Q") if by_white else ("r", "q")
        for d in BISHOP_D:
            t = s + d
            while not t & 0x88:
                if b[t] != ".":
                    if b[t] in bq:
                        return True
                    break
                t += d
        for d in ROOK_D:
            t = s + d
            while not t & 0x88:
                if b[t] != ".":
                    if b[t] in rq:
                        return True
                    break
                t += d
        return False

    def king_square(self, white):
        k = "K" if white else "k"
        for s in range(128):
            if not s & 0x88 and self.b[s] == k:
                return s
        return None

    def in_check(self):
        return self.attacked(self.king_square(self.white), not self.white)

    # ------------------------------------------------------------------
    def pseudo_moves(self):
        """Yields (from, to, promo) tuples."""
        b, w = self.b, self.white
        own = str.isupper if w else str.islower
        for s in range(128):
            if s & 0x88:
                continue
            p = b[s]
            if p == "." or not own(p):
                continue
            u = p.upper()
            if u == "P":
                up = 16 if w else -16
                start = 1 if w else 6
                last = 7 if w else 0
                t = s + up
                if not t & 0x88 and b[t] == ".":
                    if (t >> 4) == last:
                        for pr in "qrbn":
                            yield (s, t, pr)
                    else:
                        yield (s, t, None)
                        t2 = t + up
                        if (s >> 4) == start and b[t2] == ".":
                            yield (s, t2, None)
                for d in ((15, 17) if w else (-15, -17)):
                    t = s + d
                    if t & 0x88:
                        continue
                    q = b[t]
                    if (q != "." and not own(q)) or t == self.ep:
                        if (t >> 4) == last:
                            for pr in "qrbn":
                                yield (s, t, pr)
                        else:
                            yield (s, t, None)
            elif u == "N" or u == "K":
                for d in (KNIGHT_D if u == "N" else KING_D):
                    t = s + d
                    if not t & 0x88 and (b[t] == "." or not own(b[t])):
                        yield (s, t, None)
            else:
                dirs = BISHOP_D if u == "B" else ROOK_D if u == "R" else KING_D
                for d in dirs:
                    t = s + d
                    while not t & 0x88:
                        if b[t] == ".":
                            yield (s, t, None)
                        else:
                            if not own(b[t]):
                                yield (s, t, None)
                            break
                        t += d
        # castling
        if w and b[4] == "K" and not self.attacked(4, False):
            if "K" in self.castle and b[7] == "R" and b[5] == b[6] == "." and not self.attacked(5, False) \
                    and not self.attacked(6, False):
                yield (4, 6, None)
            if "Q" in self.castle and b[0] == "R" and b[1] == b[2] == b[3] == "." and not self.attacked(3, False) \
                    and not self.attacked(2, False):
                yield (4, 2, None)
        if not w and b[116] == "k" and not self.attacked(116, True):
            if "k" in self.castle and b[119] == "r" and b[117] == b[118] == "." and not self.attacked(117, True) \
                    and not self.attacked(118, True):
                yield (116, 118, None)
            if "q" in self.castle and b[112] == "r" and b[113] == b[114] == b[115] == "." \
                    and not self.attacked(115, True) and not self.attacked(114, True):
                yield (116, 114, None)

    def make(self, mv):
        """Returns a new board with move (from, to, promo) applied."""
        s, t, pr = mv
        n = self.copy()
        b = n.b
        p = b[s]
        u = p.upper()
        captured = b[t]
        b[t], b[s] = p, "."
        n.halfmove = 0 if (u == "P" or captured != ".") else self.halfmove + 1
        if u == "P":
            if t == self.ep:
                b[t - 16 if self.white else t + 16] = "."
            if pr:
                b[t] = pr.upper() if self.white else pr
        n.ep = None
        if u == "P" and abs(t - s) == 32:
            n.ep = (s + t) // 2
        if u == "K" and abs(t - s) == 2:
            if t == 6:
                b[5], b[7] = "R", "."
            elif t == 2:
                b[3], b[0] = "R", "."
            elif t == 118:
                b[117], b[119] = "r", "."
            elif t == 114:
                b[115], b[112] = "r", "."
        c = n.castle
        for sq, right in ((4, "KQ"), (116, "kq"), (0, "Q"), (7, "K"), (112, "q"), (119, "k")):
            if s == sq or t == sq:
                for r in right:
                    c = c.replace(r, "")
        n.castle = c
        if not self.white:
            n.fullmove += 1
        n.white = not self.white
        return n

    def legal_moves_raw(self):
        out = []
        for mv in self.pseudo_moves():
            n = self.make(mv)
            if not n.attacked(n.king_square(self.white), n.white):
                out.append(mv)
        return out

    def legal_moves(self):
        return [move_to_uci(m) for m in self.legal_moves_raw()]

    def push_uci(self, uci):
        for m in self.legal_moves_raw():
            if move_to_uci(m) == uci:
                return self.make(m)
        return None

    def is_capture(self, uci):
        s, t = parse_sq(uci[:2]), parse_sq(uci[2:4])
        return self.b[t] != "." or (self.b[s].upper() == "P" and t == self.ep)

    def insufficient_material(self):
        minors = []
        for s in range(128):
            if s & 0x88:
                continue
            p = self.b[s]
            if p in "PpRrQq":
                return False
            if p in "NnBb":
                minors.append((p, ((s >> 4) + (s & 7)) & 1))
        if len(minors) <= 1:
            return True
        if all(p in "Bb" for p, _ in minors) and len({c for _, c in minors}) == 1:
            return True
        return False


def move_to_uci(m):
    s, t, pr = m
    return sq_name(s) + sq_name(t) + (pr or "")


def perft(board, depth):
    moves = board.legal_moves_raw()
    if depth == 1:
        return len(moves)
    return sum(perft(board.make(m), depth - 1) for m in moves)
