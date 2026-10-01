@echo off
rem Chess System Tal Retro - PC build of the engine core (test harness only).
rem   build-host.cmd  -> bin\cstretro.exe   (clang-cl from Visual Studio)
setlocal
if not defined VCToolsInstallDir (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)
set LLVM=%VCINSTALLDIR%Tools\Llvm\x64\bin
cd /d "%~dp0"
if not exist bin mkdir bin
"%LLVM%\clang-cl.exe" /nologo /std:c++17 /O2 /GS- /EHsc /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /W3 ^
    host\main.cpp host\uci.cpp src\engine\board.cpp src\engine\eval.cpp src\engine\tt.cpp src\engine\search.cpp /Fobin\ /Febin\cstretro.exe || exit /b 1
echo built bin\cstretro.exe
