// Chess System Tal Retro on the 4.0" ESP32-32E display (lcdwiki E32R40T, sold
// as Hosyond): a classic ESP32 (no PSRAM, 4 MB flash), ST7796S 320x480 SPI
// panel, XPT2046 resistive touch. The game logic is the other boards'; this
// file is for the small machine:
//
// - No frame buffer (it would need 300 KB): everything is drawn straight to
//   the panel. Each board square is rendered in a 40x40 sprite and pushed
//   only when what it shows has changed; panel text is drawn with a
//   background, so it never flickers.
// - Everything lives in internal SRAM, so the game history is shorter
//   (CST_MAX_GAME=512) and SAN is worked out on the game itself.
// - The NNUE runs in plain C++ (the SIMD kernels are ESP32-S3 only).
// - Serial is a CH340 UART at 921600 baud (usb_out_uart.cpp).
//
// Screen (portrait): the board (320x320, 40 px squares) on top; below it the
// two clocks side by side, a status line, the move list and BACK / MENU.
// Display and touch settings as in Backgammon-NN-esp32's board header.
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
//   Debug: (no "d" frame dump here: no frame buffer, and this panel returns
//   nothing on read-back) "t X Y" simulated tap, "n" new game as White, "a"
//   autoplay (the engine also plays your side, moves printed), "b" perft
//   bench, "s" search bench, "touchlog" toggles printing every touch.
#include <LovyanGFX.hpp>
#include <Preferences.h>
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

enum { SQ = 40, SCREEN_W = 320, SCREEN_H = 480, PANEL_Y = 320 };
enum { CLOCK_Y = PANEL_Y + 4, CLOCK_H = 38, CLOCK_W = 152, STATUS_Y = CLOCK_Y + CLOCK_H + 5 };
enum { MOVES_Y = STATUS_Y + 20, MOVE_ROWS = 2, BUTTON_Y = 434, BUTTON_H = 42, BUTTON_W = 152 };

struct RGB {
  uint8_t r, g, b;
};
static const RGB LIGHT = {240, 217, 181}, DARK = {181, 136, 99};
static const RGB LAST_TINT = {214, 200, 80}, SELECT_TINT = {100, 170, 90};

static uint16_t c565(RGB c) { return lgfx::color565(c.r, c.g, c.b); }
static RGB mix(RGB a, RGB b, int t) {  // t/255 of b over a
  return {uint8_t((a.r * (255 - t) + b.r * t) / 255), uint8_t((a.g * (255 - t) + b.g * t) / 255),
          uint8_t((a.b * (255 - t) + b.b * t) / 255)};
}

static const uint16_t PANEL_BG = 0x2124, TEXT = 0xFFFF, DIM = 0x9CD3, BUTTON = 0x4A69,
                      ACTIVE = 0xFEA0, CLOCK_BG = 0x18C3;

// ---------------------------------------------------------------- hardware

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 panel;
  lgfx::Bus_SPI bus;
  lgfx::Light_PWM light;
  lgfx::Touch_XPT2046 touch;

 public:
  LGFX() {
    {
      auto c = bus.config();
      c.spi_host = HSPI_HOST;
      c.spi_mode = 0;
      c.freq_write = 40000000;
      c.freq_read = 16000000;
      c.pin_sclk = 14; c.pin_mosi = 13; c.pin_miso = 12; c.pin_dc = 2;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      bus.config(c);
      panel.setBus(&bus);
    }
    {
      auto c = panel.config();
      c.pin_cs = 15; c.pin_rst = -1; c.pin_busy = -1;
      c.panel_width = 320; c.panel_height = 480;
      c.readable = true;
      c.invert = false;
      c.rgb_order = false;
      c.bus_shared = true;
      panel.config(c);
    }
    {
      auto c = light.config();
      c.pin_bl = 27; c.invert = false; c.freq = 44100; c.pwm_channel = 7;
      light.config(c);
      panel.setLight(&light);
    }
    {
      auto c = touch.config();
      c.x_min = 300; c.x_max = 3900; c.y_min = 200; c.y_max = 3800;
      c.pin_int = 36;
      c.bus_shared = true;
      c.spi_host = HSPI_HOST;
      c.freq = 1000000;
      c.pin_sclk = 14; c.pin_mosi = 13; c.pin_miso = 12; c.pin_cs = 33;
      touch.config(c);
      panel.setTouch(&touch);
    }
    setPanel(&panel);
  }
};

static LGFX lcd;
static LGFX_Sprite sqs(&lcd);  // one board square, rendered then pushed

// Portrait, USB socket at the bottom.
enum { ROTATION = 0 };

struct TouchState {
  bool down, pressed, released;
  int x, y;
};
static TouchState touch;

// Resistive touch: a change of state must be seen twice running, and the
// position is the last one read while down (the release has none).
static void touch_update() {
  touch.pressed = touch.released = false;
  static bool last_raw;
  int32_t x, y;
  bool raw = lcd.getTouch(&x, &y);
  if (raw) {
    touch.x = int(x);
    touch.y = int(y);
  }
  bool stable = raw == last_raw;
  last_raw = raw;
  if (!stable || raw == touch.down) return;
  touch.pressed = raw;
  touch.released = !raw;
  touch.down = raw;
}

static void board_begin() {
  lcd.init();
  lcd.setRotation(ROTATION);
  lcd.fillScreen(TFT_BLACK);
  lcd.setBrightness(200);
  sqs.setColorDepth(16);
  sqs.createSprite(SQ, SQ);
}

static void beep() {}  // the board has a speaker socket; nothing is fitted

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
static Board game;
static int human = ::WHITE;
static bool flipped = false;
static Phase phase = HUMAN;
static int32_t clock_ms[2];
typedef int32_t ClockPair[2];
// (Heap, not static: the classic ESP32's static data segment is small.)
static ClockPair* clock_hist = static_cast<ClockPair*>(calloc(MAX_GAME, sizeof(ClockPair)));  // clocks at the start of each ply
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
// each of its moves (White's view).
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

// The embedded net: its 98 KB of weights are read where they are, in flash -
// this machine has 156 KB of RAM in all - and only the accumulators (17 KB)
// take memory.
static void load_net() {
  int n, h;
  size_t size = size_t(net_end - net_start);
  if (!nnue_file_shape(net_start, size, &n, &h)) {
    usb_println("net.bin is not a CSTN net: PeSTO only");
    return;
  }
  void* a = heap_caps_aligned_alloc(16, nnue_acc_bytes(n), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!a) {
    usb_println("no SRAM for the net's accumulators: PeSTO only");
    return;
  }
  nnue_ok = nnue_load_in_place(net_start, size, a);
  usb_printf("NNUE 768->%d->1 %s (weights in flash)\n", n, nnue_ok ? "loaded" : "failed");
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

// SAN of the last SAN_PLIES moves of the game (undone to the first of them,
// then replayed) and cached until the game changes.
enum { SAN_PLIES = 10 };
static char san_text[SAN_PLIES][10];
static int san_base, san_hply = -1;
static u64 san_key;

static void refresh_san() {
  if (san_hply == game.hply && san_key == game.key) return;
  int hply = game.hply;
  san_hply = hply;
  san_key = game.key;
  san_base = hply > SAN_PLIES ? hply - SAN_PLIES : 0;
  // On the game itself (no room for a copy): take the moves back, then replay
  // them. The moves are saved first: working out SAN tries moves from each
  // position, which overwrites the history entries of the plies not yet
  // replayed (replaying from the history itself left a different position on
  // the board - "checkmate" in a drawn ending).
  Move moves[SAN_PLIES];
  for (int p = san_base; p < hply; p++) moves[p - san_base] = game.hist[p].move & 0x00FFFFFF;
  while (game.hply > san_base) game.unmake();
  for (int p = san_base; p < hply; p++) {
    move_to_san(game, moves[p - san_base], san_text[p - san_base]);
    game.make(moves[p - san_base]);
  }
}

static const char* san_of(int ply) {
  refresh_san();
  if (ply < san_base || ply >= game.hply) return "?";
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

// A piece into the square sprite (or, for the promotion chooser, the sprite
// pushed elsewhere).
static void draw_piece(int p, RGB bg) {
  const uint8_t* img = PIECE_IMG[piece_color(p)][piece_type(p)];
  for (int y = 0; y < PIECE_PX; y++)
    for (int x = 0; x < PIECE_PX; x++) {
      int a = img[(y * PIECE_PX + x) * 2 + 1];
      if (!a) continue;
      uint8_t g = img[(y * PIECE_PX + x) * 2];
      sqs.drawPixel(x, y, c565(mix(bg, {g, g, g}, a)));
    }
}

// What a square shows, packed: redrawn only when this changes.
static uint16_t shown[64];
enum { SHOWN_NONE = 0xFFFF };

static void forget_screen() { memset(shown, 0xFF, sizeof shown); }

static void draw_square(int s, bool in_check) {
  int p = game.sq[s];
  bool last = last_move && (s == move_from(last_move) || s == move_to(last_move));
  bool check = in_check && p != EMPTY && piece_type(p) == KING && piece_color(p) == game.stm;
  uint16_t sig = uint16_t(p | last << 4 | (s == selected) << 5 | is_target(s) << 6 | check << 7 |
                          flipped << 8);
  uint16_t& was = shown[sq_rank(s) * 8 + sq_file(s)];
  if (was == sig) return;
  was = sig;
  RGB bg = ((sq_file(s) + sq_rank(s)) & 1) ? LIGHT : DARK;
  if (last) bg = mix(bg, LAST_TINT, 110);
  if (s == selected) bg = mix(bg, SELECT_TINT, 150);
  sqs.fillScreen(c565(bg));
  if (p != EMPTY) draw_piece(p, bg);
  if (is_target(s)) {
    uint16_t dot = c565(mix(bg, {40, 70, 40}, 150));
    if (p == EMPTY) sqs.fillCircle(SQ / 2, SQ / 2, 6, dot);
    else
      for (int k = 0; k < 3; k++) sqs.drawCircle(SQ / 2, SQ / 2, SQ / 2 - 1 - k, dot);
  }
  if (check)
    for (int k = 0; k < 3; k++) sqs.drawRect(k, k, SQ - 2 * k, SQ - 2 * k, TFT_RED);
  int x, y;
  square_xy(s, x, y);
  sqs.pushSprite(x, y);
}

static void draw_board() {
  bool in_check = game.in_check();
  for (int s = 0; s < 128; s++)
    if (on_board(s)) draw_square(s, in_check);
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
  else if (timed() && !clock_runs(side)) snprintf(buf, 12, "--:--");
  else if (timed()) fmt_clock(clock_now(side), buf);
  else snprintf(buf, 12, "%s", side == human ? "You" : "CST");
}

// A clock box: the top side's on the left, the bottom side's on the right.
static int clock_x(int side) { return side == (flipped ? ::WHITE : ::BLACK) ? 4 : 164; }

static void draw_clock(int side) {
  bool active = phase != OVER && game.stm == side;
  uint16_t bg = active ? ACTIVE : CLOCK_BG;
  int x = clock_x(side);
  lcd.fillRoundRect(x, CLOCK_Y, CLOCK_W, CLOCK_H, 5, bg);
  lcd.fillCircle(x + 14, CLOCK_Y + CLOCK_H / 2, 6, side == ::WHITE ? TFT_WHITE : TFT_BLACK);
  lcd.drawCircle(x + 14, CLOCK_Y + CLOCK_H / 2, 6, DIM);
  char buf[12];
  clock_text(side, buf);
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(active ? TFT_BLACK : TEXT, bg);
  lcd.setTextDatum(middle_center);
  lcd.setTextPadding(0);
  lcd.drawString(buf, x + CLOCK_W / 2 + 10, CLOCK_Y + CLOCK_H / 2 + 1);
}

static void draw_button(int x, int y, int w, int h, const char* label) {
  lcd.fillRoundRect(x, y, w, h, 6, BUTTON);
  lcd.setFont(&fonts::FreeSansBold9pt7b);
  lcd.setTextColor(TEXT, BUTTON);
  lcd.setTextDatum(middle_center);
  lcd.setTextPadding(0);
  lcd.drawString(label, x + w / 2, y + h / 2);
}

// Engine score shown from White's side, like a chess clock / database would.
static void fmt_score(int s, char* out) {
  if (s >= MATE_BOUND) snprintf(out, 16, "#%d", (MATE - s + 1) / 2);
  else if (s <= -MATE_BOUND) snprintf(out, 16, "#-%d", (MATE + s + 1) / 2);
  else snprintf(out, 16, "%+.2f", s / 100.0);
}

// One line under the clocks: what is going on, and the engine's depth and
// score while it thinks.
static void status_text(char* out, size_t size) {
  char sc[16] = "";
  if (eng_busy && info_depth && (phase == ENGINE || phase == UCI))
    fmt_score(game.stm == ::WHITE ? info_score : -info_score, sc);
  if (promo_from >= 0) snprintf(out, size, "Promote to:");
  else if (phase == UCI && uci_side < 0) snprintf(out, size, "UCI via USB");
  else if (phase == UCI && sc[0])
    snprintf(out, size, "UCI: CST is %s   d%d %s", uci_side == ::WHITE ? "White" : "Black", int(info_depth), sc);
  else if (phase == UCI) snprintf(out, size, "UCI: CST is %s", uci_side == ::WHITE ? "White" : "Black");
  else if (phase == OVER) snprintf(out, size, "%s - %s", result[0], result[1]);
  else if (phase == ENGINE && sc[0]) snprintf(out, size, "Thinking...   depth %d   %s", int(info_depth), sc);
  else if (phase == ENGINE) snprintf(out, size, "Thinking...");
  else
    snprintf(out, size, "%s      %s  %s", game.in_check() ? "Check!" : "Your move", TCS[tc_game].name,
             use_nnue && nnue_ok ? "NN" : "PeSTO");
}

static void draw_status(const char* text) {
  lcd.setFont(&fonts::Font2);
  lcd.setTextDatum(top_left);
  lcd.setTextColor(phase == OVER || phase == UCI ? ACTIVE : TEXT, PANEL_BG);
  lcd.setTextPadding(SCREEN_W - 8);
  lcd.drawString(text, 6, STATUS_Y);
  lcd.setTextPadding(0);
}

// The last moves in SAN, numbered from the first position the board knows;
// each engine move is followed by its score (or "book"). During a promotion
// the four pieces to choose from take this place.
static void draw_moves() {
  lcd.fillRect(0, MOVES_Y, SCREEN_W, BUTTON_Y - 4 - MOVES_Y, PANEL_BG);
  if (promo_from >= 0) {
    static const int types[4] = {QUEEN, ROOK, BISHOP, KNIGHT};
    for (int i = 0; i < 4; i++) {
      RGB bg = (i & 1) ? DARK : LIGHT;
      sqs.fillScreen(c565(bg));
      draw_piece(make_piece(human, types[i]), bg);
      sqs.pushSprite(40 + i * 64, MOVES_Y);
    }
    return;
  }
  lcd.setTextDatum(top_left);
  lcd.setTextPadding(0);
  int black_first = (game.stm ^ (game.hply & 1)) == ::BLACK;
  int first_row = 0, rows = (game.hply + black_first + 1) / 2;
  if (rows > MOVE_ROWS) first_row = rows - MOVE_ROWS;
  for (int row = first_row; row < rows; row++) {
    int y = MOVES_Y + 2 + (row - first_row) * 21;
    char buf[16];
    snprintf(buf, sizeof buf, "%d.", row + 1);
    lcd.setFont(&fonts::FreeSans9pt7b);
    lcd.setTextColor(DIM, PANEL_BG);
    lcd.drawString(buf, 4, y);
    for (int c = 0; c < 2; c++) {
      int ply = row * 2 + c - black_first, x = 42 + c * 140;
      if (ply < 0 || ply >= game.hply) continue;
      lcd.setFont(&fonts::FreeSans9pt7b);
      lcd.setTextColor(ply == game.hply - 1 ? TEXT : DIM, PANEL_BG);
      lcd.drawString(san_of(ply), x, y);
      int32_t e = eval_at(ply);
      if (e == EVAL_NONE) continue;
      if (e == EVAL_BOOK) snprintf(buf, sizeof buf, "book");
      else fmt_score(e, buf);
      lcd.setFont(&fonts::Font2);
      lcd.setTextColor(DIM, PANEL_BG);
      lcd.drawString(buf, x + 70, y);
    }
  }
}

// What the panel shows: redrawn in parts as these change.
static char shown_status[64], shown_clk[2][12];
static uint32_t shown_panel = ~0u;
static u64 shown_key;

static uint32_t panel_sig() {
  return uint32_t(phase) | uint32_t(game.stm) << 3 | uint32_t(flipped) << 4 | uint32_t(promo_from >= 0) << 5 |
         uint32_t(game.hply) << 8;
}

static void draw_panel(bool all) {
  uint32_t sig = panel_sig();
  char status[sizeof shown_status], clk[2][12];
  status_text(status, sizeof status);
  clock_text(::WHITE, clk[0]);
  clock_text(::BLACK, clk[1]);
  bool whole = all || sig != shown_panel || game.key != shown_key;
  if (all) {
    lcd.fillRect(0, PANEL_Y, SCREEN_W, SCREEN_H - PANEL_Y, PANEL_BG);
    draw_button(4, BUTTON_Y, BUTTON_W, BUTTON_H, "Back");
    draw_button(164, BUTTON_Y, BUTTON_W, BUTTON_H, "Menu");
  }
  for (int side = 0; side < 2; side++)
    if (whole || strcmp(clk[side], shown_clk[side])) draw_clock(side);
  if (whole || strcmp(status, shown_status)) draw_status(status);
  if (whole) draw_moves();
  shown_panel = sig;
  shown_key = game.key;
  strcpy(shown_status, status);
  memcpy(shown_clk, clk, sizeof clk);
}

// Menu: a column of full-width buttons.
enum { MENU_X = 10, MENU_W = 300, MENU_Y0 = 40, MENU_H = 38, MENU_GAP = 5, MENU_ITEMS = 10 };

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
    case 8: snprintf(out, 48, "Calibrate touch"); break;
    default: snprintf(out, 48, "Close"); break;
  }
}

static void draw_menu() {
  lcd.fillScreen(PANEL_BG);
  lcd.setFont(&fonts::FreeSansBold12pt7b);
  lcd.setTextColor(ACTIVE, PANEL_BG);
  lcd.setTextDatum(top_center);
  lcd.setTextPadding(0);
  lcd.drawString(menu_page ? "Finish this game" : "Chess System Tal Retro", SCREEN_W / 2, 10);
  for (int i = 0; i < menu_items(); i++) {
    char label[48];
    menu_label(i, label);
    draw_button(MENU_X, MENU_Y0 + i * (MENU_H + MENU_GAP), MENU_W, MENU_H, label);
  }
}

static bool menu_shown;  // the menu is on the screen: the game view must be redrawn whole

static void redraw() {
  if (menu_open) {
    draw_menu();
    menu_shown = true;
  } else {
    bool all = menu_shown || shown[0] == SHOWN_NONE;
    if (menu_shown) forget_screen();
    menu_shown = false;
    draw_board();
    draw_panel(all);
  }
  dirty = false;
}

// Clocks and status, a few times a second; only what changed is drawn.
static void refresh_panel() {
  if (!menu_open) draw_panel(false);
}

// Resistive touch is calibrated on the board (tap the four corner arrows)
// and kept in NVS.
static void calibrate() {
  uint16_t cal[8];
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setTextDatum(middle_center);
  lcd.setTextPadding(0);
  lcd.setFont(&fonts::FreeSans12pt7b);
  lcd.drawString("Touch calibration", SCREEN_W / 2, SCREEN_H / 2 - 16);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.drawString("Tap each corner arrow", SCREEN_W / 2, SCREEN_H / 2 + 14);
  lcd.calibrateTouch(cal, TFT_YELLOW, TFT_BLACK, 24);
  prefs.putBytes("cal", cal, sizeof cal);
  menu_shown = true;  // redraw everything
  dirty = true;
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
    case 8: set_menu(false); calibrate(); break;
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
  if (y >= PANEL_Y || square_at(x, y) == from) return;
  int s = snap(x, y, true);
  if (s >= 0 && is_target(s)) move_to_square(s);
}

static void tap_promo(int x, int y) {
  static const int types[4] = {QUEEN, ROOK, BISHOP, KNIGHT};
  int col = (x - 40) / 64;
  if (x >= 40 && col < 4 && (x - 40) % 64 < SQ + 8 && y >= MOVES_Y - 6 && y < MOVES_Y + SQ + 6) {
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
  if (y < PANEL_Y) return tap_board(x, y);
  if (y < BUTTON_Y - 6) return;  // clocks, status, moves: nothing to tap
  if (x < SCREEN_W / 2) return take_back();
  set_menu(true);
  dirty = true;
}

// ---------------------------------------------------------------- serial

// Boot report: setup() runs while the PC's USB connection is still coming
// up, so its lines are kept here too and the serial command "info" repeats
// them with the current memory figures.
static char boot_log[640];

static void boot_note(const char* what) {
  size_t n = strlen(boot_log);
  snprintf(boot_log + n, sizeof boot_log - n, "%-12s internal free %3u KB, largest %3u KB\n", what,
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
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
    usb_println("id name CST Retro (ESP32 E32R40T)");
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
    usb_printf("eval %s\n", use_nnue && nnue_ok ? "NNUE" : "PeSTO");
    uint32_t dropped, wait_ms;
    usb_out_stats(&dropped, &wait_ms);
    usb_printf("up %lu s, USB dropped %lu bytes, longest PC wait %lu ms\n",
               (unsigned long)(millis() / 1000), (unsigned long)dropped, (unsigned long)wait_ms);
    boot_note("now");
  } else if (!strcmp(line, "k")) {
    calibrate();
  } else if (!strcmp(line, "touchlog")) {
    touch_log = !touch_log;
    usb_printf("touch log %s\n", touch_log ? "on" : "off");
  }
}

static void serial_command() {
  enum { SERIAL_LINE_BYTES = 4096 };  // "position startpos moves ..." of a 500-ply game
  static char* buf = static_cast<char*>(malloc(SERIAL_LINE_BYTES));
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
  Serial.setRxBufferSize(4096);  // long "position ... moves" lines arrive while the screen redraws
  Serial.setTxBufferSize(2048);
  Serial.begin(921600);
  esp_log_level_set("*", ESP_LOG_NONE);  // nothing else may write to the UCI stream
  usb_out_begin();
  boot_note("start");

  // Internal SRAM is all there is: about 156 KB free. The net's accumulators,
  // the engine task's stack (20 KB), then the biggest TT that leaves 24 KB
  // for the display driver and the system.
  enum { ENGINE_STACK = 20 * 1024, LATER = 24 * 1024 };
  load_net();
  boot_note(nnue_ok ? "NNUE loaded" : "NNUE FAILED");
  static StaticTask_t eng_tcb;
  StackType_t* eng_stack = (StackType_t*)heap_caps_malloc(ENGINE_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  boot_note(eng_stack ? "engine stack" : "NO STACK");
  size_t tt_bytes = 64 * 1024;
  void* mem = nullptr;
  for (; tt_bytes >= 8 * 1024; tt_bytes /= 2) {
    mem = heap_caps_malloc(tt_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (mem && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= LATER) break;
    heap_caps_free(mem);
    mem = nullptr;
  }
  tt.init(mem, mem ? tt_bytes : 0);
  {
    char t[24];
    snprintf(t, sizeof t, "TT %u KB", mem ? unsigned(tt_bytes >> 10) : 0);
    boot_note(t);
  }

  board_begin();
  forget_screen();
  bool lcd_ok = true;
  boot_note("display");

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
  ply_eval = static_cast<PlyEval*>(calloc(MAX_GAME, sizeof(PlyEval)));
  // Touch calibration: ours from NVS, else ask (the first start). Retro
  // Backgammon's calibration of the same board is not reused: made in
  // landscape, it was a square out towards the edges here.
  {
    uint16_t cal[8];
    if (prefs.getBytes("cal", cal, sizeof cal) == sizeof cal) lcd.setTouchCalibrate(cal);
    else calibrate();
  }
  use_book = prefs.getBool("book", true);
  human_clock = prefs.getBool("hclock", true);
  use_nnue = nnue_ok && prefs.getBool("nnue", true);
  new_game(::WHITE);
  usb_printf("CST Retro ready, internal free %u KB (largest %u KB)\n",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
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
