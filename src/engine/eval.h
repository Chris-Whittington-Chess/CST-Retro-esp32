// Chess System Tal Retro - PeSTO evaluation (Rofchade tables, published on
// chessprogramming.org "PeSTO's Evaluation Function"): middlegame and endgame
// material + piece-square values, blended by game phase.
#pragma once
#include "board.h"

void init_eval();
// Centipawns from the side to move's point of view.
int evaluate(const Board& b);
// Neither side can mate (K v K, K+minor v K).
bool insufficient_material(const Board& b);
// Material value used for move ordering (MVV-LVA), PAWN..KING.
extern const int ORDER_VALUE[8];
