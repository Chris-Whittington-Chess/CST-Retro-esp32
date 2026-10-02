// Chess System Tal Retro - attacks and move generation on 64-bit bitboards
// (built with -DBITBOARD; movegen88.cpp is the default 0x88 version).
//
// An experiment: the ESP32 is a 32-bit CPU, so the compiler splits every u64
// operation into two 32-bit ones, and popcount / bit scans go through the
// u64 builtins. Sliders use classical ray attacks - 8 rays per square, the
// first blocker found with lsb/msb - which need 4 KB of tables; magic
// bitboards need ~800 KB, too big for internal SRAM.
// Moves and the mailbox still use 0x88 squares; bitboards use 0..63.
#ifdef BITBOARD
#include <string.h>
#include "board.h"

#if defined(__XTENSA__) && !defined(BB_BUILTIN_SCAN)
// On the ESP32-S3, GCC compiles the 64-bit builtins __builtin_ctzll/clzll to
// calls into ROM (__ctzdi2, __clzdi2). Split by hand into 32-bit halves: the
// 32-bit clz is the single NSAU instruction, and ctz(x) = 31 - clz(x & -x).
static inline int lsb(u64 b) {
  u32 lo = u32(b);
  if (lo) return 31 - __builtin_clz(lo & (0u - lo));
  u32 hi = u32(b >> 32);
  return 63 - __builtin_clz(hi & (0u - hi));
}
static inline int msb(u64 b) {
  u32 hi = u32(b >> 32);
  return hi ? 63 - __builtin_clz(hi) : 31 - __builtin_clz(u32(b));
}
#else
static inline int lsb(u64 b) { return __builtin_ctzll(b); }
static inline int msb(u64 b) { return 63 - __builtin_clzll(b); }
#endif
static inline int pop_lsb(u64& b) {
  int s = lsb(b);
  b &= b - 1;
  return s;
}
static inline u64 bit(int s) { return 1ull << s; }

enum { N, NE, E, SE, S, SW, W, NW };  // ray directions; N, NE, E, NW go up in index
static u64 RAY[8][64];
static u64 KNIGHT_ATT[64], KING_ATT[64];
static u64 PAWN_ATT[2][64];                  // squares a pawn of that colour on s attacks
static u64 DIAG_LINES[64], ORTHO_LINES[64];  // all diagonal / orthogonal rays from s
static int8_t DIR_TO[64][64];                // ray direction from a to b, -1 if none

static const u64 FILE_A = 0x0101010101010101ull, FILE_H = FILE_A << 7;
static const u64 RANK_3 = 0xFFull << 16, RANK_6 = 0xFFull << 40;
static const u64 RANK_1 = 0xFFull, RANK_8 = 0xFFull << 56;

static struct BitboardInit {
  BitboardInit() {
    static const int DF[8] = {0, 1, 1, 1, 0, -1, -1, -1}, DR[8] = {1, 1, 0, -1, -1, -1, 0, 1};
    static const int KN[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
    memset(DIR_TO, -1, sizeof DIR_TO);
    for (int s = 0; s < 64; s++) {
      int f = s & 7, r = s >> 3;
      for (int d = 0; d < 8; d++)
        for (int x = f + DF[d], y = r + DR[d]; x >= 0 && x < 8 && y >= 0 && y < 8;
             x += DF[d], y += DR[d]) {
          RAY[d][s] |= bit(y * 8 + x);
          DIR_TO[s][y * 8 + x] = int8_t(d);
        }
      for (auto& o : KN) {
        int x = f + o[0], y = r + o[1];
        if (x >= 0 && x < 8 && y >= 0 && y < 8) KNIGHT_ATT[s] |= bit(y * 8 + x);
      }
      for (int d = 0; d < 8; d++) {
        int x = f + DF[d], y = r + DR[d];
        if (x >= 0 && x < 8 && y >= 0 && y < 8) KING_ATT[s] |= bit(y * 8 + x);
      }
      if (r < 7) PAWN_ATT[WHITE][s] = (f > 0 ? bit(s + 7) : 0) | (f < 7 ? bit(s + 9) : 0);
      if (r > 0) PAWN_ATT[BLACK][s] = (f > 0 ? bit(s - 9) : 0) | (f < 7 ? bit(s - 7) : 0);
      DIAG_LINES[s] = RAY[NE][s] | RAY[SE][s] | RAY[SW][s] | RAY[NW][s];
      ORTHO_LINES[s] = RAY[N][s] | RAY[E][s] | RAY[S][s] | RAY[W][s];
    }
  }
} bitboard_init;

// Squares seen along one ray, up to and including the first blocker.
template <int D>
static inline u64 ray_att(int s, u64 occ) {
  u64 r = RAY[D][s], b = r & occ;
  if (b) r ^= RAY[D][(D == N || D == NE || D == E || D == NW) ? lsb(b) : msb(b)];
  return r;
}
static inline u64 ray_att(int d, int s, u64 occ) {
  switch (d) {
    case N: return ray_att<N>(s, occ);
    case NE: return ray_att<NE>(s, occ);
    case E: return ray_att<E>(s, occ);
    case SE: return ray_att<SE>(s, occ);
    case S: return ray_att<S>(s, occ);
    case SW: return ray_att<SW>(s, occ);
    case W: return ray_att<W>(s, occ);
    default: return ray_att<NW>(s, occ);
  }
}
static inline u64 bishop_att(int s, u64 occ) {
  return ray_att<NE>(s, occ) | ray_att<NW>(s, occ) | ray_att<SE>(s, occ) | ray_att<SW>(s, occ);
}
static inline u64 rook_att(int s, u64 occ) {
  return ray_att<N>(s, occ) | ray_att<E>(s, occ) | ray_att<S>(s, occ) | ray_att<W>(s, occ);
}

bool Board::attacked(int s88, int by) const {
  int s = sq64(s88);
  const u64* p = bb + (by << 3);
  if (PAWN_ATT[by ^ 1][s] & p[PAWN]) return true;
  if (KNIGHT_ATT[s] & p[KNIGHT]) return true;
  if (KING_ATT[s] & p[KING]) return true;
  u64 all = occ[0] | occ[1];
  u64 bq = p[BISHOP] | p[QUEEN], rq = p[ROOK] | p[QUEEN];
  if ((DIAG_LINES[s] & bq) && (bishop_att(s, all) & bq)) return true;
  if ((ORTHO_LINES[s] & rq) && (rook_att(s, all) & rq)) return true;
  return false;
}

bool Board::leaves_check(Move m, bool was_in_check) const {
  int us = stm ^ 1, k = ksq[us];
  if (was_in_check || k == move_to(m) || (m & MF_EP)) return attacked(k, stm);
  // Otherwise only a piece pinned on a line through 'from' can expose the king:
  // look along that line from the king for the first piece.
  int k64 = sq64(k), d = DIR_TO[k64][sq64(move_from(m))];
  if (d < 0) return false;
  u64 first = ray_att(d, k64, occ[0] | occ[1]) & occ[stm];
  const u64* p = bb + (stm << 3);
  u64 sliders = ((d & 1) ? p[BISHOP] : p[ROOK]) | p[QUEEN];  // odd = diagonal
  return (first & sliders) != 0;
}

static inline Move* emit(Move* m, int from, u64 targets, u64 enemy) {
  int f88 = sq88(from);
  while (targets) {
    int to = pop_lsb(targets);
    *m++ = make_move(f88, sq88(to), (enemy & bit(to)) ? MF_CAPTURE : 0);
  }
  return m;
}

static inline Move* emit_promos(Move* m, int from, int to, u32 flags) {
  for (int pt = QUEEN; pt >= KNIGHT; pt--) *m++ = make_move(sq88(from), sq88(to), flags, pt);
  return m;
}

int Board::gen(Move* out, bool quiets) const {
  Move* m = out;
  int us = stm, them = us ^ 1;
  u64 own = occ[us], enemy = occ[them], all = own | enemy, empty = ~all;
  u64 targets = quiets ? ~own : enemy;
  const u64* p = bb + (us << 3);

  // Pawns, set-wise: to-square sets, from = to - shift.
  u64 pawns = p[PAWN];
  u64 push1, push2, cap_l, cap_r, promo_rank;
  int up, left, right;  // to - from for a push and the two captures
  if (us == WHITE) {
    push1 = (pawns << 8) & empty;
    push2 = ((push1 & RANK_3) << 8) & empty;
    cap_l = ((pawns & ~FILE_A) << 7) & enemy;
    cap_r = ((pawns & ~FILE_H) << 9) & enemy;
    up = 8, left = 7, right = 9, promo_rank = RANK_8;
  } else {
    push1 = (pawns >> 8) & empty;
    push2 = ((push1 & RANK_6) >> 8) & empty;
    cap_l = ((pawns & ~FILE_A) >> 9) & enemy;
    cap_r = ((pawns & ~FILE_H) >> 7) & enemy;
    up = -8, left = -9, right = -7, promo_rank = RANK_1;
  }
  for (u64 b = push1 & promo_rank; b;) {
    int to = pop_lsb(b);
    m = emit_promos(m, to - up, to, 0);
  }
  for (u64 b = cap_l & promo_rank; b;) {
    int to = pop_lsb(b);
    m = emit_promos(m, to - left, to, MF_CAPTURE);
  }
  for (u64 b = cap_r & promo_rank; b;) {
    int to = pop_lsb(b);
    m = emit_promos(m, to - right, to, MF_CAPTURE);
  }
  for (u64 b = cap_l & ~promo_rank; b;) {
    int to = pop_lsb(b);
    *m++ = make_move(sq88(to - left), sq88(to), MF_CAPTURE);
  }
  for (u64 b = cap_r & ~promo_rank; b;) {
    int to = pop_lsb(b);
    *m++ = make_move(sq88(to - right), sq88(to), MF_CAPTURE);
  }
  if (ep != NO_SQ)
    for (u64 b = PAWN_ATT[them][sq64(ep)] & pawns; b;)
      *m++ = make_move(sq88(pop_lsb(b)), ep, MF_CAPTURE | MF_EP);
  if (quiets) {
    for (u64 b = push1 & ~promo_rank; b;) {
      int to = pop_lsb(b);
      *m++ = make_move(sq88(to - up), sq88(to));
    }
    for (u64 b = push2; b;) {
      int to = pop_lsb(b);
      *m++ = make_move(sq88(to - 2 * up), sq88(to), MF_DOUBLE);
    }
  }

  for (u64 b = p[KNIGHT]; b;) {
    int s = pop_lsb(b);
    m = emit(m, s, KNIGHT_ATT[s] & targets, enemy);
  }
  for (u64 b = p[BISHOP]; b;) {
    int s = pop_lsb(b);
    m = emit(m, s, bishop_att(s, all) & targets, enemy);
  }
  for (u64 b = p[ROOK]; b;) {
    int s = pop_lsb(b);
    m = emit(m, s, rook_att(s, all) & targets, enemy);
  }
  for (u64 b = p[QUEEN]; b;) {
    int s = pop_lsb(b);
    m = emit(m, s, (bishop_att(s, all) | rook_att(s, all)) & targets, enemy);
  }
  int k = sq64(ksq[us]);
  m = emit(m, k, KING_ATT[k] & targets, enemy);

  // Castling, as in the 0x88 version (rights imply king and rook at home).
  int base = us == WHITE ? 0x00 : 0x70;
  int ks = us == WHITE ? WK_CASTLE : BK_CASTLE, qs = us == WHITE ? WQ_CASTLE : BQ_CASTLE;
  if (quiets && (castle & (ks | qs))) {
    int kk = base + 4;
    if ((castle & ks) && sq[kk + 1] == EMPTY && sq[kk + 2] == EMPTY && !attacked(kk, them) &&
        !attacked(kk + 1, them) && !attacked(kk + 2, them))
      *m++ = make_move(kk, kk + 2, MF_CASTLE);
    if ((castle & qs) && sq[kk - 1] == EMPTY && sq[kk - 2] == EMPTY && sq[kk - 3] == EMPTY &&
        !attacked(kk, them) && !attacked(kk - 1, them) && !attacked(kk - 2, them))
      *m++ = make_move(kk, kk - 2, MF_CASTLE);
  }
  return int(m - out);
}

bool Board::has_non_pawn(int c) const {
  return (occ[c] ^ bb[c << 3 | PAWN] ^ bb[c << 3 | KING]) != 0;
}

bool Board::no_mating_material() const {
  // At most 3 pieces? (clear the lowest set bit three times; no popcount
  // needed - on the ESP32 __builtin_popcountll is a ROM call too)
  u64 all = occ[0] | occ[1];
  u64 rest = all & (all - 1);  // without the first piece
  rest &= rest - 1;            // ... and the second
  if (!rest) return true;      // two pieces: the kings
  if (rest & (rest - 1)) return false;  // four or more
  return (bb[KNIGHT] | bb[BISHOP] | bb[8 | KNIGHT] | bb[8 | BISHOP]) != 0;
}

#endif  // BITBOARD
