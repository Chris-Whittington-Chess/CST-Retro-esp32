@echo off
rem Chess System Tal Retro - PC build of the engine core (test harness only).
rem   build-host.cmd     -> bin\cstretro.exe     (0x88 board, clang-cl from Visual Studio)
rem   build-host.cmd bb  -> bin\cstretro-bb.exe  (bitboard board, -DBITBOARD)
setlocal
if not defined VCToolsInstallDir (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)
set LLVM=%VCINSTALLDIR%Tools\Llvm\x64\bin
cd /d "%~dp0"
set NAME=cstretro
set DEFS=
if /i "%1"=="bb" (
    set NAME=cstretro-bb
    set DEFS=/DBITBOARD
)
if not exist bin\%NAME% mkdir bin\%NAME%
"%LLVM%\clang-cl.exe" /nologo /std:c++17 /O2 /GS- /EHsc /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /W3 %DEFS% ^
    host\main.cpp host\uci.cpp host\pgn.cpp src\engine\board.cpp src\engine\movegen88.cpp src\engine\movegen_bb.cpp ^
    src\engine\eval.cpp src\engine\tt.cpp src\engine\search.cpp src\engine\book.cpp src\engine\uci_util.cpp src\engine\nnue.cpp ^
    /Fobin\%NAME%\ /Febin\%NAME%.exe || exit /b 1
echo built bin\%NAME%.exe
