// Chess System Tal Retro - PGN replay for NNUE training data (PC only).
//   cstretro pgnfens <out.txt> <every> <skip_plies> <file.pgn>...
// Replays every game (SAN, with [FEN] start positions), and writes every
// <every>-th position after the first <skip_plies> plies whose side to move
// is not in check, as "<fen>;<result>" with result = game result from the
// side to move's point of view (1, 0.5, 0), keeping only quiet positions
// (PeSTO quiescence == static eval). Games without a result are
// skipped. Positions are not deduplicated here (tools/nnue/label.py does).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "../src/engine/board.h"
#include "../src/engine/eval.h"
#include "../src/engine/search.h"
#include "../src/engine/tt.h"
#include "host.h"

static const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// The legal move matching a SAN token (Nf3, exd5, e8=Q, O-O, Rad1, ...); 0 if none.
static Move parse_san(Board& b, const char* tok) {
  char s[16];
  int n = 0;
  for (const char* p = tok; *p && n < 15; p++)
    if (!strchr("+#!?", *p)) s[n++] = *p;
  s[n] = 0;
  Move legal[MAX_MOVES];
  int nl = b.gen_legal(legal);
  if (!strcmp(s, "O-O") || !strcmp(s, "0-0") || !strcmp(s, "O-O-O") || !strcmp(s, "0-0-0")) {
    bool king_side = n == 3;
    for (int i = 0; i < nl; i++)
      if ((legal[i] & MF_CASTLE) && ((move_to(legal[i]) > move_from(legal[i])) == king_side))
        return legal[i];
    return 0;
  }
  int promo = 0;
  char* eq = strchr(s, '=');
  if (eq) {
    promo = int(strchr(" NBRQ", toupper(eq[1])) ? strchr(" NBRQ", toupper(eq[1])) - " NBRQ" : 0);
    *eq = 0;
    n = int(strlen(s));
  } else if (n >= 3 && strchr("NBRQ", s[n - 1]) && islower(s[0])) {  // e8Q
    promo = int(strchr(" NBRQ", s[n - 1]) - " NBRQ");
    s[--n] = 0;
  }
  if (n < 2) return 0;
  int to_f = s[n - 2] - 'a', to_r = s[n - 1] - '1';
  if (to_f < 0 || to_f > 7 || to_r < 0 || to_r > 7) return 0;
  int to = to_r * 16 + to_f;
  int type = PAWN, i0 = 0;
  if (strchr("KQRBN", s[0])) {
    type = int(strchr("PNBRQK", s[0]) - "PNBRQK");
    i0 = 1;
  }
  int dis_f = -1, dis_r = -1;  // disambiguation
  for (int i = i0; i < n - 2; i++) {
    if (s[i] >= 'a' && s[i] <= 'h') dis_f = s[i] - 'a';
    else if (s[i] >= '1' && s[i] <= '8') dis_r = s[i] - '1';
  }
  for (int i = 0; i < nl; i++) {
    Move m = legal[i];
    int from = move_from(m);
    if (move_to(m) != to || piece_type(b.sq[from]) != type || move_promo(m) != promo) continue;
    if (dis_f >= 0 && sq_file(from) != dis_f) continue;
    if (dis_r >= 0 && sq_rank(from) != dis_r) continue;
    return m;
  }
  return 0;
}

static Board pb;
static TT pgn_tt;  // quiescence() probes a TT

int pgn_fens(int argc, char** argv) {
  if (argc < 6) {
    printf("usage: cstretro pgnfens <out.txt> <every> <skip_plies> <file.pgn>...\n");
    return 1;
  }
  FILE* out = fopen(argv[2], "w");
  if (!out) { printf("cannot write %s\n", argv[2]); return 1; }
  int every = atoi(argv[3]) > 0 ? atoi(argv[3]) : 1, skip = atoi(argv[4]);
  pgn_tt.init(malloc(1 << 20), 1 << 20);
  search_init(&pgn_tt);
  long games = 0, bad_games = 0, positions = 0, counter = 0;
  for (int fi = 5; fi < argc; fi++) {
    FILE* f = fopen(argv[fi], "r");
    if (!f) { printf("cannot open %s\n", argv[fi]); continue; }
    std::string fen = START_FEN, result, movetext;
    bool in_moves = false;
    char line[4096];
    auto finish_game = [&]() {
      if (movetext.empty()) return;
      double white = result == "1-0" ? 1.0 : result == "0-1" ? 0.0 : result == "1/2-1/2" ? 0.5 : -1;
      if (white < 0 || !pb.set_fen(fen.c_str())) {
        bad_games++;
      } else {
        games++;
        // tokens, skipping comments {...}, variations (...), NAGs, move numbers
        std::vector<std::string> toks;
        int brace = 0, paren = 0;
        std::string cur;
        for (char c : movetext + " ") {
          if (c == '{') { brace++; continue; }
          if (c == '}') { brace--; continue; }
          if (brace) continue;
          if (c == '(') { paren++; continue; }
          if (c == ')') { paren--; continue; }
          if (paren) continue;
          if (isspace((unsigned char)c)) {
            if (!cur.empty()) toks.push_back(cur);
            cur.clear();
          } else {
            cur += c;
          }
        }
        int ply = 0;
        for (std::string t : toks) {
          if (t[0] == '$' || t == "1-0" || t == "0-1" || t == "1/2-1/2" || t == "*") continue;
          size_t dot = t.find_last_of('.');
          if (dot != std::string::npos) t = t.substr(dot + 1);
          if (t.empty()) continue;
          Move m = parse_san(pb, t.c_str());
          if (!m) { bad_games++; break; }
          pb.make(m);
          ply++;
          if (pb.hply >= MAX_GAME - 2) break;
          if (ply < skip || pb.in_check() || (++counter % every)) continue;
          // quiet: no capture sequence changes PeSTO's view of the position
          if (quiescence(pb) != evaluate(pb)) continue;
          char buf[100];
          pb.to_fen(buf);
          double stm = pb.stm == WHITE ? white : 1.0 - white;
          fprintf(out, "%s;%g\n", buf, stm);
          positions++;
        }
      }
      fen = START_FEN;
      result.clear();
      movetext.clear();
    };
    while (fgets(line, sizeof line, f)) {
      if (line[0] == '[') {
        if (in_moves) { finish_game(); in_moves = false; }
        char val[256];
        if (sscanf(line, "[FEN \"%255[^\"]\"]", val) == 1) fen = val;
        else if (sscanf(line, "[Result \"%255[^\"]\"]", val) == 1) result = val;
        continue;
      }
      if (line[0] == '\n' || line[0] == '\r') continue;
      in_moves = true;
      movetext += line;
      movetext += ' ';
    }
    finish_game();
    fclose(f);
    fprintf(stderr, "%s: %ld games, %ld positions so far\n", argv[fi], games, positions);
  }
  fclose(out);
  printf("pgnfens: %ld games (%ld skipped or broken), %ld positions\n", games, bad_games, positions);
  return 0;
}
