@echo off
setlocal
cd /d "%~dp0"
echo [cpposmui build] > build.log
echo Starting cmake configure... >> build.log
cmake -B build -G "Visual Studio 17 2022" -A x64 >> build.log 2>&1
echo cmake configure exit: %ERRORLEVEL% >> build.log
if %ERRORLEVEL% NEQ 0 (
    echo Configure FAILED >> build.log
    goto end
)
echo Building... >> build.log
cmake --build build --config Debug -j4 >> build.log 2>&1
echo Build exit: %ERRORLEVEL% >> build.log
:end
echo Done. See build.log for details.
pause
