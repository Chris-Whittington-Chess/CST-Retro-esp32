"""UCI bridge: lets a chess GUI or cutechess use the CoreS3 over USB as an
ordinary UCI engine (they launch engines as programs and talk on stdin/stdout;
this relays that to the board's serial port).

  python tools/uci_bridge.py [--port COM3]      (needs pyserial)
  tools/cstretro-usb.bat                        (same, for GUIs that want a file)

cutechess-cli example:
  -engine name=CSTRetro-S3 cmd=tools\\cstretro-usb.bat proto=uci

Only UCI replies are passed back (the board also prints debug lines). On
"quit" the board leaves UCI mode and returns to touch play from the final
position. The port is opened with DTR/RTS off, which does not reset the board.
"""
import argparse
import queue
import sys
import threading
import time

import serial

UCI_REPLIES = ("id ", "uciok", "readyok", "bestmove", "info", "option ", "copyprotection", "registration")

ap = argparse.ArgumentParser()
ap.add_argument("--port", default="COM3")
ap.add_argument("--log", help="append the whole conversation to this file")
a = ap.parse_args()

s = serial.Serial()
s.port, s.baudrate, s.timeout = a.port, 115200, 0.005  # short: replies pass straight through
s.dtr = s.rts = False
s.open()
s.reset_input_buffer()
log = open(a.log, "a", encoding="utf-8") if a.log else None
lock = threading.Lock()


def note(prefix, text):
    if log:
        with lock:
            log.write(f"{time.time() % 1000:8.3f} {prefix} {text}\n")
            log.flush()


to_board = queue.Queue()  # lines from the GUI


def stdin_reader():
    """Only reads stdin; the serial port is used by the main thread alone
    (sharing one pyserial port between a reading and a writing thread stalled
    on Windows: replies stopped arriving mid-search)."""
    for line in sys.stdin:
        line = line.strip()
        if line:
            to_board.put(line)
    to_board.put("quit")  # GUI went away


threading.Thread(target=stdin_reader, daemon=True).start()
buf = b""
quitting = False
while True:
    try:
        while True:
            line = to_board.get_nowait()
            note(">", line)
            s.write((line + "\n").encode())
            if line == "quit":
                quitting = True
    except queue.Empty:
        pass
    if quitting:
        break
    try:
        chunk = s.read(max(1, s.in_waiting))  # whatever has arrived (5 ms timeout)
    except Exception as e:  # log it and keep going
        note("!", f"read error: {e!r}")
        sys.stderr.write(f"uci_bridge: read error: {e!r}\n")
        time.sleep(0.01)
        continue
    if not chunk:
        continue
    buf += chunk
    while b"\n" in buf:
        raw, buf = buf.split(b"\n", 1)
        line = raw.decode(errors="replace").rstrip("\r")
        note("<", line)
        if line.startswith(UCI_REPLIES):
            sys.stdout.write(line + "\n")
            sys.stdout.flush()
s.flush()
s.close()
