# Chess System Tal Retro (ESP32)

A self-contained play-against-the-engine chess computer on ESP32 display boards:
touch board, clocks / time controls, take back, levels, opening book. It also
speaks UCI over USB, so a PC can set up positions or run engine matches.

The engine borrows ideas from CStal-5 (Chess System Tal 5) but is a fresh,
small-machine design: 32-bit CPU, ~300 KB internal SRAM, PSRAM for the big stuff.

## Design

| Part | Choice | Why |
|---|---|---|
| Board | 0x88 mailbox + piece lists (king first) | 32-bit Xtensa: 64-bit bitboards cost 2x, magic tables (~800 KB) don't fit SRAM |
| Attacks | 0x88 delta tables; legality only tested for king moves, ep, evasions, pin candidates | perft 247 -> 418 knps on the CoreS3 |
| Moves | `u32` from/to/promo/flags, pseudo-legal + make/check/unmake | simple, small |
| Make | make/unmake with a game-history stack | the history is the take-back list and the repetition list |
| Keys | standard Polyglot Zobrist | opening book |
| Eval | PeSTO tapered PST, kept incrementally in make/unmake | 442 -> 50 cycles/node |
| Search | ID + aspiration, PVS, qsearch, TT, null move, RFP, LMR, check extension, killers/history | move lists on a shared static stack |
| Hash | 128 KB TT in internal SRAM | a PSRAM TT costs ~10x per probe (search 75 vs 54 knps) |
| Book | Jeroen.bin (Jeroen Noomen), 668 KB in flash | weighted random choice |

## Targets

- `cores3` - the game on the M5Stack CoreS3 (ESP32-S3, 240 MHz, 8 MB PSRAM, 320x240 touch), COM3.
- `cores3-bench` / `cores3-prof` - engine benchmarks (perft, search bench, cycle counters).
- later: a 4" board (ESP32-S3 or P4).

## Build

PC test harness (engine core only):

    build-host.cmd
    bin\cstretro.exe bench | keytest | sbench [depth] [hash_kb]
    bin\cstretro.exe perft data\perft.epd [max_nodes]
    bin\cstretro.exe divide "<fen>" <depth>
    bin\cstretro.exe book data\Jeroen.bin [uci moves...]
    bin\cstretro.exe            (UCI engine, for PC test matches)

Device:

    ..\m5-llm\.venv\Scripts\pio.exe run -e cores3 -t upload

## USB: UCI and debug commands

The CoreS3 reads one command per line on its USB serial port (COM3, 115200).

- **Set up a position for touch play:** send `position fen <fen>` (or
  `position startpos moves ...`). The board shows it and you play the side to
  move, with the clock and level from the menu.
- **UCI engine:** after `uci` the board is a UCI engine (the screen follows the
  game, touch moves are off). Supported: `isready`, `ucinewgame`, `position`,
  `go` (wtime/btime/winc/binc/movestogo/movetime/depth/nodes/infinite), `stop`,
  `setoption name OwnBook value true|false` (default false), `quit` (back to
  touch play from the final position; so does the menu's "Leave UCI").
- **GUIs / cutechess** start engines as programs, so use the bridge:
  `tools\cstretro-usb.bat` (runs `tools/uci_bridge.py`; `--port COM3`,
  `--log file` to record the conversation with timestamps). Example:

      cutechess-cli -engine name=CSTRetro-S3 cmd=tools\cstretro-usb.bat proto=uci ^
                    -engine name=other cmd=other.exe proto=uci -each tc=20+0.2 ...

- **Debug:** `d` frame dump (`tools/grab.py out.png`), `t X Y` tap, `n` new game,
  `a` autoplay, `b` perft bench, `s` search bench, `touchlog` (every touch with
  its square).

USB notes: output does not use `Serial.print` - the arduino-esp32 2.0.17 HWCDC
driver could hold a reply until the PC sent something (a `bestmove` deadlock);
`src/cores3/usb_out.cpp` writes the hardware FIFO from its own task. The
receive buffer is 16 KB (long `position ... moves` lines overflowed 256 bytes).

## Measurements

Stage 1 - movegen/perft (bench = 6 standard positions, 16,046,250 nodes):

| Where | Bench | Notes |
|---|---|---|
| PC (Ryzen 9 8945HS, clang-cl /O2) | 36-39 Mnps | perft.epd, marcel, ferdy suites: 0 failures; Polyglot keys 9/9 |
| CoreS3 (240 MHz, -O2) | 418 knps | perft.epd (174 positions, 705 checks <= 1M nodes): 0 failures |

Stage 2 - search bench (12 positions, depth 8, node counts identical on PC and device):

| Where | Speed | Notes |
|---|---|---|
| CoreS3, TT 128 KB internal | 80 knps (real games ~85-110 knps) | ~3000 cycles/node: gen 770, order 550, make 690, check 550, eval 50, TT 53 |
| CoreS3, TT 4 MB PSRAM | 54 knps | TT probe ~520 cycles/node |

Device notes:
- Hot code in IRAM (IRAM_ATTR) made no difference: the loop already lives in
  the instruction cache.
- The Arduino loop task has an 8 KB stack: move lists live on a shared static
  stack; the engine runs in its own task (32 KB stack, ~9 KB used).
- Time management: no new iteration after 60% of the move's target (stopping
  at the target itself overran it 2-3x and drained the clock); USB adds
  ~50 ms per move, 100 ms is reserved.

## Credits

- PeSTO evaluation tables: Ronald Friederich (Rofchade), via chessprogramming.org.
- Opening book `data/Jeroen.bin`: Jeroen Noomen (Rebel), freely distributed.
- Piece images rendered from DejaVu Sans (Bitstream Vera licence).
- Polyglot Zobrist keys: the standard Polyglot book format.
