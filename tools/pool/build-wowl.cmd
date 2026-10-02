@echo off
rem Build Wowl 1.3.7 and 1.3.8 (official repo eric-ycw/wowl, tags v1.3.7 / v1.3.8)
rem with clang-cl -> engines\wowl-1.3.7.exe, engines\wowl-1.3.8.exe
setlocal
if not defined VCToolsInstallDir (
    call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)
set LLVM=%VCINSTALLDIR%Tools\Llvm\x64\bin
cd /d "%~dp0..\..\engines"
for %%v in (1.3.7 1.3.8) do (
    if not exist wowl-v%%v-src\obj mkdir wowl-v%%v-src\obj
    "%LLVM%\clang-cl.exe" /nologo /std:c++17 /O2 /EHsc /DNDEBUG /D_CRT_SECURE_NO_WARNINGS -mpopcnt -w ^
        wowl-v%%v-src\src\*.cpp /Fowowl-v%%v-src\obj\ /Fewowl-%%v.exe || exit /b 1
    echo built engines\wowl-%%v.exe
)
