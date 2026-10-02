// Chess System Tal Retro - static exchange evaluation. See see.h.
#include "see.h"
#include <string.h>

const int SEE_VALUE[8] = {100, 320, 330, 500, 900, 20000, 0, 0};

namespace {

const int KNIGHT_D[8] = {33, 31, 18, 14, -33, -31, -18, -14};
const int KING_D[8] = {1, 15, 16, 17, -1, -15, -16, -17};
const int DIAG_D[4] = {15, 17, -15, -17};
const int ORTHO_D[4] = {1, 16, -1, -16};

// The least valuable piece of 'side' attacking 'to', ignoring removed[]
// squares (x-rays come through them); returns its square, or -1.
int least_attacker(const Board& b, int to, int side, const u8* removed, int& type) {
  int best = -1, best_type = 7;
  auto consider = [&](int s, int t) {
    if (t < best_type) best = s, best_type = t;
  };
  int pawn = make_piece(side, PAWN);
  int back = side == WHITE ? -16 : 16;  // a pawn of 'side' attacks 'to' from to+back+-1
  for (int d = -1; d <= 1; d += 2) {
    int s = to + back + d;
    if (on_board(s) && !removed[s] && b.sq[s] == pawn) {
      type = PAWN;
      return s;  // nothing is cheaper
    }
  }
  for (int d : KNIGHT_D) {
    int s = to + d;
    if (on_board(s) && !removed[s] && b.sq[s] == make_piece(side, KNIGHT)) {
      type = KNIGHT;
      return s;
    }
  }
  for (int k = 0; k < 2; k++) {
    const int* dirs = k ? ORTHO_D : DIAG_D;
    int own_slider = k ? ROOK : BISHOP;
    for (int i = 0; i < 4; i++) {
      int s = to + dirs[i];
      while (on_board(s) && (b.sq[s] == EMPTY || removed[s])) s += dirs[i];
      if (!on_board(s)) continue;
      int p = b.sq[s];
      if (piece_color(p) != side) continue;
      int t = piece_type(p);
      if (t == own_slider || t == QUEEN) consider(s, t);
    }
  }
  if (best_type > KING)
    for (int d : KING_D) {
      int s = to + d;
      if (on_board(s) && !removed[s] && b.sq[s] == make_piece(side, KING)) consider(s, KING);
    }
  type = best_type;
  return best;
}

}  // namespace

bool see_ge(const Board& b, Move m, int threshold) {
  if (m & MF_CASTLE) return 0 >= threshold;
  int from = move_from(m), to = move_to(m), us = b.stm;
  int captured = (m & MF_EP) ? PAWN : piece_type(b.sq[to]);
  int swap = (captured == EMPTY ? 0 : SEE_VALUE[captured]) - threshold;
  if (move_promo(m)) swap += SEE_VALUE[move_promo(m)] - SEE_VALUE[PAWN];
  if (swap < 0) return false;
  int mover = move_promo(m) ? move_promo(m) : piece_type(b.sq[from]);
  swap = SEE_VALUE[mover] - swap;
  if (swap <= 0) return true;

  u8 removed[128];
  memset(removed, 0, sizeof removed);
  removed[from] = 1;
  if (m & MF_EP) removed[to + (us == WHITE ? -16 : 16)] = 1;
  int side = us, res = 1;
  for (;;) {
    side ^= 1;
    int type, s = least_attacker(b, to, side, removed, type);
    if (s < 0) break;
    res ^= 1;
    if (type == KING) {  // the king may only capture if the other side has no attacker left
      int t2;
      return least_attacker(b, to, side ^ 1, removed, t2) >= 0 ? res ^ 1 : res;
    }
    swap = SEE_VALUE[type] - swap;
    if (swap < res) break;
    removed[s] = 1;
  }
  return res != 0;
}

int see_value(const Board& b, Move m) {
  int lo = -30000, hi = 30000;
  while (lo < hi) {
    int mid = lo + (hi - lo + 1) / 2;
    if (see_ge(b, m, mid)) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}
