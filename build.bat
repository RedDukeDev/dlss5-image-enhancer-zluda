@echo off
REM Configures and builds with MSVC. Qt's MSVC kit is required: see the note in
REM CMakeLists.txt for why MinGW is refused rather than merely discouraged.
setlocal
if "%QT_DIR%"=="" set QT_DIR=C:\Qt\6.11.2\msvc2022_64
set VS=C:\Program Files\Microsoft Visual Studio\18\Community
if not exist "%QT_DIR%\lib\cmake\Qt6" (
    echo Qt for MSVC was not found at "%QT_DIR%".
    echo Install the "MSVC 2022 64-bit" component with the Qt Maintenance Tool,
    echo or set QT_DIR to where it is.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QT_DIR%
if errorlevel 1 exit /b 1
cmake --build build
if errorlevel 1 exit /b 1
"%QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-opengl-sw --no-system-dxc-compiler --no-network --no-svg build\dlss5-image-enhancer.exe
if errorlevel 1 exit /b 1

REM A clean copy of just what running the program needs, separate from
REM build\, which keeps CMake's and Ninja's own bookkeeping (CMakeFiles,
REM build.ninja, the *_autogen staging directories Qt's MOC step uses) so
REM later builds stay incremental. Deleting any of that from build\ itself
REM would work for one clean folder, but the next build would have to start
REM over from nothing -- full reconfigure, every source recompiled, Qt's
REM generated sources rebuilt from scratch -- for every change from then on.
REM
REM /MIR mirrors, so a file windeployqt drops on one run (say, before a flag
REM above was added) does not linger in dist\ after a later run stops
REM producing it.
robocopy build dist /MIR /NFL /NDL /NJH /NJS ^
    /XD CMakeFiles dlss5-image-enhancer_autogen nvngx_autogen .qt ^
    /XF CMakeCache.txt build.ninja cmake_install.cmake *.pdb *.lib *.exp .ninja_log .ninja_deps
REM robocopy's own exit codes are a bitmask where 0-7 all mean success (0 =
REM nothing needed copying); only 8 and above is a real failure.
if errorlevel 8 exit /b 1
exit /b 0
