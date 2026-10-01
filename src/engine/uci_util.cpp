// Chess System Tal Retro - shared UCI pieces. See uci_util.h.
#include "uci_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

bool uci_position(Board& b, const char* args) {
  while (*args == ' ') args++;
  const char* moves = strstr(args, "moves");
  if (!strncmp(args, "startpos", 8)) {
    b.set_fen(START_FEN);
  } else if (!strncmp(args, "fen ", 4)) {
    char fen[128];
    size_t n = moves ? size_t(moves - args - 4) : strlen(args + 4);
    if (n >= sizeof fen) n = sizeof fen - 1;
    memcpy(fen, args + 4, n);
    fen[n] = 0;
    if (!b.set_fen(fen)) {
      b.set_fen(START_FEN);
      return false;
    }
  } else {
    return false;
  }
  if (!moves) return true;
  for (const char* p = moves + 5; *p;) {
    while (*p == ' ') p++;
    char mv[8] = {0};
    int n = 0;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r' && n < 7) mv[n++] = *p++;
    if (!n) break;
    Move m = parse_uci(b, mv);
    if (!m) return false;
    b.make(m);
    if (b.hply >= MAX_GAME - MAX_PLY - 1) return false;
  }
  return true;
}

// Value of "<name> <number>" among the space-separated tokens of line.
static long arg(const char* line, const char* name, long def) {
  size_t len = strlen(name);
  for (const char* p = line; (p = strstr(p, name)); p += len) {
    bool start = p == line || p[-1] == ' ';
    if (start && p[len] == ' ') return atol(p + len + 1);
  }
  return def;
}

Limits clock_limits(int32_t left_ms, int32_t inc_ms, int movestogo, int32_t overhead_ms) {
  Limits lim;
  int32_t left = left_ms - overhead_ms > 1 ? left_ms - overhead_ms : 1;
  int32_t target = left / (movestogo ? movestogo + 1 : 25) + inc_ms * 3 / 4;
  // soft = when not to start another iteration: the next one typically takes
  // about twice the time so far, so stopping at 60% of the target lands near
  // it on average (stopping at the target itself overran it 2-3x).
  int32_t soft = target * 6 / 10;
  int32_t hard = target * 3;
  if (hard > left / 3 + inc_ms / 2) hard = left / 3 + inc_ms / 2;
  if (hard > left) hard = left;
  if (soft > hard) soft = hard;
  lim.soft_ms = u32(soft > 1 ? soft : 1);
  lim.hard_ms = u32(hard > 1 ? hard : 1);
  return lim;
}

Limits uci_go(const char* line, int stm, int32_t overhead_ms) {
  Limits lim;
  long movetime = arg(line, "movetime", 0);
  long time = arg(line, stm == WHITE ? "wtime" : "btime", -1);
  if (movetime) {
    long t = movetime - overhead_ms > 1 ? movetime - overhead_ms : 1;
    lim.soft_ms = lim.hard_ms = u32(t);
  } else if (time >= 0) {
    lim = clock_limits(int32_t(time), int32_t(arg(line, stm == WHITE ? "winc" : "binc", 0)),
                       int(arg(line, "movestogo", 0)), overhead_ms);
  }
  lim.depth = int(arg(line, "depth", MAX_PLY - 1));
  if (lim.depth > MAX_PLY - 1) lim.depth = MAX_PLY - 1;
  lim.nodes = u64(arg(line, "nodes", 0));
  return lim;
}

int uci_info(const SearchReport& r, char* out, int size) {
  char score[24];
  if (r.score >= MATE_BOUND) snprintf(score, sizeof score, "mate %d", (MATE - r.score + 1) / 2);
  else if (r.score <= -MATE_BOUND) snprintf(score, sizeof score, "mate %d", -(MATE + r.score) / 2);
  else snprintf(score, sizeof score, "cp %d", r.score);
  int n = snprintf(out, size, "info depth %d seldepth %d score %s nodes %llu time %u nps %llu pv",
                   r.depth, r.seldepth, score, (unsigned long long)r.nodes, unsigned(r.ms),
                   (unsigned long long)(r.ms ? r.nodes * 1000 / r.ms : 0));
  for (int i = 0; i < r.pvlen && n < size - 7; i++) {
    out[n++] = ' ';
    move_to_uci(r.pv[i], out + n);
    n += int(strlen(out + n));
  }
  return n;
}
