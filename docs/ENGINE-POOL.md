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

## The pool (clean through round-robins 1 and 2)

23 engines, none with a failure in any test. Round-robin 2 (2026-10-02):
the 8 survivors of round-robin 1 + batch 3 + the emulated board, 552 games
at 1+0.6, 46 per engine - all 552 ended by mate or a normal draw (no time
losses, crashes, stalls or illegal moves). "Fitted" = ML Elo over the clean
games of both round-robins, anchored to CCRL (`rate.py`).

| CCRL | Fitted | Engine | Path (engines/) | Proto | Source |
|---|---|---|---|---|---|
| 1516 | 1542 | Pulse 1.6.1 | pulse-1.6.1-cpp/pulse-1.6.1-cpp/pulse-1.6.1-cpp.exe | uci | fluxroot/pulse 1.6.1 |
| 1600 | 1662 | TSCP 1.81 | tscp181/tscp181.exe | xboard | tckerrigan.com/Chess/TSCP |
| 1618 | 1776 | Shallow Blue 2.0.0 | shallowblue_x86-64.exe | uci | GunshipPenguin/shallow-blue v2.0.0 |
| 1983 | 1996 | Goldfish 1.13.0 | Goldfish.v1.13.0.64bit.exe | uci | bsamseth/Goldfish v1.13.0 |
| 2008 | 1946 | Maxwell 3.1-3 | Maxwell.v3.1.Patch.3.-.Windows.64-bit/maxwell-v3.1-3.exe | uci | eboatwright/Maxwell v3.1-3 |
| 2028 | 2092 | Gunborg 1.35 | gunborg1.35_w64m.exe | uci | torgnil/gunborg v1.35 |
| 2040 | 2231 | Trinket 3.0.0 | trinket-v3.0.0.exe | uci | DkeRee/Trinket V3.0.0 |
| 2081 | 2310 | Barbarossa 0.4.0 | Barbarossa-0.4.0-w64.exe | uci | nionita/Barbarossa v0.4.0 |
| 2112 | 2119 | Halogen 3.0 | Halogen3.0_x64.exe | uci | KierenP/Halogen v3.0 |
| 2177 | 2147 | Smallbrain 1.1 | smallbrain_1.1-x64-windows.exe | uci | Disservin/Smallbrain 1.1.0 |
| 2201 | 2154 | KhepriChess 4.0.1 | kheprichess_4.0.1-win.exe | uci | kurt1288/KhepriChess v4.0.1 |
| 2235 | 2338 | Smallbrain 2.0 | smallbrain_2.0-x64-windows.exe | uci | Disservin/Smallbrain 2.0.0 |
| 2235 | 2357 | Barbarossa 0.5.0 | Barbarossa-0.5.0-win10-64.exe | uci | nionita/Barbarossa v0.5.0 |
| 2252 | 2163 | Dumb 1.3 | dumb-windows/dumb-win64.exe | uci | abulmo/Dumb v1.3 |
| 2338 | 2387 | Prophet 4.1 | prophet4_1_windows/prophet4_1_windows/prophet4_1.exe | xboard | jswaff/prophet v4.1 |
| 2340 | 2292 | Tantabus 1.0.2 | tantabus-windows-2019-x86-64-v2.exe | uci | analog-hors/tantabus v1.0.2 |
| 2346 | 2214 | Peacekeeper 1.10 | peacekeeper-110.exe | uci | Sazgr/peacekeeper v1.10 |
| 2363 | 2408 | Barbarossa 0.6.0 | Barbarossa-0.6.0.exe | uci | nionita/Barbarossa 0.6.0 |
| 2372 | 2197 | Altair 1.0.0 | Altair_windows_64.exe | uci | Alex2262/AltairChessEngine v1.0.0 |
| 2374 | 2180 | Clarity 2.0.0 | Clarity_2.0.0_Magic.exe | uci | Vast342/Clarity V2.0.0 |
| 2388 | 2257 | byte-knight 3.0.0 | byte-knight.exe-x86_64-pc-windows-msvc.exe | uci | DeveloperPaul123/byte-knight v3.0.0 |
| 2396 | 2284 | Avalanche 0.2 | Avalanche_x86_64_windows.exe | uci | SnowballSH/Avalanche v0.2 |
| 2440 | 2390 | Halogen 6 | Halogen6-x64-popcnt.exe | uci | KierenP/Halogen v6 |

Pool fit vs CCRL: 112 Elo rms (66 in round-robin 1 alone). At 1+0.6 some
engines play far off their list rating - Barbarossa 0.4.0 +229, Trinket
+191, Shallow Blue +158, Clarity -194, Altair -175 - which mostly cancels
in the anchoring but matters if a gauntlet uses only a few opponents.

Out (round-robin 1 and earlier checks):

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

## The emulated board

`CST-Retro-S3emu` = the PC build with `EmulateNPS=52000` (the CoreS3's
measured NNUE speed, middlegame-weighted: 45-55k middlegame, 68-94k
endgame), `HashKB=32`, NNUE + SEE. It plays on a virtual clock (nodes /
EmulateNPS + 50 ms USB per move) with node limits, so each move searches what
the board would in that time.

Round-robin 1: 2072 CCRL (95% 1979-2130), 66.7% over 60 games.
Round-robin 2: 2197 (95% 2070-2293), 53.3% over 46 games vs the stronger pool.
Both combined (clean games only, 78): **2152 CCRL (95% 2079-2223)**.
