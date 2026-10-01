// Chess System Tal Retro - PC test harness for the portable engine core.
//   cstretro perft <file.epd> [max_nodes]   check every "; Dn count" up to max_nodes
//   cstretro divide "<fen>" <depth>
//   cstretro bench                          fixed perft set, prints nodes/s
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "../src/engine/board.h"
#include "../src/engine/bench.h"

static Board board;  // ~16 KB of history: keep it off the stack

static double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static int run_epd(const char* path, u64 max_nodes) {
  FILE* f = fopen(path, "r");
  if (!f) { printf("cannot open %s\n", path); return 1; }
  char line[1024];
  int positions = 0, checks = 0, fails = 0;
  u64 total = 0;
  double t0 = now();
  while (fgets(line, sizeof line, f)) {
    char* semi = strchr(line, ';');
    if (!semi) continue;
    *semi = 0;
    if (!board.set_fen(line)) { printf("bad fen: %s\n", line); fails++; continue; }
    positions++;
    for (char* p = semi + 1; (p = strchr(p, 'D')); ) {
      int depth = atoi(p + 1);
      char* sp = strchr(p, ' ');
      if (!sp) break;
      u64 want = strtoull(sp + 1, &p, 10);
      if (want > max_nodes) continue;
      u64 got = perft(board, depth);
      total += got;
      checks++;
      if (got != want) {
        fails++;
        printf("FAIL %s D%d want %llu got %llu\n", line, depth, (unsigned long long)want,
               (unsigned long long)got);
      }
    }
  }
  fclose(f);
  double dt = now() - t0;
  printf("%s: %d positions, %d checks, %d failures, %llu nodes, %.2f s, %.1f Mnps\n", path,
         positions, checks, fails, (unsigned long long)total, dt, total / dt / 1e6);
  return fails != 0;
}

// Incremental key == from-scratch key at every node.
static u64 key_walk(int depth) {
  if (board.key != board.compute_key()) return 1;
  if (!depth) return 0;
  Move moves[MAX_MOVES];
  int n = board.gen_legal(moves);
  u64 bad = 0;
  for (int i = 0; i < n; i++) {
    board.make(moves[i]);
    bad += key_walk(depth - 1);
    board.unmake();
  }
  return bad;
}

// The published Polyglot key examples (book_format.html).
static int key_test() {
  static const struct { const char* moves; u64 key; } T[] = {
      {"", 0x463b96181691fc9cull},
      {"e2e4", 0x823c9b50fd114196ull},
      {"e2e4 d7d5", 0x0756b94461c50fb0ull},
      {"e2e4 d7d5 e4e5", 0x662fafb965db29d4ull},
      {"e2e4 d7d5 e4e5 f7f5", 0x22a48b5a8e47ff78ull},
      {"e2e4 d7d5 e4e5 f7f5 e1e2", 0x652a607ca3f242c1ull},
      {"e2e4 d7d5 e4e5 f7f5 e1e2 e8f7", 0x00fdd303c946bdd9ull},
      {"a2a4 b7b5 h2h4 b5b4 c2c4", 0x3c8123ea7b067637ull},
      {"a2a4 b7b5 h2h4 b5b4 c2c4 b4c3 a1a3", 0x5c3f9b829b279560ull},
  };
  int fails = 0;
  for (auto& t : T) {
    board.set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    std::string s = t.moves;
    for (size_t i = 0; i + 4 <= s.size(); i += 5) {
      Move m = parse_uci(board, s.substr(i, 4).c_str());
      if (!m) { printf("illegal %s\n", s.substr(i, 4).c_str()); fails++; break; }
      board.make(m);
    }
    if (board.key != t.key) {
      printf("FAIL \"%s\" key %016llx want %016llx\n", t.moves, (unsigned long long)board.key,
             (unsigned long long)t.key);
      fails++;
    }
  }
  board.set_fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
  u64 bad = key_walk(4);
  printf("keytest: %d/%d Polyglot keys ok, %llu incremental key mismatches\n",
         int(sizeof T / sizeof T[0]) - fails, int(sizeof T / sizeof T[0]), (unsigned long long)bad);
  return fails || bad;
}

int main(int argc, char** argv) {
  if (argc >= 2 && !strcmp(argv[1], "keytest")) return key_test();
  if (argc >= 3 && !strcmp(argv[1], "perft"))
    return run_epd(argv[2], argc >= 4 ? strtoull(argv[3], nullptr, 10) : ~0ull);
  if (argc >= 4 && !strcmp(argv[1], "divide")) {
    if (!board.set_fen(argv[2])) { printf("bad fen\n"); return 1; }
    Move moves[MAX_MOVES];
    int n = board.gen_legal(moves), depth = atoi(argv[3]);
    u64 total = 0;
    for (int i = 0; i < n; i++) {
      char buf[6];
      move_to_uci(moves[i], buf);
      board.make(moves[i]);
      u64 c = depth > 1 ? perft(board, depth - 1) : 1;
      board.unmake();
      total += c;
      printf("%s %llu\n", buf, (unsigned long long)c);
    }
    printf("total %llu\n", (unsigned long long)total);
    return 0;
  }
  if (argc >= 2 && !strcmp(argv[1], "bench")) {
    double t0 = now();
    BenchResult r = run_bench(board);
    double dt = now() - t0;
    printf("bench: %llu nodes, %d failures, %.2f s, %.2f Mnps\n", (unsigned long long)r.nodes,
           r.failures, dt, r.nodes / dt / 1e6);
    return r.failures != 0;
  }
  printf("usage: cstretro perft <file.epd> [max_nodes] | divide \"<fen>\" <depth> | bench\n");
  return 1;
}
