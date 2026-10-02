# Low-Elo engine pool (CCRL 40/15)

Engines for rating CST Retro (the CoreS3 over USB, or the PC build with a time
handicap). All are open source, from the author's official release or tag;
each passed a cutechess check (4 games vs the PC build, no crashes, illegal
moves or time losses). `engines/` is git-ignored: re-fetch with the sources
below (`gh release download <tag> -R <repo> -p <file>`), Deepov and Wowl with
`tools/pool/build-deepov.cmd` / `build-wowl.cmd` after cloning the tag.

Use a move-based opening book (`8moves_v3.pgn`): TSCP has no `setboard`, so
FEN/EPD openings make it play from the initial position (illegal-move losses).

| CCRL | Engine | Path (engines/) | Proto | Source | Notes |
|---|---|---|---|---|---|
| 1516 | Pulse 1.6.1 | pulse-1.6.1-cpp/pulse-1.6.1-cpp/pulse-1.6.1-cpp.exe | uci | fluxroot/pulse 1.6.1 | bottom anchor |
| 1600 | TSCP 1.81 | tscp181/tscp181.exe | xboard | tckerrigan.com/Chess/TSCP | PGN openings only |
| 1618 | Shallow Blue 2.0.0 | shallowblue_x86-64.exe | uci | GunshipPenguin/shallow-blue v2.0.0 | |
| 1745 | Apollo 1.2.1 | apollo-popcnt64.exe | uci | stnevans/Apollo 1.2.1 | illegal PV lines (cosmetic) |
| 1820 | Deepov 0.4.1 | deepov-0.4.1.exe | uci | RomainGoussault/Deepov v0.4.1 | built from source |
| 1848 | Wowl 1.3.7 | wowl-1.3.7.exe | uci | eric-ycw/wowl v1.3.7 | built; 1.3.8 fixes a rare illegal castle |
| 1879 | Wowl 1.3.8 | wowl-1.3.8.exe | uci | eric-ycw/wowl v1.3.8 | built from source |
| 1925 | Snowy 0.2 | snowy_0_2_bin_win64/snowy_0_2_x64.exe | uci | JasonCreighton/snowy v0.2 | |
| 1970 | Cinnamon 2.0 | cinnamon_win_2.0/cinnamon_2.0_x64-modern.exe | uci | gekomad/Cinnamon v2.0 | illegal PV lines (cosmetic) |
| 1983 | Goldfish 1.13.0 | Goldfish.v1.13.0.64bit.exe | uci | bsamseth/Goldfish v1.13.0 | |
| 2052 | Cinnamon 2.2a | cinnamon_win_2.2a/cinnamon_win_2.2a/cinnamon_2.2a_x64-modern.exe | uci | gekomad/Cinnamon v2.2a | illegal PV lines (cosmetic) |
| 2112 | Halogen 3.0 | Halogen3.0_x64.exe | uci | KierenP/Halogen v3.0 | |
| 2177 | Smallbrain 1.1 | smallbrain_1.1-x64-windows.exe | uci | Disservin/Smallbrain 1.1.0 | |
| 2235 | Smallbrain 2.0 | smallbrain_2.0-x64-windows.exe | uci | Disservin/Smallbrain 2.0.0 | |
| 2440 | Halogen 6 | Halogen6-x64-popcnt.exe | uci | KierenP/Halogen v6 | |

Rejected: Cinnamon 2.5 (disconnects - 2 crashes in 4 games), Pigeon 1.5.1
(no UCI output at all).
