"""UCI smoke test for the low-Elo pool: handshake, options, a short search.

  python tools/pool/check_engines.py
"""
import os
import subprocess
import threading
import time

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engines")
ENGINES = [  # (CCRL 40/15, label, path relative to engines/)
    (1618, "Shallow Blue 2.0.0", "shallowblue_x86-64.exe"),
    (1820, "Deepov 0.4.1", "deepov-0.4.1.exe"),
    (1970, "Cinnamon 2.0", "cinnamon_win_2.0/cinnamon_2.0_x64-modern.exe"),
    (1983, "Goldfish 1.13.0", "Goldfish.v1.13.0.64bit.exe"),
    (2052, "Cinnamon 2.2a", "cinnamon_win_2.2a/cinnamon_win_2.2a/cinnamon_2.2a_x64-modern.exe"),
    (2112, "Halogen 3.0", "Halogen3.0_x64.exe"),
    (2177, "Smallbrain 1.1", "smallbrain_1.1-x64-windows.exe"),
    (2235, "Smallbrain 2.0", "smallbrain_2.0-x64-windows.exe"),
    (2336, "Cinnamon 2.5", "cinnamon_2.5_x64-INTEL.exe"),
    (2440, "Halogen 6", "Halogen6-x64-popcnt.exe"),
]


def run(path, script, timeout=15):
    p = subprocess.Popen([path], cwd=os.path.dirname(path), stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    lines = []

    def reader():
        for line in p.stdout:
            lines.append(line.rstrip())

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    for cmd, wait in script:
        try:
            p.stdin.write(cmd + "\n")
            p.stdin.flush()
        except OSError:
            break
        end = time.time() + wait
        while time.time() < end and not any(l.startswith(("uciok", "readyok", "bestmove"))
                                            and l not in lines[:-1] for l in lines[-1:]):
            time.sleep(0.05)
    t.join(timeout=2)
    try:
        p.wait(timeout=3)
    except subprocess.TimeoutExpired:
        p.kill()
        lines.append("(killed: did not quit)")
    return lines


for elo, label, rel in ENGINES:
    path = os.path.join(HERE, rel)
    if not os.path.exists(path):
        print(f"{elo} {label}: MISSING {rel}")
        continue
    out = run(path, [("uci", 3), ("isready", 3), ("ucinewgame", 0.2), ("isready", 3),
                     ("position startpos moves e2e4", 0.2), ("go movetime 500", 4), ("quit", 1)])
    name = next((l[8:] for l in out if l.startswith("id name")), "?")
    uciok = any(l.startswith("uciok") for l in out)
    ready = any(l.startswith("readyok") for l in out)
    best = next((l for l in reversed(out) if l.startswith("bestmove")), None)
    books = [l for l in out if l.startswith("option") and "book" in l.lower()]
    status = "OK" if uciok and ready and best else "PROBLEM"
    print(f"{elo} {label:20s} {status:8s} id='{name}'  {best or 'no bestmove'}")
    for b in books:
        print(f"      {b}")
    if status != "OK":
        for l in out[-8:]:
            print(f"      | {l}")
