@echo off
rem Builds the release executable for players, build\pc\LetsGoPikachu.exe: one file, no console,
rem runs from the player's NSPs and keys (README, Getting started).
rem   build-release.bat            build (runs the recompiler and configures build\pc the first time)
rem   build-release.bat --recomp   run the recompiler again first (after recomp\ changes)
setlocal
call "%~dp0tools\build_exe.cmd" LetsGoPikachu %*
set "RESULT=%ERRORLEVEL%"
rem started from Explorer (cmd /c): keep the window open to show the result
setlocal EnableDelayedExpansion
set "LINE=!CMDCMDLINE!"
if /i not "!LINE:/c=!"=="!LINE!" pause
exit /b %RESULT%
