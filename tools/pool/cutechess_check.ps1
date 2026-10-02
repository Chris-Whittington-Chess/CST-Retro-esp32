param([int]$batch = 1, [int]$conc = 4, [int]$rounds = 2)  # -batch 2/3: later pool batches
# 4 quick games per pool engine vs the CST Retro PC build under cutechess,
# then a summary of results and terminations (crashes, illegal moves, time).
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$engines = Join-Path $root "engines"
$cute = "C:\Program Files (x86)\Cute Chess\cutechess-cli.exe"
# Move-based openings: TSCP has no setboard, so FEN/EPD books break it.
$book = "C:\Users\chris\source\repos\Chess_Utilities_Various\openings\8moves_v3.pgn"
$pools = @{
 1 = @(
  @('ShallowBlue-2.0.0', 'shallowblue_x86-64.exe', 'uci'),
  @('Deepov-0.4.1', 'deepov-0.4.1.exe', 'uci'),
  @('Cinnamon-2.0', 'cinnamon_win_2.0\cinnamon_2.0_x64-modern.exe', 'uci'),
  @('Goldfish-1.13.0', 'Goldfish.v1.13.0.64bit.exe', 'uci'),
  @('Cinnamon-2.2a', 'cinnamon_win_2.2a\cinnamon_win_2.2a\cinnamon_2.2a_x64-modern.exe', 'uci'),
  @('Halogen-3.0', 'Halogen3.0_x64.exe', 'uci'),
  @('Smallbrain-1.1', 'smallbrain_1.1-x64-windows.exe', 'uci'),
  @('Smallbrain-2.0', 'smallbrain_2.0-x64-windows.exe', 'uci'),
  @('Cinnamon-2.5', 'cinnamon_2.5_x64-INTEL.exe', 'uci'),
  @('Halogen-6', 'Halogen6-x64-popcnt.exe', 'uci'));
 2 = @(
  @('Pulse-1.6.1', 'pulse-1.6.1-cpp\pulse-1.6.1-cpp\pulse-1.6.1-cpp.exe', 'uci'),
  @('TSCP-1.81', 'tscp181\tscp181.exe', 'xboard'),
  @('Apollo-1.2.1', 'apollo-popcnt64.exe', 'uci'),
  @('Pigeon-1.5.1', 'pigeon-1.5.1-windows\pigeon-1.5.1\pigeon-1.5.1.exe', 'uci'),
  @('Wowl-1.3.7', 'wowl-1.3.7.exe', 'uci'),
  @('Wowl-1.3.8', 'wowl-1.3.8.exe', 'uci'),
  @('Snowy-0.2', 'snowy_0_2_bin_win64\snowy_0_2_x64.exe', 'uci'));
 3 = @(
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
  @('Avalanche-0.2', 'Avalanche_x86_64_windows.exe', 'uci'))
}
$pool = $pools[$batch]
New-Item -ItemType Directory -Force "$root\match\pool-check" | Out-Null
foreach ($e in $pool) {
  $exe = Join-Path $engines $e[1]
  $dir = Split-Path -Parent $exe
  $log = "$root\match\pool-check\$($e[0]).log"
  & $cute -engine "name=$($e[0])" "cmd=$exe" "dir=$dir" "proto=$($e[2])" `
          -engine name=CSTRetro "cmd=$root\bin\cstretro-see.exe" proto=uci `
          -each tc=5+0.05 -openings "file=$book" format=pgn order=random -games 2 -rounds $rounds -repeat `
          -concurrency $conc -pgnout "$root\match\pool-check\$($e[0]).pgn" -recover *> $log
  $score = (Select-String -Path $log -Pattern "^Score of" | Select-Object -Last 1).Line
  $bad = Select-String -Path $log -Pattern "illegal|disconnect|stalls|crash|loses on time|forfeit" |
         ForEach-Object { $_.Line.Trim() } | Select-Object -Unique
  "{0,-18} {1}" -f $e[0], $score
  $bad | ForEach-Object { "      ! $_" }
}
