// Chess System Tal Retro on the M5Stack CoreS3 - engine core benchmarks on
// the device. All engine work runs in its own task (24 KB stack); loop() only
// reads serial commands (115200):
//   'b' perft bench   'p' perft.epd suite (checks <= 1M nodes)   'm' memory
//   's' search bench depth 8, TT 4 MB in PSRAM
//   'i' search bench depth 8, TT 64 KB in internal SRAM (PeSTO)
//   '1' '2' '3'  the same with a random-weight NNUE 768->32->1, 768->64->1,
//                768->32->32->1 computed at every node (EVAL_NNUE_COST:
//                PeSTO's tree, so the time difference is the network's
//                cost), PIE SIMD kernels; '5' '6' '7' the same in plain C++
//   '9'          NNUE self-test: SIMD == C++, incremental == refresh
#include <M5Unified.h>
#include "../engine/bench.h"
#include "../engine/board.h"
#include "../engine/eval.h"
#include "../engine/nnue.h"
#include "../engine/profile.h"
#include "../engine/search.h"
#include "../engine/tt.h"

extern const char perft_epd[] asm("_binary_data_perft_epd_start");

static Board board;  // ~16 KB of history: static, not on the task stack
static TT tt;
static void* tt_psram;     // 4 MB
static void* tt_internal;  // 64 KB
static TaskHandle_t engine_task;
static volatile int command;

u32 engine_now_ms() { return millis(); }

static void say(const char* fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Serial.println(buf);
  M5.Display.println(buf);
}

static void memory_report() {
  say("internal free %u KB (largest %u KB)", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
  say("PSRAM free %u KB", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
  say("Board %u bytes", unsigned(sizeof(Board)));
}

static void bench() {
  say("bench...");
  uint32_t t0 = millis();
  BenchResult r = run_bench(board);
  float s = (millis() - t0) / 1000.0f;
  say("bench %llu nodes, %d fail", (unsigned long long)r.nodes, r.failures);
  say("  %.2f s, %.0f knps", s, r.nodes / s / 1000);
}

static void suite(u64 max_nodes) {
  say("perft.epd (<= %llu nodes)...", (unsigned long long)max_nodes);
  int positions = 0, checks = 0, fails = 0;
  u64 total = 0;
  uint32_t t0 = millis();
  const char* p = perft_epd;
  char line[256];
  while (*p) {
    int n = 0;
    while (*p && *p != '\n' && n < 255) line[n++] = *p++;
    line[n] = 0;
    if (*p == '\n') p++;
    char* semi = strchr(line, ';');
    if (!semi) continue;
    *semi = 0;
    if (!board.set_fen(line)) { fails++; continue; }
    positions++;
    for (char* q = semi + 1; (q = strchr(q, 'D'));) {
      int depth = atoi(q + 1);
      char* sp = strchr(q, ' ');
      if (!sp) break;
      u64 want = strtoull(sp + 1, &q, 10);
      if (want > max_nodes) continue;
      u64 got = perft(board, depth);
      total += got;
      checks++;
      if (got != want) {
        fails++;
        Serial.printf("FAIL %s D%d want %llu got %llu\n", line, depth, want, got);
      }
    }
  }
  float s = (millis() - t0) / 1000.0f;
  say("%d pos, %d checks, %d fail", positions, checks, fails);
  say("  %llu nodes, %.1f s, %.0f knps", (unsigned long long)total, s, total / s / 1000);
}

static void search_bench(bool psram) {
  size_t bytes = psram ? 4u << 20 : 64u << 10;
  void* mem = psram ? tt_psram : tt_internal;
  if (!mem) { say("no TT memory"); return; }
  tt.init(mem, bytes);
  say("search bench d8, TT %u KB %s...", unsigned(bytes >> 10), psram ? "PSRAM" : "internal");
#ifdef SEARCH_PROFILE
  for (auto& p : prof_total) p = 0;
#endif
  uint32_t t0 = millis();
  u64 n = run_search_bench(board, 8);
  float s = (millis() - t0) / 1000.0f;
  say("  %llu nodes, %.1f s, %.1f knps", (unsigned long long)n, s, n / s / 1000);
#ifdef SEARCH_PROFILE
  static const char* names[P_COUNT] = {"gen", "order", "make", "check", "eval", "tt"};
  double total = s * getCpuFrequencyMhz() * 1e6 / n, part = 0;
  for (int i = 0; i < P_COUNT; i++) {
    double c = double(prof_total[i]) / n;
    part += c;
    say("  %-6s %6.0f cycles/node", names[i], c);
  }
  say("  other  %6.0f  (total %.0f)", total - part, total);
#endif
  say("  engine stack free %u bytes", unsigned(uxTaskGetStackHighWaterMark(nullptr)));
}

// 16-byte aligned (the SIMD kernels need it), internal SRAM if possible.
static void* alloc_prefer_internal(size_t bytes, const char** where) {
  void* p = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  *where = "internal";
  if (!p) {
    p = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM);
    *where = "PSRAM";
  }
  return p;
}

static void* nn_w;
static void* nn_a;

static bool nnue_load(int n, int h) {
  heap_caps_free(nn_w);
  heap_caps_free(nn_a);
  const char *wwhere, *awhere;
  nn_w = alloc_prefer_internal(nnue_weight_bytes(n, h), &wwhere);
  nn_a = alloc_prefer_internal(nnue_acc_bytes(n), &awhere);
  if (!nn_w || !nn_a || !nnue_setup_random(n, h, 12345, nn_w, nn_a)) {
    say("NNUE 768->%d->%d: no memory", n, h);
    return false;
  }
  say("NNUE 768->%d%s%s->1: weights %u KB %s, acc %u KB %s", n, h ? "->" : "",
      h ? String(h).c_str() : "", unsigned(nnue_weight_bytes(n, h) >> 10), wwhere,
      unsigned(nnue_acc_bytes(n) >> 10), awhere);
  return true;
}

static void nnue_bench(int n, int h, bool simd) {
  if (!nnue_load(n, h)) return;
  nnue_set_simd(simd);
  say("  kernels: %s", nnue_simd() ? "PIE SIMD" : "C++");
  eval_mode = EVAL_NNUE_COST;
  search_bench(false);
  eval_mode = EVAL_PESTO;
  nnue_set_simd(true);
}

// SIMD vs C++ kernels at every node of depth-3 trees, and incremental vs
// full refresh, for each shape.
static void nnue_selftest() {
  static const int shapes[3][2] = {{32, 0}, {64, 0}, {32, 32}};
  for (auto& sh : shapes) {
    if (!nnue_load(sh[0], sh[1])) continue;
    int bad = 0, nodes = 0;
    for (const char* f : SEARCH_BENCH_FENS) {
      board.set_fen(f);
      bad += nnue_compare_paths(board, 2);
      nodes++;
    }
    board.set_fen(SEARCH_BENCH_FENS[1]);
    nnue_new_root(board);
    int incr_bad = 0;
    Move moves[MAX_MOVES];
    int n = board.gen_legal(moves);
    for (int i = 0; i < n; i++) {
      board.make(moves[i]);
      if (!nnue_check(board)) incr_bad++;
      board.unmake();
    }
    say("  SIMD vs C++: %d mismatches; incremental vs refresh: %d", bad, incr_bad);
  }
}

static void engine_loop(void*) {
  search_init(&tt);
  for (;;) {
    int c = command;
    if (c) {
      if (c == 'b') bench();
      if (c == 'p') suite(1000000);
      if (c == 'm') memory_report();
      if (c == 's') search_bench(true);
      if (c == 'i') search_bench(false);
      if (c == '1' || c == '5') nnue_bench(32, 0, c == '1');
      if (c == '2' || c == '6') nnue_bench(64, 0, c == '2');
      if (c == '3' || c == '7') nnue_bench(32, 32, c == '3');
      if (c == '9') nnue_selftest();
      command = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setTextSize(1);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextScroll(true);
  M5.Display.clear();
  say("Chess System Tal Retro - stage 2");
  say("CPU %u MHz", getCpuFrequencyMhz());
  tt_psram = heap_caps_malloc(4u << 20, MALLOC_CAP_SPIRAM);
  tt_internal = heap_caps_malloc(64u << 10, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  memory_report();
  xTaskCreatePinnedToCore(engine_loop, "engine", 24 * 1024, nullptr, 1, &engine_task, 1);
}

void loop() {
  M5.update();
  int c = Serial.read();
  if (c > ' ' && !command) command = c;
  delay(10);
}
