// Chess System Tal Retro on the M5Stack CoreS3 (320x240 touch): play the
// engine with clocks, take back, levels.
//
// Screen: the board (240x240, 30 px squares) on the left, a panel on the
// right with the two clocks, status and the BACK / MENU buttons. Everything is
// drawn into one full-screen canvas in PSRAM and pushed, so nothing flickers.
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
//   Debug: "d" dump frame ("FRAME\n" + 320*240 LE RGB565, tools/grab.py),
//   "t X Y" simulated tap, "n" new game as White, "a" autoplay (the engine
//   also plays your side, moves printed), "b" perft bench, "s" search bench,
//   "touchlog" toggles printing every touch.
#include <M5Unified.h>
#include <Preferences.h>
#include "usb_out.h"
#include "../engine/bench.h"
#include "../engine/board.h"
#include "../engine/book.h"
#include "../engine/eval.h"
#include "../engine/search.h"
#include "../engine/tt.h"
#include "../engine/uci_util.h"
#include "pieces.h"

extern const uint8_t book_start[] asm("_binary_data_Jeroen_bin_start");
extern const uint8_t book_end[] asm("_binary_data_Jeroen_bin_end");

// ---------------------------------------------------------------- layout

enum { SQ = 30, PANEL_X = 240, PANEL_W = 80, SCREEN_W = 320, SCREEN_H = 240 };
enum { CLOCK_H = 34, TOP_CLOCK_Y = 2, BOTTOM_CLOCK_Y = 156, BUTTON_Y = 196, BUTTON_H = 42 };
enum { TT_BYTES = 128 * 1024 };

struct RGB {
  uint8_t r, g, b;
};
static const RGB LIGHT = {240, 217, 181}, DARK = {181, 136, 99};
static const RGB LAST_TINT = {214, 200, 80}, SELECT_TINT = {100, 170, 90};

static M5Canvas frame(&M5.Display);

static uint16_t c565(RGB c) { return M5.Display.color565(c.r, c.g, c.b); }
static RGB mix(RGB a, RGB b, int t) {  // t/255 of b over a
  return {uint8_t((a.r * (255 - t) + b.r * t) / 255), uint8_t((a.g * (255 - t) + b.g * t) / 255),
          uint8_t((a.b * (255 - t) + b.b * t) / 255)};
}

static const uint16_t PANEL_BG = 0x2124, TEXT = 0xFFFF, DIM = 0x9CD3, BUTTON = 0x4A69,
                      ACTIVE = 0xFEA0, CLOCK_BG = 0x18C3;

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
static int32_t clock_hist[MAX_GAME][2];  // clocks at the start of each ply
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
static bool uci_own_book;  // UCI option OwnBook (off: GUIs/matches bring their own)
static bool touch_log;

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

// UCI info lines from the engine task, printed by loop() so that all serial
// output comes from one task (single producer / single consumer ring).
enum { INFO_SLOTS = 8, INFO_LEN = 400 };
static char info_ring[INFO_SLOTS][INFO_LEN];
static volatile u32 info_head, info_tail;

u32 engine_now_ms() { return millis(); }

static void on_report(const SearchReport& r) {
  info_depth = r.depth;
  info_score = r.score;
  if (eng_uci && info_head - info_tail < INFO_SLOTS) {
    uci_info(r, info_ring[info_head % INFO_SLOTS], INFO_LEN);
    info_head = info_head + 1;
  }
}

static void flush_info() {
  while (info_tail != info_head) {
    usb_println(info_ring[info_tail % INFO_SLOTS]);
    info_tail = info_tail + 1;
  }
}

static void engine_loop(void*) {
  search_init(&tt);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int c = eng_command;
    if (c == 'g') {
      SearchResult r = search(eng_board, eng_limits, on_report);
      eng_move = r.best;
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
  memcpy(&eng_board, &game, sizeof game);
  const TimeControl& tc = TCS[tc_game];
  Limits lim;
  if (tc.move_ms) lim.soft_ms = lim.hard_ms = tc.move_ms;
  else lim = clock_limits(clock_ms[game.stm], int32_t(tc.inc_ms), 0, 50);  // same as UCI
  eng_limits = lim;
  eng_uci = false;
  engine_command('g');
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

static void beep() { M5.Speaker.tone(1800, 25); }

static void play(Move m, bool engine = false) {
  u32 now = millis();
  if (!engine) last_from_book = false;
  int us = game.stm;
  clock_hist[game.hply][0] = clock_ms[0];
  clock_hist[game.hply][1] = clock_ms[1];
  if (timed() && game.hply) clock_ms[us] += int32_t(TCS[tc_game].inc_ms) - int32_t(now - turn_start);
  if (autoplay) {
    char mv[6];
    move_to_uci(m, mv);
    if (game.stm == ::WHITE) usb_printf("%d. ", game.hply / 2 + 1);
    usb_printf("%s ", mv);
  }
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
    if (p == EMPTY) frame.fillCircle(x + SQ / 2, y + SQ / 2, 5, dot);
    else
      for (int k = 0; k < 2; k++) frame.drawCircle(x + SQ / 2, y + SQ / 2, SQ / 2 - 1 - k, dot);
  }
  if (p != EMPTY && piece_type(p) == KING && piece_color(p) == game.stm && game.in_check())
    for (int k = 0; k < 2; k++) frame.drawRect(x + k, y + k, SQ - 2 * k, SQ - 2 * k, TFT_RED);
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

static int32_t clock_now(int side) {
  int32_t ms = clock_ms[side];
  if ((phase == HUMAN || phase == ENGINE) && game.stm == side && game.hply)
    ms -= int32_t(millis() - turn_start);
  return ms;
}

static void draw_clock(int side, int y) {
  bool active = phase != OVER && game.stm == side;
  frame.fillRoundRect(PANEL_X + 2, y, PANEL_W - 4, CLOCK_H, 4, active ? ACTIVE : CLOCK_BG);
  // which side this clock belongs to
  frame.fillCircle(PANEL_X + 11, y + CLOCK_H / 2, 4, side == ::WHITE ? TFT_WHITE : TFT_BLACK);
  frame.drawCircle(PANEL_X + 11, y + CLOCK_H / 2, 4, DIM);
  char buf[12];
  if (phase == UCI) snprintf(buf, sizeof buf, "%s", side == ::WHITE ? "White" : "Black");
  else if (timed()) fmt_clock(clock_now(side), buf);
  else snprintf(buf, sizeof buf, "%s", side == human ? "You" : "CST");
  frame.setFont(&fonts::FreeSansBold9pt7b);
  frame.setTextColor(active ? TFT_BLACK : TEXT);
  frame.setTextDatum(middle_center);
  frame.drawString(buf, PANEL_X + PANEL_W / 2 + 6, y + CLOCK_H / 2 + 1);
}

static void draw_button(int x, int y, int w, int h, const char* label) {
  frame.fillRoundRect(x, y, w, h, 5, BUTTON);
  frame.setFont(&fonts::Font2);
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

enum { STATUS_Y = TOP_CLOCK_Y + CLOCK_H + 6, PROMO_Y = STATUS_Y + 16 };

static void draw_status() {
  frame.setFont(&fonts::Font2);
  frame.setTextDatum(top_left);
  int x = PANEL_X + 4, y = STATUS_Y;
  auto line = [&](uint16_t col, const char* s) {
    frame.setTextColor(col);
    frame.drawString(s, x, y);
    y += 16;
  };
  char buf[24];
  if (promo_from >= 0) {
    line(TEXT, "Promote to");
    static const int types[4] = {QUEEN, ROOK, BISHOP, KNIGHT};
    for (int i = 0; i < 4; i++) {
      int bx = PANEL_X + 6 + (i % 2) * 36, by = PROMO_Y + (i / 2) * 36;
      RGB bg = (i == 0 || i == 3) ? LIGHT : DARK;
      frame.fillRect(bx, by, SQ, SQ, c565(bg));
      draw_piece(bx, by, make_piece(human, types[i]), bg);
    }
    return;
  }
  if (phase == UCI) {
    line(ACTIVE, "UCI via USB");
    line(TEXT, eng_busy ? "Thinking..." : "Waiting");
    if (eng_busy && info_depth) {
      snprintf(buf, sizeof buf, "depth %d", info_depth);
      line(DIM, buf);
      fmt_score(game.stm == ::WHITE ? info_score : -info_score, buf);
      line(DIM, buf);
    }
  } else if (phase == OVER) {
    line(ACTIVE, result[0]);
    line(ACTIVE, result[1]);
  } else if (phase == ENGINE) {
    line(TEXT, "Thinking...");
    if (info_depth) {
      snprintf(buf, sizeof buf, "depth %d", info_depth);
      line(DIM, buf);
      fmt_score(game.stm == ::WHITE ? info_score : -info_score, buf);
      line(DIM, buf);
    }
  } else {
    line(TEXT, game.in_check() ? "Check!" : "Your move");
  }
  y = BOTTOM_CLOCK_Y - 36;
  if (last_move) {
    char mv[6];
    move_to_uci(last_move, mv);
    snprintf(buf, sizeof buf, "%s %s", last_from_book ? "book" : "last", mv);
    line(DIM, buf);
  }
  if (phase != UCI) line(DIM, TCS[tc_game].name);
}

static void draw_panel() {
  frame.fillRect(PANEL_X, 0, PANEL_W, SCREEN_H, PANEL_BG);
  int top = flipped ? ::WHITE : ::BLACK;
  draw_clock(top, TOP_CLOCK_Y);
  draw_clock(top ^ 1, BOTTOM_CLOCK_Y);
  draw_status();
  draw_button(PANEL_X + 2, BUTTON_Y, 37, BUTTON_H, "Back");
  draw_button(PANEL_X + 41, BUTTON_Y, 37, BUTTON_H, "Menu");
}

// Menu: a column of full-width buttons.
enum { MENU_X = 20, MENU_W = 280, MENU_Y0 = 30, MENU_H = 30, MENU_GAP = 5, MENU_ITEMS = 6 };

static void menu_label(int i, char* out) {
  switch (i) {
    case 0:
      snprintf(out, 48, phase == UCI ? "Leave UCI - play from here" : "New game - play White");
      break;
    case 1: snprintf(out, 48, "New game - play Black"); break;
    case 2:
      snprintf(out, 48, "Time: %s%s", TCS[tc_index].name, tc_index != tc_game ? " (next game)" : "");
      break;
    case 3: snprintf(out, 48, "Opening book: %s", use_book ? "on" : "off"); break;
    case 4: snprintf(out, 48, "Flip board"); break;
    default: snprintf(out, 48, "Close"); break;
  }
}

static void draw_menu() {
  frame.fillScreen(PANEL_BG);
  frame.setFont(&fonts::FreeSansBold9pt7b);
  frame.setTextColor(ACTIVE);
  frame.setTextDatum(top_center);
  frame.drawString("Chess System Tal Retro", SCREEN_W / 2, 8);
  for (int i = 0; i < MENU_ITEMS; i++) {
    char label[48];
    menu_label(i, label);
    draw_button(MENU_X, MENU_Y0 + i * (MENU_H + MENU_GAP), MENU_W, MENU_H, label);
  }
}

static void redraw() {
  if (menu_open) {
    draw_menu();
  } else {
    draw_board();
    draw_panel();
  }
  frame.pushSprite(0, 0);
  dirty = false;
}

// Clocks and status only (cheap, a few times a second).
static void refresh_panel() {
  if (menu_open) return;
  draw_panel();
  M5.Display.setClipRect(PANEL_X, 0, PANEL_W, SCREEN_H);
  frame.pushSprite(0, 0);
  M5.Display.clearClipRect();
}

// ---------------------------------------------------------------- touch

static void tap_menu(int x, int y) {
  if (x < MENU_X || x >= MENU_X + MENU_W || y < MENU_Y0) return;
  int i = (y - MENU_Y0) / (MENU_H + MENU_GAP);
  if (i >= MENU_ITEMS || (y - MENU_Y0) % (MENU_H + MENU_GAP) >= MENU_H) return;
  switch (i) {
    case 0:
      menu_open = false;
      if (phase == UCI) play_from_here();
      else new_game(::WHITE);
      break;
    case 1: menu_open = false; new_game(::BLACK); break;
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
      use_book = !use_book;
      prefs.putBool("book", use_book);
      break;
    case 4: flipped = !flipped; menu_open = false; break;
    default: menu_open = false; break;
  }
  dirty = true;
}

// Forgiving touch: taps a little outside the intended square (the touch
// panel is least accurate at the edges, and square borders are only 30 px
// apart) snap to the nearest square that makes sense: a legal target of the
// selected piece, or one of your pieces that can move.
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
  int col = (x - PANEL_X - 6) / 36, row = (y - PROMO_Y) / 36;
  if (x >= PANEL_X + 6 && y >= PROMO_Y && col < 2 && row < 2) {
    int t = types[row * 2 + col];
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
  if (y >= BUTTON_Y && x < PANEL_X + 40) return take_back();
  if (y >= BUTTON_Y) {
    menu_open = true;
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
  menu_open = false;
  selected = -1;
  nsel = 0;
  promo_from = -1;
  phase = UCI;
  dirty = true;
}

static void uci_go_cmd(const char* line) {
  if (phase != UCI) enter_uci();
  stop_engine();
  if (uci_own_book) {
    Move m = book_pick(game, book_start, size_t(book_end - book_start), esp_random());
    if (m) {
      char mv[6];
      move_to_uci(m, mv);
      uci_send("bestmove %s", mv);
      return;
    }
  }
  memcpy(&eng_board, &game, sizeof game);
  eng_limits = uci_go(line, game.stm, 100);  // USB + bridge + screen ~50 ms per move
  eng_uci = true;
  info_depth = 0;
  engine_command('g');
  dirty = true;
}

// The engine answered a UCI "go": report it and show the move on the board
// (the GUI's next "position" replaces the board anyway).
static void uci_done() {
  flush_info();
  Move m = eng_move;
  char mv[6] = "0000";
  if (m) move_to_uci(m, mv);
  uci_send("bestmove %s", mv);
  eng_uci = false;
  if (m && game.hply < MAX_GAME - MAX_PLY - 2) {
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
    usb_println("id name CST Retro (ESP32)");
    usb_println("id author Chris Whittington");
    usb_println("option name OwnBook type check default false");
    uci_send("uciok");
  } else if (!strcmp(line, "isready")) {
    uci_send("readyok");
  } else if (!strcmp(line, "ucinewgame")) {
    stop_engine();
    search_new_game();
  } else if (!strncmp(line, "setoption name OwnBook value ", 29)) {
    uci_own_book = !strcmp(line + 29, "true");
  } else if (!strncmp(line, "position ", 9)) {
    uci_position_cmd(line + 9);
  } else if (!strncmp(line, "go", 2) && (line[2] == 0 || line[2] == ' ')) {
    uci_go_cmd(line);
  } else if (!strcmp(line, "stop")) {
    if (eng_uci) search_stop = true;  // the search ends and answers bestmove
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
  } else if (!strcmp(line, "touchlog")) {
    touch_log = !touch_log;
    usb_printf("touch log %s\n", touch_log ? "on" : "off");
  }
}

static void serial_command() {
  static char buf[8192];  // "position startpos moves ..." can be long
  static int n = 0;
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (n < int(sizeof buf) - 1) buf[n++] = char(c);
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
  auto cfg = M5.config();
  M5.begin(cfg);
  // The default 256-byte receive buffer overflowed on long UCI "position ...
  // moves" lines arriving while the screen redraws (bytes lost, newline too).
  Serial.setRxBufferSize(16384);
  Serial.begin(115200);  // input only; output goes through usb_out
  usb_out_begin();
  M5.Speaker.setVolume(80);
  frame.setPsram(true);
  frame.setColorDepth(16);
  frame.createSprite(SCREEN_W, SCREEN_H);

  void* mem = heap_caps_malloc(TT_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  tt.init(mem, TT_BYTES);
  disableCore0WDT();  // the engine keeps core 0 busy while it thinks
  xTaskCreatePinnedToCore(engine_loop, "engine", 32 * 1024, nullptr, 1, &eng_task, 0);

  prefs.begin("cstretro", false);
  tc_index = prefs.getInt("tc", DEFAULT_TC);
  if (tc_index < 0 || tc_index >= NUM_TCS) tc_index = DEFAULT_TC;
  use_book = prefs.getBool("book", true);
  new_game(::WHITE);
  usb_printf("CST Retro ready, internal free %u KB\n",
                unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

// Touch diagnostics on serial: every press and release with its raw
// position and square, plus the slowest loop pass since the last print.
static u32 loop_max_ms;

void loop() {
  u32 loop_t0 = millis();
  M5.update();
  serial_command();

  auto t = M5.Touch.getDetail();
  if (touch_log && (t.wasPressed() || t.wasReleased())) {
    int s = square_at(t.x, t.y);
    char name[3] = "--";
    if (s >= 0) { name[0] = char('a' + sq_file(s)); name[1] = char('1' + sq_rank(s)); }
    usb_printf("touch %s x=%d y=%d sq=%s (slowest loop %u ms)\n",
                  t.wasPressed() ? "down" : "up  ", t.x, t.y, name, unsigned(loop_max_ms));
    loop_max_ms = 0;
  }
  if (t.wasPressed()) tap(t.x, t.y);
  if (t.wasReleased()) release_board(t.x, t.y);

  flush_info();
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
    set_result(game.stm == ::WHITE ? "White flagged" : "Black flagged",
               game.stm == ::WHITE ? "Black wins" : "White wins");
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
