"""One ready-to-flash image per board, written at address 0, for users who
don't build from source.

Usage (after building the boards' game envs), with PlatformIO's Python:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools/merge_firmware.py 0.1.0 [ws7 cores3]

Writes dist/cst-retro-<version>-<board>.bin (bootloader, partition table,
boot_app0 and the app in one file) and dist/manifest-<board>.json for a
browser installer (ESP Web Tools). Flash with:
    esptool --chip esp32s3 write-flash 0x0 dist/cst-retro-<version>-<board>.bin

Both boards build on arduino-esp32 3.x (pioarduino), which makes the merged
image itself (firmware.factory.bin); this copies it under a release name and
writes the manifest.
"""
import json
import shutil
import sys
from pathlib import Path

BOARDS = {  # env -> what the user sees
    "ws7": "Waveshare ESP32-S3-Touch-LCD-7 (7 inch, 800x480)",
    "cores3": "M5Stack CoreS3 (2 inch, 320x240)",
}

if len(sys.argv) < 2:
    sys.exit(__doc__)
version = sys.argv[1]
boards = sys.argv[2:] or list(BOARDS)

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / "dist"
DIST.mkdir(exist_ok=True)
FRAMEWORKS = Path.home() / ".platformio" / "packages"

for board in boards:
    if board not in BOARDS:
        sys.exit(f"unknown board {board}: one of {', '.join(BOARDS)}")
    build = ROOT / ".pio" / "build" / board
    out = DIST / f"cst-retro-{version}-{board}.bin"
    factory = build / "firmware.factory.bin"  # the merged image the build makes
    if not factory.exists():
        sys.exit(f"missing {factory} - build first: pio run -e {board}")
    shutil.copyfile(factory, out)
    manifest = {
        "name": f"CST Retro - {BOARDS[board]}",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": out.name, "offset": 0}]}],
    }
    (DIST / f"manifest-{board}.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"{out}  ({out.stat().st_size:,} bytes)")
