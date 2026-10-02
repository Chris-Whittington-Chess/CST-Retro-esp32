// Chess System Tal Retro on the M5Stack CoreS3 - engine core benchmarks on
// the device. All engine work runs in its own task (24 KB stack); loop() only
// reads serial commands (115200):
//   'b' perft bench   'p' perft.epd suite (checks <= 1M nodes)   'm' memory
//   's' search bench depth 8, TT 4 MB in PSRAM
//   'i' search bench depth 8, TT 64 KB in internal SRAM (PeSTO)
//   '1'..'4' the same with a random-weight NNUE of width 32/64/128/256
//            computed at every node (EVAL_NNUE_COST: PeSTO's tree, so the
//            time difference is the network's cost); weights in internal
//            SRAM when they fit, else PSRAM
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

static void* alloc_prefer_internal(size_t bytes, const char** where) {
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  *where = "internal";
  if (!p) {
    p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    *where = "PSRAM";
  }
  return p;
}

static void nnue_bench(int n) {
  const char *wwhere, *awhere;
  void* w = alloc_prefer_internal(nnue_weight_bytes(n), &wwhere);
  void* a = alloc_prefer_internal(nnue_acc_bytes(n), &awhere);
  if (!w || !a || !nnue_setup_random(n, 12345, w, a)) {
    say("NNUE N=%d: no memory", n);
  } else {
    say("NNUE 768->%d: weights %u KB %s, acc %u KB %s", n, unsigned(nnue_weight_bytes(n) >> 10),
        wwhere, unsigned(nnue_acc_bytes(n) >> 10), awhere);
    eval_mode = EVAL_NNUE_COST;
    search_bench(false);
    eval_mode = EVAL_PESTO;
  }
  free(w);
  free(a);
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
      if (c >= '1' && c <= '4') nnue_bench(32 << (c - '1'));
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
