"""One ready-to-flash image per board, written at address 0, for users who
don't build from source.

Usage (after building the boards' game envs), with PlatformIO's Python:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools/merge_firmware.py 0.1.0 [ws7 cores3]

Writes dist/cst-retro-<version>-<board>.bin (bootloader, partition table,
boot_app0 and the app in one file) and dist/manifest-<board>.json for a
browser installer (ESP Web Tools). Flash with:
    esptool --chip esp32s3 write-flash 0x0 dist/cst-retro-<version>-<board>.bin

ws7 (arduino-esp32 3.x) already builds the merged image (firmware.factory.bin);
cores3 (arduino-esp32 2.0.17) is merged here with esptool.
"""
import json
import shutil
import subprocess
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
    factory = build / "firmware.factory.bin"
    if board == "ws7":
        if not factory.exists():
            sys.exit(f"missing {factory} - build first: pio run -e ws7")
        shutil.copyfile(factory, out)
    else:
        boot_app0 = next(FRAMEWORKS.glob("framework-arduinoespressif32*/tools/partitions/boot_app0.bin"), None)
        parts = [("0x0", build / "bootloader.bin"), ("0x8000", build / "partitions.bin"),
                 ("0xe000", boot_app0), ("0x10000", build / "firmware.bin")]
        for _, f in parts:
            if not f or not f.exists():
                sys.exit(f"missing {f} - build first: pio run -e {board}")
        cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "merge-bin", "-o", str(out),
               "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep"]
        for off, f in parts:
            cmd += [off, str(f)]
        subprocess.run(cmd, check=True)
    manifest = {
        "name": f"CST Retro - {BOARDS[board]}",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": out.name, "offset": 0}]}],
    }
    (DIST / f"manifest-{board}.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"{out}  ({out.stat().st_size:,} bytes)")
