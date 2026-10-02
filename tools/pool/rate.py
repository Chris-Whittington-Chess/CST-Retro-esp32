"""Ratings from a pool PGN, anchored to CCRL 40/15.

  python tools/pool/rate.py match/rr1.pgn [more.pgn ...]

Maximum-likelihood logistic Elo for every player (draw = half a win), then
shifted so the pool engines best match their CCRL ratings (least squares).
Prints each engine's fitted rating next to its CCRL rating - engines far off
their list rating would skew a gauntlet - and every non-pool player (CST
Retro) with a bootstrap 95% interval.
"""
import math
import random
import re
import sys
from collections import defaultdict

CCRL = {
    "Pulse-1.6.1": 1516, "TSCP-1.81": 1600, "ShallowBlue-2.0.0": 1618, "Apollo-1.2.1": 1745,
    "Deepov-0.4.1": 1820, "Wowl-1.3.7": 1848, "Wowl-1.3.8": 1879, "Snowy-0.2": 1925,
    "Cinnamon-2.0": 1970, "Goldfish-1.13.0": 1983, "Cinnamon-2.2a": 2052, "Halogen-3.0": 2112,
    "Smallbrain-1.1": 2177, "Smallbrain-2.0": 2235, "Halogen-6": 2440,
    # batch 3
    "Maxwell-3.1-3": 2008, "Gunborg-1.35": 2028, "Trinket-3.0.0": 2040, "Barbarossa-0.4.0": 2081,
    "KhepriChess-4.0.1": 2201, "Barbarossa-0.5.0": 2235, "Dumb-1.3": 2252, "Prophet-4.1": 2338,
    "Tantabus-1.0.2": 2340, "Peacekeeper-1.10": 2346, "Barbarossa-0.6.0": 2363,
    "Altair-1.0.0": 2372, "Clarity-2.0.0": 2374, "byte-knight-3.0.0": 2388, "Avalanche-0.2": 2396,
}


def read_games(paths):
    games = []
    for path in paths:
        tags = {}
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.match(r'\[(\w+) "(.*)"\]', line)
                if m:
                    tags[m.group(1)] = m.group(2)
                    if m.group(1) == "Result":
                        r = {"1-0": 1.0, "0-1": 0.0, "1/2-1/2": 0.5}.get(m.group(2))
                        if r is not None and "White" in tags and "Black" in tags:
                            games.append((tags["White"], tags["Black"], r))
                        tags = {}
    return games


PRIOR_DRAWS = 2  # virtual draws vs an average player: keeps 100% / 0% scores finite


def fit(games, iters=3000, lr=8.0):
    players = sorted({p for g in games for p in g[:2]})
    r = {p: 0.0 for p in players}
    n = defaultdict(int)
    for w, b, _ in games:
        n[w] += 1
        n[b] += 1
    for _ in range(iters):
        grad = defaultdict(float)
        for w, b, s in games:
            e = 1 / (1 + 10 ** ((r[b] - r[w]) / 400))
            grad[w] += s - e
            grad[b] -= s - e
        mean = sum(r.values()) / len(r)
        for p in players:
            e = 1 / (1 + 10 ** ((mean - r[p]) / 400))
            grad[p] += PRIOR_DRAWS * (0.5 - e)
            r[p] += lr * grad[p] / (n[p] + PRIOR_DRAWS) * 4
    anchors = [p for p in players if p in CCRL]
    shift = sum(CCRL[p] - r[p] for p in anchors) / len(anchors) if anchors else 0
    return {p: r[p] + shift for p in players}


games = read_games(sys.argv[1:])
if not games:
    sys.exit("no games")
rating = fit(games)
count = defaultdict(int)
score = defaultdict(float)
for w, b, s in games:
    count[w] += 1
    count[b] += 1
    score[w] += s
    score[b] += 1 - s

others = [p for p in rating if p not in CCRL]
random.seed(1)
boot = defaultdict(list)
for _ in range(60):  # bootstrap over games
    sample = [random.choice(games) for _ in games]
    rb = fit(sample, iters=600)
    for p in others:
        if p in rb:
            boot[p].append(rb[p])

print(f"{len(games)} games\n")
print(f"{'player':20s} {'games':>5s} {'score':>6s} {'fitted':>7s} {'CCRL':>6s} {'diff':>6s}")
for p in sorted(rating, key=lambda p: -rating[p]):
    c = CCRL.get(p)
    extra = f"{c:6d} {rating[p] - c:+6.0f}" if c else ""
    if p in boot and boot[p]:
        b = sorted(boot[p])
        extra = f"   95% {b[int(0.025 * len(b))]:.0f}..{b[int(0.975 * len(b)) - 1]:.0f}"
    print(f"{p:20s} {count[p]:5d} {100 * score[p] / count[p]:5.1f}% {rating[p]:7.0f} {extra}")
anch = [p for p in rating if p in CCRL]
rms = math.sqrt(sum((rating[p] - CCRL[p]) ** 2 for p in anch) / len(anch))
print(f"\npool fit vs CCRL: rms {rms:.0f} Elo over {len(anch)} engines")
