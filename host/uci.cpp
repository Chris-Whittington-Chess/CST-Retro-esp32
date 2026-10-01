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
      printf("uciok\n");
    } else if (!strcmp(line, "isready")) {
      printf("readyok\n");
    } else if (!strcmp(line, "ucinewgame")) {
      search_new_game();
    } else if (!strncmp(line, "setoption name Hash value ", 26)) {
      set_hash(atoi(line + 26));
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
