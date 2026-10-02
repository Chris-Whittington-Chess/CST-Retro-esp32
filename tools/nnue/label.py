"""Label positions with a teacher engine (CStal-5) for NNUE training.

  python tools/nnue/label.py --out data.txt [--workers 4] [--depth 0]
         [--engine <exe>] [--limit N] positions.txt ...

Input lines: "<fen>;<result>" from `cstretro pgnfens` (quiet positions, result
from the side to move's view). Positions are deduplicated (board + side to
move). Each worker runs one teacher process:
  --depth 0   static NNUE eval ("nneval", CStal-5's own command) - fast
  --depth D   "go depth D", the final score (positions whose best move is a
              capture or promotion are dropped as not quiet; mates dropped)
Output lines: "<fen>;<result>;<score cp, side to move>".
"""
import argparse
import queue
import re
import subprocess
import sys
import threading
import time

DEFAULT_ENGINE = r"C:\Users\chris\source\repos\CStal-5\bin\cstal5-pext-clang-pgo.exe"

ap = argparse.ArgumentParser()
ap.add_argument("inputs", nargs="+")
ap.add_argument("--out", required=True)
ap.add_argument("--engine", default=DEFAULT_ENGINE)
ap.add_argument("--workers", type=int, default=4)
ap.add_argument("--depth", type=int, default=0)
ap.add_argument("--limit", type=int, default=0)
a = ap.parse_args()

# ---- read + dedupe ----
seen, jobs = set(), []
for path in a.inputs:
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            fen, _, result = line.partition(";")
            key = " ".join(fen.split()[:4])  # board, side, castling, ep
            if key in seen:
                continue
            seen.add(key)
            jobs.append((fen, result))
            if a.limit and len(jobs) >= a.limit:
                break
print(f"{len(jobs)} unique positions, {a.workers} workers, "
      f"{'static nneval' if a.depth == 0 else f'depth {a.depth}'}", flush=True)

todo = queue.Queue()
for j in jobs:
    todo.put(j)
done = queue.Queue()

SCORE = re.compile(r"score (cp|mate) (-?\d+)")
NNEVAL = re.compile(r"cp=(-?\d+)")


def board_piece(fen, square):
    """Piece letter on a square like 'e4' in a FEN, or ''."""
    rows = fen.split()[0].split("/")
    f, r = ord(square[0]) - 97, int(square[1])
    file = 0
    for ch in rows[8 - r]:
        if ch.isdigit():
            file += int(ch)
        else:
            if file == f:
                return ch
            file += 1
    return ""


def worker():
    p = subprocess.Popen([a.engine], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def send(cmd):
        p.stdin.write(cmd + "\n")
        p.stdin.flush()

    def read_until(prefix):
        lines = []
        while True:
            line = p.stdout.readline()
            if not line:
                raise RuntimeError("teacher exited")
            lines.append(line)
            if line.startswith(prefix):
                return lines

    send("uci")
    read_until("uciok")
    send("isready")
    read_until("readyok")
    while True:
        try:
            fen, result = todo.get_nowait()
        except queue.Empty:
            break
        send(f"position fen {fen}")
        if a.depth == 0:
            send("nneval")
            line = read_until("info string nnue")[-1]
            m = NNEVAL.search(line)
            if m:
                done.put(f"{fen};{result};{m.group(1)}")
            else:
                done.put(None)
            continue
        send(f"go depth {a.depth}")
        lines = read_until("bestmove")
        score = None
        for line in lines:
            m = SCORE.search(line)
            if m:
                score = m.groups()
        best = lines[-1].split()[1]
        quiet = (len(best) == 4 and not board_piece(fen, best[2:4])
                 and not (board_piece(fen, best[:2]) in "Pp" and best[0] != best[2]))
        if score and score[0] == "cp" and quiet:
            done.put(f"{fen};{result};{score[1]}")
        else:
            done.put(None)
    send("quit")
    p.wait(timeout=5)


threads = [threading.Thread(target=worker, daemon=True) for _ in range(a.workers)]
for t in threads:
    t.start()
t0, written, dropped = time.time(), 0, 0
with open(a.out, "w") as out:
    while any(t.is_alive() for t in threads) or not done.empty():
        try:
            r = done.get(timeout=0.5)
        except queue.Empty:
            continue
        if r is None:
            dropped += 1
        else:
            out.write(r + "\n")
            written += 1
        n = written + dropped
        if n % 20000 == 0:
            dt = time.time() - t0
            print(f"  {n}/{len(jobs)}  {n / dt:.0f}/s  kept {written}", flush=True)
dt = time.time() - t0
print(f"labelled {written} (dropped {dropped}) in {dt:.0f} s, {(written + dropped) / max(dt, 1e-9):.0f}/s")
