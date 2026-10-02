// Chess System Tal Retro - evaluation: PeSTO (Rofchade tables, published on
// chessprogramming.org "PeSTO's Evaluation Function": middlegame and endgame
// material + piece-square values, blended by game phase), or the NNUE.
#pragma once
#include "board.h"

// Which evaluation the search uses. EVAL_NNUE_COST computes the NNUE at every
// node but returns PeSTO: the search tree stays PeSTO's, so it measures the
// network's cost exactly (benchmarks with random weights).
enum EvalMode { EVAL_PESTO, EVAL_NNUE, EVAL_NNUE_COST };
extern EvalMode eval_mode;

void init_eval();
// Centipawns from the side to move's point of view (per eval_mode; NNUE
// modes fall back to PeSTO while no net is set up).
int evaluate(const Board& b);
int evaluate_pesto(const Board& b);
// Neither side can mate (K v K, K+minor v K).
bool insufficient_material(const Board& b);
// Material value used for move ordering (MVV-LVA), PAWN..KING.
extern const int ORDER_VALUE[8];
