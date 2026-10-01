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
| PC (Ryzen 9 8945HS, clang-cl /O2) | 22 Mnps | perft.epd, marcel, ferdy suites: 0 failures; Polyglot keys 9/9 |
| CoreS3 | - | to measure |

## Plan

1. Board + movegen + perft (done on PC).
2. PeSTO eval, alpha-beta/PVS, quiescence, iterative deepening, time control - on PC first.
3. On device: nodes/s, TT in PSRAM vs internal SRAM.
4. Game UI: touch board, clocks, take back, levels, new game.
