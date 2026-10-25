@echo off
rem Build one of the two executables into build\pc (build-dev.bat calls this):
rem   tools\build_exe.cmd letsgo          (development: build\pc\letsgo.exe)
rem   tools\build_exe.cmd LetsGoPikachu   (release: build\pc\LetsGoPikachu.exe)
rem Add --recomp to run the recompiler again (after recomp\ changes). The first time it also does
rem the README's Setup steps 1 and 2: it sets up toolchain\, asks for the game's files and extracts
rem them into orig\, then runs the recompiler and configures build\pc. It needs Git for Windows
rem and Python 3.11 or newer; CMake and Ninja are found or downloaded.
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

python -c "import sys; sys.exit(sys.version_info < (3, 11))" >nul 2>nul || (
    echo Python 3.11 or newer isn't on PATH. Install it from python.org with "Add python.exe to PATH" ticked.
    exit /b 1
)
rem Git Bash runs the setup scripts: Git for Windows' bin\bash.exe (not Windows' own bash.exe, which is WSL)
set "BASH="
for /f "delims=" %%G in ('where git 2^>nul') do (
    if not defined BASH if exist "%%~dpG..\bin\bash.exe" set "BASH=%%~dpG..\bin\bash.exe"
    if not defined BASH if exist "%%~dpG..\..\bin\bash.exe" set "BASH=%%~dpG..\..\bin\bash.exe"
)
if not defined BASH if exist "%ProgramFiles%\Git\bin\bash.exe" set "BASH=%ProgramFiles%\Git\bin\bash.exe"
if not defined BASH if exist "%LocalAppData%\Programs\Git\bin\bash.exe" set "BASH=%LocalAppData%\Programs\Git\bin\bash.exe"
if not defined BASH (
    echo Git for Windows wasn't found. Install it from git-scm.com: its Git Bash runs the setup scripts.
    exit /b 1
)

rem Setup step 1: toolchain\, when anything the build uses is missing
call :find_tools
set "SETUP="
for %%F in (llvm-mingw\bin\clang.exe guest-sysroot nnheaders src\Vulkan-Headers-vulkan-sdk-1.4.363.0) do if not exist "toolchain\%%F" set "SETUP=1"
for %%F in (SDL3-static\lib\libSDL3.a glslang\lib\libglslang.a codecs\lib\libopus.a codecs\lib\libvpx.a codecs\lib\libvorbis.a codecs\lib\libogg.a) do if not exist "toolchain\%%F" set "SETUP=1"
python -c "import cryptography, lz4, capstone" >nul 2>nul || set "SETUP=1"
if defined SETUP (
    echo Setting up toolchain\: the compilers and libraries are downloaded and built. This takes a while.
    "%BASH%" tools/setup_toolchain.sh || exit /b 1
    call :find_tools
)
if not defined CMAKE (
    echo CMake wasn't found. Run: "%BASH%" tools/setup_toolchain.sh
    exit /b 1
)
if not defined NINJA (
    echo Ninja wasn't found. Run: "%BASH%" tools/setup_toolchain.sh
    exit /b 1
)

rem Setup step 2: the game's files, picked once and extracted into orig\
set "PREPARE="
for %%F in (exefs\main exefs\sdk elf\main.elf elf\sdk.elf romfs\bin) do if not exist "orig\%%F" set "PREPARE=1"
if defined PREPARE (
    powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%ROOT%\tools\prepare_dump.ps1" -Bash "%BASH%" || exit /b 1
)
set "PATH=%ROOT%\toolchain\llvm-mingw\bin;%PATH%"

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

rem CMake and Ninja: from PATH, else the ones Visual Studio or its Build Tools install, else
rem toolchain\'s (downloaded by tools/setup_toolchain.sh when there are none)
:find_tools
for /f "delims=" %%P in ('where cmake 2^>nul') do if not defined CMAKE set "CMAKE=%%P"
for /f "delims=" %%P in ('where ninja 2^>nul') do if not defined NINJA set "NINJA=%%P"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" for /f "usebackq delims=" %%D in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%D"
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if defined VSDIR if not defined CMAKE if exist "%VSCMAKE%\CMake\bin\cmake.exe" set "CMAKE=%VSCMAKE%\CMake\bin\cmake.exe"
if defined VSDIR if not defined NINJA if exist "%VSCMAKE%\Ninja\ninja.exe" set "NINJA=%VSCMAKE%\Ninja\ninja.exe"
if not defined CMAKE if exist "%ROOT%\toolchain\cmake\bin\cmake.exe" set "CMAKE=%ROOT%\toolchain\cmake\bin\cmake.exe"
if not defined NINJA if exist "%ROOT%\toolchain\ninja\ninja.exe" set "NINJA=%ROOT%\toolchain\ninja\ninja.exe"
exit /b 0
