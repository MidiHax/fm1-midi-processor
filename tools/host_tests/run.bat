@echo off
rem Host (PC) tests of firmware logic that needs no hardware.
rem clock_test.c: clock lockout, metronome timing, BPM (src/app/clock.c).
rem monitor\monitor_test.c: MIDI Monitor search.
rem Needs Visual Studio Build Tools (C++). Builds in %TEMP%\fm1_host_tests.
rem clock.c is copied next to the stub midiproc.h, so the stub is used
rem instead of the real header (which needs the SDK).
setlocal
set "OUT=%TEMP%\fm1_host_tests"
set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio C++ build tools not found & exit /b 1)
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
copy /y "%~dp0midiproc.h" "%OUT%\" >nul
copy /y "%~dp0clock_test.c" "%OUT%\" >nul
copy /y "%~dp0..\..\src\app\clock.c" "%OUT%\" >nul
pushd "%OUT%"
cl /nologo /W3 /O2 /std:c11 clock_test.c clock.c /Fe:clock_test.exe > build.log 2>&1
if errorlevel 1 (type build.log & popd & exit /b 1)
popd
"%OUT%\clock_test.exe" || set FAILED=1

rem monitor_test.c: MIDI Monitor search and the rule builder
rem (monitor.c, monitor_search.c, rule_builder.c).
set "MON=%OUT%\monitor"
if not exist "%MON%" mkdir "%MON%"
copy /y "%~dp0monitor\midiproc.h" "%MON%\" >nul
copy /y "%~dp0monitor\monitor_test.c" "%MON%\" >nul
copy /y "%~dp0..\..\src\app\monitor.c" "%MON%\" >nul
copy /y "%~dp0..\..\src\app\monitor_search.c" "%MON%\" >nul
copy /y "%~dp0..\..\src\app\rule_builder.c" "%MON%\" >nul
pushd "%MON%"
cl /nologo /W3 /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS monitor_test.c monitor.c monitor_search.c rule_builder.c /Fe:monitor_test.exe > build.log 2>&1
if errorlevel 1 (type build.log & popd & exit /b 1)
popd
"%MON%\monitor_test.exe" || set FAILED=1

rem codec\: the web editor's patch codec (html\patch-codec.js) on its own, then
rem its test vectors through the real patch.c, remap.c and triggers.c.
set "COD=%OUT%\codec"
if not exist "%COD%" mkdir "%COD%"
node "%~dp0codec\codec_test.js" "%COD%\vectors.txt" || set FAILED=1
pushd "%COD%"
cl /nologo /W3 /O2 /std:c11 /D_CRT_SECURE_NO_WARNINGS /FI "%~dp0codec\msvc_compat.h" /I "%~dp0..\..\src\app" /I "%~dp0..\..\src\sdk\include" "%~dp0codec\firmware_check.c" "%~dp0..\..\src\app\patch.c" "%~dp0..\..\src\app\remap.c" "%~dp0..\..\src\app\triggers.c" /Fe:firmware_check.exe > build.log 2>&1
if errorlevel 1 (type build.log & popd & exit /b 1)
popd
"%COD%\firmware_check.exe" "%COD%\vectors.txt" || set FAILED=1
if defined FAILED exit /b 1
