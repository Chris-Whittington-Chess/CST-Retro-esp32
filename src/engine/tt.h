// Chess System Tal Retro - transposition table. The caller supplies the
// memory (PSRAM or internal SRAM on the ESP32, any heap on the PC), so the
// engine core has no allocator of its own. 16-byte entries, 2-way buckets.
#pragma once
#include <stddef.h>
#include "board.h"

enum { BOUND_NONE, BOUND_UPPER, BOUND_LOWER, BOUND_EXACT };

struct TTEntry {
  u32 check;  // upper 32 bits of the key
  Move move;
  int16_t score;
  int16_t eval;
  u8 depth;
  u8 genbound;  // generation << 2 | bound
  u16 pad;
  int bound() const { return genbound & 3; }
};

struct TT {
  TTEntry* table = nullptr;
  u32 mask = 0;  // bucket count - 1 (two entries per bucket)
  u8 generation = 0;

  // bytes is rounded down to a power of two (>= 32).
  void init(void* mem, size_t bytes);
  void clear();
  void new_search() { generation = u8((generation + 1) & 63); }
  TTEntry* probe(u64 key, bool& hit);
  void store(TTEntry* e, u64 key, Move m, int score, int eval, int depth, int bound);
  // Permille of the first 1000 entries written this search (UCI hashfull).
  int hashfull() const;
};
