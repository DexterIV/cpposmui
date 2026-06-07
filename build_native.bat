@echo off
setlocal
set VSLANG=1033
set VS=C:\Program Files\Microsoft Visual Studio\2022\Community
set VCPKG_ROOT=%VS%\VC\vcpkg
set CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe

cd /d "%~dp0"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat"

echo ===== CONFIGURE =====
"%CMAKE%" -B build -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows
set CFG=%errorlevel%
echo CONFIGURE_EXIT=%CFG%
if not "%CFG%"=="0" ( echo RESULT=CONFIGURE_FAILED & exit /b 2 )

echo ===== BUILD =====
"%CMAKE%" --build build -j 8
set BLD=%errorlevel%
echo BUILD_EXIT=%BLD%
if not "%BLD%"=="0" ( echo RESULT=BUILD_FAILED & exit /b 3 )

echo RESULT=BUILD_OK
