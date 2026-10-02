// Chess System Tal Retro - small NNUE. See nnue.h.
#include "nnue.h"
#include <string.h>
#include "search.h"

#ifdef NNUE_PIE
extern "C" {
void nn_add_sub(int16_t* dst, const int16_t* src, const int16_t* add, const int16_t* sub, int n);
void nn_add(int16_t* dst, const int16_t* src, const int16_t* w, int n);
void nn_sub(int16_t* dst, const int16_t* src, const int16_t* w, int n);
void nn_crelu(int16_t* dst, const int16_t* src, int n, const int16_t* qa8);
int32_t nn_crelu_dot(const int16_t* x, const int16_t* w, int n, const int16_t* qa8);
int32_t nn_dot(const int16_t* x, const int16_t* w, int n);
void nn_layer(const int16_t* x, const int16_t* w, const int32_t* b, int16_t* out, int in_n, int out_n);
}
static_assert(NNUE_H_SHIFT == 6 && NNUE_QA == 255, "nn_layer in nnue_s3.S hard-codes these");
#endif

namespace {

int N = 0, H = 0;
int16_t* ft_w;   // [768][N]
int16_t* ft_b;   // [N]
int16_t* l1_w;   // H > 0: [H][2N]
int32_t* l1_b;   // H > 0: [H]
int16_t* out_w;  // [2N] (H = 0) or [H]; side to move's half first
int32_t out_b;

enum { STACK = MAX_PLY + 2 };
int16_t* acc;  // [STACK][2][N]
u64 acc_key[STACK];
int root_hply;
bool simd = true;

alignas(16) const int16_t QA8[8] = {NNUE_QA, NNUE_QA, NNUE_QA, NNUE_QA,
                                     NNUE_QA, NNUE_QA, NNUE_QA, NNUE_QA};

// ---- kernels: SIMD on the S3, plain C++ otherwise (and as the reference) ----

inline int clamp_qa(int v) { return v < 0 ? 0 : v > NNUE_QA ? NNUE_QA : v; }

void k_add_sub(int16_t* d, const int16_t* s, const int16_t* a, const int16_t* b, int n) {
#ifdef NNUE_PIE
  if (simd) return nn_add_sub(d, s, a, b, n);
#endif
  for (int i = 0; i < n; i++) d[i] = int16_t(s[i] + a[i] - b[i]);
}
void k_add(int16_t* d, const int16_t* s, const int16_t* w, int n) {
#ifdef NNUE_PIE
  if (simd) return nn_add(d, s, w, n);
#endif
  for (int i = 0; i < n; i++) d[i] = int16_t(s[i] + w[i]);
}
void k_sub(int16_t* d, const int16_t* s, const int16_t* w, int n) {
#ifdef NNUE_PIE
  if (simd) return nn_sub(d, s, w, n);
#endif
  for (int i = 0; i < n; i++) d[i] = int16_t(s[i] - w[i]);
}
void k_crelu(int16_t* d, const int16_t* s, int n) {
#ifdef NNUE_PIE
  if (simd) return nn_crelu(d, s, n, QA8);
#endif
  for (int i = 0; i < n; i++) d[i] = int16_t(clamp_qa(s[i]));
}
int32_t k_crelu_dot(const int16_t* x, const int16_t* w, int n) {
#ifdef NNUE_PIE
  if (simd) return nn_crelu_dot(x, w, n, QA8);
#endif
  int32_t sum = 0;
  for (int i = 0; i < n; i++) sum += clamp_qa(x[i]) * w[i];
  return sum;
}
int32_t k_dot(const int16_t* x, const int16_t* w, int n) {
#ifdef NNUE_PIE
  if (simd) return nn_dot(x, w, n);
#endif
  int32_t sum = 0;
  for (int i = 0; i < n; i++) sum += x[i] * w[i];
  return sum;
}

// out[j] = clamp((dot(x, w[j]) + b[j]) >> H_SHIFT, 0, QA), w row-major [out_n][in_n]
void k_layer(const int16_t* x, const int16_t* w, const int32_t* b, int16_t* out, int in_n, int out_n) {
#ifdef NNUE_PIE
  if (simd) return nn_layer(x, w, b, out, in_n, out_n);
#endif
  for (int j = 0; j < out_n; j++) {
    int32_t sum = b[j];
    const int16_t* row = w + size_t(j) * in_n;
    for (int i = 0; i < in_n; i++) sum += x[i] * row[i];
    out[j] = int16_t(clamp_qa(sum >> NNUE_H_SHIFT));
  }
}

// ---- accumulators ----

inline int16_t* acc_at(int k, int side) { return acc + (size_t(k) * 2 + side) * N; }
inline const int16_t* column(int f) { return ft_w + size_t(f) * N; }

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
    for (int s = 0; s < 128; s++)
      if (on_board(s) && b.sq[s] != EMPTY) k_add(a, a, column(feature(side, b.sq[s], s)), N);
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
    if (!adds) {
      memcpy(dst, src, sizeof(int16_t) * N);
      continue;
    }
    // first add + first sub in one pass, then any extra (capture / castling)
    k_add_sub(dst, src, column(feature(side, add_p[0], add_s[0])),
              column(feature(side, sub_p[0], sub_s[0])), N);
    for (int j = 1; j < adds; j++) k_add(dst, dst, column(feature(side, add_p[j], add_s[j])), N);
    for (int j = 1; j < subs; j++) k_sub(dst, dst, column(feature(side, sub_p[j], sub_s[j])), N);
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

int output(const int16_t* us, const int16_t* them) {
  int32_t sum;
  if (!H) {
    sum = k_crelu_dot(us, out_w, N) + k_crelu_dot(them, out_w + N, N) + out_b;
  } else {
    alignas(16) int16_t x[2 * NNUE_MAX_N];
    alignas(16) int16_t h[NNUE_MAX_H];
    k_crelu(x, us, N);
    k_crelu(x + N, them, N);
    k_layer(x, l1_w, l1_b, h, 2 * N, H);
    sum = k_dot(h, out_w, H) + out_b;
  }
  // sum * 400 / (255 * 64) without overflowing int32: 400/16320 = 25/1020.
  static_assert(NNUE_SCALE == 400 && NNUE_QA * NNUE_QB == 16320, "rescale below");
  int cp = int(sum * 25 / 1020);
  return cp > NNUE_MAX_CP ? NNUE_MAX_CP : cp < -NNUE_MAX_CP ? -NNUE_MAX_CP : cp;
}

u32 rng_state;
int16_t rnd(int range) {  // uniform in [-range, range]
  rng_state = rng_state * 1664525u + 1013904223u;
  return int16_t(int((rng_state >> 16) % u32(2 * range + 1)) - range);
}

}  // namespace

size_t nnue_weight_bytes(int n, int h) {
  size_t b = sizeof(int16_t) * (size_t(NNUE_INPUTS) * n + n);  // feature weights + biases
  if (h) b += sizeof(int16_t) * size_t(h) * 2 * n + sizeof(int32_t) * h + sizeof(int16_t) * h;
  else b += sizeof(int16_t) * 2 * n;
  return b;
}
size_t nnue_acc_bytes(int n) { return sizeof(int16_t) * size_t(STACK) * 2 * n; }

bool nnue_setup_random(int n, int h, u32 seed, void* wmem, void* amem) {
  if (n <= 0 || n > NNUE_MAX_N || n % 8 || h < 0 || h > NNUE_MAX_H || h % 8 || !wmem || !amem)
    return false;
  if ((uintptr_t(wmem) | uintptr_t(amem)) & 15) return false;  // SIMD needs 16-byte alignment
  N = n;
  H = h;
  ft_w = static_cast<int16_t*>(wmem);
  ft_b = ft_w + size_t(NNUE_INPUTS) * n;
  int16_t* p = ft_b + n;  // n is a multiple of 8: still 16-byte aligned
  if (h) {
    l1_w = p;
    l1_b = reinterpret_cast<int32_t*>(l1_w + size_t(h) * 2 * n);
    out_w = reinterpret_cast<int16_t*>(l1_b + h);
  } else {
    l1_w = nullptr;
    l1_b = nullptr;
    out_w = p;
  }
  acc = static_cast<int16_t*>(amem);
  rng_state = seed;
  for (size_t i = 0; i < size_t(NNUE_INPUTS) * n; i++) ft_w[i] = rnd(16);
  for (int i = 0; i < n; i++) ft_b[i] = rnd(32);
  if (h) {
    for (size_t i = 0; i < size_t(h) * 2 * n; i++) l1_w[i] = rnd(16);
    for (int i = 0; i < h; i++) l1_b[i] = rnd(256);
    for (int i = 0; i < h; i++) out_w[i] = rnd(64);
  } else {
    for (int i = 0; i < 2 * n; i++) out_w[i] = rnd(64);
  }
  out_b = 0;
  memset(acc_key, 0, sizeof acc_key);
  root_hply = 0;
  return true;
}

static u32 read_u32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }

bool nnue_file_shape(const void* data, size_t size, int* n, int* h) {
  const u8* p = static_cast<const u8*>(data);
  if (size < 16 || memcmp(p, "CSTN", 4) || read_u32(p + 4) != 1) return false;
  int fn = int(read_u32(p + 8)), fh = int(read_u32(p + 12));
  if (fn <= 0 || fn > NNUE_MAX_N || fn % 8 || fh < 0 || fh > NNUE_MAX_H || fh % 8) return false;
  if (size != 16 + nnue_weight_bytes(fn, fh) + 4) return false;
  *n = fn;
  *h = fh;
  return true;
}

bool nnue_load(const void* data, size_t size, void* wmem, void* amem) {
  int n, h;
  if (!nnue_file_shape(data, size, &n, &h)) return false;
  if (!nnue_setup_random(n, h, 1, wmem, amem)) return false;  // shape, pointers
  const u8* p = static_cast<const u8*>(data) + 16;
  size_t bytes = nnue_weight_bytes(n, h);
  memcpy(wmem, p, bytes);  // same layout as in memory
  out_b = int32_t(read_u32(p + bytes));
  memset(acc_key, 0, sizeof acc_key);
  return true;
}

bool nnue_ready() { return N != 0; }
int nnue_width() { return N; }
int nnue_hidden() { return H; }

void nnue_set_simd(bool on) { simd = on; }
bool nnue_simd() {
#ifdef NNUE_PIE
  return simd;
#else
  return false;
#endif
}

void nnue_new_root(const Board& b) {
  if (!N) return;
  root_hply = b.hply;
  refresh(b, 0);
  acc_key[0] = b.key;
  for (int i = 1; i < STACK; i++) acc_key[i] = 0;
}

int nnue_evaluate(const Board& b) {
  int k = ensure(b);
  return output(acc_at(k, b.stm), acc_at(k, b.stm ^ 1));
}

bool nnue_check(const Board& b) {
  int k = ensure(b);
  alignas(16) static int16_t fresh[2 * NNUE_MAX_N];
  refresh_into(b, fresh);
  return memcmp(fresh, acc_at(k, 0), sizeof(int16_t) * 2 * N) == 0;
}

// Both kernel paths from a fresh root at every node: same accumulators
// (incremental, from the root) and the same evaluation.
static int compare_walk(Board& b, int depth, int root) {
  int bad = 0;
  bool was = simd;
  alignas(16) static int16_t a_simd[2 * NNUE_MAX_N];
  simd = true;
  int e1 = nnue_evaluate(b);
  memcpy(a_simd, acc_at(b.hply - root_hply, 0), sizeof(int16_t) * 2 * N);
  simd = false;
  for (int i = 1; i < STACK; i++) acc_key[i] = 0;  // recompute this line with C++
  int e2 = nnue_evaluate(b);
  if (e1 != e2 || memcmp(a_simd, acc_at(b.hply - root_hply, 0), sizeof(int16_t) * 2 * N)) bad++;
  simd = was;
  for (int i = 1; i < STACK; i++) acc_key[i] = 0;
  if (depth > 0) {
    Move moves[MAX_MOVES];
    int n = b.gen_legal(moves);
    for (int i = 0; i < n; i++) {
      b.make(moves[i]);
      bad += compare_walk(b, depth - 1, root);
      b.unmake();
    }
  }
  (void)root;
  return bad;
}

int nnue_compare_paths(Board& b, int depth) {
  if (!N) return -1;
  nnue_new_root(b);
  return compare_walk(b, depth, b.hply);
}
