// Chess System Tal Retro - board, make/unmake, move generation. See board.h.
#include "board.h"
#include <string.h>
#include "polyglot.h"

static const int KNIGHT_DIRS[8] = {33, 31, 18, 14, -33, -31, -18, -14};
static const int KING_DIRS[8] = {1, 15, 16, 17, -1, -15, -16, -17};
static const int BISHOP_DIRS[4] = {15, 17, -15, -17};
static const int ROOK_DIRS[4] = {1, 16, -1, -16};

// Castling rights kept when a move touches a square.
static u8 castle_mask(int s) {
  switch (s) {
    case 0x00: return u8(~WQ_CASTLE);
    case 0x07: return u8(~WK_CASTLE);
    case 0x04: return u8(~(WK_CASTLE | WQ_CASTLE));
    case 0x70: return u8(~BQ_CASTLE);
    case 0x77: return u8(~BK_CASTLE);
    case 0x74: return u8(~(BK_CASTLE | BQ_CASTLE));
    default: return 0xFF;
  }
}

static inline u64 piece_key(int p, int s) {
  int kind = 2 * piece_type(p) + (piece_color(p) == WHITE);
  return Polyglot64[64 * kind + sq64(s)];
}
static inline u64 castle_key(int rights) {
  u64 k = 0;
  for (int i = 0; i < 4; i++)
    if (rights & (1 << i)) k ^= Polyglot64[768 + i];
  return k;
}
static inline u64 ep_key(int ep) { return ep == NO_SQ ? 0 : Polyglot64[772 + sq_file(ep)]; }
static const u64 SIDE_KEY = Polyglot64[780];

// ---- piece placement (keeps lists, idx and key in step) ----

void Board::put(int s, int p) {
  int c = piece_color(p);
  sq[s] = u8(p);
  idx[s] = count[c];
  list[c][count[c]++] = u8(s);
  key ^= piece_key(p, s);
}

void Board::remove(int s) {
  int p = sq[s], c = piece_color(p);
  int i = idx[s], last = list[c][--count[c]];
  list[c][i] = u8(last);
  idx[last] = u8(i);
  sq[s] = EMPTY;
  key ^= piece_key(p, s);
}

void Board::shift(int from, int to) {
  int p = sq[from], c = piece_color(p);
  sq[to] = u8(p);
  sq[from] = EMPTY;
  idx[to] = idx[from];
  list[c][idx[to]] = u8(to);
  key ^= piece_key(p, from) ^ piece_key(p, to);
}

// ep square only counts (for the key and FEN-equality) when a pawn of the side
// to move stands beside the pawn that just double-pushed - Polyglot's rule.
static int ep_if_capturable(const Board& b, int to, int mover) {
  int enemy_pawn = make_piece(mover ^ 1, PAWN);
  int target = to + (mover == WHITE ? -16 : 16);
  if ((on_board(to - 1) && b.sq[to - 1] == enemy_pawn) ||
      (on_board(to + 1) && b.sq[to + 1] == enemy_pawn))
    return target;
  return NO_SQ;
}

// ---- FEN ----

bool Board::set_fen(const char* fen) {
  memset(sq, EMPTY, sizeof sq);
  count[0] = count[1] = 0;
  key = 0;
  hply = 0;
  // Collect the pieces, then place kings first so they sit at list index 0.
  int n = 0;
  const char* p = fen;
  int rank = 7, file = 0;
  struct Placed { int s, piece; } placed[64];
  for (; *p && *p != ' '; p++) {
    char ch = *p;
    if (ch == '/') { rank--; file = 0; continue; }
    if (ch >= '1' && ch <= '8') { file += ch - '0'; continue; }
    const char* names = "pnbrqk";
    int t = -1;
    for (int i = 0; i < 6; i++)
      if ((ch | 32) == names[i]) t = i;
    if (t < 0 || rank < 0 || file > 7 || n >= 64) return false;
    placed[n++] = {rank * 16 + file, make_piece(ch >= 'a' ? BLACK : WHITE, t)};
    file++;
  }
  int kings = 0;
  for (int i = 0; i < n; i++)
    if (piece_type(placed[i].piece) == KING) { put(placed[i].s, placed[i].piece); kings++; }
  if (kings != 2 || piece_color(sq[list[WHITE][0]]) != WHITE || count[BLACK] != 1) return false;
  for (int i = 0; i < n; i++)
    if (piece_type(placed[i].piece) != KING) {
      if (count[piece_color(placed[i].piece)] >= 16) return false;
      put(placed[i].s, placed[i].piece);
    }
  while (*p == ' ') p++;
  stm = (*p == 'b') ? BLACK : WHITE;
  if (*p) p++;
  while (*p == ' ') p++;
  castle = 0;
  for (; *p && *p != ' '; p++) {
    if (*p == 'K') castle |= WK_CASTLE;
    if (*p == 'Q') castle |= WQ_CASTLE;
    if (*p == 'k') castle |= BK_CASTLE;
    if (*p == 'q') castle |= BQ_CASTLE;
  }
  // Drop rights the position cannot have (keeps make's mask logic simple).
  if (sq[0x04] != make_piece(WHITE, KING)) castle &= ~(WK_CASTLE | WQ_CASTLE);
  if (sq[0x07] != make_piece(WHITE, ROOK)) castle &= ~WK_CASTLE;
  if (sq[0x00] != make_piece(WHITE, ROOK)) castle &= ~WQ_CASTLE;
  if (sq[0x74] != make_piece(BLACK, KING)) castle &= ~(BK_CASTLE | BQ_CASTLE);
  if (sq[0x77] != make_piece(BLACK, ROOK)) castle &= ~BK_CASTLE;
  if (sq[0x70] != make_piece(BLACK, ROOK)) castle &= ~BQ_CASTLE;
  while (*p == ' ') p++;
  ep = NO_SQ;
  if (p[0] >= 'a' && p[0] <= 'h' && p[1] >= '1' && p[1] <= '8') {
    int target = (p[1] - '1') * 16 + (p[0] - 'a');
    int pushed = target + (stm == WHITE ? -16 : 16);  // the pawn that double-pushed
    ep = u8(ep_if_capturable(*this, pushed, stm ^ 1));
    p += 2;
  }
  while (*p && *p != ' ') p++;
  int r50 = 0;
  if (*p == ' ') {
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') r50 = r50 * 10 + (*p++ - '0');
  }
  rule50 = u8(r50 > 255 ? 255 : r50);
  key = compute_key();
  return !attacked(list[stm ^ 1][0], stm);  // side not to move may not be in check
}

void Board::to_fen(char* out) const {
  const char* names = "PNBRQK";
  for (int r = 7; r >= 0; r--) {
    int empty = 0;
    for (int f = 0; f < 8; f++) {
      int p = sq[r * 16 + f];
      if (p == EMPTY) { empty++; continue; }
      if (empty) { *out++ = char('0' + empty); empty = 0; }
      char ch = names[piece_type(p)];
      *out++ = piece_color(p) == BLACK ? char(ch | 32) : ch;
    }
    if (empty) *out++ = char('0' + empty);
    if (r) *out++ = '/';
  }
  *out++ = ' ';
  *out++ = stm == WHITE ? 'w' : 'b';
  *out++ = ' ';
  if (!castle) *out++ = '-';
  if (castle & WK_CASTLE) *out++ = 'K';
  if (castle & WQ_CASTLE) *out++ = 'Q';
  if (castle & BK_CASTLE) *out++ = 'k';
  if (castle & BQ_CASTLE) *out++ = 'q';
  *out++ = ' ';
  if (ep == NO_SQ) *out++ = '-';
  else { *out++ = char('a' + sq_file(ep)); *out++ = char('1' + sq_rank(ep)); }
  *out++ = ' ';
  int r50 = rule50;
  if (r50 >= 100) *out++ = char('0' + r50 / 100);
  if (r50 >= 10) *out++ = char('0' + r50 / 10 % 10);
  *out++ = char('0' + r50 % 10);
  int fullmove = 1 + hply / 2;  // relative to the loaded position
  char tmp[8];
  int n = 0;
  do { tmp[n++] = char('0' + fullmove % 10); fullmove /= 10; } while (fullmove);
  *out++ = ' ';
  while (n) *out++ = tmp[--n];
  *out = 0;
}

u64 Board::compute_key() const {
  u64 k = 0;
  for (int c = 0; c < 2; c++)
    for (int i = 0; i < count[c]; i++) k ^= piece_key(sq[list[c][i]], list[c][i]);
  k ^= castle_key(castle) ^ ep_key(ep);
  if (stm == WHITE) k ^= SIDE_KEY;
  return k;
}

// ---- attacks ----

bool Board::attacked(int s, int by) const {
  // Pawns: a white pawn attacks s from s-15 / s-17, a black one from s+15 / s+17.
  int pawn = make_piece(by, PAWN);
  int pd = by == WHITE ? -16 : 16;
  if (on_board(s + pd - 1) && sq[s + pd - 1] == pawn) return true;
  if (on_board(s + pd + 1) && sq[s + pd + 1] == pawn) return true;
  int knight = make_piece(by, KNIGHT);
  for (int d : KNIGHT_DIRS)
    if (on_board(s + d) && sq[s + d] == knight) return true;
  int king = make_piece(by, KING);
  for (int d : KING_DIRS)
    if (on_board(s + d) && sq[s + d] == king) return true;
  int bishop = make_piece(by, BISHOP), rook = make_piece(by, ROOK), queen = make_piece(by, QUEEN);
  for (int d : BISHOP_DIRS) {
    int t = s + d;
    while (on_board(t) && sq[t] == EMPTY) t += d;
    if (on_board(t) && (sq[t] == bishop || sq[t] == queen)) return true;
  }
  for (int d : ROOK_DIRS) {
    int t = s + d;
    while (on_board(t) && sq[t] == EMPTY) t += d;
    if (on_board(t) && (sq[t] == rook || sq[t] == queen)) return true;
  }
  return false;
}

// ---- move generation ----

int Board::gen(Move* out) const {
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
          } else {
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
          if (sq[to] == EMPTY) *m++ = make_move(from, to);
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
              if (sq[to] == EMPTY) { *m++ = make_move(from, to); continue; }
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
  if (castle & (ks | qs)) {
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

int Board::gen_legal(Move* out) {
  Move tmp[MAX_MOVES];
  int n = gen(tmp), legal = 0;
  for (int i = 0; i < n; i++) {
    make(tmp[i]);
    if (!illegal()) out[legal++] = tmp[i];
    unmake();
  }
  return legal;
}

// ---- make / unmake ----

void Board::make(Move m) {
  Undo& u = hist[hply++];
  u.key = key;
  u.move = m;
  u.castle = castle;
  u.ep = ep;
  u.rule50 = rule50;
  int from = move_from(m), to = move_to(m), us = stm;
  int p = sq[from];

  key ^= ep_key(ep) ^ castle_key(castle) ^ SIDE_KEY;
  ep = NO_SQ;
  rule50++;

  if (m & MF_EP) {
    int victim = to + (us == WHITE ? -16 : 16);
    u.captured = sq[victim];
    remove(victim);
  } else {
    u.captured = sq[to];
    if (u.captured != EMPTY) remove(to);
  }
  if (u.captured != EMPTY || piece_type(p) == PAWN) rule50 = 0;

  shift(from, to);
  if (int promo = move_promo(m)) {
    remove(to);
    put(to, make_piece(us, promo));
  } else if (m & MF_DOUBLE) {
    ep = u8(ep_if_capturable(*this, to, us));
  } else if (m & MF_CASTLE) {
    if (to > from) shift(to + 1, to - 1);  // h-rook to f
    else shift(to - 2, to + 1);            // a-rook to d
  }

  castle &= castle_mask(from) & castle_mask(to);
  stm = u8(us ^ 1);
  key ^= ep_key(ep) ^ castle_key(castle);
}

void Board::unmake() {
  Undo& u = hist[--hply];
  Move m = u.move;
  int from = move_from(m), to = move_to(m);
  stm ^= 1;
  int us = stm;

  if (move_promo(m)) {
    remove(to);
    put(to, make_piece(us, PAWN));
  } else if (m & MF_CASTLE) {
    if (to > from) shift(to - 1, to + 1);
    else shift(to + 1, to - 2);
  }
  shift(to, from);
  if (u.captured != EMPTY) {
    int s = (m & MF_EP) ? to + (us == WHITE ? -16 : 16) : to;
    put(s, u.captured);
  }
  castle = u.castle;
  ep = u.ep;
  rule50 = u.rule50;
  key = u.key;
}

// ---- notation, perft ----

void move_to_uci(Move m, char* out) {
  int f = move_from(m), t = move_to(m);
  out[0] = char('a' + sq_file(f));
  out[1] = char('1' + sq_rank(f));
  out[2] = char('a' + sq_file(t));
  out[3] = char('1' + sq_rank(t));
  int n = 4;
  if (int p = move_promo(m)) out[n++] = " nbrq"[p];
  out[n] = 0;
}

Move parse_uci(Board& b, const char* s) {
  Move moves[MAX_MOVES];
  int n = b.gen_legal(moves);
  char buf[6];
  for (int i = 0; i < n; i++) {
    move_to_uci(moves[i], buf);
    if (!strcmp(buf, s)) return moves[i];
  }
  return 0;
}

u64 perft(Board& b, int depth) {
  Move moves[MAX_MOVES];
  int n = b.gen(moves);
  u64 nodes = 0;
  for (int i = 0; i < n; i++) {
    b.make(moves[i]);
    if (!b.illegal()) nodes += depth > 1 ? perft(b, depth - 1) : 1;
    b.unmake();
  }
  return nodes;
}
