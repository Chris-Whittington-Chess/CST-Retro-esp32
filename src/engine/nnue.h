// Chess System Tal Retro - a small NNUE: 768 -> N (per side) -> CReLU -> 1.
//   inputs: 12 piece types x 64 squares, from each side's point of view
//           (the black side sees the board flipped vertically)
//   hidden: one accumulator of N int16 per side, updated incrementally
//   output: [stm acc | other acc] clipped to [0, QA], dot with 2N weights
// Weights live in caller-supplied memory (internal SRAM on the ESP32: PSRAM
// is too slow for the per-move column reads). Accumulators are computed
// lazily, per search ply, from the parent ply and the move in the history;
// each is tagged with its position key, so lines left by unmake are never
// reused by mistake.
#pragma once
#include <stddef.h>
#include "board.h"

enum { NNUE_INPUTS = 768, NNUE_MAX_N = 256, NNUE_QA = 255, NNUE_QB = 64, NNUE_SCALE = 400 };

// Bytes of weights for width n (feature weights + biases + output weights).
size_t nnue_weight_bytes(int n);
// Bytes of accumulator stack for width n.
size_t nnue_acc_bytes(int n);

// Use width n with weights at wmem (nnue_weight_bytes) and accumulators at
// amem (nnue_acc_bytes). Fills the weights with random values (seed) - a
// stand-in until a trained net exists. Returns false on a bad n.
bool nnue_setup_random(int n, u32 seed, void* wmem, void* amem);
bool nnue_ready();
int nnue_width();

// Search hooks: the root of a new search (the accumulator is refreshed
// there), and the evaluation of b (centipawns, side to move).
void nnue_new_root(const Board& b);
int nnue_evaluate(const Board& b);

// Test: incremental accumulators equal a full refresh at b (b.hply within
// MAX_PLY of the last nnue_new_root).
bool nnue_check(const Board& b);
