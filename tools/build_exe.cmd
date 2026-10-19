@echo off
rem Build one of the two executables into build\pc (build-dev.bat and build-release.bat call this):
rem   tools\build_exe.cmd letsgo          (development: build\pc\letsgo.exe)
rem   tools\build_exe.cmd LetsGoPikachu   (release: build\pc\LetsGoPikachu.exe)
rem Add --recomp to run the recompiler again (after recomp\ changes). It also runs when build\gen
rem is missing, and build\pc is configured the first time. Setup steps 1 and 2 of the README
rem (toolchain\ and orig\) must be done first.
setlocal EnableExtensions
set "TARGET=%~1"
if /i not "%TARGET%"=="letsgo" if /i not "%TARGET%"=="LetsGoPikachu" (
    echo usage: tools\build_exe.cmd letsgo^|LetsGoPikachu [--recomp]
    exit /b 2
)
set "RECOMP="
if /i "%~2"=="--recomp" set "RECOMP=1"
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
cd /d "%ROOT%" || exit /b 1

if not exist "toolchain\llvm-mingw\bin\clang.exe" (
    echo The toolchain isn't set up. In Git Bash, run: bash tools/setup_toolchain.sh
    echo ^(README, Setup step 1^)
    exit /b 1
)
if not exist "orig\exefs\main" (
    echo orig\ is missing. In Git Bash, run: bash tools/prepare.sh BASE.nsp UPDATE.nsp prod.keys
    echo ^(README, Setup step 2^)
    exit /b 1
)
set "PATH=%ROOT%\toolchain\llvm-mingw\bin;%PATH%"
where python >nul 2>nul || (
    echo Python isn't on PATH.
    exit /b 1
)

rem CMake and Ninja: from PATH, otherwise the ones Visual Studio or its Build Tools install
set "CMAKE="
set "NINJA="
for /f "delims=" %%P in ('where cmake 2^>nul') do if not defined CMAKE set "CMAKE=%%P"
for /f "delims=" %%P in ('where ninja 2^>nul') do if not defined NINJA set "NINJA=%%P"
if not defined CMAKE call :find_vs_tools
if not defined NINJA call :find_vs_tools
if not defined CMAKE (
    echo CMake wasn't found: install it, or Visual Studio Build Tools with "C++ CMake tools for Windows".
    exit /b 1
)
if not defined NINJA (
    echo Ninja wasn't found: install it, or Visual Studio Build Tools with "C++ CMake tools for Windows".
    exit /b 1
)

set "RECONFIGURE="
if not exist "build\gen\main\table.c" set "RECOMP=1"
if not exist "build\gen\sdk\table.c" set "RECOMP=1"
if defined RECOMP (
    echo Recompiling main and sdk into build\gen. This takes a few minutes.
    python -P recomp\recomp.py main sdk || exit /b 1
    rem the build lists build\gen's files when it's configured
    set "RECONFIGURE=1"
)
if not exist "build\pc\build.ninja" set "RECONFIGURE=1"
if defined RECONFIGURE (
    "%CMAKE%" -S . -B build\pc -G Ninja -DCMAKE_BUILD_TYPE=Release ^
        "-DCMAKE_TOOLCHAIN_FILE=%ROOT:\=/%/cmake/host-x86_64.cmake" "-DCMAKE_MAKE_PROGRAM=%NINJA:\=/%" || exit /b 1
)

echo Building %TARGET%. The first build compiles about 95,000 recompiled functions and takes a while.
"%CMAKE%" --build build\pc --target %TARGET% || exit /b 1
echo.
echo Built %ROOT%\build\pc\%TARGET%.exe
exit /b 0

:find_vs_tools
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 0
set "VSDIR="
for /f "usebackq delims=" %%D in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%D"
if not defined VSDIR exit /b 0
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if not defined CMAKE if exist "%VSCMAKE%\CMake\bin\cmake.exe" set "CMAKE=%VSCMAKE%\CMake\bin\cmake.exe"
if not defined NINJA if exist "%VSCMAKE%\Ninja\ninja.exe" set "NINJA=%VSCMAKE%\Ninja\ninja.exe"
exit /b 0
