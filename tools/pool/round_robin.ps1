# Round-robin of the low-Elo pool plus "CST-Retro-S3emu": the PC build
# emulating the CoreS3 (EmulateNPS = measured board NNUE speed, 32 KB TT,
# NNUE + SEE). Stability (crashes, illegal moves, time losses) and calibration
# against CCRL; ratings via tools/pool/rate.py.
param(
  [string]$tc = "60+0.6",
  [int]$rounds = 2,          # each pair plays 2 * rounds games
  [int]$conc = 12,
  [int]$nps = 52000,
  [string]$tag = "rr1",
  [string]$set = "rr1"       # engine set: rr1 (first pool) or rr2 (survivors + batch 3)
)
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$e = Join-Path $root "engines"
$cute = "C:\Program Files (x86)\Cute Chess\cutechess-cli.exe"
$book = "C:\Users\chris\source\repos\Chess_Utilities_Various\openings\8moves_v3.pgn"
$net = Join-Path $root "data\net.bin"
$survivors = @(  # clean through rr1 (one-failure rule)
  @('Pulse-1.6.1', 'pulse-1.6.1-cpp\pulse-1.6.1-cpp\pulse-1.6.1-cpp.exe', 'uci'),
  @('TSCP-1.81', 'tscp181\tscp181.exe', 'xboard'),
  @('ShallowBlue-2.0.0', 'shallowblue_x86-64.exe', 'uci'),
  @('Goldfish-1.13.0', 'Goldfish.v1.13.0.64bit.exe', 'uci'),
  @('Halogen-3.0', 'Halogen3.0_x64.exe', 'uci'),
  @('Smallbrain-1.1', 'smallbrain_1.1-x64-windows.exe', 'uci'),
  @('Smallbrain-2.0', 'smallbrain_2.0-x64-windows.exe', 'uci'),
  @('Halogen-6', 'Halogen6-x64-popcnt.exe', 'uci')
)
$batch3 = @(
  @('Maxwell-3.1-3', 'Maxwell.v3.1.Patch.3.-.Windows.64-bit\maxwell-v3.1-3.exe', 'uci'),
  @('Gunborg-1.35', 'gunborg1.35_w64m.exe', 'uci'),
  @('Trinket-3.0.0', 'trinket-v3.0.0.exe', 'uci'),
  @('Barbarossa-0.4.0', 'Barbarossa-0.4.0-w64.exe', 'uci'),
  @('KhepriChess-4.0.1', 'kheprichess_4.0.1-win.exe', 'uci'),
  @('Barbarossa-0.5.0', 'Barbarossa-0.5.0-win10-64.exe', 'uci'),
  @('Dumb-1.3', 'dumb-windows\dumb-win64.exe', 'uci'),
  @('Prophet-4.1', 'prophet4_1_windows\prophet4_1_windows\prophet4_1.exe', 'xboard'),
  @('Tantabus-1.0.2', 'tantabus-windows-2019-x86-64-v2.exe', 'uci'),
  @('Peacekeeper-1.10', 'peacekeeper-110.exe', 'uci'),
  @('Barbarossa-0.6.0', 'Barbarossa-0.6.0.exe', 'uci'),
  @('Altair-1.0.0', 'Altair_windows_64.exe', 'uci'),
  @('Clarity-2.0.0', 'Clarity_2.0.0_Magic.exe', 'uci'),
  @('byte-knight-3.0.0', 'byte-knight.exe-x86_64-pc-windows-msvc.exe', 'uci'),
  @('Avalanche-0.2', 'Avalanche_x86_64_windows.exe', 'uci')
)
$rr1 = @(
  @('Pulse-1.6.1', 'pulse-1.6.1-cpp\pulse-1.6.1-cpp\pulse-1.6.1-cpp.exe', 'uci'),
  @('TSCP-1.81', 'tscp181\tscp181.exe', 'xboard'),
  @('ShallowBlue-2.0.0', 'shallowblue_x86-64.exe', 'uci'),
  @('Apollo-1.2.1', 'apollo-popcnt64.exe', 'uci'),
  @('Deepov-0.4.1', 'deepov-0.4.1.exe', 'uci'),
  @('Wowl-1.3.7', 'wowl-1.3.7.exe', 'uci'),
  @('Wowl-1.3.8', 'wowl-1.3.8.exe', 'uci'),
  @('Snowy-0.2', 'snowy_0_2_bin_win64\snowy_0_2_x64.exe', 'uci'),
  @('Cinnamon-2.0', 'cinnamon_win_2.0\cinnamon_2.0_x64-modern.exe', 'uci'),
  @('Goldfish-1.13.0', 'Goldfish.v1.13.0.64bit.exe', 'uci'),
  @('Cinnamon-2.2a', 'cinnamon_win_2.2a\cinnamon_win_2.2a\cinnamon_2.2a_x64-modern.exe', 'uci'),
  @('Halogen-3.0', 'Halogen3.0_x64.exe', 'uci'),
  @('Smallbrain-1.1', 'smallbrain_1.1-x64-windows.exe', 'uci'),
  @('Smallbrain-2.0', 'smallbrain_2.0-x64-windows.exe', 'uci'),
  @('Halogen-6', 'Halogen6-x64-popcnt.exe', 'uci')
)
$pool = if ($set -eq "rr2") { $survivors + $batch3 } else { $rr1 }
$cargs = @()
foreach ($p in $pool) {
  $exe = Join-Path $e $p[1]
  $cargs += @('-engine', "name=$($p[0])", "cmd=$exe", "dir=$(Split-Path -Parent $exe)", "proto=$($p[2])")
}
# copy so a rebuild of bin\cstretro.exe can't change the engine mid-tournament
Copy-Item -Force "$root\bin\cstretro.exe" "$root\bin\cstretro-emu.exe"
$cargs += @('-engine', 'name=CST-Retro-S3emu', "cmd=$root\bin\cstretro-emu.exe", 'proto=uci',
           "option.EvalFile=$net", 'option.Eval=NNUE', 'option.HashKB=32', "option.EmulateNPS=$nps")
$cargs += @('-each', "tc=$tc", 'option.Hash=16',
           '-tournament', 'round-robin', '-games', '2', '-rounds', "$rounds", '-repeat',
           '-openings', "file=$book", 'format=pgn', 'order=random',
           '-concurrency', "$conc", '-recover',
           '-pgnout', "$root\match\$tag.pgn")
New-Item -ItemType Directory -Force "$root\match" | Out-Null
& $cute @cargs *> "$root\match\$tag.log"
