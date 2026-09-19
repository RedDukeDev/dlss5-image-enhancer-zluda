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
REM Qt6::Sql is linked for one reason -- merging the per-translation module
REM caches in core\precompile.cpp -- and that reason is SQLite. Left alone,
REM windeployqt also ships the Oracle, ODBC, PostgreSQL, Firebird and Mimer
REM drivers, none of which this program can reach: about 600 KB of database
REM clients in a folder for enhancing images.
REM
REM The directory is cleared first because windeployqt only ever adds: a plugin
REM it deployed before an exclusion was added stays where it is, and the mirror
REM below would then carry it into dist faithfully. Deleting it here is what
REM makes the exclusion mean anything on a tree that has already been built.
if exist "build\sqldrivers" rmdir /s /q "build\sqldrivers"
"%QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-opengl-sw --no-system-dxc-compiler --no-network --no-svg --exclude-plugins qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql build\dlss5-image-enhancer.exe
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
    /XD CMakeFiles dlss5-image-enhancer_autogen nvngx_autogen .qt zluda ^
    /XF CMakeCache.txt build.ninja cmake_install.cmake *.pdb *.lib *.exp .ninja_log .ninja_deps ^
        nvngx.dll
REM robocopy's own exit codes are a bitmask where 0-7 all mean success (0 =
REM nothing needed copying); only 8 and above is a real failure.
if errorlevel 8 exit /b 1

REM Our NGX runtime lives under zluda\, and is excluded from the mirror above so
REM that it is not also left loose in dist\. Both modes use it: on AMD it drives
REM the snippet's CUDA interface on the stand-in driver, on NVIDIA its D3D12 one
REM (the direct route the working NVIDIA mods use; the driver's own NGX core is
REM only reached with DLSS_NGX_CORE=driver). A second nvngx.dll beside the
REM executable would only invite the wrong one to be picked up.
if not exist "dist\zluda" mkdir "dist\zluda"
copy /Y "build\nvngx.dll" "dist\zluda\nvngx.dll" >nul
if errorlevel 1 exit /b 1
if exist "dist\nvngx.dll" del /q "dist\nvngx.dll"
exit /b 0
