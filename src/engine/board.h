// Chess System Tal Retro - board, make/unmake, move generation.
//
// Small-machine design for the ESP32 (32-bit, little SRAM): a 0x88 mailbox
// with piece lists instead of 64-bit bitboards and magic tables.
//   square  = rank * 16 + file, A1 = 0, H8 = 0x77; (sq & 0x88) != 0 = off board
//   piece   = color << 3 | type, WHITE = 0, PAWN..KING = 0..5, EMPTY = 7
//   list[c] = squares of color c's pieces, king always at index 0
// The game history doubles as the search stack, so take back = unmake.
#pragma once
#include <stdint.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

enum { WHITE, BLACK };
enum { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, EMPTY = 7 };
enum { NO_SQ = 0x80 };
enum { WK_CASTLE = 1, WQ_CASTLE = 2, BK_CASTLE = 4, BQ_CASTLE = 8 };

inline int piece_type(int p) { return p & 7; }
inline int piece_color(int p) { return p >> 3; }
inline int make_piece(int c, int t) { return c << 3 | t; }
inline bool on_board(int sq) { return !(sq & 0x88); }
inline int sq_file(int sq) { return sq & 7; }
inline int sq_rank(int sq) { return sq >> 4; }
inline int sq64(int sq) { return (sq + (sq & 7)) >> 1; }  // 0x88 -> 0..63
inline int sq88(int s) { return s + (s & ~7); }           // 0..63 -> 0x88

// Move: from | to << 8 | promotion type << 16 | flags. Promotion 0 = none
// (KNIGHT..QUEEN are 1..4, a pawn never promotes to PAWN).
typedef u32 Move;
enum : u32 {
  MF_CAPTURE = 1u << 20,
  MF_EP = 1u << 21,
  MF_CASTLE = 1u << 22,
  MF_DOUBLE = 1u << 23,
};
inline int move_from(Move m) { return m & 0x7F; }
inline int move_to(Move m) { return (m >> 8) & 0x7F; }
inline int move_promo(Move m) { return (m >> 16) & 7; }
inline Move make_move(int from, int to, u32 flags = 0, int promo = 0) {
  return u32(from) | u32(to) << 8 | u32(promo) << 16 | flags;
}

enum { MAX_MOVES = 256, MAX_GAME = 1024 };

struct Undo {
  u64 key;
  Move move;
  u8 captured;  // piece taken (EMPTY if none)
  u8 castle, ep, rule50;
};

struct Board {
  u8 sq[128];      // piece per square
  u8 idx[128];     // index of that piece in list[color]
  u8 list[2][16];  // piece squares, king first
  u8 count[2];
  u8 stm, castle, ep;  // ep: target square, NO_SQ unless a capture is possible
  u8 rule50;
  u64 key;  // Polyglot Zobrist key
  int hply;
  Undo hist[MAX_GAME];

  bool set_fen(const char* fen);
  void to_fen(char* out) const;  // >= 92 bytes
  void make(Move m);
  void unmake();
  bool attacked(int s, int by) const;
  bool in_check() const { return attacked(list[stm][0], stm ^ 1); }
  // Pseudo-legal moves (may leave the king in check); returns the count.
  int gen(Move* out) const;
  // Legal moves only (make + check + unmake each).
  int gen_legal(Move* out);
  // The side that just moved left its king attacked.
  bool illegal() const { return attacked(list[stm ^ 1][0], stm); }
  u64 compute_key() const;

 private:
  void put(int s, int p);
  void remove(int s);
  void shift(int from, int to);
};

// Long algebraic (e2e4, e7e8q). out >= 6 bytes.
void move_to_uci(Move m, char* out);
// Find the legal move matching a long algebraic string; 0 if none.
Move parse_uci(Board& b, const char* s);

// Depth <= MAX_PERFT_DEPTH (returns 0 beyond it); move lists live on a shared
// static stack with MAX_MOVES room per ply (16 KB).
enum { MAX_PERFT_DEPTH = 16, MOVE_STACK_SIZE = MAX_PERFT_DEPTH * MAX_MOVES };
u64 perft(Board& b, int depth);
