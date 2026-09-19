@echo off
REM Builds the processor smoke test into ..\bin. This exercises the DLSS side on
REM its own, which is the half that can actually fail.
REM
REM It used to need no Qt at all, and the widgets half still does not appear
REM here. Qt's SQLite comes in through core\precompile.cpp, which merges the
REM per-translation cache databases it now writes. Giving this build a
REM different, merge-free precompile was the alternative and is a worse one: two
REM builds of the same source behaving differently is how a test comes to pass
REM while the program it stands for does not. Running the result needs
REM %QT_DIR%\bin on PATH, which the line below arranges.
setlocal
set VS=C:\Program Files\Microsoft Visual Studio\18\Community
if "%QT_DIR%"=="" set QT_DIR=C:\Qt\6.11.2\msvc2022_64
if not exist "%QT_DIR%\include\QtSql" (
    echo Qt for MSVC was not found at "%QT_DIR%".
    echo Set QT_DIR to where it is, as build.bat does.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
set ROOT=%~dp0
set OUT=%ROOT%..\bin
if not exist "%OUT%" mkdir "%OUT%"
REM /permissive- is not a preference: Qt 6.11 refuses to compile without it, by
REM static assertion. CMake sets it for the program itself through Qt's own
REM target, which is why the same sources build there; this line is that same
REM requirement spelled out for a build that does not go through CMake.
cl /nologo /std:c++17 /EHsc /O2 /W3 /permissive- /Zc:__cplusplus /D_CRT_SECURE_NO_WARNINGS ^
   /I "%ROOT%dlss_layer" /I "%QT_DIR%\include" ^
   "%ROOT%tests\processor_smoke.cpp" "%ROOT%core\image_processor.cpp" ^
   "%ROOT%core\precompile.cpp" ^
   "%ROOT%dlss_layer\dlss_cuda.cpp" "%ROOT%dlss_layer\frame_blit.cpp" ^
   wintrust.lib ^
   /Fo:"%OUT%\\" /Fe:"%OUT%\processor_smoke.exe" ^
   /link /LIBPATH:"%QT_DIR%\lib" Qt6Core.lib Qt6Sql.lib
if errorlevel 1 exit /b 1
del "%OUT%\*.obj" >nul 2>&1
echo Built %OUT%\processor_smoke.exe
exit /b 0
