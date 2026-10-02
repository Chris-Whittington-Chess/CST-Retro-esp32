// Chess System Tal Retro - static exchange evaluation (SEE): is the capture
// sequence a move starts on its target square worth at least 'threshold'
// to the side making it? Both sides recapture with their least valuable
// attacker, x-rays included; pins are ignored. Works on the 0x88 mailbox,
// so on either board variant.
#pragma once
#include "board.h"

extern const int SEE_VALUE[8];  // P 100, N 320, B 330, R 500, Q 900, K 20000

bool see_ge(const Board& b, Move m, int threshold);
// The exchange value itself (binary search over see_ge; tests only).
int see_value(const Board& b, Move m);
