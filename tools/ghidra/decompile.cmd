@echo off
rem Write Ghidra's decompiled C for functions in the analyzed main.elf.
rem   tools\ghidra\decompile.cmd <outdir> <address> [<address> ...]
rem Output is derived from the game's code: keep <outdir> under orig\.
setlocal
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
if not defined GHIDRA_HEADLESS_MAXMEM set "GHIDRA_HEADLESS_MAXMEM=6G"
call "%ROOT%\toolchain\ghidra\support\analyzeHeadless.bat" "%ROOT%\orig\ghidra" letsgo ^
  -process main.elf -noanalysis -readOnly -scriptPath "%ROOT%\tools\ghidra" ^
  -postScript LetsGoExportDecomp.java %*
