# Gauntlet of the real board (over USB, tools/uci_bridge.py) against engines
# from the clean pool whose fitted ratings match CCRL well (docs/ENGINE-POOL.md),
# one game at a time. Ratings: python tools/pool/rate.py match/<tag>.pgn
# (with the round-robin PGNs for a joint fit).
param(
  [string]$port = "COM5",      # the board: COM5 = 7" Waveshare, COM3 = CoreS3
  [string]$name = "CST-Retro-ws7",
  [string]$tc = "60+0.6",
  [int]$rounds = 20,           # each opponent: 2 * rounds games
  [string]$tag = "board1",
  [string]$only = ""           # one opponent by name (a quick check)
)
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$e = Join-Path $root "engines"
$cute = "C:\Program Files (x86)\Cute Chess\cutechess-cli.exe"
$book = "C:\Users\chris\source\repos\Chess_Utilities_Various\openings\8moves_v3.pgn"
$py = Join-Path (Split-Path -Parent $root) "m5-llm\.venv\Scripts\python.exe"
$opponents = @(  # CCRL 40/15, fitted (round-robins 1+2) within ~90 Elo
  @('Goldfish-1.13.0', 'Goldfish.v1.13.0.64bit.exe', 'uci'),      # 1983 / 1996
  @('Gunborg-1.35', 'gunborg1.35_w64m.exe', 'uci'),                # 2028 / 2092
  @('Halogen-3.0', 'Halogen3.0_x64.exe', 'uci'),                   # 2112 / 2119
  @('Smallbrain-1.1', 'smallbrain_1.1-x64-windows.exe', 'uci'),    # 2177 / 2147
  @('KhepriChess-4.0.1', 'kheprichess_4.0.1-win.exe', 'uci'),      # 2201 / 2154
  @('Dumb-1.3', 'dumb-windows\dumb-win64.exe', 'uci'),             # 2252 / 2163
  @('Prophet-4.1', 'prophet4_1_windows\prophet4_1_windows\prophet4_1.exe', 'xboard'),  # 2338 / 2387
  @('Tantabus-1.0.2', 'tantabus-windows-2019-x86-64-v2.exe', 'uci') # 2340 / 2292
)
if ($only) {
  $keep = @()
  foreach ($p in $opponents) { if ($p[0] -eq $only) { $keep += , $p } }
  $opponents = $keep
}
New-Item -ItemType Directory -Force "$root\match" | Out-Null
$cargs = @('-engine', "name=$name", "cmd=$py",
           "arg=-u", "arg=$root\tools\uci_bridge.py", "arg=--port", "arg=$port",
           "arg=--log", "arg=$root\match\$tag-bridge.log",
           'proto=uci', 'option.Eval=NNUE', 'option.OwnBook=false')
foreach ($p in $opponents) {
  $exe = Join-Path $e $p[1]
  $cargs += @('-engine', "name=$($p[0])", "cmd=$exe", "dir=$(Split-Path -Parent $exe)", "proto=$($p[2])")
}
# Adjudication by cutechess from both engines' reported scores (the board
# never resigns or claims itself): resign at 10 pawns for 4 moves, both
# agreeing; draw from move 40 at |score| <= 0.10 for 8 moves.
$cargs += @('-resign', 'movecount=4', 'score=1000', 'twosided=true',
            '-draw', 'movenumber=40', 'movecount=8', 'score=10')
$cargs += @('-each', "tc=$tc",
           '-tournament', 'gauntlet', '-games', '2', '-rounds', "$rounds", '-repeat',
           '-openings', "file=$book", 'format=pgn', 'order=random',
           '-concurrency', '1', '-recover',
           '-pgnout', "$root\match\$tag.pgn")
& $cute @cargs *> "$root\match\$tag.log"
