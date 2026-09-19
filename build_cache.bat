@echo off
REM Builds the ComputeCache that ships under dist\zluda, so a user does not wait
REM for the network to be translated on their first run. See the header of
REM tools\prepare_cache.py for what it does; this only finds Python and passes
REM the arguments through.
REM
REM Usage: build_cache.bat <nvngx_dlssnr.dll> [more options]
REM
REM Needs ZLUDA's nvcuda.dll in dist\zluda (put there by build.bat plus a ZLUDA
REM build) and an AMD GPU with the HIP SDK installed. Run it again after every
REM ZLUDA rebuild: the cache key includes ZLUDA's own version, so entries made
REM by an older build are never found by a newer one.
REM
REM It translates several modules at once, deciding how many from the memory
REM actually free at each moment rather than from a number fixed at the start.
REM Pass --jobs 1 to go back to one at a time.
setlocal
cd /d %~dp0

if "%~1"=="" (
    echo usage: build_cache.bat ^<nvngx_dlssnr.dll^> [options]
    echo.
    echo   --driver PATH    ZLUDA's nvcuda.dll ^(default: dist\zluda\nvcuda.dll^)
    echo   --out DIR        where to write zluda2.db ^(default: dist\zluda\ComputeCache^)
    echo   --targets LIST   comma-separated GPU targets, overriding the defaults
    echo   --jobs N         translations at once ^(0 = decide from the machine^)
    echo   --keep           keep the scratch directory under build\cache
    exit /b 2
)

where python >nul 2>&1
if errorlevel 1 (
    echo Python was not found in PATH. Install it, or run tools\prepare_cache.py
    echo with the interpreter of your choice.
    exit /b 1
)

python "%~dp0tools\prepare_cache.py" %*
exit /b %errorlevel%
