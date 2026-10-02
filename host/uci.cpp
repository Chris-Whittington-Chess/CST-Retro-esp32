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
static bool nnue_file(const char* path);

void print_info(const SearchReport& r) {
  char line[1024];
  uci_info(r, line, sizeof line);
  puts(line);
  fflush(stdout);
}

// ---- emulating the board's speed (EmulateNPS) ----
// The PC searches ~50x faster than the CoreS3, so plain time odds would mean
// a few ms per move. Instead the engine keeps a virtual clock: a move costs
// nodes / EmulateNPS seconds plus the board's USB overhead, and each move's
// budget comes from that virtual clock (same time management as the board)
// as node limits. The real clock barely moves; a real-time safety stop stays.
static int emulate_nps;       // 0 = off
static int hash_kb_override;  // HashKB option: TT size in KB, ignores Hash
static int32_t vclock = -1;   // virtual ms left, -1 = take the first go's clock
static int virtual_flags;
enum { BOARD_OVERHEAD_MS = 50, BOARD_RESERVE_MS = 100 };

static long go_arg(const char* line, const char* name, long def) {
  size_t n = strlen(name);
  for (const char* p = strstr(line, name); p; p = strstr(p + 1, name))
    if ((p == line || p[-1] == ' ') && p[n] == ' ') return atol(p + n + 1);
  return def;
}

static void go(const char* line) {
  Limits lim = uci_go(line, board.stm, 30);
  long real_left = go_arg(line, board.stm == WHITE ? "wtime" : "btime", -1);
  bool emulate = emulate_nps > 0 && real_left >= 0 && !strstr(line, "movetime");
  int32_t inc = int32_t(go_arg(line, board.stm == WHITE ? "winc" : "binc", 0));
  if (emulate) {
    if (vclock < 0) vclock = int32_t(real_left);  // game start: the same base time
    Limits v = clock_limits(vclock, inc, int(go_arg(line, "movestogo", 0)), BOARD_RESERVE_MS);
    lim.soft_nodes = u64(v.soft_ms) * u64(emulate_nps) / 1000;
    lim.nodes = u64(v.hard_ms) * u64(emulate_nps) / 1000;
    if (lim.nodes < 1) lim.nodes = 1;
    lim.soft_ms = 0;
    lim.hard_ms = u32(real_left / 3 > 1 ? real_left / 3 : 1);  // real-time safety only
  }
  search_stop = false;
  SearchResult r = search(board, lim, print_info);
  if (emulate) {
    int32_t spent = int32_t(r.nodes * 1000 / u64(emulate_nps)) + BOARD_OVERHEAD_MS;
    vclock += inc - spent;
    if (vclock <= 0) {
      virtual_flags++;
      printf("info string virtual flag #%d (the board would have lost on time)\n", virtual_flags);
      vclock = 1;
    }
  }
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
      printf("option name EvalFile type string default <none>\n");
      printf("option name SEE type check default true\n");
      printf("option name HashKB type spin default 0 min 0 max 1048576\n");
      printf("option name EmulateNPS type spin default 0 min 0 max 100000000\n");
      printf("uciok\n");
    } else if (!strcmp(line, "isready")) {
      printf("readyok\n");
    } else if (!strcmp(line, "ucinewgame")) {
      search_new_game();
      vclock = -1;
    } else if (!strncmp(line, "setoption name Hash value ", 26)) {
      if (!hash_kb_override) set_hash(atoi(line + 26));
    } else if (!strncmp(line, "setoption name HashKB value ", 28)) {
      hash_kb_override = atoi(line + 28);  // wins over Hash, in either order (board: 32)
      if (hash_kb_override > 0) set_hash_kb(hash_kb_override);
    } else if (!strncmp(line, "setoption name EmulateNPS value ", 32)) {
      emulate_nps = atoi(line + 32);
    } else if (!strncmp(line, "setoption name SEE value ", 25)) {
      bool on = !strcmp(line + 25, "true");  // all three SEE uses (A/B tests)
      search_options.see_order = search_options.see_qsearch = search_options.see_prune = on;
    } else if (!strncmp(line, "setoption name EvalFile value ", 30)) {
      nnue_file(line + 30);
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

// Load a trained net file (tools/nnue/export.py) into fresh memory.
static bool nnue_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    printf("info string cannot open %s\n", path);
    return false;
  }
  std::string data;
  char chunk[65536];
  size_t got;
  while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) data.append(chunk, got);
  fclose(f);
  int n, h;
  if (!nnue_file_shape(data.data(), data.size(), &n, &h)) {
    printf("info string %s is not a CSTN net\n", path);
    return false;
  }
  free(nn_w);
  free(nn_acc);
  nn_w = malloc(nnue_weight_bytes(n, h));
  nn_acc = malloc(nnue_acc_bytes(n));
  if (!nnue_load(data.data(), data.size(), nn_w, nn_acc)) return false;
  printf("info string NNUE %s: 768->%d%s%s->1\n", path, n, h ? "->" : "",
         h ? std::to_string(h).c_str() : "");
  return true;
}

// cstretro nneval <net.bin> <fens.txt>: the net's eval (cp, side to move) of
// each FEN (text up to ';' or end of line) - for the export parity check.
int nnue_eval_file(int argc, char** argv) {
  if (argc < 4) {
    printf("usage: cstretro nneval <net.bin> <fens.txt>\n");
    return 1;
  }
  if (!nnue_file(argv[2])) return 1;
  FILE* f = fopen(argv[3], "r");
  if (!f) {
    printf("cannot open %s\n", argv[3]);
    return 1;
  }
  char line[512];
  while (fgets(line, sizeof line, f)) {
    char* end = strpbrk(line, ";\r\n");
    if (end) *end = 0;
    if (!line[0] || !board.set_fen(line)) continue;
    nnue_new_root(board);
    printf("%d\n", nnue_evaluate(board));
  }
  fclose(f);
  return 0;
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
