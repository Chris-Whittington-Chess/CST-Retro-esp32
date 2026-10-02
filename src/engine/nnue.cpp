// Chess System Tal Retro - small NNUE. See nnue.h.
#include "nnue.h"
#include <string.h>
#include "search.h"

namespace {

int N = 0;
int16_t* ft_w;   // [768][N]
int16_t* ft_b;   // [N]
int16_t* out_w;  // [2N]: side to move's half first
int32_t out_b;

enum { STACK = MAX_PLY + 2 };
int16_t* acc;  // [STACK][2][N]
u64 acc_key[STACK];
int root_hply;

inline int16_t* acc_at(int k, int side) { return acc + (size_t(k) * 2 + side) * N; }

// Feature index of piece p on 0x88 square s, seen from side 'persp'.
inline int feature(int persp, int p, int s) {
  int s64 = sq64(s);
  if (persp == BLACK) s64 ^= 56;
  int own = piece_color(p) == persp ? 0 : 6;
  return (own + piece_type(p)) * 64 + s64;
}

// Full computation of both accumulators of b into dst[2][N].
void refresh_into(const Board& b, int16_t* dst) {
  for (int side = 0; side < 2; side++) {
    int16_t* a = dst + side * N;
    memcpy(a, ft_b, sizeof(int16_t) * N);
    for (int s = 0; s < 128; s++) {
      if (!on_board(s) || b.sq[s] == EMPTY) continue;
      const int16_t* w = ft_w + size_t(feature(side, b.sq[s], s)) * N;
      for (int i = 0; i < N; i++) a[i] += w[i];
    }
  }
}

void refresh(const Board& b, int k) { refresh_into(b, acc_at(k, 0)); }

// acc[k] = acc[k-1] + the change made by the move hist[root + k - 1].
void update(const Board& b, int k) {
  const Undo& u = b.hist[root_hply + k - 1];
  Move m = u.move;
  int adds = 0, subs = 0;
  int add_p[2], add_s[2], sub_p[2], sub_s[2];
  if (m) {  // 0 = null move: no change
    int p = (m >> 24) & 15, from = move_from(m), to = move_to(m), c = piece_color(p);
    sub_p[subs] = p, sub_s[subs++] = from;
    add_p[adds] = move_promo(m) ? make_piece(c, move_promo(m)) : p, add_s[adds++] = to;
    if (u.captured != EMPTY) {
      sub_p[subs] = u.captured;
      sub_s[subs++] = (m & MF_EP) ? to + (c == WHITE ? -16 : 16) : to;
    } else if (m & MF_CASTLE) {
      int rook = make_piece(c, ROOK);
      sub_p[subs] = rook, sub_s[subs++] = to > from ? to + 1 : to - 2;
      add_p[adds] = rook, add_s[adds++] = to > from ? to - 1 : to + 1;
    }
  }
  for (int side = 0; side < 2; side++) {
    const int16_t* src = acc_at(k - 1, side);
    int16_t* dst = acc_at(k, side);
    const int16_t* a0 = adds > 0 ? ft_w + size_t(feature(side, add_p[0], add_s[0])) * N : nullptr;
    const int16_t* s0 = subs > 0 ? ft_w + size_t(feature(side, sub_p[0], sub_s[0])) * N : nullptr;
    if (adds == 1 && subs == 1) {  // quiet move: one pass
      for (int i = 0; i < N; i++) dst[i] = int16_t(src[i] + a0[i] - s0[i]);
      continue;
    }
    memcpy(dst, src, sizeof(int16_t) * N);
    for (int j = 0; j < adds; j++) {
      const int16_t* w = ft_w + size_t(feature(side, add_p[j], add_s[j])) * N;
      for (int i = 0; i < N; i++) dst[i] += w[i];
    }
    for (int j = 0; j < subs; j++) {
      const int16_t* w = ft_w + size_t(feature(side, sub_p[j], sub_s[j])) * N;
      for (int i = 0; i < N; i++) dst[i] -= w[i];
    }
  }
}

inline u64 key_at(const Board& b, int k) {  // position key at search ply k
  int h = root_hply + k;
  return h == b.hply ? b.key : b.hist[h].key;
}

// Make acc[k] valid for the current line, k = b.hply - root_hply.
int ensure(const Board& b) {
  int k = b.hply - root_hply;
  if (k < 0 || k >= STACK) {  // outside the stack: rebase on this position
    root_hply = b.hply;
    refresh(b, 0);
    acc_key[0] = b.key;
    return 0;
  }
  int j = k;
  while (j > 0 && acc_key[j] != key_at(b, j)) j--;
  if (acc_key[j] != key_at(b, j)) {  // j == 0 and stale
    refresh(b, 0);
    acc_key[0] = key_at(b, 0);
  }
  for (int i = j + 1; i <= k; i++) {
    update(b, i);
    acc_key[i] = key_at(b, i);
  }
  return k;
}

u32 rng_state;
int16_t rnd(int range) {  // uniform in [-range, range]
  rng_state = rng_state * 1664525u + 1013904223u;
  return int16_t(int((rng_state >> 16) % u32(2 * range + 1)) - range);
}

}  // namespace

size_t nnue_weight_bytes(int n) {
  return sizeof(int16_t) * (size_t(NNUE_INPUTS) * n + n + 2 * n);
}
size_t nnue_acc_bytes(int n) { return sizeof(int16_t) * size_t(STACK) * 2 * n; }

bool nnue_setup_random(int n, u32 seed, void* wmem, void* amem) {
  if (n <= 0 || n > NNUE_MAX_N || n % 8 || !wmem || !amem) return false;
  N = n;
  ft_w = static_cast<int16_t*>(wmem);
  ft_b = ft_w + size_t(NNUE_INPUTS) * n;
  out_w = ft_b + n;
  acc = static_cast<int16_t*>(amem);
  rng_state = seed;
  for (size_t i = 0; i < size_t(NNUE_INPUTS) * n; i++) ft_w[i] = rnd(16);
  for (int i = 0; i < n; i++) ft_b[i] = rnd(32);
  for (int i = 0; i < 2 * n; i++) out_w[i] = rnd(64);
  out_b = 0;
  memset(acc_key, 0, sizeof acc_key);
  root_hply = 0;
  return true;
}

bool nnue_ready() { return N != 0; }
int nnue_width() { return N; }

void nnue_new_root(const Board& b) {
  if (!N) return;
  root_hply = b.hply;
  refresh(b, 0);
  acc_key[0] = b.key;
  for (int i = 1; i < STACK; i++) acc_key[i] = 0;
}

int nnue_evaluate(const Board& b) {
  int k = ensure(b);
  const int16_t* us = acc_at(k, b.stm);
  const int16_t* them = acc_at(k, b.stm ^ 1);
  int32_t sum = 0;
  for (int i = 0; i < N; i++) {
    int v = us[i] < 0 ? 0 : us[i] > NNUE_QA ? NNUE_QA : us[i];
    sum += v * out_w[i];
  }
  for (int i = 0; i < N; i++) {
    int v = them[i] < 0 ? 0 : them[i] > NNUE_QA ? NNUE_QA : them[i];
    sum += v * out_w[N + i];
  }
  return int((sum + out_b) * NNUE_SCALE / (NNUE_QA * NNUE_QB));
}

bool nnue_check(const Board& b) {
  int k = ensure(b);
  static int16_t fresh[2 * NNUE_MAX_N];
  refresh_into(b, fresh);
  return memcmp(fresh, acc_at(k, 0), sizeof(int16_t) * 2 * N) == 0;
}
