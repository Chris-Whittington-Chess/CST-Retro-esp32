"""Train the CST Retro NNUE (768 -> N per side -> CReLU -> 1) and export it.

  python tools/nnue/train.py --out net.bin [--hidden 64] [--epochs 20]
         [--lam 0.9] [--k 400] [--batch 16384] [--lr 1e-3] labelled.txt ...

Input lines: "<fen>;<result>;<score>" (tools/nnue/label.py), result and
score from the side to move's view. Target = lam * sigmoid(score / k) +
(1 - lam) * result; loss = MSE against sigmoid(pred / k), pred in cp.

Float model (matches src/engine/nnue.cpp after quantisation):
  acc_side = ft_b + sum of ft_w[feature]           (one per side, shared weights)
  x = [clamp(acc_stm, 0, 1), clamp(acc_other, 0, 1)]
  pred_cp = 400 * (out_w . x + out_b)
Export (int): ft_* x 255 (int16), out_w x 64 (int16), out_b x 255 x 64 (int32).
Features: side-relative, (own ? 0 : 6) + type, square flipped for black.
Parsed data is cached next to the first input as <name>.npz.
"""
import argparse
import os
import struct
import time

import numpy as np
import torch
import torch.nn as nn

QA, QB, SCALE = 255, 64, 400
PIECES = "PNBRQK"
PAD = 768  # padding feature index (zero row)

ap = argparse.ArgumentParser()
ap.add_argument("data", nargs="+")
ap.add_argument("--out", required=True)
ap.add_argument("--hidden", type=int, default=64)
ap.add_argument("--epochs", type=int, default=20)
ap.add_argument("--batch", type=int, default=16384)
ap.add_argument("--lr", type=float, default=1e-3)
ap.add_argument("--lam", type=float, default=0.9)
ap.add_argument("--k", type=float, default=400.0)
ap.add_argument("--val", type=float, default=0.02)
ap.add_argument("--threads", type=int, default=0)
ap.add_argument("--seed", type=int, default=1)
a = ap.parse_args()
if a.threads:
    torch.set_num_threads(a.threads)
torch.manual_seed(a.seed)
np.random.seed(a.seed)


def features(fen):
    """Feature indices from the side to move's and the other side's view."""
    board, stm = fen.split()[:2]
    us = 0 if stm == "w" else 1
    f_us, f_them = [], []
    rank = 7
    for row in board.split("/"):
        file = 0
        for ch in row:
            if ch.isdigit():
                file += int(ch)
                continue
            color = 0 if ch.isupper() else 1
            t = PIECES.index(ch.upper())
            sq = rank * 8 + file
            for persp, out in ((us, f_us), (1 - us, f_them)):
                s = sq ^ 56 if persp == 1 else sq
                out.append(((0 if color == persp else 6) + t) * 64 + s)
            file += 1
        rank -= 1
    return f_us, f_them


def load(paths):
    cache = os.path.splitext(paths[0])[0] + (".npz" if len(paths) == 1 else f".{len(paths)}files.npz")
    if os.path.exists(cache) and all(os.path.getmtime(cache) > os.path.getmtime(p) for p in paths):
        d = np.load(cache)
        return d["fu"], d["ft"], d["score"], d["result"]
    rows = []
    for p in paths:
        with open(p) as f:
            rows += [l.strip().split(";") for l in f if l.count(";") == 2]
    n = len(rows)
    fu = np.full((n, 32), PAD, np.int16)
    ft = np.full((n, 32), PAD, np.int16)
    score = np.zeros(n, np.float32)
    result = np.zeros(n, np.float32)
    t0 = time.time()
    for i, (fen, res, sc) in enumerate(rows):
        u, t = features(fen)
        fu[i, :len(u)] = u
        ft[i, :len(t)] = t
        score[i] = float(sc)
        result[i] = float(res)
        if i and i % 500000 == 0:
            print(f"  parsed {i}/{n} ({time.time() - t0:.0f} s)", flush=True)
    np.savez(cache, fu=fu, ft=ft, score=score, result=result)
    return fu, ft, score, result


class Net(nn.Module):
    def __init__(self, n):
        super().__init__()
        self.ft = nn.EmbeddingBag(769, n, mode="sum", padding_idx=PAD)
        self.ft_b = nn.Parameter(torch.zeros(n))
        self.out = nn.Linear(2 * n, 1)
        nn.init.normal_(self.ft.weight, 0, 0.1)
        with torch.no_grad():
            self.ft.weight[PAD].zero_()
            self.ft_b.fill_(0.1)

    def forward(self, fu, ft):
        us = torch.clamp(self.ft(fu) + self.ft_b, 0, 1)
        them = torch.clamp(self.ft(ft) + self.ft_b, 0, 1)
        return SCALE * self.out(torch.cat([us, them], 1)).squeeze(1)

    def clip(self):
        # keep the int16 accumulator safe: 32 features x 1.98 x 255 < 32767
        with torch.no_grad():
            self.ft.weight.clamp_(-1.98, 1.98)
            self.ft.weight[PAD].zero_()
            self.out.weight.clamp_(-QA * 2, QA * 2)


def export(net, path):
    n = net.ft_b.shape[0]
    ft_w = np.round(net.ft.weight.detach().numpy()[:768] * QA).astype(np.int16)  # [768][n]
    ft_b = np.round(net.ft_b.detach().numpy() * QA).astype(np.int16)
    out_w = np.round(net.out.weight.detach().numpy()[0] * QB).astype(np.int16)  # [2n]
    out_b = int(round(float(net.out.bias.detach()[0]) * QA * QB))
    with open(path, "wb") as f:
        f.write(b"CSTN" + struct.pack("<III", 1, n, 0))
        f.write(ft_w.tobytes() + ft_b.tobytes() + out_w.tobytes())
        f.write(struct.pack("<i", out_b))
    return ft_w, ft_b, out_w, out_b


def int_eval(q, fu_row, ft_row):
    """The engine's integer evaluation (src/engine/nnue.cpp) of one position."""
    ft_w, ft_b, out_w, out_b = q
    n = ft_b.shape[0]
    acc = []
    for row in (fu_row, ft_row):
        a = ft_b.astype(np.int32).copy()
        for f in row:
            if f != PAD:
                a += ft_w[f]
        acc.append(np.clip(a, 0, QA))
    s = int((acc[0] * out_w[:n]).sum() + (acc[1] * out_w[n:]).sum()) + out_b
    cp = int(s * 25 / 1020)  # C++ truncates toward zero
    return max(-20000, min(20000, cp))


fu, ft, score, result = load(a.data)
n = len(score)
perm = np.random.permutation(n)
nval = max(1, int(n * a.val))
val_idx, tr_idx = perm[:nval], perm[nval:]
print(f"{n} positions ({len(tr_idx)} train, {nval} val), 768->{a.hidden}->1, "
      f"lam {a.lam}, k {a.k}", flush=True)

fu_t = torch.from_numpy(fu.astype(np.int64))
ft_t = torch.from_numpy(ft.astype(np.int64))
target = torch.from_numpy(a.lam / (1 + np.exp(-score / a.k)) + (1 - a.lam) * result).float()

net = Net(a.hidden)
opt = torch.optim.Adam(net.parameters(), lr=a.lr)
sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=a.epochs)


def loss_on(idx):
    i = torch.from_numpy(idx)
    pred = net(fu_t[i], ft_t[i])
    return ((torch.sigmoid(pred / a.k) - target[i]) ** 2).mean()


for epoch in range(a.epochs):
    t0 = time.time()
    net.train()
    np.random.shuffle(tr_idx)
    total, batches = 0.0, 0
    for b in range(0, len(tr_idx), a.batch):
        loss = loss_on(tr_idx[b:b + a.batch])
        opt.zero_grad()
        loss.backward()
        opt.step()
        net.clip()
        total += loss.item()
        batches += 1
    sched.step()
    net.eval()
    with torch.no_grad():
        vl = np.mean([loss_on(val_idx[b:b + a.batch]).item() for b in range(0, nval, a.batch)])
    print(f"epoch {epoch + 1:3d}  train {total / batches:.6f}  val {vl:.6f}  "
          f"lr {sched.get_last_lr()[0]:.2e}  {time.time() - t0:.1f} s", flush=True)

q = export(net, a.out)
print(f"wrote {a.out} ({os.path.getsize(a.out)} bytes)")

# Parity file: FENs + the integer evaluation the engine must reproduce.
rows = []
for p in a.data:
    with open(p) as f:
        rows += [l.strip().split(";")[0] for l in f if l.count(";") == 2]
check = sorted(np.random.choice(n, min(200, n), replace=False))
with open(a.out + ".fens", "w") as f:
    for i in check:
        f.write(rows[i] + "\n")
with open(a.out + ".expect", "w") as f:
    for i in check:
        f.write(f"{int_eval(q, fu[i], ft[i])}\n")
with torch.no_grad():
    i = torch.from_numpy(np.array(check))
    fl = net(fu_t[i], ft_t[i]).numpy()
ints = np.array([int_eval(q, fu[j], ft[j]) for j in check])
print(f"quantisation: mean |float - int| = {np.abs(fl - ints).mean():.1f} cp over {len(check)} positions")
print(f"parity check: bin\\cstretro.exe nneval {a.out} {a.out}.fens  vs  {a.out}.expect")
