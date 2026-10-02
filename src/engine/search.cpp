// Chess System Tal Retro - search. See search.h.
//
// Memory: move lists and their ordering scores live on one shared static
// stack (not the task stack - the ESP32 tasks have small stacks); a node
// whose list would not fit is evaluated statically instead.
#include "search.h"
#include <math.h>
#include <string.h>
#include "eval.h"
#include "nnue.h"
#include "profile.h"
#include "see.h"

SearchOptions search_options;
#ifdef SEARCH_PROFILE
uint64_t prof_total[P_COUNT];
#endif
volatile bool search_stop = false;
const char* search_stack_floor = nullptr;

namespace {

enum { MOVE_STACK = 4096, MAX_QUIETS = 64 };
// Ordering score offset for losing captures: below every quiet (history is
// +-16384, killers 1 << 27).
enum { BAD_CAPTURE = (1 << 28) + (1 << 20), LOSING = -(1 << 19) };

TT* tt;
Move move_stack[MOVE_STACK];
int score_stack[MOVE_STACK];
Move killers[MAX_PLY + 1][2];
int16_t history[16][128];  // [piece][to]
Move pv[MAX_PLY + 1][MAX_PLY + 1];
int pvlen[MAX_PLY + 1];
u8 lmr_table[64][64];

u64 nodes, node_limit;
u32 start_ms, hard_ms;
bool stopped;
int seldepth;

// ---- helpers ----

int score_to_tt(int s, int ply) {
  return s >= MATE_BOUND ? s + ply : s <= -MATE_BOUND ? s - ply : s;
}
int score_from_tt(int s, int ply) {
  return s >= MATE_BOUND ? s - ply : s <= -MATE_BOUND ? s + ply : s;
}

bool is_repetition(const Board& b) {
  for (int i = b.hply - 2; i >= 0 && i >= b.hply - b.rule50; i -= 2)
    if (b.hist[i].key == b.key) return true;
  return false;
}


bool is_quiet(Move m) { return !(m & MF_CAPTURE) && !move_promo(m); }

void check_time() {
  if (search_stop || (node_limit && nodes >= node_limit) ||
      (hard_ms && engine_now_ms() - start_ms >= hard_ms))
    stopped = true;
}

void update_history(int piece, int to, int bonus) {
  int16_t& h = history[piece][to];
  int v = h + bonus - h * (bonus < 0 ? -bonus : bonus) / 16384;
  h = int16_t(v);
}

// Ordering: TT move, captures/promotions by MVV-LVA, killers, quiets by history.
void score_moves(const Board& b, Move* m, int* sc, int n, Move tt_move, int ply) {
  for (int i = 0; i < n; i++) {
    Move mv = m[i] & 0xFFFFFF;
    if (mv == (tt_move & 0xFFFFFF)) sc[i] = 1 << 30;
    else if (!is_quiet(mv)) {
      int victim = (mv & MF_EP) ? PAWN : piece_type(b.sq[move_to(mv)]);
      int v = (mv & MF_CAPTURE) ? ORDER_VALUE[victim] : 0;
      if (move_promo(mv)) v += ORDER_VALUE[move_promo(mv)];
      sc[i] = (1 << 28) + v * 16 - piece_type(b.sq[move_from(mv)]);
      if (search_options.see_order && !see_ge(b, mv, 0)) sc[i] -= BAD_CAPTURE;  // after quiets
    } else if (mv == killers[ply][0]) sc[i] = (1 << 27) + 1;
    else if (mv == killers[ply][1]) sc[i] = 1 << 27;
    else sc[i] = history[b.sq[move_from(mv)]][move_to(mv)];
  }
}

// Bring the best remaining move to position i.
Move pick(Move* m, int* sc, int n, int i) {
  int best = i;
  for (int j = i + 1; j < n; j++)
    if (sc[j] > sc[best]) best = j;
  Move t = m[i]; m[i] = m[best]; m[best] = t;
  int s = sc[i]; sc[i] = sc[best]; sc[best] = s;
  return m[i];
}

// The stack guard (search_stack_floor): this frame is near the bottom of the
// engine task's stack. One compare per node.
inline bool stack_low() {
  char here;
  return search_stack_floor && uintptr_t(&here) < uintptr_t(search_stack_floor);
}

// ---- quiescence ----

int qsearch(Board& b, int alpha, int beta, int ply, int base) {
  pvlen[ply] = ply;
  if ((++nodes & 1023) == 0) check_time();
  if (stopped) return 0;
  if (ply > seldepth) seldepth = ply;
  if (ply >= MAX_PLY || base + MAX_MOVES > MOVE_STACK || stack_low()) return evaluate(b);

  bool hit;
  TTEntry* e;
  PROF(P_TT, e = tt->probe(b.key, hit));
  if (hit) {
    int s = score_from_tt(e->score, ply);
    if ((e->bound() == BOUND_EXACT) || (e->bound() == BOUND_LOWER && s >= beta) ||
        (e->bound() == BOUND_UPPER && s <= alpha))
      return s;
  }

  bool in_check;
  PROF(P_CHECK, in_check = b.in_check());
  int best;
  if (in_check) {
    best = -MATE + ply;
  } else {
    PROF(P_EVAL, best = hit ? e->eval : evaluate(b));
    if (best >= beta) return best;
    if (best > alpha) alpha = best;
  }

  Move* m = move_stack + base;
  int* sc = score_stack + base;
  int n;
  PROF(P_GEN, n = b.gen(m, in_check));  // all evasions when in check, else captures/promotions
  PROF(P_ORDER, score_moves(b, m, sc, n, hit ? e->move : 0, ply));
  int legal = 0;
  for (int i = 0; i < n; i++) {
    Move mv;
    PROF(P_ORDER, mv = pick(m, sc, n, i));
    // Losing captures come last: the rest is not worth searching here.
    if (!in_check && search_options.see_qsearch && sc[i] < LOSING) break;
    PROF(P_MAKE, b.make(mv));
    bool bad;
    PROF(P_CHECK, bad = b.leaves_check(mv, in_check));
    if (bad) { PROF(P_MAKE, b.unmake()); continue; }
    legal++;
    int s = -qsearch(b, -beta, -alpha, ply + 1, base + n);
    PROF(P_MAKE, b.unmake());
    if (stopped) return 0;
    if (s > best) {
      best = s;
      if (s > alpha) {
        alpha = s;
        if (s >= beta) break;
      }
    }
  }
  if (in_check && !legal) return -MATE + ply;
  return best;
}

// ---- main search ----

int search(Board& b, int alpha, int beta, int depth, int ply, int base, bool null_ok,
           bool in_check) {
  if (depth <= 0) return qsearch(b, alpha, beta, ply, base);
  pvlen[ply] = ply;
  if ((++nodes & 1023) == 0) check_time();
  if (stopped) return 0;
  if (ply > seldepth) seldepth = ply;

  const bool pv_node = beta - alpha > 1;
  const bool root = ply == 0;
  if (!root) {
    if (b.rule50 >= 100 || is_repetition(b) || insufficient_material(b)) return 0;
    if (ply >= MAX_PLY || base + MAX_MOVES > MOVE_STACK || stack_low()) return evaluate(b);
    // Mate distance pruning.
    if (alpha < -MATE + ply) alpha = -MATE + ply;
    if (beta > MATE - ply - 1) beta = MATE - ply - 1;
    if (alpha >= beta) return alpha;
  }

  bool hit;
  TTEntry* e;
  PROF(P_TT, e = tt->probe(b.key, hit));
  Move tt_move = hit ? e->move : 0;
  if (hit && !pv_node && e->depth >= depth) {
    int s = score_from_tt(e->score, ply);
    if ((e->bound() == BOUND_EXACT) || (e->bound() == BOUND_LOWER && s >= beta) ||
        (e->bound() == BOUND_UPPER && s <= alpha))
      return s;
  }

  int static_eval;
  PROF(P_EVAL, static_eval = in_check ? -INF : hit ? e->eval : evaluate(b));

  if (!pv_node && !in_check) {
    // Reverse futility: far above beta near the leaves.
    if (search_options.rfp && depth <= 6 && static_eval - 80 * depth >= beta &&
        beta > -MATE_BOUND && beta < MATE_BOUND)
      return static_eval;
    // Null move.
    if (search_options.null_move && null_ok && depth >= 3 && static_eval >= beta &&
        b.has_non_pawn(b.stm)) {
      int r = 3 + depth / 4;
      b.make_null();
      int s = -search(b, -beta, -beta + 1, depth - 1 - r, ply + 1, base, false, false);
      b.unmake_null();
      if (stopped) return 0;
      if (s >= beta) return s >= MATE_BOUND ? beta : s;
    }
  }

  Move* m = move_stack + base;
  int* sc = score_stack + base;
  int n;
  PROF(P_GEN, n = b.gen(m));
  PROF(P_ORDER, score_moves(b, m, sc, n, tt_move, ply));

  // The quiet moves searched before a cutoff get a history malus. Rather than
  // a local list (256 bytes a ply: the 7" board's 16 KB engine stack
  // overflowed at ~35 plies in a long endgame search), each picked move's
  // score slot - not used again once picked - marks it: 1 = a searched quiet.
  int nquiets = 0, legal = 0, best = -INF;
  Move best_move = 0;
  const int orig_alpha = alpha;

  for (int i = 0; i < n; i++) {
    Move mv;
    PROF(P_ORDER, mv = pick(m, sc, n, i));
    sc[i] = 0;  // not (yet) a searched quiet
    // SEE pruning: near the leaves, skip moves that lose material outright
    // (never the first move, the TT move, or when in check).
    if (search_options.see_prune && !root && !in_check && legal && depth <= 8 &&
        mv != tt_move && best > -MATE_BOUND) {
      int threshold = is_quiet(mv) ? -30 * depth * depth : -100 * depth;
      if (!see_ge(b, mv, threshold)) continue;
    }
    PROF(P_MAKE, b.make(mv));
    bool bad;
    PROF(P_CHECK, bad = b.leaves_check(mv, in_check));
    if (bad) { PROF(P_MAKE, b.unmake()); continue; }
    legal++;
    const bool quiet = is_quiet(mv);
    bool gives_check;
    PROF(P_CHECK, gives_check = b.in_check());
    int new_depth = depth - 1 + (search_options.check_ext && gives_check ? 1 : 0);

    int s;
    if (legal == 1) {
      s = -search(b, -beta, -alpha, new_depth, ply + 1, base + n, true, gives_check);
    } else {
      int r = 0;
      if (search_options.lmr && depth >= 3 && quiet && !in_check && !gives_check && legal > 3) {
        r = lmr_table[depth < 63 ? depth : 63][legal < 63 ? legal : 63];
        if (pv_node) r--;
        if (mv == killers[ply][0] || mv == killers[ply][1]) r--;
        if (r < 0) r = 0;
        if (r > new_depth - 1) r = new_depth - 1;
      }
      s = -search(b, -alpha - 1, -alpha, new_depth - r, ply + 1, base + n, true, gives_check);
      if (s > alpha && r > 0)
        s = -search(b, -alpha - 1, -alpha, new_depth, ply + 1, base + n, true, gives_check);
      if (s > alpha && s < beta)
        s = -search(b, -beta, -alpha, new_depth, ply + 1, base + n, true, gives_check);
    }
    PROF(P_MAKE, b.unmake());
    if (stopped) return 0;

    if (s > best) {
      best = s;
      if (s > alpha) {
        alpha = s;
        best_move = mv;
        pv[ply][ply] = mv;
        for (int j = ply + 1; j < pvlen[ply + 1]; j++) pv[ply][j] = pv[ply + 1][j];
        pvlen[ply] = pvlen[ply + 1] > ply + 1 ? pvlen[ply + 1] : ply + 1;
        if (s >= beta) {
          if (quiet) {
            if (killers[ply][0] != mv) {
              killers[ply][1] = killers[ply][0];
              killers[ply][0] = mv;
            }
            int bonus = depth * depth > 1200 ? 1200 : depth * depth;
            update_history(b.sq[move_from(mv)], move_to(mv), bonus);
            for (int q = 0; q < i; q++)
              if (sc[q] == 1) update_history(b.sq[move_from(m[q])], move_to(m[q]), -bonus);
          }
          break;
        }
      }
    }
    if (quiet && nquiets < MAX_QUIETS) {
      sc[i] = 1;
      nquiets++;
    }
  }

  if (!legal) return in_check ? -MATE + ply : 0;

  int bound = best >= beta ? BOUND_LOWER : alpha > orig_alpha ? BOUND_EXACT : BOUND_UPPER;
  PROF(P_TT, tt->store(e, b.key, best_move, score_to_tt(best, ply), in_check ? 0 : static_eval,
                       depth, bound));
  return best;
}

}  // namespace

// ---- public ----

void search_init(TT* t) {
  tt = t;
  init_eval();
  for (int d = 0; d < 64; d++)
    for (int m = 0; m < 64; m++)
      lmr_table[d][m] = d && m ? u8(0.75 + log(double(d)) * log(double(m)) / 2.25) : 0;
}

int quiescence(Board& b) {
  stopped = false;
  hard_ms = 0;
  node_limit = 0;
  return qsearch(b, -INF, INF, 0, 0);
}

void search_new_game() {
  tt->clear();
  memset(history, 0, sizeof history);
  memset(killers, 0, sizeof killers);
}

SearchResult search(Board& b, const Limits& lim, ReportFn report) {
  SearchResult r = {0, 0, 0, 0, 0};
  start_ms = engine_now_ms();
  hard_ms = lim.hard_ms;
  node_limit = lim.nodes;
  nodes = 0;
  stopped = false;
  tt->new_search();
  if (eval_mode != EVAL_PESTO) nnue_new_root(b);
  memset(killers, 0, sizeof killers);
  for (auto& row : history)
    for (auto& h : row) h = int16_t(h / 2);

  // Fallback move in case even depth 1 is interrupted; one legal move = play it.
  Move legal[MAX_MOVES];
  int nlegal = b.gen_legal(legal);
  if (!nlegal) return r;
  r.best = legal[0];
  Move best_pv[MAX_PLY + 1];
  int best_pvlen = 0;

  int score = 0;
  for (int depth = 1; depth <= lim.depth; depth++) {
    seldepth = 0;
    int delta = 25;
    int alpha = -INF, beta = INF;
    if (search_options.aspiration && depth >= 4) {
      alpha = score - delta > -INF ? score - delta : -INF;
      beta = score + delta < INF ? score + delta : INF;
    }
    int s;
    for (;;) {
      s = search(b, alpha, beta, depth, 0, 0, false, b.in_check());
      if (stopped) break;
      if (s <= alpha) {
        beta = (alpha + beta) / 2;
        alpha = s - delta > -INF ? s - delta : -INF;
      } else if (s >= beta) {
        beta = s + delta < INF ? s + delta : INF;
      } else {
        break;
      }
      delta += delta;
    }
    if (stopped) break;

    score = s;
    best_pvlen = pvlen[0];
    memcpy(best_pv, pv[0], sizeof(Move) * best_pvlen);
    r.best = best_pv[0];
    r.ponder = best_pvlen > 1 ? best_pv[1] : 0;
    r.score = score;
    r.depth = depth;
    if (report) {
      SearchReport rep = {depth, seldepth, score, nodes, engine_now_ms() - start_ms, best_pv,
                          best_pvlen};
      report(rep);
    }
    if (nlegal == 1) break;
    if (lim.soft_ms && engine_now_ms() - start_ms >= lim.soft_ms) break;
    if (lim.soft_nodes && nodes >= lim.soft_nodes) break;
    if (score >= MATE_BOUND && MATE - score <= depth) break;  // found a mate we can't improve
  }
  r.nodes = nodes;
  return r;
}
