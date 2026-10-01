// Chess System Tal Retro - transposition table. See tt.h.
#include "tt.h"
#include <string.h>

void TT::init(void* mem, size_t bytes) {
  size_t buckets = 1;
  while (buckets * 2 * 2 * sizeof(TTEntry) <= bytes) buckets *= 2;
  table = static_cast<TTEntry*>(mem);
  mask = u32(buckets - 1);
  clear();
}

void TT::clear() {
  if (table) memset(table, 0, (size_t(mask) + 1) * 2 * sizeof(TTEntry));
  generation = 0;
}

TTEntry* TT::probe(u64 key, bool& hit) {
  TTEntry* b = table + 2 * (u32(key) & mask);
  u32 check = u32(key >> 32);
  for (int i = 0; i < 2; i++)
    if (b[i].check == check && b[i].genbound) {
      hit = true;
      return &b[i];
    }
  hit = false;
  // Replace the entry from an older search, else the shallower one.
  auto worth = [&](const TTEntry& e) {
    int age = (generation - (e.genbound >> 2)) & 63;
    return int(e.depth) - 8 * age;
  };
  return worth(b[0]) <= worth(b[1]) ? &b[0] : &b[1];
}

void TT::store(TTEntry* e, u64 key, Move m, int score, int eval, int depth, int bound) {
  u32 check = u32(key >> 32);
  // Keep the old move if we have none for the same position.
  if (m || e->check != check) e->move = m;
  // Don't overwrite a deeper entry for the same position with a non-exact one.
  if (e->check == check && bound != BOUND_EXACT && depth + 3 < e->depth && e->genbound) return;
  e->check = check;
  e->score = int16_t(score);
  e->eval = int16_t(eval);
  e->depth = u8(depth < 0 ? 0 : depth);
  e->genbound = u8(generation << 2 | bound);
}

int TT::hashfull() const {
  int n = 0, total = 0;
  for (u32 i = 0; i < 1000 && i < (mask + 1) * 2; i++, total++)
    if (table[i].genbound && (table[i].genbound >> 2) == generation) n++;
  return total ? n * 1000 / total : 0;
}
