param([int]$batch = 1)  # -batch 2 for the second pool batch
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
  @('Snowy-0.2', 'snowy_0_2_bin_win64\snowy_0_2_x64.exe', 'uci'))
}
$pool = $pools[$batch]
New-Item -ItemType Directory -Force "$root\match\pool-check" | Out-Null
foreach ($e in $pool) {
  $exe = Join-Path $engines $e[1]
  $dir = Split-Path -Parent $exe
  $log = "$root\match\pool-check\$($e[0]).log"
  & $cute -engine "name=$($e[0])" "cmd=$exe" "dir=$dir" "proto=$($e[2])" `
          -engine name=CSTRetro "cmd=$root\bin\cstretro-see.exe" proto=uci `
          -each tc=5+0.05 -openings "file=$book" format=pgn order=random -games 2 -rounds 2 -repeat `
          -concurrency 4 -pgnout "$root\match\pool-check\$($e[0]).pgn" -recover *> $log
  $score = (Select-String -Path $log -Pattern "^Score of" | Select-Object -Last 1).Line
  $bad = Select-String -Path $log -Pattern "illegal|disconnect|stalls|crash|loses on time|forfeit" |
         ForEach-Object { $_.Line.Trim() } | Select-Object -Unique
  "{0,-18} {1}" -f $e[0], $score
  $bad | ForEach-Object { "      ! $_" }
}
