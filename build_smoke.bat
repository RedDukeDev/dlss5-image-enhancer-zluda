@echo off
REM Builds the processor smoke test into ..\bin. No Qt: this exercises the DLSS
REM side on its own, which is the half that can actually fail.
setlocal
set VS=C:\Program Files\Microsoft Visual Studio\18\Community
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
set ROOT=%~dp0
set OUT=%ROOT%..\bin
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /O2 /W3 /D_CRT_SECURE_NO_WARNINGS ^
   /I "%ROOT%dlss_layer" ^
   "%ROOT%tests\processor_smoke.cpp" "%ROOT%core\image_processor.cpp" ^
   "%ROOT%dlss_layer\dlss_cuda.cpp" "%ROOT%dlss_layer\frame_blit.cpp" ^
   /Fo:"%OUT%\\" /Fe:"%OUT%\processor_smoke.exe"
if errorlevel 1 exit /b 1
del "%OUT%\*.obj" >nul 2>&1
echo Built %OUT%\processor_smoke.exe
exit /b 0
