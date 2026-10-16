@echo off
rem Import an ELF from orig\elf into the Ghidra project orig\ghidra\letsgo and run
rem auto-analysis. main is seeded with the function database first.
rem   tools\ghidra\import.cmd main         (image base 0x7100000000)
rem   tools\ghidra\import.cmd sdk          (image base 0x7180000000, reference only)
setlocal
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "MOD=%~1"
if "%MOD%"=="" set "MOD=main"
set "BASE=7100000000"
if /i "%MOD%"=="sdk" set "BASE=7180000000"
if /i "%MOD%"=="subsdk0" set "BASE=7190000000"
if /i "%MOD%"=="subsdk1" set "BASE=71a0000000"
if not defined GHIDRA_HEADLESS_MAXMEM set "GHIDRA_HEADLESS_MAXMEM=6G"
if not exist "%ROOT%\orig\ghidra" mkdir "%ROOT%\orig\ghidra"
set "PRE="
if /i "%MOD%"=="main" set PRE=-preScript LetsGoApplyFunctions.java "%ROOT%\data\main_functions.csv"
call "%ROOT%\toolchain\ghidra\support\analyzeHeadless.bat" "%ROOT%\orig\ghidra" letsgo ^
  -import "%ROOT%\orig\elf\%MOD%.elf" -loader ElfLoader -loader-imagebase %BASE% ^
  -scriptPath "%ROOT%\tools\ghidra" %PRE% ^
  -log "%ROOT%\orig\ghidra\analyze_%MOD%.log" -scriptlog "%ROOT%\orig\ghidra\script_%MOD%.log"
