// Chess System Tal Retro - board, make/unmake, move generation. See board.h.
#include "board.h"
#include <string.h>
#include "polyglot.h"

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

// ---- piece placement (keeps lists or bitboards, psq and key in step) ----

#ifdef BITBOARD
// 0x88 square -> its bitboard bit (a 64-bit variable shift is emulated on a
// 32-bit CPU; a table load is two 32-bit loads).
static u64 SQ_BIT[128];
static struct SqBitInit {
  SqBitInit() {
    for (int s = 0; s < 128; s++) SQ_BIT[s] = on_board(s) ? 1ull << sq64(s) : 0;
  }
} sq_bit_init;
#endif

template <bool KEY>
void Board::put(int s, int p) {
  int c = piece_color(p);
  sq[s] = u8(p);
#ifdef BITBOARD
  u64 b = SQ_BIT[s];
  bb[p] |= b;
  occ[c] |= b;
  if (piece_type(p) == KING) ksq[c] = u8(s);
#else
  idx[s] = count[c];
  list[c][count[c]++] = u8(s);
#endif
  psq[c] += PST[p][s];
  phase += PHASE_INC[piece_type(p)];
  if (KEY) key ^= piece_key(p, s);
}

template <bool KEY>
void Board::remove(int s) {
  int p = sq[s], c = piece_color(p);
#ifdef BITBOARD
  u64 b = SQ_BIT[s];
  bb[p] ^= b;
  occ[c] ^= b;
#else
  int i = idx[s], last = list[c][--count[c]];
  list[c][i] = u8(last);
  idx[last] = u8(i);
#endif
  sq[s] = EMPTY;
  psq[c] -= PST[p][s];
  phase -= PHASE_INC[piece_type(p)];
  if (KEY) key ^= piece_key(p, s);
}

template <bool KEY>
void Board::shift(int from, int to) {
  int p = sq[from], c = piece_color(p);
  sq[to] = u8(p);
  sq[from] = EMPTY;
#ifdef BITBOARD
  u64 b = SQ_BIT[from] | SQ_BIT[to];
  bb[p] ^= b;
  occ[c] ^= b;
  if (piece_type(p) == KING) ksq[c] = u8(to);
#else
  idx[to] = idx[from];
  list[c][idx[to]] = u8(to);
#endif
  psq[c] += PST[p][to] - PST[p][from];
  if (KEY) key ^= piece_key(p, from) ^ piece_key(p, to);
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
#ifdef BITBOARD
  memset(bb, 0, sizeof bb);
  occ[0] = occ[1] = 0;
#else
  count[0] = count[1] = 0;
#endif
  key = 0;
  psq[0] = psq[1] = 0;
  phase = 0;
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
  int kings[2] = {0, 0}, pieces[2] = {0, 0};
  for (int i = 0; i < n; i++) {
    int c = piece_color(placed[i].piece);
    if (piece_type(placed[i].piece) == KING) kings[c]++;
    if (++pieces[c] > 16) return false;
  }
  if (kings[WHITE] != 1 || kings[BLACK] != 1) return false;
  for (int i = 0; i < n; i++)
    if (piece_type(placed[i].piece) == KING) put<true>(placed[i].s, placed[i].piece);
  for (int i = 0; i < n; i++)
    if (piece_type(placed[i].piece) != KING) put<true>(placed[i].s, placed[i].piece);
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
  return !attacked(king_sq(stm ^ 1), stm);  // side not to move may not be in check
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
  for (int s = 0; s < 128; s++)
    if (on_board(s) && sq[s] != EMPTY) k ^= piece_key(sq[s], s);
  k ^= castle_key(castle) ^ ep_key(ep);
  if (stm == WHITE) k ^= SIDE_KEY;
  return k;
}

int Board::gen_legal(Move* out) {
  Move tmp[MAX_MOVES];
  int n = gen(tmp), legal = 0;
  bool check = in_check();
  for (int i = 0; i < n; i++) {
    make(tmp[i]);
    if (!leaves_check(tmp[i], check)) out[legal++] = tmp[i];
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
    remove<true>(victim);
  } else {
    u.captured = sq[to];
    if (u.captured != EMPTY) remove<true>(to);
  }
  if (u.captured != EMPTY || piece_type(p) == PAWN) rule50 = 0;

  shift<true>(from, to);
  if (int promo = move_promo(m)) {
    remove<true>(to);
    put<true>(to, make_piece(us, promo));
  } else if (m & MF_DOUBLE) {
    ep = u8(ep_if_capturable(*this, to, us));
  } else if (m & MF_CASTLE) {
    if (to > from) shift<true>(to + 1, to - 1);  // h-rook to f
    else shift<true>(to - 2, to + 1);            // a-rook to d
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
    remove<false>(to);
    put<false>(to, make_piece(us, PAWN));
  } else if (m & MF_CASTLE) {
    if (to > from) shift<false>(to - 1, to + 1);
    else shift<false>(to + 1, to - 2);
  }
  shift<false>(to, from);
  if (u.captured != EMPTY) {
    int s = (m & MF_EP) ? to + (us == WHITE ? -16 : 16) : to;
    put<false>(s, u.captured);
  }
  castle = u.castle;
  ep = u.ep;
  rule50 = u.rule50;
  key = u.key;
}

// Null move: pass the turn. rule50 = 0 stops repetition scans at it.
void Board::make_null() {
  Undo& u = hist[hply++];
  u.key = key;
  u.move = 0;
  u.captured = EMPTY;
  u.castle = castle;
  u.ep = ep;
  u.rule50 = rule50;
  key ^= ep_key(ep) ^ SIDE_KEY;
  ep = NO_SQ;
  rule50 = 0;
  stm ^= 1;
}

void Board::unmake_null() {
  Undo& u = hist[--hply];
  stm ^= 1;
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

// Move lists go on one shared stack, not the task stack: the ESP32 Arduino
// loop task has only 8 KB, and a 1 KB list per ply overflowed it at depth 6.
static Move move_stack[MOVE_STACK_SIZE];

static u64 perft_at(Board& b, int depth, Move* moves) {
  int n = b.gen(moves);
  bool check = b.in_check();
  u64 nodes = 0;
  for (int i = 0; i < n; i++) {
    b.make(moves[i]);
    if (!b.leaves_check(moves[i], check)) nodes += depth > 1 ? perft_at(b, depth - 1, moves + MAX_MOVES) : 1;
    b.unmake();
  }
  return nodes;
}

u64 perft(Board& b, int depth) {
  if (depth < 1 || depth > MAX_PERFT_DEPTH) return 0;
  return perft_at(b, depth, move_stack);
}
