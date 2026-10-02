// Chess System Tal Retro - attacks and move generation on the 0x88 mailbox +
// piece lists (the default; movegen_bb.cpp is the -DBITBOARD alternative).
#ifndef BITBOARD
#include "board.h"

static const int KNIGHT_DIRS[8] = {33, 31, 18, 14, -33, -31, -18, -14};
static const int KING_DIRS[8] = {1, 15, 16, 17, -1, -15, -16, -17};
static const int BISHOP_DIRS[4] = {15, 17, -15, -17};
static const int ROOK_DIRS[4] = {1, 16, -1, -16};

// ---- attacks ----

// 0x88 delta tables, indexed by target - from + 119:
//   ATTACK_MASK: which piece types could attack along that delta on an empty
//   board (bit per type; pawns split by colour into bits 6 / 7)
//   STEP: the unit step from 'from' toward 'target' on a queen line, else 0
enum { WPAWN_BIT = 1 << 6, BPAWN_BIT = 1 << 7 };
static u8 ATTACK_MASK[240];
static int8_t STEP[240];

static struct DeltaInit {
  DeltaInit() {
    for (int d : KNIGHT_DIRS) ATTACK_MASK[d + 119] |= 1 << KNIGHT;
    for (int d : KING_DIRS) ATTACK_MASK[d + 119] |= 1 << KING;
    ATTACK_MASK[15 + 119] |= WPAWN_BIT;
    ATTACK_MASK[17 + 119] |= WPAWN_BIT;
    ATTACK_MASK[-15 + 119] |= BPAWN_BIT;
    ATTACK_MASK[-17 + 119] |= BPAWN_BIT;
    for (int dir = 0; dir < 8; dir++) {
      int d = KING_DIRS[dir];
      bool diag = d == 15 || d == 17 || d == -15 || d == -17;
      for (int from = 0; from < 128; from++) {
        if (!on_board(from)) continue;
        for (int t = from + d; on_board(t); t += d) {
          ATTACK_MASK[t - from + 119] |= u8((1 << QUEEN) | (1 << (diag ? BISHOP : ROOK)));
          STEP[t - from + 119] = int8_t(d);
        }
      }
    }
  }
} delta_init;

bool Board::attacked(int s, int by) const {
  for (int i = 0; i < count[by]; i++) {
    int from = list[by][i], p = sq[from], t = piece_type(p);
    int mask = ATTACK_MASK[s - from + 119];
    if (t == PAWN) {
      if (mask & (by == WHITE ? WPAWN_BIT : BPAWN_BIT)) return true;
      continue;
    }
    if (!(mask & (1 << t))) continue;
    if (t == KNIGHT || t == KING) return true;
    int step = STEP[s - from + 119], x = from + step;
    while (x != s && sq[x] == EMPTY) x += step;
    if (x == s) return true;
  }
  return false;
}

bool Board::leaves_check(Move m, bool was_in_check) const {
  int us = stm ^ 1, k = list[us][0];
  if (was_in_check || k == move_to(m) || (m & MF_EP)) return attacked(k, stm);
  // Otherwise only a piece pinned on a line through 'from' can expose the king.
  int from = move_from(m), step = STEP[from - k + 119];
  if (!step) return false;
  int x = k + step;
  while (x != from && sq[x] == EMPTY) x += step;
  if (x != from) return false;  // something (maybe the moved piece) still shields
  for (x = from + step; on_board(x) && sq[x] == EMPTY;) x += step;
  if (!on_board(x) || piece_color(sq[x]) != stm) return false;
  int t = piece_type(sq[x]);
  bool diag = step == 15 || step == 17 || step == -15 || step == -17;
  return t == QUEEN || t == (diag ? BISHOP : ROOK);
}

// ---- move generation ----

int Board::gen(Move* out, bool quiets) const {
  Move* m = out;
  int us = stm, them = us ^ 1;
  for (int i = 0; i < count[us]; i++) {
    int from = list[us][i], t = piece_type(sq[from]);
    switch (t) {
      case PAWN: {
        int up = us == WHITE ? 16 : -16;
        int start_rank = us == WHITE ? 1 : 6, last_rank = us == WHITE ? 7 : 0;
        int to = from + up;
        bool promo = sq_rank(to) == last_rank;
        if (sq[to] == EMPTY) {
          if (promo) {
            for (int pt = QUEEN; pt >= KNIGHT; pt--) *m++ = make_move(from, to, 0, pt);
          } else if (quiets) {
            *m++ = make_move(from, to);
            if (sq_rank(from) == start_rank && sq[to + up] == EMPTY)
              *m++ = make_move(from, to + up, MF_DOUBLE);
          }
        }
        for (int side = -1; side <= 1; side += 2) {
          int c = to + side;
          if (!on_board(c)) continue;
          if (sq[c] != EMPTY && piece_color(sq[c]) == them) {
            if (promo) {
              for (int pt = QUEEN; pt >= KNIGHT; pt--) *m++ = make_move(from, c, MF_CAPTURE, pt);
            } else {
              *m++ = make_move(from, c, MF_CAPTURE);
            }
          } else if (c == ep) {
            *m++ = make_move(from, c, MF_CAPTURE | MF_EP);
          }
        }
        break;
      }
      case KNIGHT:
      case KING: {
        const int* dirs = t == KNIGHT ? KNIGHT_DIRS : KING_DIRS;
        for (int k = 0; k < 8; k++) {
          int to = from + dirs[k];
          if (!on_board(to)) continue;
          if (sq[to] == EMPTY) { if (quiets) *m++ = make_move(from, to); }
          else if (piece_color(sq[to]) == them) *m++ = make_move(from, to, MF_CAPTURE);
        }
        break;
      }
      default: {  // sliders
        const int* dirs = t == ROOK ? ROOK_DIRS : BISHOP_DIRS;
        for (int pass = 0; pass < (t == QUEEN ? 2 : 1); pass++) {
          if (pass) dirs = ROOK_DIRS;
          for (int k = 0; k < 4; k++) {
            int d = dirs[k];
            for (int to = from + d; on_board(to); to += d) {
              if (sq[to] == EMPTY) { if (quiets) *m++ = make_move(from, to); continue; }
              if (piece_color(sq[to]) == them) *m++ = make_move(from, to, MF_CAPTURE);
              break;
            }
          }
        }
        break;
      }
    }
  }
  // Castling: rights imply king and rook are on their home squares (set_fen
  // drops impossible rights, make removes them as pieces leave/are taken).
  int base = us == WHITE ? 0x00 : 0x70;
  int ks = us == WHITE ? WK_CASTLE : BK_CASTLE, qs = us == WHITE ? WQ_CASTLE : BQ_CASTLE;
  if (quiets && (castle & (ks | qs))) {
    int k = base + 4;
    if ((castle & ks) && sq[k + 1] == EMPTY && sq[k + 2] == EMPTY && !attacked(k, them) &&
        !attacked(k + 1, them) && !attacked(k + 2, them))
      *m++ = make_move(k, k + 2, MF_CASTLE);
    if ((castle & qs) && sq[k - 1] == EMPTY && sq[k - 2] == EMPTY && sq[k - 3] == EMPTY &&
        !attacked(k, them) && !attacked(k - 1, them) && !attacked(k - 2, them))
      *m++ = make_move(k, k - 2, MF_CASTLE);
  }
  return int(m - out);
}

bool Board::has_non_pawn(int c) const {
  for (int i = 1; i < count[c]; i++)
    if (piece_type(sq[list[c][i]]) != PAWN) return true;
  return false;
}

bool Board::no_mating_material() const {
  int n = count[WHITE] + count[BLACK];
  if (n == 2) return true;
  if (n == 3)
    for (int c = 0; c < 2; c++)
      for (int i = 1; i < count[c]; i++) {
        int t = piece_type(sq[list[c][i]]);
        if (t == KNIGHT || t == BISHOP) return true;
      }
  return false;
}

#endif  // !BITBOARD
