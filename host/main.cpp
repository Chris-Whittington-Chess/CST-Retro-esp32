// Chess System Tal Retro - PC test harness for the portable engine core.
//   cstretro perft <file.epd> [max_nodes]   check every "; Dn count" up to max_nodes
//   cstretro divide "<fen>" <depth>
//   cstretro bench                          fixed perft set, prints nodes/s
//   cstretro sbench [depth] [hash_kb]       fixed-depth search set (node signature)
//   cstretro [uci]                          UCI engine for test matches
//   cstretro book <file.bin> [uci moves...]  book moves after those moves
//   cstretro nncheck <N> [H]                NNUE incremental == refresh (random net)
//   cstretro nnbench <N> [H] [depth] [hash_kb]  search bench paying the NNUE cost
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "../src/engine/board.h"
#include "../src/engine/bench.h"
#include "../src/engine/book.h"
#include "../src/engine/see.h"
#include "../src/engine/search.h"
#include "host.h"

static Board board;  // ~16 KB of history: keep it off the stack

static double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

u32 engine_now_ms() { return u32(u64(now() * 1000)); }

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

static int book_list(int argc, char** argv) {
  FILE* f = fopen(argv[2], "rb");
  if (!f) { printf("cannot open %s\n", argv[2]); return 1; }
  std::string data;
  char chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) data.append(chunk, n);
  fclose(f);
  board.set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
  for (int i = 3; i < argc; i++) {
    Move m = parse_uci(board, argv[i]);
    if (!m) { printf("illegal %s\n", argv[i]); return 1; }
    board.make(m);
  }
  BookMove moves[64];
  int k = book_moves(board, (const u8*)data.data(), data.size(), moves, 64);
  printf("%zu entries, key %016llx: %d book moves\n", data.size() / 16, (unsigned long long)board.key, k);
  for (int i = 0; i < k; i++) {
    char buf[6];
    move_to_uci(moves[i].move, buf);
    printf("  %s %d\n", buf, moves[i].weight);
  }
  return 0;
}

// Textbook exchanges (CStal-5's seetest set, P=100 N=320 B=330 R=500 Q=900).
static int see_test() {
  static const struct { const char* fen; const char* move; int value; } T[] = {
      {"4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1", "e4d5", 100},
      {"4k3/8/2p5/3p4/4P3/8/8/4K3 w - - 0 1", "e4d5", 0},
      {"4k3/8/2p5/3p4/8/8/3Q4/4K3 w - - 0 1", "d2d5", -800},
      {"4k3/8/2p5/3p4/8/3R4/3R4/4K3 w - - 0 1", "d3d5", -300},
      {"1k1r4/1pp4p/p7/4p3/8/P5P1/1PP4P/2K1R3 w - - 0 1", "e1e5", 100},
      {"1k1r3q/1ppn3p/p4b2/4p3/8/P2N2P1/1PP1R1BP/2K1Q3 w - - 0 1", "d3e5", -220},
      {"4k3/8/8/3r4/8/8/3R4/3RK3 w - - 0 1", "d2d5", 500},
  };
  int ok = 0;
  for (auto& t : T) {
    board.set_fen(t.fen);
    Move m = parse_uci(board, t.move);
    int v = m ? see_value(board, m) : 99999;
    ok += v == t.value;
    printf("%s %5d (expect %5d)  %s  %s\n", v == t.value ? "ok  " : "FAIL", v, t.value, t.move, t.fen);
  }
  printf("see: %d/%d\n", ok, int(sizeof T / sizeof T[0]));
  return ok != int(sizeof T / sizeof T[0]);
}

int main(int argc, char** argv) {
  if (argc < 2 || !strcmp(argv[1], "uci")) return uci_loop();
  if (!strcmp(argv[1], "seetest")) return see_test();
  if (argc >= 3 && !strcmp(argv[1], "book")) return book_list(argc, argv);
  if (!strcmp(argv[1], "pgnfens")) return pgn_fens(argc, argv);
  if (!strcmp(argv[1], "nneval")) return nnue_eval_file(argc, argv);
  if (argc >= 3 && !strcmp(argv[1], "nncheck"))
    return nnue_test(atoi(argv[2]), argc >= 4 ? atoi(argv[3]) : 0);
  if (argc >= 3 && !strcmp(argv[1], "nnbench"))
    return nnue_bench(atoi(argv[2]), argc >= 4 ? atoi(argv[3]) : 0, argc >= 5 ? atoi(argv[4]) : 8,
                      argc >= 6 ? atoi(argv[5]) : 128);
  if (!strcmp(argv[1], "sbench")) return search_bench(argc >= 3 ? atoi(argv[2]) : 10, argc >= 4 ? atoi(argv[3]) : 16384);
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
  printf("usage: cstretro [uci] | sbench [depth] | perft <file.epd> [max_nodes] | divide \"<fen>\" <depth> | bench | keytest\n");
  return 1;
}
