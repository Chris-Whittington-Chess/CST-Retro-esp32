# Installing CST Retro

CST Retro is built for specific boards: the screen, touch controller and
memory layout are different on each, so every board has its own firmware
image. Pick your board below; if it isn't listed, see
[Other screens](#other-screens).

| Board | Screen | Firmware image | PlatformIO env |
|---|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-7 | 7", 800x480, capacitive touch | `cst-retro-<version>-ws7.bin` | `ws7` |
| M5Stack CoreS3 | 2", 320x240, capacitive touch | `cst-retro-<version>-cores3.bin` | `cores3` |

The images are in [`dist/`](../dist). Each is the whole flash contents
(bootloader, partition table and program, with the opening book and the
NNUE built in), written at address 0. Flashing resets the board's saved
settings (time control, book, evaluation, your clock) to their defaults.

## 1. Connect the board

- **Waveshare 7"**: use the socket marked **USB**, not UART (the UART socket
  can't flash). The first time, if no new serial port appears, unplug, hold
  **BOOT** while plugging back in, release it, flash, then unplug and plug in
  again (without BOOT) to start the game. Once CST Retro is on the board,
  later updates need no buttons.
- **M5Stack CoreS3**: USB-C. If the port doesn't appear or flashing fails,
  hold the reset button for about 3 seconds (the green LED lights) to enter
  download mode.

Windows shows the board as a "USB Serial Device (COMn)"; macOS as
`/dev/cu.usbmodem...`; Linux as `/dev/ttyACM0`.

## 2. Flash

### With esptool (any PC)

Install esptool once (Python 3 needed):

```
pip install esptool
```

then, with your board's image and port:

```
esptool --chip esp32s3 --port COM5 write-flash 0x0 cst-retro-0.1.1-ws7.bin
```

(Older esptool versions spell it `esptool.py ... write_flash`.) It takes
about 10 seconds; the board restarts into the game.

### From the browser

The `dist/manifest-<board>.json` files are for a browser installer (ESP Web
Tools: Chrome or Edge, board on USB, one Install button per board). It needs
the images hosted on a web page - not set up yet.

### From source

With [PlatformIO](https://platformio.org) (VS Code extension or the `pio`
command line), in a clone of this repo:

```
pio run -e ws7 -t upload --upload-port COM5
```

Use `-e cores3` for the CoreS3. The `ws7` env uses the pioarduino platform
(arduino-esp32 3.x), downloaded on the first build. On Windows build `ws7`
from PowerShell or cmd, not Git Bash/MSYS (ESP-IDF's tool installer refuses
to run there).

## 3. Check it

The board starts with a new game, you playing White. Over USB it is also a
UCI engine - see the README for the PC bridge (`tools/uci_bridge.py`). The
serial command `info` reports memory and whether the NNUE loaded.

## Other screens

There is no single image for every display: each board needs its own small
port - display output, touch input and a screen layout. What a port needs:

- **ESP32-S3 with PSRAM** (8 MB is plenty; octal or quad) for the screen
  canvas, and about 200 KB of free internal SRAM for the NNUE (96 KB in one
  block), the hash table and the engine. An ESP32-S3 is required for the
  SIMD NNUE kernels; a plain ESP32 could run the PeSTO evaluation only.
- **8 MB flash or more** (the program, book and net are about 1.4 MB; the
  boards here use 16 MB partitions).
- **Touch**: capacitive is best; the board's squares need to be at least
  about 30 px.

What a port changes: one new `src/<board>/main.cpp` (start from
`src/ws7/main.cpp` for RGB panels such as the other Waveshare
ESP32-S3-Touch-LCD sizes, or `src/cores3/main.cpp` for SPI panels), new
pieces at the board's square size (`tools/make_pieces_burnett.py <px>
<board>`), and a `[env:<board>]` in `platformio.ini`. The engine, book, NNUE
and UCI code are shared and need no changes.

Boards that should port easily: Waveshare ESP32-S3-Touch-LCD-4.3 / -5
(same panel family and GT911 touch as the 7"). An ESP32-P4 board would be
faster still but needs its own SIMD work.
