@echo off
rem Builds the development executable, build\pc\letsgo.exe: a console window with the log, runs
rem the game extracted in orig\, test and debug options (README, Setup steps 4 and 5).
rem   build-dev.bat            build (runs the recompiler and configures build\pc the first time)
rem   build-dev.bat --recomp   run the recompiler again first (after recomp\ changes)
setlocal
call "%~dp0tools\build_exe.cmd" letsgo %*
set "RESULT=%ERRORLEVEL%"
rem started from Explorer (cmd /c): keep the window open to show the result
setlocal EnableDelayedExpansion
set "LINE=!CMDCMDLINE!"
if /i not "!LINE:/c=!"=="!LINE!" pause
exit /b %RESULT%
