// Chess System Tal Retro - UCI pieces shared by the PC test build and the
// device (which speaks UCI over USB alongside its touch screen).
#pragma once
#include "board.h"
#include "search.h"

// "position" arguments ("startpos ..." / "fen <fen> ..." [moves m1 m2 ...]).
// Returns false on a bad FEN or an illegal move (moves up to it are kept).
bool uci_position(Board& b, const char* args);

// Limits for a "go ..." line with side to move stm; overhead_ms is reserved
// per move for the link (30 on the PC, more over USB).
Limits uci_go(const char* line, int stm, int32_t overhead_ms);

// Time for one move from a clock: a target of ~1/25 of what's left (or
// left/(mtg+1)) plus most of the increment; no new iteration after 60% of the
// target, hard stop at 3x the target and never more than a third of the clock.
Limits clock_limits(int32_t left_ms, int32_t inc_ms, int movestogo, int32_t overhead_ms);

// "info depth ... pv ..." without a newline; returns the length.
int uci_info(const SearchReport& r, char* out, int size);
