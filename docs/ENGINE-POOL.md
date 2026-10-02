# Low-Elo engine pool (CCRL 40/15)

Engines for rating CST Retro: the CoreS3 over USB, or the PC build emulating
the board (`EmulateNPS`, see below). All are open source, from the author's
official release or tag. `engines/` is git-ignored: re-fetch with the sources
below (`gh release download <tag> -R <repo> -p <file>`).

**Rule: one failure and an engine is out** - any crash/disconnect, stall,
illegal move (including "0000") or time loss, in any test.

Use a move-based opening book (`8moves_v3.pgn`): TSCP has no `setboard`, so
FEN/EPD openings make it play from the initial position (illegal-move losses).

Tools (`tools/pool/`): `round_robin.ps1` (pool + "CST-Retro-S3emu"),
`rate.py` (ML Elo anchored to CCRL), `cutechess_check.ps1` (quick checks),
`build-deepov.cmd` / `build-wowl.cmd` (for the record - both engines failed).

## Status after round-robin 1 (480 games, 1+0.6, 60 per engine)

Clean - kept:

| CCRL | Engine | Path (engines/) | Proto | Source |
|---|---|---|---|---|
| 1516 | Pulse 1.6.1 | pulse-1.6.1-cpp/pulse-1.6.1-cpp/pulse-1.6.1-cpp.exe | uci | fluxroot/pulse 1.6.1 |
| 1600 | TSCP 1.81 | tscp181/tscp181.exe | xboard | tckerrigan.com/Chess/TSCP |
| 1618 | Shallow Blue 2.0.0 | shallowblue_x86-64.exe | uci | GunshipPenguin/shallow-blue v2.0.0 |
| 1983 | Goldfish 1.13.0 | Goldfish.v1.13.0.64bit.exe | uci | bsamseth/Goldfish v1.13.0 |
| 2112 | Halogen 3.0 | Halogen3.0_x64.exe | uci | KierenP/Halogen v3.0 |
| 2177 | Smallbrain 1.1 | smallbrain_1.1-x64-windows.exe | uci | Disservin/Smallbrain 1.1.0 |
| 2235 | Smallbrain 2.0 | smallbrain_2.0-x64-windows.exe | uci | Disservin/Smallbrain 2.0.0 |
| 2440 | Halogen 6 | Halogen6-x64-popcnt.exe | uci | KierenP/Halogen v6 |

Out:

| CCRL | Engine | Failures in round-robin 1 |
|---|---|---|
| 1745 | Apollo 1.2.1 (stnevans/Apollo) | time loss, crash, illegal-move draw |
| 1820 | Deepov 0.4.1 (built, RomainGoussault/Deepov) | time loss, 2 stalls |
| 1848 | Wowl 1.3.7 (built, eric-ycw/wowl) | 2x "0000", illegal-move draw |
| 1879 | Wowl 1.3.8 (built, eric-ycw/wowl) | 4x "0000" |
| 1925 | Snowy 0.2 (JasonCreighton/snowy) | 2 crashes |
| 1970 | Cinnamon 2.0 (gekomad/Cinnamon) | illegal move |
| 2052 | Cinnamon 2.2a (gekomad/Cinnamon) | time loss |
| 2336 | Cinnamon 2.5 | 2 crashes in the first 4-game check |
| 1754 | Pigeon 1.5.1 | no UCI output |
| 2085 | Stash 14 | not built: needs MinGW/pthreads |

## Batch 3 (being tested in round-robin 2)

| CCRL | Engine | Path (engines/) | Proto | Source |
|---|---|---|---|---|
| 2008 | Maxwell 3.1-3 | Maxwell.v3.1.Patch.3.-.Windows.64-bit/maxwell-v3.1-3.exe | uci | eboatwright/Maxwell v3.1-3 |
| 2028 | Gunborg 1.35 | gunborg1.35_w64m.exe | uci | torgnil/gunborg v1.35 |
| 2040 | Trinket 3.0.0 | trinket-v3.0.0.exe | uci | DkeRee/Trinket V3.0.0 |
| 2081 | Barbarossa 0.4.0 | Barbarossa-0.4.0-w64.exe | uci | nionita/Barbarossa v0.4.0 |
| 2201 | KhepriChess 4.0.1 | kheprichess_4.0.1-win.exe | uci | kurt1288/KhepriChess v4.0.1 |
| 2235 | Barbarossa 0.5.0 | Barbarossa-0.5.0-win10-64.exe | uci | nionita/Barbarossa v0.5.0 |
| 2252 | Dumb 1.3 | dumb-windows/dumb-win64.exe | uci | abulmo/Dumb v1.3 |
| 2338 | Prophet 4.1 | prophet4_1_windows/prophet4_1_windows/prophet4_1.exe | xboard | jswaff/prophet v4.1 |
| 2340 | Tantabus 1.0.2 | tantabus-windows-2019-x86-64-v2.exe | uci | analog-hors/tantabus v1.0.2 |
| 2346 | Peacekeeper 1.10 | peacekeeper-110.exe | uci | Sazgr/peacekeeper v1.10 |
| 2363 | Barbarossa 0.6.0 | Barbarossa-0.6.0.exe | uci | nionita/Barbarossa 0.6.0 |
| 2372 | Altair 1.0.0 | Altair_windows_64.exe | uci | Alex2262/AltairChessEngine v1.0.0 |
| 2374 | Clarity 2.0.0 | Clarity_2.0.0_Magic.exe | uci | Vast342/Clarity V2.0.0 |
| 2388 | byte-knight 3.0.0 | byte-knight.exe-x86_64-pc-windows-msvc.exe | uci | DeveloperPaul123/byte-knight v3.0.0 |
| 2396 | Avalanche 0.2 | Avalanche_x86_64_windows.exe | uci | SnowballSH/Avalanche v0.2 |

## The emulated board

`CST-Retro-S3emu` = the PC build with `EmulateNPS=52000` (the CoreS3's
measured NNUE speed, middlegame-weighted: 45-55k middlegame, 68-94k
endgame), `HashKB=32`, NNUE + SEE. It plays on a virtual clock (nodes /
EmulateNPS + 50 ms USB per move) with node limits, so each move searches what
the board would in that time.

Round-robin 1: **2072 CCRL (95% 1979-2130)**, 66.7% over 60 games; pool fit
vs CCRL 66 Elo rms.
