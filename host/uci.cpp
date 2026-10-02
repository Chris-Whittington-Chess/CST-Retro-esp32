// Chess System Tal Retro - minimal UCI front end for PC test matches (the
// device has its own, over USB). Search runs synchronously: "stop" is not
// needed by cutechess for timed games.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "../src/engine/bench.h"
#include "../src/engine/board.h"
#include "../src/engine/eval.h"
#include "../src/engine/nnue.h"
#include "../src/engine/search.h"
#include "../src/engine/uci_util.h"
#include "host.h"

static Board board;
static TT tt;
static void* tt_mem = nullptr;

static void set_hash_kb(int kb) {
  free(tt_mem);
  size_t bytes = size_t(kb < 1 ? 1 : kb) << 10;
  tt_mem = malloc(bytes);
  tt.init(tt_mem, bytes);
}
static void set_hash(int mb) { set_hash_kb(mb << 10); }

void print_info(const SearchReport& r) {
  char line[1024];
  uci_info(r, line, sizeof line);
  puts(line);
  fflush(stdout);
}

static void go(const char* line) {
  Limits lim = uci_go(line, board.stm, 30);
  search_stop = false;
  SearchResult r = search(board, lim, print_info);
  char best[6] = "0000";
  if (r.best) move_to_uci(r.best, best);
  printf("bestmove %s\n", best);
  fflush(stdout);
}

int uci_loop() {
  set_hash(16);
  search_init(&tt);
  board.set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
  char line[16384];
  while (fgets(line, sizeof line, stdin)) {
    char* nl = strpbrk(line, "\r\n");
    if (nl) *nl = 0;
    if (!strcmp(line, "uci")) {
      printf("id name CST Retro\nid author Chris Whittington\n");
      printf("option name Hash type spin default 16 min 1 max 1024\n");
      printf("option name Eval type combo default PeSTO var PeSTO var NNUE\n");
      printf("uciok\n");
    } else if (!strcmp(line, "isready")) {
      printf("readyok\n");
    } else if (!strcmp(line, "ucinewgame")) {
      search_new_game();
    } else if (!strncmp(line, "setoption name Hash value ", 26)) {
      set_hash(atoi(line + 26));
    } else if (!strncmp(line, "setoption name Eval value ", 26)) {
      eval_mode = strcmp(line + 26, "NNUE") ? EVAL_PESTO : EVAL_NNUE;
      if (eval_mode == EVAL_NNUE && !nnue_ready())
        printf("info string no NNUE loaded, using PeSTO\n");
    } else if (!strncmp(line, "position ", 9)) {
      uci_position(board, line + 9);
    } else if (!strncmp(line, "go", 2)) {
      go(line);
    } else if (!strcmp(line, "d")) {
      char fen[100];
      board.to_fen(fen);
      printf("%s\neval %d\n", fen, evaluate(board));
    } else if (!strcmp(line, "quit")) {
      break;
    }
    fflush(stdout);
  }
  return 0;
}

// ---- NNUE (random weights until a trained net exists) ----

static void* nn_w;
static void* nn_acc;

static bool nnue_random(int n, int h) {
  free(nn_w);
  free(nn_acc);
  nn_w = malloc(nnue_weight_bytes(n, h));
  nn_acc = malloc(nnue_acc_bytes(n));
  if (!nnue_setup_random(n, h, 12345, nn_w, nn_acc)) {
    printf("bad NNUE shape %d/%d (multiples of 8, <= %d/%d)\n", n, h, int(NNUE_MAX_N),
           int(NNUE_MAX_H));
    return false;
  }
  return true;
}

static u64 checked, mismatches;
static void check_walk(int depth) {
  checked++;
  if (!nnue_check(board)) mismatches++;
  if (!depth) return;
  Move moves[MAX_MOVES];
  int n = board.gen_legal(moves);
  for (int i = 0; i < n; i++) {
    board.make(moves[i]);
    check_walk(depth - 1);
    board.unmake();
  }
}

// Incremental accumulators == full refresh at every node of depth-3 trees
// (also exercises re-entering sibling lines after unmake), and null moves.
int nnue_test(int n, int h) {
  if (!nnue_random(n, h)) return 1;
  checked = mismatches = 0;
  for (const char* f : SEARCH_BENCH_FENS) {
    board.set_fen(f);
    nnue_new_root(board);
    check_walk(3);
    board.make_null();
    check_walk(2);
    board.unmake_null();
  }
  printf("nncheck 768->%d->%d: %llu positions, %llu mismatches\n", n, h, (unsigned long long)checked,
         (unsigned long long)mismatches);
  return mismatches != 0;
}

// The search bench with the NNUE computed at every node but PeSTO's score
// returned: same tree as sbench, the time difference is the network's cost.
int nnue_bench(int n, int h, int depth, int hash_kb) {
  if (!nnue_random(n, h)) return 1;
  set_hash_kb(hash_kb);
  search_init(&tt);
  eval_mode = EVAL_NNUE_COST;
  u32 t0 = engine_now_ms();
  u64 total = run_search_bench(board, depth);
  u32 ms = engine_now_ms() - t0;
  eval_mode = EVAL_PESTO;
  printf("nnbench 768->%d->%d depth %d hash %d KB: %llu nodes, %u ms, %llu knps\n", n, h, depth, hash_kb,
         (unsigned long long)total, ms, (unsigned long long)(ms ? total / ms : 0));
  return 0;
}

// Fixed-depth search over the shared position set: node count is a determinism signature.
int search_bench(int depth, int hash_kb) {
  set_hash_kb(hash_kb);
  search_init(&tt);
  u32 t0 = engine_now_ms();
  u64 total = run_search_bench(board, depth);
  u32 ms = engine_now_ms() - t0;
  printf("sbench depth %d hash %d KB: %llu nodes, %u ms, %llu knps\n", depth, hash_kb, (unsigned long long)total, ms,
         (unsigned long long)(ms ? total / ms : 0));
  return 0;
}
