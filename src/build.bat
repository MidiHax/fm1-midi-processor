@echo off
rem SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
rem Build the FM-1 MIDI processor firmware and its local updater page.
rem Requires FM1_PI32_TOOLCHAIN (see README.md).
setlocal
if /i "%~1"=="--help" goto help
if not "%~1"=="" goto invalid
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sdk\Build.ps1" -FirmwareOnly
if errorlevel 1 exit /b %errorlevel%
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Make-Updater.ps1"
exit /b %errorlevel%
:invalid
echo Usage: build.bat
exit /b 2
:help
echo build.bat   Build firmware and updater:
echo             build\dx7\generated\FM1_OFFICIAL_OPEN.fwsc
echo             build\updater\FM1-MidiProc-Updater.html
echo Toolchain setup: README.md
exit /b 0
