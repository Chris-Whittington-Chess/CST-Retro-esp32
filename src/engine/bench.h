// Chess System Tal Retro - fixed perft bench shared by the PC harness and the
// device (the standard chessprogramming.org positions, ~16M nodes).
#pragma once
#include "board.h"
#include "search.h"

struct BenchPos {
  const char* fen;
  int depth;
  u64 nodes;
};

static const BenchPos BENCH_POSITIONS[] = {
    {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609},
    {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603},
    {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624},
    {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333},
    {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487},
    {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594},
};

struct BenchResult {
  u64 nodes;
  int failures;
};

inline BenchResult run_bench(Board& b) {
  BenchResult r = {0, 0};
  for (const BenchPos& p : BENCH_POSITIONS) {
    b.set_fen(p.fen);
    u64 n = perft(b, p.depth);
    r.nodes += n;
    if (n != p.nodes) r.failures++;
  }
  return r;
}

// Fixed-depth search set (new game per position); the node total is a
// determinism signature that must match between the PC and the device.
static const char* const SEARCH_BENCH_FENS[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
    "2r3k1/pp3ppp/2n1b3/3p4/3P4/2PB1N2/P4PPP/R5K1 w - - 0 20",
    "8/5pk1/6p1/8/3R4/6P1/5PK1/1r6 w - - 0 40",
    "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1",
    "r2q1rk1/pb1nbppp/1p2pn2/2pp4/2PP4/1PN1PN2/PB2BPPP/R2Q1RK1 w - - 0 10",
    "8/8/4k3/8/2K5/3P4/8/8 w - - 0 1",
};

// search_init() must have been called.
inline u64 run_search_bench(Board& b, int depth) {
  u64 total = 0;
  for (const char* f : SEARCH_BENCH_FENS) {
    b.set_fen(f);
    search_new_game();
    Limits lim;
    lim.depth = depth;
    total += search(b, lim, nullptr).nodes;
  }
  return total;
}
