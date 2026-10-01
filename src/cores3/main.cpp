// Chess System Tal Retro on the M5Stack CoreS3 - stage 1: the engine core's
// perft bench and correctness suite on the device.
// Serial (115200): 'b' bench, 'p' perft.epd suite (checks <= 1M nodes),
// 'm' memory report. The bench also runs once at boot.
#include <M5Unified.h>
#include "../engine/bench.h"
#include "../engine/board.h"

extern const char perft_epd[] asm("_binary_data_perft_epd_start");

static Board board;  // ~16 KB of history: static, not on the task stack

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

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  M5.Display.setTextSize(1);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextScroll(true);
  M5.Display.clear();
  say("Chess System Tal Retro - stage 1");
  say("CPU %u MHz", getCpuFrequencyMhz());
  memory_report();
  bench();
}

void loop() {
  M5.update();
  int c = Serial.read();
  if (c == 'b') bench();
  if (c == 'p') suite(1000000);
  if (c == 'm') memory_report();
  delay(10);
}
