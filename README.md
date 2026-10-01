# Chess System Tal Retro (ESP32)

A self-contained play-against-the-engine chess computer on ESP32 display boards:
touch board, clocks / time controls, take back, levels. No UCI.

The engine borrows ideas from CStal-5 (Chess System Tal 5) but is a fresh,
small-machine design: 32-bit CPU, ~300 KB internal SRAM, PSRAM for the big stuff.

## Design

| Part | Choice | Why |
|---|---|---|
| Board | 0x88 mailbox + piece lists (king first) | 32-bit Xtensa: 64-bit bitboards cost 2x, magic tables (~800 KB) don't fit SRAM |
| Moves | `u32` from/to/promo/flags, pseudo-legal + make/check/unmake | simple, small |
| Make | make/unmake with a game-history stack | the history is the take-back list and the repetition list |
| Keys | standard Polyglot Zobrist | opening books later |
| Eval (next) | PeSTO tapered PST (~3 KB) | |
| Hash (next) | TT in PSRAM, size to be measured | |

## Targets

- `cores3` - M5Stack CoreS3 (ESP32-S3, 240 MHz, 8 MB PSRAM, 320x240 touch), COM3.
- later: ESP32-P4 board.

## Build

PC test harness (engine core only):

    build-host.cmd
    bin\cstretro.exe bench
    bin\cstretro.exe keytest
    bin\cstretro.exe perft data\perft.epd [max_nodes]
    bin\cstretro.exe divide "<fen>" <depth>

Device:

    ..\m5-llm\.venv\Scripts\pio.exe run -e cores3 -t upload

Serial (115200): `b` bench, `p` perft.epd suite (checks <= 1M nodes), `m` memory.

## Measurements

Stage 1 - movegen/perft (bench = 6 standard positions, 16,046,250 nodes):

| Where | Bench | Notes |
|---|---|---|
| PC (Ryzen 9 8945HS, clang-cl /O2) | 22-30 Mnps | perft.epd, marcel, ferdy suites: 0 failures; Polyglot keys 9/9 |
| CoreS3 (240 MHz, -O2) | 247 knps (65 s) | perft.epd (174 positions, 705 checks <= 1M nodes): 0 failures, 206 knps |

Device notes:
- Hot code in IRAM (IRAM_ATTR) made no difference (64.99 s both ways): the
  loop already lives in the instruction cache. Not used.
- ~970 cycles per perft node; most of it is the full `attacked()` legality
  test after every move. Cheaper legality (only test king moves, en passant,
  checks and pinned pieces) is the obvious speed-up if movegen ever matters.
- The Arduino loop task has an 8 KB stack: move lists must not live on it
  (1 KB per ply overflowed at depth 6). Perft, and later the search, use a
  shared static move stack.

## Plan

1. Board + movegen + perft (done on PC).
2. PeSTO eval, alpha-beta/PVS, quiescence, iterative deepening, time control - on PC first.
3. On device: nodes/s, TT in PSRAM vs internal SRAM.
4. Game UI: touch board, clocks, take back, levels, new game.
