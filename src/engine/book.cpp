// Chess System Tal Retro - Polyglot opening book. See book.h.
#include "book.h"

static u64 be64(const u8* p) {
  u64 v = 0;
  for (int i = 0; i < 8; i++) v = v << 8 | p[i];
  return v;
}
static int be16(const u8* p) { return p[0] << 8 | p[1]; }

// Polyglot move: to file 0-2, to row 3-5, from file 6-8, from row 9-11,
// promotion 12-14 (1 n .. 4 q, same as our KNIGHT..QUEEN). Castling is
// written as the king taking its own rook (e1h1 = O-O).
static Move decode(Board& b, int pm, const Move* legal, int n) {
  int to = ((pm >> 3) & 7) * 16 + (pm & 7);
  int from = ((pm >> 9) & 7) * 16 + ((pm >> 6) & 7);
  int promo = (pm >> 12) & 7;
  if (piece_type(b.sq[from]) == KING && sq_file(from) == 4 && sq_rank(from) == sq_rank(to)) {
    if (sq_file(to) == 7) to = from + 2;
    else if (sq_file(to) == 0) to = from - 2;
  }
  for (int i = 0; i < n; i++)
    if (move_from(legal[i]) == from && move_to(legal[i]) == to && move_promo(legal[i]) == promo)
      return legal[i];
  return 0;
}

int book_moves(Board& b, const u8* book, size_t bytes, BookMove* out, int cap) {
  size_t n = bytes / 16;
  if (!book || !n) return 0;
  // lower bound of the key
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (be64(book + mid * 16) < b.key) lo = mid + 1;
    else hi = mid;
  }
  Move legal[MAX_MOVES];
  int nlegal = b.gen_legal(legal), count = 0;
  for (size_t i = lo; i < n && count < cap && be64(book + i * 16) == b.key; i++) {
    const u8* e = book + i * 16;
    Move m = decode(b, be16(e + 8), legal, nlegal);
    if (m) out[count++] = {m, be16(e + 10)};
  }
  return count;
}

Move book_pick(Board& b, const u8* book, size_t bytes, u32 random) {
  BookMove moves[64];
  int n = book_moves(b, book, bytes, moves, 64);
  u32 total = 0;
  for (int i = 0; i < n; i++) total += u32(moves[i].weight);
  if (!total) return 0;
  u32 r = random % total;
  for (int i = 0; i < n; i++) {
    if (r < u32(moves[i].weight)) return moves[i].move;
    r -= u32(moves[i].weight);
  }
  return 0;
}
