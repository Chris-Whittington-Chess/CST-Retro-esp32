// Chess System Tal Retro - Polyglot opening book read from memory (on the
// ESP32 the .bin is embedded in flash). Entries are 16 bytes, big-endian,
// sorted by key: key u64, move u16, weight u16, learn u32.
#pragma once
#include <stddef.h>
#include "board.h"

struct BookMove {
  Move move;
  int weight;
};

// All book moves for the position that are legal here; returns the count.
int book_moves(Board& b, const u8* book, size_t bytes, BookMove* out, int cap);

// A weighted random book move (random: any 32-bit random number); 0 if none.
Move book_pick(Board& b, const u8* book, size_t bytes, u32 random);
