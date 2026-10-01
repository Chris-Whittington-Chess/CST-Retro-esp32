// Chess System Tal Retro - minimal UCI front end, PC only, for engine-vs-engine
// test matches (the device itself has no UCI). Search runs synchronously:
// "stop" is not needed by cutechess for timed games.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "../src/engine/bench.h"
#include "../src/engine/board.h"
#include "../src/engine/eval.h"
#include "../src/engine/search.h"
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
  char score[32];
  if (r.score >= MATE_BOUND) snprintf(score, sizeof score, "mate %d", (MATE - r.score + 1) / 2);
  else if (r.score <= -MATE_BOUND) snprintf(score, sizeof score, "mate %d", -(MATE + r.score) / 2);
  else snprintf(score, sizeof score, "cp %d", r.score);
  printf("info depth %d seldepth %d score %s nodes %llu time %u nps %llu pv", r.depth, r.seldepth,
         score, (unsigned long long)r.nodes, r.ms,
         (unsigned long long)(r.ms ? r.nodes * 1000 / r.ms : 0));
  for (int i = 0; i < r.pvlen; i++) {
    char buf[6];
    move_to_uci(r.pv[i], buf);
    printf(" %s", buf);
  }
  printf("\n");
  fflush(stdout);
}

static void position(const char* args) {
  const char* moves = strstr(args, "moves");
  if (!strncmp(args, "startpos", 8)) {
    board.set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
  } else if (!strncmp(args, "fen ", 4)) {
    std::string fen = moves ? std::string(args + 4, moves - args - 4) : std::string(args + 4);
    board.set_fen(fen.c_str());
  }
  if (!moves) return;
  for (const char* p = moves + 5; *p;) {
    while (*p == ' ') p++;
    char mv[8] = {0};
    int n = 0;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r' && n < 7) mv[n++] = *p++;
    if (!n) break;
    Move m = parse_uci(board, mv);
    if (!m) break;
    board.make(m);
    if (board.hply >= MAX_GAME - MAX_PLY - 1) break;
  }
}

static long arg(const char* line, const char* name, long def) {
  const char* p = strstr(line, name);
  if (!p) return def;
  return atol(p + strlen(name));
}

static void go(const char* line) {
  Limits lim;
  lim.depth = int(arg(line, " depth ", MAX_PLY - 1));
  lim.nodes = u64(arg(line, " nodes ", 0));
  long movetime = arg(line, " movetime ", 0);
  long time = arg(line, board.stm == WHITE ? " wtime " : " btime ", -1);
  long inc = arg(line, board.stm == WHITE ? " winc " : " binc ", 0);
  long mtg = arg(line, " movestogo ", 0);
  if (movetime) {
    lim.soft_ms = lim.hard_ms = u32(movetime);
  } else if (time >= 0) {
    const long overhead = 30;
    long left = time - overhead > 1 ? time - overhead : 1;
    long soft = left / (mtg ? mtg + 1 : 25) + inc * 3 / 4;
    long hard = soft * 4;
    if (hard > left / 2 + inc / 2) hard = left / 2 + inc / 2;
    if (soft > hard) soft = hard;
    lim.soft_ms = u32(soft > 1 ? soft : 1);
    lim.hard_ms = u32(hard > 1 ? hard : 1);
  }
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
      position(line + 9);
    } else if (!strncmp(line, "go", 2)) {
      std::string l = std::string(line) + " ";
      go(l.c_str());
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
