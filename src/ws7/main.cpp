// Chess System Tal Retro on the Waveshare ESP32-S3-Touch-LCD-7 (800x480 RGB
// panel, GT911 touch): play the engine with clocks, take back, levels.
// The game logic is the CoreS3 one (src/cores3/main.cpp); this file swaps in
// the 7" hardware and a bigger layout with a move list.
//
// Screen: the board (480x480, 60 px squares) on the left, a panel on the
// right with the two clocks, status, the move list and the BACK / MENU
// buttons. Everything is drawn into one full-screen canvas in PSRAM and copied
// into the RGB panel's frame buffer (also PSRAM, scanned out by DMA).
//
// Hardware: the panel through ESP-IDF's esp_lcd RGB driver (M5GFX has no RGB
// bus driver; its canvas does the drawing). A CH422G I2C expander drives the
// backlight, the LCD and touch resets and the USB/CAN switch - which must
// select USB, or the PC loses the board. The GT911 touch controller shares
// the I2C bus. No speaker.
//
// Tasks: the engine runs in its own task on core 0 (the idle-task watchdog
// there is disabled, the engine keeps that core busy while it thinks); the UI
// runs in loop() on core 1. The engine searches a copy of the game board and
// hands back a move.
//
// USB serial (115200), one command per line:
//   UCI: "uci" switches to UCI mode - the device is then a UCI engine (for a
//   GUI or cutechess via tools/uci_bridge.py) and the screen follows the game;
//   the menu's "Play from here" returns to touch play. "position ..." outside
//   UCI mode sets up the touch game with you to move. Also isready,
//   ucinewgame, go, stop, setoption name OwnBook, quit (= leave UCI mode).
//   Debug: "d" dump frame ("FRAME\n" + 800*480 LE RGB565, tools/grab.py
//   --size 800x480), "t X Y" simulated tap, "n" new game as White, "a"
//   autoplay (the engine also plays your side, moves printed), "b" perft
//   bench, "s" search bench, "touchlog" toggles printing every touch.
#include <M5GFX.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include "../cores3/usb_out.h"
#include "../engine/bench.h"
#include "../engine/board.h"
#include "../engine/book.h"
#include "../engine/eval.h"
#include "../engine/nnue.h"
#include "../engine/search.h"
#include "../engine/tt.h"
#include "../engine/uci_util.h"
#include "pieces.h"

extern const uint8_t book_start[] asm("_binary_data_Jeroen_bin_start");
extern const uint8_t book_end[] asm("_binary_data_Jeroen_bin_end");
// The trained NNUE (tools/nnue/train.py), copied to internal SRAM at boot.
extern const uint8_t net_start[] asm("_binary_data_net_bin_start");
extern const uint8_t net_end[] asm("_binary_data_net_bin_end");

// ---------------------------------------------------------------- layout

enum { SQ = 60, PANEL_X = 480, PANEL_W = 320, SCREEN_W = 800, SCREEN_H = 480 };
enum { CLOCK_H = 64, TOP_CLOCK_Y = 8, BOTTOM_CLOCK_Y = 328, BUTTON_Y = 400, BUTTON_H = 72 };
enum { BACK_X = PANEL_X + 8, MENU_BTN_X = PANEL_X + 164, BUTTON_W = 148 };

struct RGB {
  uint8_t r, g, b;
};
static const RGB LIGHT = {240, 217, 181}, DARK = {181, 136, 99};
static const RGB LAST_TINT = {214, 200, 80}, SELECT_TINT = {100, 170, 90};

static M5Canvas frame;

static uint16_t c565(RGB c) { return lgfx::color565(c.r, c.g, c.b); }
static RGB mix(RGB a, RGB b, int t) {  // t/255 of b over a
  return {uint8_t((a.r * (255 - t) + b.r * t) / 255), uint8_t((a.g * (255 - t) + b.g * t) / 255),
          uint8_t((a.b * (255 - t) + b.b * t) / 255)};
}

static const uint16_t PANEL_BG = 0x2124, TEXT = 0xFFFF, DIM = 0x9CD3, BUTTON = 0x4A69,
                      ACTIVE = 0xFEA0, CLOCK_BG = 0x18C3;

// ---------------------------------------------------------------- hardware

// CH422G: not register based - each I2C "address" is a command.
enum { CH422_SET = 0x24, CH422_OUT = 0x38 };
enum { EXIO_TP_RST = 1 << 1, EXIO_BL = 1 << 2, EXIO_LCD_RST = 1 << 3, EXIO_SD_CS = 1 << 4,
       EXIO_USB_SEL = 1 << 5 };  // USB_SEL low = USB, high = CAN
enum { TP_INT = 4, I2C_SDA = 8, I2C_SCL = 9 };
static uint8_t exio;

static void exio_write(uint8_t v) {
  exio = v;
  Wire.beginTransmission(CH422_OUT);
  Wire.write(v);
  Wire.endTransmission();
}

static esp_lcd_panel_handle_t panel;

// ST7262 800x480 timings from the board's official ESP32_Display_Panel config.
static bool lcd_begin() {
  esp_lcd_rgb_panel_config_t cfg = {};
#if ESP_IDF_VERSION_MAJOR >= 5
  // ESP-IDF 5 (env ws7, arduino-esp32 3.x): bounce buffers. The DMA reads two
  // small internal-SRAM buffers that an interrupt refills from the PSRAM frame
  // buffer, so the engine's flash fetches (flash and PSRAM share the bus) no
  // longer starve the LCD - on IDF 4.4 that showed as a jumping picture.
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = 16 * 1000 * 1000;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 1;
  cfg.bounce_buffer_size_px = SCREEN_W * 8;  // 2 x 12.5 KB internal; must divide the frame
  cfg.dma_burst_size = 64;
#else
  // IDF 4.4 (no bounce buffers): the DMA reads PSRAM directly and glitches
  // when the bus is busy. 160/11 = 14.5 MHz eases it a little; keep an integer
  // divider of 160 MHz - 12 MHz (fractional) left the panel blank.
  cfg.clk_src = LCD_CLK_SRC_PLL160M;
  cfg.timings.pclk_hz = 160 * 1000 * 1000 / 11;
  cfg.psram_trans_align = 64;
#endif
  cfg.timings.h_res = SCREEN_W;
  cfg.timings.v_res = SCREEN_H;
  cfg.timings.hsync_pulse_width = 4;
  cfg.timings.hsync_back_porch = 8;
  cfg.timings.hsync_front_porch = 8;
  cfg.timings.vsync_pulse_width = 4;
  cfg.timings.vsync_back_porch = 8;
  cfg.timings.vsync_front_porch = 8;
  cfg.timings.flags.pclk_active_neg = 1;
  cfg.data_width = 16;
  cfg.hsync_gpio_num = 46;
  cfg.vsync_gpio_num = 3;
  cfg.de_gpio_num = 5;
  cfg.pclk_gpio_num = 7;
  // D0..D15: B3..B7, G2..G7, R3..R7
  static const int data[16] = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40};
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data[i];
  cfg.disp_gpio_num = -1;
  cfg.flags.fb_in_psram = 1;
  if (esp_lcd_new_rgb_panel(&cfg, &panel) != ESP_OK) return false;
  esp_lcd_panel_reset(panel);
  esp_lcd_panel_init(panel);
  return true;
}

// Copy a rectangle of the canvas to the panel. The canvas holds RGB565
// byte-swapped (the SPI order M5GFX uses); the RGB bus wants it native.
static void lcd_push(int x, int y, int w, int h) {
  static uint16_t line[SCREEN_W];
  const uint16_t* src = (const uint16_t*)frame.getBuffer();
  for (int r = y; r < y + h; r++) {
    const uint16_t* s = src + r * SCREEN_W + x;
    for (int i = 0; i < w; i++) line[i] = __builtin_bswap16(s[i]);
    esp_lcd_panel_draw_bitmap(panel, x, r, x + w, r + 1, line);
  }
}

// GT911 touch: address 0x5D (INT held low through reset), else 0x14.
static uint8_t gt911 = 0x5D;

static bool gt_read(uint16_t reg, uint8_t* buf, int n) {
  Wire.beginTransmission(gt911);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(int(gt911), n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static void gt_write8(uint16_t reg, uint8_t v) {
  Wire.beginTransmission(gt911);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(v);
  Wire.endTransmission();
}

struct TouchState {
  bool down, pressed, released;
  int x, y;
};
static TouchState touch;

static void touch_update() {
  touch.pressed = touch.released = false;
  uint8_t st;
  if (!gt_read(0x814E, &st, 1) || !(st & 0x80)) return;  // no new report: unchanged
  bool down = false;
  if (st & 0x0F) {
    uint8_t p[5];  // track id, x lo/hi, y lo/hi of the first point
    if (gt_read(0x814F, p, 5)) {
      touch.x = p[1] | p[2] << 8;
      touch.y = p[3] | p[4] << 8;
      down = true;
    }
  }
  gt_write8(0x814E, 0);
  touch.pressed = down && !touch.down;
  touch.released = !down && touch.down;
  touch.down = down;
}

static void board_begin() {
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  Wire.beginTransmission(CH422_SET);
  Wire.write(0x01);  // IO0..7 as outputs
  Wire.endTransmission();
  // Resets low, backlight off, SD deselected, USB selected.
  exio_write(EXIO_SD_CS);
  pinMode(TP_INT, OUTPUT);
  digitalWrite(TP_INT, LOW);  // GT911 address 0x5D
  delay(10);
  exio_write(exio | EXIO_TP_RST | EXIO_LCD_RST);
  delay(100);
  pinMode(TP_INT, INPUT);
  delay(50);
  Wire.beginTransmission(gt911);
  if (Wire.endTransmission() != 0) gt911 = 0x14;
}

static void beep() {}

// ---------------------------------------------------------------- time controls

struct TimeControl {
  const char* name;
  u32 base_ms, inc_ms, move_ms;  // move_ms: fixed engine time per move, no clocks
};
static const TimeControl TCS[] = {
    {"1 s / move", 0, 0, 1000},  {"3 s / move", 0, 0, 3000},    {"10 s / move", 0, 0, 10000},
    {"1 min", 60000, 0, 0},      {"3 + 2", 180000, 2000, 0},    {"5 min", 300000, 0, 0},
    {"10 min", 600000, 0, 0},    {"15 + 10", 900000, 10000, 0}, {"30 min", 1800000, 0, 0},
};
enum { NUM_TCS = sizeof TCS / sizeof TCS[0], DEFAULT_TC = 6 };
static int tc_index = DEFAULT_TC;  // chosen in the menu
static int tc_game = DEFAULT_TC;   // the one the current game uses
static Preferences prefs;

static bool timed() { return TCS[tc_game].move_ms == 0; }

// ---------------------------------------------------------------- game state

// UCI: driven over USB (the board shows the game, touch moves are off).
enum Phase { HUMAN, ENGINE, OVER, UCI };
// UI-side data that is not speed critical lives in PSRAM: internal SRAM is
// kept for the NNUE (one 96 KB block), the TT and the display's bounce
// buffers. (The IDF 5 libraries can't place .bss in PSRAM, so these are
// allocated during static initialisation - PSRAM is up by then.)
template <class T>
static T* psram_new(size_t n = 1) {
  return static_cast<T*>(heap_caps_calloc(n, sizeof(T), MALLOC_CAP_SPIRAM));
}

static Board& game = *psram_new<Board>();
static int human = ::WHITE;
static bool flipped = false;
static Phase phase = HUMAN;
static int32_t clock_ms[2];
typedef int32_t ClockPair[2];
static ClockPair* clock_hist = psram_new<ClockPair>(MAX_GAME);  // clocks at the start of each ply
static int start_ply;                    // take back stops here (set-up positions)
static u32 turn_start;
static char result[2][24];  // two lines
static Move last_move;
static int selected = -1;
static Move sel_moves[MAX_MOVES];
static int nsel;
static int promo_from = -1, promo_to;  // promotion chooser open
static bool menu_open;
static bool dirty = true;  // full redraw wanted
static bool autoplay;      // serial 'a': the engine plays both sides
static bool use_book = true;
static bool last_from_book;
static u32 book_move_at;  // a book move is shown after a short pause
static char uci_last_bestmove[16];  // re-sent on a "stop" after the answer (see serial_line)
static int uci_side = -1;  // UCI: the board's colour in the game (the side it was last asked to move)
static bool uci_own_book;  // UCI option OwnBook (off: GUIs/matches bring their own)
static bool touch_log;
static bool nnue_ok;        // net loaded at boot
static bool use_nnue;       // menu / UCI choice (saved in NVS from the menu)
static EvalMode last_mode = EVAL_PESTO;
static bool human_clock = true;  // menu: your clock counts down (off: only the engine's runs)
static u32 menu_opened_at;
static int menu_page;  // 0 the menu, 1 finish the game: resign / claim win / agree draw

static bool clock_runs(int side) { return side != human || human_clock; }

// ---------------------------------------------------------------- engine task

static Board eng_board;
static TT tt;
static TaskHandle_t eng_task;
static Limits eng_limits;
static volatile int eng_command;  // 'g' search, 'b' perft bench, 's' search bench
static volatile bool eng_busy, eng_done;
static volatile Move eng_move;
static volatile int info_depth, info_score;
static volatile bool eng_uci;  // the running search answers a UCI "go"

// ---- notation and evals: SAN for the moves shown, the engine's score for
// each of its moves (White's view). Both live in PSRAM, allocated in setup().
enum : int32_t { EVAL_NONE = INT32_MIN, EVAL_BOOK = INT32_MIN + 1 };
// Per ply: the position (key) and move a score belongs to, so a score only
// shows while the game still has that move there - after take back or a new
// "position" from the PC (UCI resends the whole game every move) the old
// ply's score is not mistaken for the new one.
struct PlyEval {
  u64 key;
  Move move;
  int32_t score;  // EVAL_BOOK or the engine's score, White's view
};
static PlyEval* ply_eval;
static Board* san_board;   // scratch copy of the game for working out SAN
static volatile int eng_score;  // the finished search's score, side to move's view

// UCI output during a search ("info" lines, then "bestmove") goes out from the
// engine task itself, at once: through loop() it waited for the screen redraw
// (~0.3 s on the 7-inch board), which cost moves in time trouble.
enum { INFO_LEN = 400 };

u32 engine_now_ms() { return millis(); }

static void on_report(const SearchReport& r) {
  info_depth = r.depth;
  info_score = r.score;
  if (eng_uci) {
    char line[INFO_LEN];
    uci_info(r, line, INFO_LEN);
    usb_println(line);
  }
}

static void engine_loop(void*) {
  // Stack guard: nodes stop deepening 4 KB above the bottom of this stack -
  // room for the evaluation at the node where it fires (NNUE: ~1.9 KB)
  // (STACK_GUARD_MARGIN: a test build sets it high to make the guard fire).
#ifndef STACK_GUARD_MARGIN
#define STACK_GUARD_MARGIN 4096
#endif
  search_stack_floor = (const char*)pxTaskGetStackStart(nullptr) + STACK_GUARD_MARGIN;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int c = eng_command;
    if (c == 'g') {
      SearchResult r = search(eng_board, eng_limits, on_report);
      eng_score = r.score;
      eng_move = r.best;
      if (eng_uci) {  // answer the GUI now; loop() then updates the board
        char line[24] = "bestmove 0000";
        if (r.best) move_to_uci(r.best, line + 9);
        snprintf(uci_last_bestmove, sizeof uci_last_bestmove, "%s", line + 9);
        usb_println(line);
      }
      eng_done = true;
    } else if (c == 'b') {
      u32 t0 = millis();
      BenchResult r = run_bench(eng_board);
      u32 ms = millis() - t0;
      usb_printf("perft bench %llu nodes, %d fail, %u ms, %llu knps\n", r.nodes, r.failures,
                    unsigned(ms), r.nodes / (ms ? ms : 1));
    } else if (c == 's') {
      u32 t0 = millis();
      u64 n = run_search_bench(eng_board, 8);
      u32 ms = millis() - t0;
      usb_printf("search bench d8: %llu nodes, %u ms, %llu knps, engine stack free %u\n", n,
                    unsigned(ms), n / (ms ? ms : 1), unsigned(uxTaskGetStackHighWaterMark(nullptr)));
      search_new_game();
    }
    eng_busy = false;
  }
}

static void engine_command(int c) {
  eng_command = c;
  eng_done = false;
  search_stop = false;
  eng_busy = true;  // set before notifying, so a quick stop_engine() waits for it
  xTaskNotifyGive(eng_task);
}

static void stop_engine() {
  eng_done = false;  // also cancels a book move waiting to be shown
  if (!eng_busy) return;
  search_stop = true;
  while (eng_busy) delay(1);
  eng_done = false;
  search_stop = false;
}

// The embedded net into 16-byte aligned internal SRAM (the SIMD kernels and
// the per-move column reads need it there).
static void load_net() {
  int n, h;
  size_t size = size_t(net_end - net_start);
  if (!nnue_file_shape(net_start, size, &n, &h)) {
    usb_println("net.bin is not a CSTN net: PeSTO only");
    return;
  }
  void* w = heap_caps_aligned_alloc(16, nnue_weight_bytes(n, h), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  void* a = heap_caps_aligned_alloc(16, nnue_acc_bytes(n), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!w || !a) {
    usb_println("no internal SRAM for the net: PeSTO only");
    heap_caps_free(w);
    heap_caps_free(a);
    return;
  }
  nnue_ok = nnue_load(net_start, size, w, a);
  usb_printf("NNUE 768->%d->1 %s\n", n, nnue_ok ? "loaded" : "failed");
}

// Set the evaluation before a search (only while the engine is idle); a
// change clears the TT, whose entries hold the other evaluation's scores.
static void apply_eval_mode() {
  EvalMode m = (use_nnue && nnue_ok) ? EVAL_NNUE : EVAL_PESTO;
  if (m != last_mode) {
    eval_mode = m;
    search_new_game();
    last_mode = m;
  }
}

static void start_engine() {
  phase = ENGINE;
  info_depth = 0;
  if (use_book) {
    Move m = book_pick(game, book_start, size_t(book_end - book_start), esp_random());
    if (m) {
      eng_move = m;
      eng_done = true;
      last_from_book = true;
      book_move_at = millis() + (autoplay ? 0 : 600);
      return;
    }
  }
  last_from_book = false;
  apply_eval_mode();
  memcpy(&eng_board, &game, sizeof game);
  const TimeControl& tc = TCS[tc_game];
  Limits lim;
  if (tc.move_ms) lim.soft_ms = lim.hard_ms = tc.move_ms;
  else lim = clock_limits(clock_ms[game.stm], int32_t(tc.inc_ms), 0, 50);  // same as UCI
  eng_limits = lim;
  eng_uci = false;
  engine_command('g');
}

static void clear_evals() {
  if (ply_eval) memset(ply_eval, 0, MAX_GAME * sizeof(PlyEval));
}

// Record the score for move m, about to be made in the game's position.
static void record_eval(Move m, int32_t score) {
  if (ply_eval) ply_eval[game.hply] = {game.key, Move(m & 0x00FFFFFF), score};
}

// The score shown for the game's move at ply (EVAL_NONE: none, e.g. yours).
static int32_t eval_at(int ply) {
  if (!ply_eval || ply < 0 || ply >= game.hply) return EVAL_NONE;
  const PlyEval& e = ply_eval[ply];
  if (e.key != game.hist[ply].key || e.move != (game.hist[ply].move & 0x00FFFFFF)) return EVAL_NONE;
  return e.score;
}

// The engine's score for the move about to be made (from its side's view).
static int32_t white_view(int score) { return game.stm == ::WHITE ? score : -score; }

// SAN of the last SAN_PLIES moves of the game, worked out on a copy (undone
// to the first of them, then replayed) and cached until the game changes.
enum { SAN_PLIES = 10 };
static char san_text[SAN_PLIES][10];
static int san_base, san_hply = -1;
static u64 san_key;

static void refresh_san() {
  if (!san_board || (san_hply == game.hply && san_key == game.key)) return;
  san_hply = game.hply;
  san_key = game.key;
  san_base = game.hply > SAN_PLIES ? game.hply - SAN_PLIES : 0;
  memcpy(san_board, &game, sizeof(Board));
  while (san_board->hply > san_base) san_board->unmake();
  for (int p = san_base; p < game.hply; p++) {
    Move m = game.hist[p].move & 0x00FFFFFF;
    move_to_san(*san_board, m, san_text[p - san_base]);
    san_board->make(m);
  }
}

static const char* san_of(int ply) {
  refresh_san();
  if (!san_board || ply < san_base || ply >= game.hply) return "?";
  return san_text[ply - san_base];
}

// ---------------------------------------------------------------- rules / game flow

static void set_result(const char* a, const char* b) {
  snprintf(result[0], sizeof result[0], "%s", a);
  snprintf(result[1], sizeof result[1], "%s", b);
  phase = OVER;
}

static int repetitions() {
  int n = 0;
  for (int i = game.hply - 2; i >= 0 && i >= game.hply - game.rule50; i -= 2)
    if (game.hist[i].key == game.key) n++;
  return n;
}

static void check_game_over() {
  Move tmp[MAX_MOVES];
  int n = game.gen_legal(tmp);
  const char* wins = game.stm == ::WHITE ? "Black wins" : "White wins";
  if (!n && game.in_check()) set_result("Checkmate", wins);
  else if (!n) set_result("Stalemate", "Draw");
  else if (game.rule50 >= 100) set_result("50-move rule", "Draw");
  else if (insufficient_material(game)) set_result("No mating force", "Draw");
  else if (repetitions() >= 2) set_result("Repetition", "Draw");
  else if (game.hply >= MAX_GAME - MAX_PLY - 2) set_result("Game too long", "Draw");
}


static void play(Move m, bool engine = false) {
  u32 now = millis();
  if (!engine) last_from_book = false;
  int us = game.stm;
  clock_hist[game.hply][0] = clock_ms[0];
  clock_hist[game.hply][1] = clock_ms[1];
  if (timed() && game.hply && clock_runs(us)) clock_ms[us] += int32_t(TCS[tc_game].inc_ms) - int32_t(now - turn_start);
  if (autoplay) {
    char mv[6];
    move_to_uci(m, mv);
    if (game.stm == ::WHITE) usb_printf("%d. ", game.hply / 2 + 1);
    usb_printf("%s ", mv);
  }
  if (engine) record_eval(m, last_from_book ? EVAL_BOOK : white_view(eng_score));
  game.make(m);
  last_move = m;
  turn_start = now;
  selected = -1;
  nsel = 0;
  promo_from = -1;
  beep();
  check_game_over();
  if (phase != OVER) {
    if (game.stm == human && !autoplay) phase = HUMAN;
    else start_engine();
  } else if (autoplay) {
    autoplay = false;
    usb_printf("\nresult: %s, %s (%d plies)\n", result[0], result[1], game.hply);
  }
  dirty = true;
}

static void new_game(int color) {
  stop_engine();
  game.set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
  clear_evals();
  search_new_game();
  tc_game = tc_index;
  human = color;
  flipped = color == ::BLACK;
  clock_ms[0] = clock_ms[1] = int32_t(TCS[tc_game].base_ms);
  start_ply = 0;
  last_move = 0;
  selected = -1;
  nsel = 0;
  promo_from = -1;
  turn_start = millis();
  phase = HUMAN;
  if (game.stm != human) start_engine();
  dirty = true;
}

// Touch play from the current position, you to move (after UCI or a
// "position" command from the PC).
static void play_from_here() {
  stop_engine();
  autoplay = false;
  tc_game = tc_index;
  human = game.stm;
  flipped = human == ::BLACK;
  clock_ms[0] = clock_ms[1] = int32_t(TCS[tc_game].base_ms);
  last_move = game.hply ? game.hist[game.hply - 1].move : 0;
  last_from_book = false;
  selected = -1;
  nsel = 0;
  promo_from = -1;
  turn_start = millis();
  start_ply = game.hply;
  clock_hist[start_ply][0] = clock_ms[0];
  clock_hist[start_ply][1] = clock_ms[1];
  phase = HUMAN;
  check_game_over();
  dirty = true;
}

// Undo back to the most recent position with the human to move.
static void take_back() {
  if (phase == UCI) return;
  autoplay = false;
  stop_engine();
  if (game.hply <= start_ply) return;
  do game.unmake();
  while (game.hply > start_ply && game.stm != human);
  clock_ms[0] = clock_hist[game.hply][0];
  clock_ms[1] = clock_hist[game.hply][1];
  last_move = game.hply ? game.hist[game.hply - 1].move : 0;
  selected = -1;
  nsel = 0;
  promo_from = -1;
  turn_start = millis();
  phase = HUMAN;
  if (game.stm != human) start_engine();  // the engine had the first move
  dirty = true;
}

// The menu pauses your clock: on closing, your turn restarts later by the
// time it was open (counted from when your turn began, if the engine moved
// while the menu was up).
static void set_menu(bool open) {
  if (open == menu_open) return;
  u32 now = millis();
  if (open) {
    menu_opened_at = now;
    menu_page = 0;
  } else if (phase == HUMAN) {
    u32 from = int32_t(turn_start - menu_opened_at) > 0 ? turn_start : menu_opened_at;
    turn_start += now - from;
  }
  menu_open = open;
}

// Can colour c still mate at all? FIDE 6.9: losing on time to a lone king,
// or to king + one minor piece, is a draw.
static bool can_mate(int c) {
  int pieces = 0, minors = 0;
  for (int s = 0; s < 128; s++) {
    if (!on_board(s) || game.sq[s] == EMPTY || piece_color(game.sq[s]) != c) continue;
    int t = piece_type(game.sq[s]);
    if (t == KING) continue;
    pieces++;
    if (t == KNIGHT || t == BISHOP) minors++;
  }
  return pieces > 1 || (pieces == 1 && minors == 0);
}

// ---------------------------------------------------------------- drawing

static void square_xy(int s, int& x, int& y) {
  int f = sq_file(s), r = sq_rank(s);
  x = (flipped ? 7 - f : f) * SQ;
  y = (flipped ? r : 7 - r) * SQ;
}

static int square_at(int x, int y) {
  if (x < 0 || x >= 8 * SQ || y < 0 || y >= 8 * SQ) return -1;
  int f = x / SQ, r = 7 - y / SQ;
  if (flipped) { f = 7 - f; r = 7 - r; }
  return r * 16 + f;
}

static bool is_target(int s) {
  for (int i = 0; i < nsel; i++)
    if (move_to(sel_moves[i]) == s) return true;
  return false;
}

static void draw_piece(int x0, int y0, int p, RGB bg) {
  const uint8_t* img = PIECE_IMG[piece_color(p)][piece_type(p)];
  for (int y = 0; y < PIECE_PX; y++)
    for (int x = 0; x < PIECE_PX; x++) {
      int a = img[(y * PIECE_PX + x) * 2 + 1];
      if (!a) continue;
      uint8_t g = img[(y * PIECE_PX + x) * 2];
      frame.drawPixel(x0 + x, y0 + y, c565(mix(bg, {g, g, g}, a)));
    }
}

static void draw_square(int s) {
  int x, y;
  square_xy(s, x, y);
  RGB bg = ((sq_file(s) + sq_rank(s)) & 1) ? LIGHT : DARK;
  if (last_move && (s == move_from(last_move) || s == move_to(last_move)))
    bg = mix(bg, LAST_TINT, 110);
  if (s == selected) bg = mix(bg, SELECT_TINT, 150);
  frame.fillRect(x, y, SQ, SQ, c565(bg));
  int p = game.sq[s];
  if (p != EMPTY) draw_piece(x, y, p, bg);
  if (is_target(s)) {
    uint16_t dot = c565(mix(bg, {40, 70, 40}, 150));
    if (p == EMPTY) frame.fillCircle(x + SQ / 2, y + SQ / 2, 10, dot);
    else
      for (int k = 0; k < 4; k++) frame.drawCircle(x + SQ / 2, y + SQ / 2, SQ / 2 - 1 - k, dot);
  }
  if (p != EMPTY && piece_type(p) == KING && piece_color(p) == game.stm && game.in_check())
    for (int k = 0; k < 3; k++) frame.drawRect(x + k, y + k, SQ - 2 * k, SQ - 2 * k, TFT_RED);
}

static void draw_board() {
  for (int s = 0; s < 128; s++)
    if (on_board(s)) draw_square(s);
}

static void fmt_clock(int32_t ms, char* out) {
  if (ms < 0) ms = 0;
  u32 s = (u32(ms) + 999) / 1000;
  if (ms > 0 && ms < 10000)
    snprintf(out, 12, "%u.%u", unsigned(ms / 1000), unsigned(ms / 100 % 10));
  else if (s >= 3600)
    snprintf(out, 12, "%u:%02u:%02u", unsigned(s / 3600), unsigned(s / 60 % 60), unsigned(s % 60));
  else
    snprintf(out, 12, "%u:%02u", unsigned(s / 60), unsigned(s % 60));
}

// Your clock stands still while the menu is open (the engine's does not).
static int32_t clock_now(int side) {
  int32_t ms = clock_ms[side];
  if ((phase == HUMAN || phase == ENGINE) && game.stm == side && game.hply && clock_runs(side) &&
      !(menu_open && phase == HUMAN))
    ms -= int32_t(millis() - turn_start);
  return ms;
}

static void clock_text(int side, char* buf) {
  if (phase == UCI) snprintf(buf, 12, "%s", side == uci_side ? "CST" : uci_side < 0 ? "" : "PC");
  else if (timed() && !clock_runs(side)) snprintf(buf, 12, "No clock");
  else if (timed()) fmt_clock(clock_now(side), buf);
  else snprintf(buf, 12, "%s", side == human ? "You" : "CST");
}

static void draw_clock(int side, int y) {
  bool active = phase != OVER && game.stm == side;
  frame.fillRoundRect(PANEL_X + 8, y, PANEL_W - 16, CLOCK_H, 8, active ? ACTIVE : CLOCK_BG);
  // which side this clock belongs to
  frame.fillCircle(PANEL_X + 34, y + CLOCK_H / 2, 11, side == ::WHITE ? TFT_WHITE : TFT_BLACK);
  frame.drawCircle(PANEL_X + 34, y + CLOCK_H / 2, 11, DIM);
  char buf[12];
  clock_text(side, buf);
  frame.setFont(&fonts::FreeSansBold24pt7b);
  frame.setTextColor(active ? TFT_BLACK : TEXT);
  frame.setTextDatum(middle_center);
  frame.drawString(buf, PANEL_X + PANEL_W / 2 + 14, y + CLOCK_H / 2 + 2);
}

static void draw_button(int x, int y, int w, int h, const char* label) {
  frame.fillRoundRect(x, y, w, h, 8, BUTTON);
  frame.setFont(&fonts::FreeSansBold12pt7b);
  frame.setTextColor(TEXT);
  frame.setTextDatum(middle_center);
  frame.drawString(label, x + w / 2, y + h / 2);
}

// Engine score shown from White's side, like a chess clock / database would.
static void fmt_score(int s, char* out) {
  if (s >= MATE_BOUND) snprintf(out, 16, "#%d", (MATE - s + 1) / 2);
  else if (s <= -MATE_BOUND) snprintf(out, 16, "#-%d", (MATE + s + 1) / 2);
  else snprintf(out, 16, "%+.2f", s / 100.0);
}

enum { STATUS_Y = TOP_CLOCK_Y + CLOCK_H + 12, PROMO_Y = STATUS_Y + 36, MOVES_Y = 214, MOVE_ROWS = 4 };

// The last few moves in SAN, numbered from the first position the board
// knows, each engine move followed by its score (or "book").
static void draw_moves() {
  frame.setTextDatum(top_left);
  int black_first = (game.stm ^ (game.hply & 1)) == ::BLACK;
  int first_row = 0, rows = (game.hply + black_first + 1) / 2;
  if (rows > MOVE_ROWS) first_row = rows - MOVE_ROWS;
  for (int row = first_row; row < rows; row++) {
    int y = MOVES_Y + (row - first_row) * 26;
    char buf[16];
    snprintf(buf, sizeof buf, "%d.", row + 1);
    frame.setFont(&fonts::FreeSans12pt7b);
    frame.setTextColor(DIM);
    frame.drawString(buf, PANEL_X + 8, y);
    for (int c = 0; c < 2; c++) {
      int ply = row * 2 + c - black_first, x = PANEL_X + 50 + c * 130;
      if (ply < 0 || ply >= game.hply) continue;
      frame.setFont(&fonts::FreeSans12pt7b);
      frame.setTextColor(ply == game.hply - 1 ? TEXT : DIM);
      frame.drawString(san_of(ply), x, y);
      int32_t e = eval_at(ply);
      if (e == EVAL_NONE) continue;
      if (e == EVAL_BOOK) snprintf(buf, sizeof buf, "book");
      else fmt_score(e, buf);
      frame.setFont(&fonts::FreeSans9pt7b);
      frame.setTextColor(DIM);
      frame.drawString(buf, x + 80, y + 4);
    }
  }
}

static void draw_status() {
  frame.setTextDatum(top_left);
  int x = PANEL_X + 12, y = STATUS_Y;
  auto line = [&](uint16_t col, const char* s) {
    frame.setTextColor(col);
    frame.drawString(s, x, y);
    y += 30;
  };
  char buf[32];
  frame.setFont(&fonts::FreeSansBold12pt7b);
  if (promo_from >= 0) {
    line(TEXT, "Promote to");
    static const int types[4] = {QUEEN, ROOK, BISHOP, KNIGHT};
    for (int i = 0; i < 4; i++) {
      int bx = PANEL_X + 12 + i * 76;
      RGB bg = (i & 1) ? DARK : LIGHT;
      frame.fillRect(bx, PROMO_Y, SQ, SQ, c565(bg));
      draw_piece(bx, PROMO_Y, make_piece(human, types[i]), bg);
    }
    return;
  }
  // first line: what is going on; second: the engine's depth and score
  if (phase == UCI) {
    if (uci_side < 0) snprintf(buf, sizeof buf, "UCI via USB");
    else snprintf(buf, sizeof buf, "UCI: CST is %s", uci_side == ::WHITE ? "White" : "Black");
    line(ACTIVE, buf);
  } else if (phase == OVER) {
    line(ACTIVE, result[0]);
    line(ACTIVE, result[1]);
  } else if (phase == ENGINE) line(TEXT, "Thinking...");
  else line(TEXT, game.in_check() ? "Check!" : "Your move");
  frame.setFont(&fonts::FreeSans12pt7b);
  if (phase == OVER) {
    // the result took the second line
  } else if (eng_busy && info_depth && (phase == ENGINE || phase == UCI)) {
    char sc[16];
    fmt_score(game.stm == ::WHITE ? info_score : -info_score, sc);
    snprintf(buf, sizeof buf, "depth %d   %s", info_depth, sc);
    line(DIM, buf);
  } else if (last_move && game.hply) {
    snprintf(buf, sizeof buf, "%s %s", last_from_book ? "book" : "last", san_of(game.hply - 1));
    line(DIM, buf);
  } else {
    y += 30;
  }
  if (phase != UCI) {
    snprintf(buf, sizeof buf, "%s   %s", TCS[tc_game].name, use_nnue && nnue_ok ? "NNUE" : "PeSTO");
    line(DIM, buf);
  }
  draw_moves();
}

static void draw_panel() {
  frame.fillRect(PANEL_X, 0, PANEL_W, SCREEN_H, PANEL_BG);
  int top = flipped ? ::WHITE : ::BLACK;
  draw_clock(top, TOP_CLOCK_Y);
  draw_clock(top ^ 1, BOTTOM_CLOCK_Y);
  draw_status();
  draw_button(BACK_X, BUTTON_Y, BUTTON_W, BUTTON_H, "Back");
  draw_button(MENU_BTN_X, BUTTON_Y, BUTTON_W, BUTTON_H, "Menu");
}

// Menu: a column of full-width buttons.
enum { MENU_X = 150, MENU_W = 500, MENU_Y0 = 56, MENU_H = 42, MENU_GAP = 5, MENU_ITEMS = 9 };

enum { END_ITEMS = 4 };

static bool game_on() { return phase == HUMAN || phase == ENGINE; }

// Resign / claim win / draw (item 7) is for board play only: under UCI the
// GUI or cutechess adjudicates, so the item is not shown at all.
enum { FINISH_ITEM = 7 };

static int menu_items() { return menu_page ? END_ITEMS : MENU_ITEMS - (phase == UCI); }

static int menu_item_id(int i) { return phase == UCI && i >= FINISH_ITEM ? i + 1 : i; }

static void menu_label(int i, char* out) {
  if (menu_page) {
    static const char* const END[END_ITEMS] = {"Resign - you lose", "Claim win - you win", "Agree draw",
                                               "Back"};
    snprintf(out, 48, "%s%s", END[i], i < 3 && !game_on() ? " (no game on)" : "");
    return;
  }
  switch (menu_item_id(i)) {
    case 0:
      snprintf(out, 48, phase == UCI ? "Leave UCI - play from here" : "New game - play White");
      break;
    case 1: snprintf(out, 48, "New game - play Black"); break;
    case 2:
      snprintf(out, 48, "Time: %s%s", TCS[tc_index].name, tc_index != tc_game ? " (next game)" : "");
      break;
    case 3: snprintf(out, 48, "Your clock: %s", human_clock ? "counts down" : "off - no time limit"); break;
    case 4: snprintf(out, 48, "Opening book: %s", use_book ? "on" : "off"); break;
    case 5:
      snprintf(out, 48, "Evaluation: %s", !nnue_ok ? "PeSTO (no net)" : use_nnue ? "NNUE" : "PeSTO");
      break;
    case 6: snprintf(out, 48, "Flip board"); break;
    case 7: snprintf(out, 48, "Resign / claim win / draw"); break;
    default: snprintf(out, 48, "Close"); break;
  }
}

static void draw_menu() {
  frame.fillScreen(PANEL_BG);
  frame.setFont(&fonts::FreeSansBold18pt7b);
  frame.setTextColor(ACTIVE);
  frame.setTextDatum(top_center);
  frame.drawString(menu_page ? "Finish this game" : "Chess System Tal Retro", SCREEN_W / 2, 14);
  for (int i = 0; i < menu_items(); i++) {
    char label[48];
    menu_label(i, label);
    draw_button(MENU_X, MENU_Y0 + i * (MENU_H + MENU_GAP), MENU_W, MENU_H, label);
  }
}

static char panel_sig[96], panel_clk[2][12];  // what the panel shows (refresh_panel)

static void panel_state(char* sig, char clk[2][12]) {
  snprintf(sig, sizeof panel_sig, "%d %d %d %d %d %d %d %u %d", int(phase), int(eng_busy),
           int(info_depth), int(info_score), game.hply, promo_from, int(last_from_book),
           unsigned(last_move), int(flipped));
  clock_text(::WHITE, clk[0]);
  clock_text(::BLACK, clk[1]);
}

static void redraw() {
  if (menu_open) {
    draw_menu();
  } else {
    draw_board();
    draw_panel();
  }
  lcd_push(0, 0, SCREEN_W, SCREEN_H);
  panel_state(panel_sig, panel_clk);
  dirty = false;
}

// Clocks and status, a few times a second - but only what changed reaches
// the panel. Every copy into the PSRAM frame buffer competes with the LCD's
// DMA reading it, and the IDF 4.4 RGB driver shows that as flicker.
static void refresh_panel() {
  if (menu_open) return;
  char sig[sizeof panel_sig], clk[2][12];
  panel_state(sig, clk);
  if (strcmp(sig, panel_sig)) {
    draw_panel();
    lcd_push(PANEL_X, 0, PANEL_W, SCREEN_H);
  } else {
    int top = flipped ? ::WHITE : ::BLACK;
    for (int side = 0; side < 2; side++) {
      if (!strcmp(clk[side], panel_clk[side])) continue;
      int y = side == top ? TOP_CLOCK_Y : BOTTOM_CLOCK_Y;
      draw_clock(side, y);
      lcd_push(PANEL_X + 8, y, PANEL_W - 16, CLOCK_H);
    }
  }
  strcpy(panel_sig, sig);
  memcpy(panel_clk, clk, sizeof clk);
}

// ---------------------------------------------------------------- touch

// Resign (0), claim a win (1) or agree a draw (2): the game ends like a mate.
static void end_game(int how) {
  stop_engine();
  const char* you = human == ::WHITE ? "White" : "Black";
  const char* cst = human == ::WHITE ? "Black" : "White";
  char a[24], b[24];
  if (how == 0) {
    snprintf(a, sizeof a, "%s resigns", you);
    snprintf(b, sizeof b, "%s wins", cst);
  } else if (how == 1) {
    snprintf(a, sizeof a, "Win claimed");
    snprintf(b, sizeof b, "%s wins", you);
  } else {
    snprintf(a, sizeof a, "Draw agreed");
    snprintf(b, sizeof b, "Draw");
  }
  set_result(a, b);
}

static void tap_menu(int x, int y) {
  if (x < MENU_X || x >= MENU_X + MENU_W || y < MENU_Y0) return;
  int i = (y - MENU_Y0) / (MENU_H + MENU_GAP);
  if (i >= menu_items() || (y - MENU_Y0) % (MENU_H + MENU_GAP) >= MENU_H) return;
  if (menu_page) {
    if (i < 3 && game_on()) {
      end_game(i);
      set_menu(false);
    } else {
      menu_page = 0;
    }
    dirty = true;
    return;
  }
  switch (menu_item_id(i)) {
    case 0:
      set_menu(false);
      if (phase == UCI) play_from_here();
      else new_game(::WHITE);
      break;
    case 1: set_menu(false); new_game(::BLACK); break;
    case 2:
      tc_index = (tc_index + 1) % NUM_TCS;
      prefs.putInt("tc", tc_index);
      if (game.hply == 0 && phase == HUMAN) {  // nothing played yet: apply now
        tc_game = tc_index;
        clock_ms[0] = clock_ms[1] = int32_t(TCS[tc_game].base_ms);
        turn_start = millis();
      }
      break;
    case 3:
      // off: your clock stands still (no flag); on again: it runs from now
      human_clock = !human_clock;
      prefs.putBool("hclock", human_clock);
      turn_start = millis();
      break;
    case 4:
      use_book = !use_book;
      prefs.putBool("book", use_book);
      break;
    case 5:
      if (nnue_ok) {
        use_nnue = !use_nnue;
        prefs.putBool("nnue", use_nnue);
      }
      break;
    case 6: flipped = !flipped; set_menu(false); break;
    case 7: menu_page = 1; break;
    default: set_menu(false); break;
  }
  dirty = true;
}

// Forgiving touch: taps a little outside the intended square snap to the
// nearest square that makes sense: a legal target of the selected piece, or
// one of your pieces that can move.
static Move legal_now[MAX_MOVES];
static int nlegal_now;
static u64 legal_key = ~0ull;
static int drag_from = -1;  // piece selected by the current press (drag to move)

static void refresh_legal() {
  if (legal_key == game.key) return;
  nlegal_now = game.gen_legal(legal_now);
  legal_key = game.key;
}

static bool movable(int s) {
  refresh_legal();
  for (int i = 0; i < nlegal_now; i++)
    if (move_from(legal_now[i]) == s) return true;
  return false;
}

static int snap(int x, int y, bool targets_only) {
  auto wanted = [&](int s) { return is_target(s) || (!targets_only && movable(s)); };
  int s = square_at(x, y < 8 * SQ ? y : 8 * SQ - 1);
  if (s >= 0 && wanted(s)) return s;
  int best = -1, best_d = (SQ * 2 / 3) * (SQ * 2 / 3);
  for (int c = 0; c < 128; c++) {
    if (!on_board(c) || !wanted(c)) continue;
    int cx, cy;
    square_xy(c, cx, cy);
    cx += SQ / 2;
    cy += SQ / 2;
    int d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
    if (d < best_d) { best_d = d; best = c; }
  }
  return best >= 0 ? best : s;
}

static void select_square(int s) {
  refresh_legal();
  selected = s;
  nsel = 0;
  for (int i = 0; i < nlegal_now; i++)
    if (move_from(legal_now[i]) == s) sel_moves[nsel++] = legal_now[i];
}

static void move_to_square(int s) {
  for (int i = 0; i < nsel; i++)
    if (move_to(sel_moves[i]) == s) {
      Move m = sel_moves[i];
      if (move_promo(m)) {
        promo_from = selected;
        promo_to = s;
        dirty = true;
        return;
      }
      play(m);
      return;
    }
}

static void tap_board(int x, int y) {
  if (phase != HUMAN) return;
  int s = snap(x, y, false);
  if (s < 0) return;
  if (selected >= 0 && is_target(s)) {
    move_to_square(s);
  } else if (movable(s)) {
    select_square(s);  // tapping the selected piece again keeps it selected
    drag_from = s;
  } else {
    selected = -1;
    nsel = 0;
  }
  dirty = true;
}

// Release after pressing a piece: if the finger moved to another square,
// treat it as a drag and play the move to the (snapped) target.
static void release_board(int x, int y) {
  int from = drag_from;
  drag_from = -1;
  if (phase != HUMAN || from < 0 || selected != from || menu_open || promo_from >= 0) return;
  if (x >= PANEL_X || square_at(x, y < 8 * SQ ? y : 8 * SQ - 1) == from) return;
  int s = snap(x, y, true);
  if (s >= 0 && is_target(s)) move_to_square(s);
}

static void tap_promo(int x, int y) {
  static const int types[4] = {QUEEN, ROOK, BISHOP, KNIGHT};
  int col = (x - PANEL_X - 12) / 76;
  if (x >= PANEL_X + 12 && col < 4 && y >= PROMO_Y && y < PROMO_Y + SQ) {
    int t = types[col];
    for (int i = 0; i < nsel; i++)
      if (move_to(sel_moves[i]) == promo_to && move_promo(sel_moves[i]) == t) {
        play(sel_moves[i]);
        return;
      }
  }
  promo_from = -1;  // tapped elsewhere: cancel
  dirty = true;
}

static void tap(int x, int y) {
  if (menu_open) return tap_menu(x, y);
  if (promo_from >= 0) return tap_promo(x, y);
  if (x < PANEL_X) return tap_board(x, y);
  if (y >= BUTTON_Y - 8 && x < MENU_BTN_X - 4) return take_back();
  if (y >= BUTTON_Y - 8) {
    set_menu(true);
    dirty = true;
  }
}

// ---------------------------------------------------------------- serial

static void dump_frame() {
  usb_print("FRAME\n");
  static uint16_t line[SCREEN_W];
  for (int y = 0; y < SCREEN_H; y++) {
    for (int x = 0; x < SCREEN_W; x++) line[x] = frame.readPixel(x, y);
    usb_write((uint8_t*)line, sizeof line);
  }
  // A trailer line marks the end of the frame for tools/grab.py-style readers
  // (they read exactly the frame size and ignore it).
  usb_print("\nEND FRAME\n");
}

// Boot report: setup() runs while the PC's USB connection is still coming
// up, so its lines are kept here too and the serial command "info" repeats
// them with the current memory figures.
static char boot_log[640];

static void boot_note(const char* what) {
  size_t n = strlen(boot_log);
  snprintf(boot_log + n, sizeof boot_log - n, "%-12s internal free %3u KB, largest %3u KB\n", what,
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

// ---- UCI over USB ----

// All output goes through usb_out (not Serial.print), see usb_out.cpp.
static void uci_send(const char* fmt, ...) {
  char line[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  usb_println(line);
}

static void enter_uci() {
  stop_engine();
  autoplay = false;
  set_menu(false);
  selected = -1;
  nsel = 0;
  promo_from = -1;
  phase = UCI;
  dirty = true;
}

static void uci_go_cmd(const char* line) {
  if (phase != UCI) enter_uci();
  stop_engine();
  // The board's colour: its side goes to the bottom, its clock box says CST.
  uci_side = game.stm;
  uci_last_bestmove[0] = 0;
  if (flipped != (uci_side == ::BLACK)) flipped = !flipped;
  dirty = true;
  if (uci_own_book) {
    Move m = book_pick(game, book_start, size_t(book_end - book_start), esp_random());
    if (m) {
      char mv[6];
      move_to_uci(m, mv);
      uci_send("bestmove %s", mv);
      snprintf(uci_last_bestmove, sizeof uci_last_bestmove, "%s", mv);
      return;
    }
  }
  apply_eval_mode();
  memcpy(&eng_board, &game, sizeof game);
  eng_limits = uci_go(line, game.stm, 100);  // USB + bridge + screen ~50 ms per move
  eng_uci = true;
  info_depth = 0;
  engine_command('g');
  dirty = true;
}

// The engine answered a UCI "go" (its task has sent the bestmove): show the
// move on the board (the GUI's next "position" replaces the board anyway).
static void uci_done() {
  Move m = eng_move;  // already sent as bestmove by the engine task
  eng_uci = false;
  if (m && game.hply < MAX_GAME - MAX_PLY - 2) {
    record_eval(m, white_view(eng_score));
    game.make(m);
    last_move = m;
    beep();
  }
  dirty = true;
}

static void uci_position_cmd(const char* args) {
  stop_engine();
  if (!uci_position(game, args)) uci_send("info string bad position or move");
  last_move = game.hply ? game.hist[game.hply - 1].move : 0;
  last_from_book = false;
  if (phase == UCI) {
    selected = -1;
    nsel = 0;
    dirty = true;
  } else {
    play_from_here();  // set up from the PC, then play it by touch
  }
}

static void serial_line(char* line) {
  if (!strcmp(line, "uci")) {
    enter_uci();
    usb_println("id name CST Retro (ESP32 7in)");
    usb_println("id author Chris Whittington");
    usb_println("option name OwnBook type check default false");
    usb_println("option name Eval type combo default PeSTO var PeSTO var NNUE");
    uci_send("uciok");
  } else if (!strcmp(line, "isready")) {
    uci_send("readyok");
  } else if (!strcmp(line, "ucinewgame")) {
    stop_engine();
    clear_evals();
    search_new_game();
  } else if (!strncmp(line, "setoption name Eval value ", 26)) {
    use_nnue = nnue_ok && !strcmp(line + 26, "NNUE");
    if (!strcmp(line + 26, "NNUE") && !nnue_ok) uci_send("info string no NNUE in this firmware");
  } else if (!strncmp(line, "setoption name OwnBook value ", 29)) {
    uci_own_book = !strcmp(line + 29, "true");
  } else if (!strncmp(line, "position ", 9)) {
    uci_position_cmd(line + 9);
  } else if (!strncmp(line, "go", 2) && (line[2] == 0 || line[2] == ' ')) {
    uci_go_cmd(line);
  } else if (!strcmp(line, "stop")) {
    if (eng_uci) {
      search_stop = true;  // the search ends and answers bestmove
    } else if (phase == UCI && uci_last_bestmove[0] && !eng_busy) {
      // Already answered, yet the GUI still waits for a move: our bestmove
      // line must have been lost on the way (cutechess sends "stop" only to
      // a thinking engine). Say it again rather than forfeit the game.
      uci_send("bestmove %s", uci_last_bestmove);
    }
  } else if (!strcmp(line, "quit")) {
    if (phase == UCI) play_from_here();
  } else if (!strcmp(line, "d")) {
    dump_frame();
  } else if (line[0] == 't' && line[1] == ' ') {
    int x, y;
    if (sscanf(line + 2, "%d %d", &x, &y) == 2) tap(x, y);
  } else if (!strcmp(line, "n")) {
    autoplay = false;
    new_game(::WHITE);
  } else if (!strcmp(line, "a") && phase == HUMAN) {
    autoplay = true;
    usb_println("autoplay:");
    start_engine();
  } else if ((!strcmp(line, "b") || !strcmp(line, "s")) && phase != ENGINE && !eng_busy) {
    memcpy(&eng_board, &game, sizeof game);
    engine_command(line[0]);
  } else if (!strcmp(line, "info")) {
    usb_print(boot_log);
    usb_printf("eval %s, PSRAM free %u KB\n", use_nnue && nnue_ok ? "NNUE" : "PeSTO",
               unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    uint32_t dropped, wait_ms;
    usb_out_stats(&dropped, &wait_ms);
    usb_printf("up %lu s, USB dropped %lu bytes, longest PC wait %lu ms\n",
               (unsigned long)(millis() / 1000), (unsigned long)dropped, (unsigned long)wait_ms);
    boot_note("now");
  } else if (!strcmp(line, "touchlog")) {
    touch_log = !touch_log;
    usb_printf("touch log %s\n", touch_log ? "on" : "off");
  }
}

static void serial_command() {
  enum { SERIAL_LINE_BYTES = 8192 };  // "position startpos moves ..." can be long
  static char* buf = psram_new<char>(SERIAL_LINE_BYTES);
  static int n = 0;
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (n < SERIAL_LINE_BYTES - 1) buf[n++] = char(c);
      continue;
    }
    buf[n] = 0;
    n = 0;
    char* line = buf;
    while (*line == ' ') line++;
    serial_line(line);
  }
}

// ---------------------------------------------------------------- main

void setup() {
  // The default 256-byte receive buffer overflowed on long UCI "position ...
  // moves" lines arriving while the screen redraws (bytes lost, newline too).
  // 8 KB holds a 300-ply game's "position startpos moves ..." five times over.
  Serial.setRxBufferSize(8192);
  Serial.begin(115200);  // input only; output goes through usb_out
  esp_log_level_set("*", ESP_LOG_NONE);  // nothing else may write to the UCI stream
  usb_out_begin();
  if (!&game || !clock_hist) usb_println("PSRAM allocation failed");
  boot_note("start");

  // Internal SRAM, biggest contiguous blocks first: the engine task's stack
  // (20 KB in one piece, taken while the big block is whole), the net (96 KB
  // in one piece, PeSTO only without it), then the biggest TT that leaves room
  // for the display's bounce buffers (2 x 12.5 KB) and 18 KB for the system.
  // Engine stack: ~11.3 KB used at selective depth 42; 20 KB covers the
  // 64-ply limit (16 KB overflowed in a long endgame search).
  enum { ENGINE_STACK = 20 * 1024, LATER = 2 * 13 * 1024 + 18 * 1024 };
  static StaticTask_t eng_tcb;
  StackType_t* eng_stack = (StackType_t*)heap_caps_malloc(ENGINE_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  boot_note(eng_stack ? "engine stack" : "NO STACK");
  load_net();
  boot_note(nnue_ok ? "NNUE loaded" : "NNUE FAILED");
  size_t tt_bytes = 128 * 1024;
  void* mem = nullptr;
  for (; tt_bytes >= 8 * 1024; tt_bytes /= 2) {
    mem = heap_caps_malloc(tt_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (mem && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= LATER) break;
    heap_caps_free(mem);
    mem = nullptr;
  }
  tt.init(mem, mem ? tt_bytes : 0);
  {
    char t[24];
    snprintf(t, sizeof t, "TT %u KB", mem ? unsigned(tt_bytes >> 10) : 0);
    boot_note(t);
  }

  board_begin();  // USB selected, LCD and touch out of reset
  frame.setPsram(true);
  frame.setColorDepth(16);
  frame.createSprite(SCREEN_W, SCREEN_H);
  frame.fillScreen(TFT_BLACK);
  bool lcd_ok = lcd_begin();
  if (lcd_ok) lcd_push(0, 0, SCREEN_W, SCREEN_H);
  boot_note(lcd_ok ? "LCD ok" : "LCD FAILED");
  exio_write(exio | EXIO_BL);

  // The engine keeps core 0 busy while it thinks: stop watching its idle task.
#if ESP_IDF_VERSION_MAJOR >= 5
  // (disableCore0WDT() on IDF 5 leaves the idle hook feeding a watchdog it
  // was removed from - an error line every tick.)
  esp_task_wdt_config_t wdt = {};
  wdt.timeout_ms = 5000;
  wdt.idle_core_mask = 1 << 1;
  wdt.trigger_panic = false;
  esp_task_wdt_reconfigure(&wdt);
#else
  disableCore0WDT();
#endif
  if (eng_stack)
    eng_task = xTaskCreateStaticPinnedToCore(engine_loop, "engine", ENGINE_STACK, nullptr, 1, eng_stack,
                                             &eng_tcb, 0);
  if (!eng_task) usb_println("engine task not created: out of internal SRAM");
  boot_note(eng_task ? "engine task" : "NO ENGINE");
  search_init(&tt);  // before anything (new_game) uses the TT - not in the engine task
  usb_printf("LCD %s, TT %u KB\n", lcd_ok ? "ok" : "FAILED", mem ? unsigned(tt_bytes >> 10) : 0);

  prefs.begin("cstretro", false);
  tc_index = prefs.getInt("tc", DEFAULT_TC);
  if (tc_index < 0 || tc_index >= NUM_TCS) tc_index = DEFAULT_TC;
  ply_eval = (PlyEval*)heap_caps_malloc(MAX_GAME * sizeof(PlyEval), MALLOC_CAP_SPIRAM);
  san_board = (Board*)heap_caps_malloc(sizeof(Board), MALLOC_CAP_SPIRAM);
  use_book = prefs.getBool("book", true);
  human_clock = prefs.getBool("hclock", true);
  use_nnue = nnue_ok && prefs.getBool("nnue", true);
  new_game(::WHITE);
  usb_printf("CST Retro ready, internal free %u KB (largest %u KB)\n",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

// Touch diagnostics on serial: every press and release with its raw
// position and square, plus the slowest loop pass since the last print.
static u32 loop_max_ms;

void loop() {
  u32 loop_t0 = millis();
  touch_update();
  serial_command();

  if (touch_log && (touch.pressed || touch.released)) {
    int s = square_at(touch.x, touch.y);
    char name[3] = "--";
    if (s >= 0) { name[0] = char('a' + sq_file(s)); name[1] = char('1' + sq_rank(s)); }
    usb_printf("touch %s x=%d y=%d sq=%s (slowest loop %u ms)\n",
                  touch.pressed ? "down" : "up  ", touch.x, touch.y, name, unsigned(loop_max_ms));
    loop_max_ms = 0;
  }
  if (touch.pressed) tap(touch.x, touch.y);
  if (touch.released) release_board(touch.x, touch.y);

  if (eng_done && phase == UCI) {
    eng_done = false;
    if (eng_uci) uci_done();
  } else if (eng_done && (!last_from_book || int32_t(millis() - book_move_at) >= 0)) {
    eng_done = false;
    if (phase == ENGINE && eng_move) play(eng_move, true);
  }

  // flag fall
  if ((phase == HUMAN || phase == ENGINE) && timed() && clock_now(game.stm) <= 0) {
    if (phase == ENGINE) stop_engine();
    clock_ms[game.stm] = 0;
    const char* wins = game.stm == ::WHITE ? "Black wins" : "White wins";
    set_result(game.stm == ::WHITE ? "White flagged" : "Black flagged",
               can_mate(game.stm ^ 1) ? wins : "Draw");
    dirty = true;
  }

  static u32 last_panel;
  if (dirty) {
    redraw();
    last_panel = millis();
  } else if (millis() - last_panel >= 200) {
    refresh_panel();
    last_panel = millis();
  }
  u32 dt = millis() - loop_t0;
  if (dt > loop_max_ms) loop_max_ms = dt;
  delay(5);
}
