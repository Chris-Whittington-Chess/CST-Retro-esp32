// Chess System Tal Retro - optional cycle counters for the search
// (build with -DSEARCH_PROFILE; otherwise PROF(...) is just the statement).
#pragma once
#include <stdint.h>

enum { P_GEN, P_ORDER, P_MAKE, P_CHECK, P_EVAL, P_TT, P_COUNT };

#ifdef SEARCH_PROFILE
#if defined(ESP_PLATFORM)
static inline uint32_t prof_cycles() {
  uint32_t c;
  asm volatile("rsr %0, ccount" : "=a"(c));
  return c;
}
#else
#include <intrin.h>
static inline uint32_t prof_cycles() { return uint32_t(__rdtsc()); }
#endif
extern uint64_t prof_total[P_COUNT];
#define PROF(i, stmt)                          \
  do {                                         \
    uint32_t prof_t0_ = prof_cycles();         \
    stmt;                                      \
    prof_total[i] += prof_cycles() - prof_t0_; \
  } while (0)
#else
#define PROF(i, stmt) \
  do {                \
    stmt;             \
  } while (0)
#endif
