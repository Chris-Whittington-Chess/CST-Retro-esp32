@echo off
rem Build Deepov 0.4.1 (official repo, tag v0.4.1) with clang-cl -> engines\deepov-0.4.1.exe
setlocal
if not defined VCToolsInstallDir (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)
set LLVM=%VCINSTALLDIR%Tools\Llvm\x64\bin
cd /d "%~dp0..\..\engines\deepov-src"
if not exist obj mkdir obj
"%LLVM%\clang-cl.exe" /nologo /std:c++14 /O2 /EHsc /DNDEBUG /D_CRT_SECURE_NO_WARNINGS -mpopcnt -w ^
    src\*.cpp /Foobj\ /Fe..\deepov-0.4.1.exe || exit /b 1
echo built engines\deepov-0.4.1.exe
