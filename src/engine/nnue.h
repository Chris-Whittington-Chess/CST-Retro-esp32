// Chess System Tal Retro - a small NNUE:
//   768 -> N (per side) -> CReLU -> 1            (H = 0)
//   768 -> N (per side) -> CReLU -> H -> CReLU -> 1   (H > 0, one hidden layer)
//   inputs: 12 piece types x 64 squares, from each side's point of view
//           (the black side sees the board flipped vertically)
//   N:      one accumulator of N int16 per side, updated incrementally
//   output: [stm acc | other acc] clipped to [0, QA], then the dense layer(s)
// Weights live in caller-supplied memory (internal SRAM on the ESP32: PSRAM
// is too slow for the per-move column reads), 16-byte aligned for the S3's
// SIMD kernels (nnue_s3.S, -DNNUE_PIE). Accumulators are computed lazily,
// per search ply, from the parent ply and the move in the history; each is
// tagged with its position key, so lines left by unmake are never reused.
#pragma once
#include <stddef.h>
#include "board.h"

enum {
  NNUE_INPUTS = 768,
  NNUE_MAX_N = 256,
  NNUE_MAX_H = 64,
  NNUE_QA = 255,   // activation clip
  NNUE_QB = 64,    // weight scale
  NNUE_H_SHIFT = 6,
  NNUE_SCALE = 400,
  NNUE_MAX_CP = 20000,  // evaluations are clamped well below mate scores
};

// Bytes of weights / accumulator stack for width n and hidden layer h.
size_t nnue_weight_bytes(int n, int h);
size_t nnue_acc_bytes(int n);

// Use width n (multiple of 8) and hidden layer h (0, or a multiple of 8)
// with weights at wmem and accumulators at amem (both 16-byte aligned),
// filled with random values (seed) - a stand-in until a trained net exists.
bool nnue_setup_random(int n, int h, u32 seed, void* wmem, void* amem);

// A trained net (tools/nnue/export.py), little-endian:
//   "CSTN", u32 version (1), u32 N, u32 H,
//   int16 ft_w[768][N], int16 ft_b[N],
//   H > 0: int16 l1_w[H][2N], int32 l1_b[H], int16 out_w[H]
//   H = 0: int16 out_w[2N]
//   int32 out_b
// nnue_file_shape reads N and H (false if not a net); nnue_load copies the
// weights into wmem (nnue_weight_bytes(N, H), 16-byte aligned).
bool nnue_file_shape(const void* data, size_t size, int* n, int* h);
bool nnue_load(const void* data, size_t size, void* wmem, void* amem);
// As nnue_load, but the weights are used where the file is (e.g. in flash:
// no RAM for a copy); amem holds the accumulators. Plain C++ evaluation only.
bool nnue_load_in_place(const void* data, size_t size, void* amem);
bool nnue_ready();
int nnue_width();
int nnue_hidden();

// SIMD kernels on/off (only meaningful in -DNNUE_PIE builds).
void nnue_set_simd(bool on);
bool nnue_simd();

// Search hooks: the root of a new search (the accumulator is refreshed
// there), and the evaluation of b (centipawns, side to move).
void nnue_new_root(const Board& b);
int nnue_evaluate(const Board& b);

// Tests. nnue_check: incremental accumulators equal a full refresh at b.
// nnue_compare_paths: SIMD and C++ kernels give the same accumulators and
// evaluation at every node of a depth-limited tree from b; returns the
// number of mismatches.
bool nnue_check(const Board& b);
int nnue_compare_paths(Board& b, int depth);
