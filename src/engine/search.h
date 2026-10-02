// Chess System Tal Retro - search: iterative deepening, aspiration windows,
// PVS with TT, null move, reverse futility, LMR, check extension, quiescence,
// SEE (ordering, quiescence, low-depth pruning).
// Single-threaded; on the ESP32 it runs in its own task while the UI runs on
// the other core.
#pragma once
#include "board.h"
#include "tt.h"

enum { MAX_PLY = 64, INF = 32000, MATE = 31000, MATE_BOUND = MATE - MAX_PLY };

struct Limits {
  int depth = MAX_PLY - 1;
  u64 nodes = 0;    // 0 = no limit
  u32 soft_ms = 0;  // don't start a new iteration after this (0 = none)
  u32 hard_ms = 0;  // abort the search at this (0 = none)
  u64 soft_nodes = 0;  // don't start a new iteration after this many nodes (0 = none)
};

struct SearchReport {
  int depth, seldepth, score;  // score from the side to move, mate = MATE - plies
  u64 nodes;
  u32 ms;
  const Move* pv;
  int pvlen;
};
typedef void (*ReportFn)(const SearchReport&);

struct SearchResult {
  Move best, ponder;
  int score, depth;
  u64 nodes;
};

// Feature switches (for testing one at a time against a baseline).
struct SearchOptions {
  bool null_move = true;
  bool rfp = true;
  bool lmr = true;
  bool check_ext = true;
  bool aspiration = true;
  bool see_order = true;    // losing captures (SEE < 0) ordered after quiets
  bool see_qsearch = true;  // ... and not searched in quiescence
  bool see_prune = true;    // low-depth pruning of moves losing material
};
extern SearchOptions search_options;

// Set from another task/thread to abort the current search.
extern volatile bool search_stop;

// Platform clock in milliseconds (supplied by the host program / firmware).
u32 engine_now_ms();

void search_init(TT* tt);
void search_new_game();  // clears TT, history, killers
SearchResult search(Board& b, const Limits& lim, ReportFn report);
// The quiescence-search score of b (side to move), e.g. to find quiet
// positions (== static eval) for training data. search_init() first.
int quiescence(Board& b);
