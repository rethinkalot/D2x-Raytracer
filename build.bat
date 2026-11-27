@echo off
rem PARITY TEST BUILD: D2X Raytracer (D2Rsp) from C:\Games\D2sRestore.
rem Deploys only to C:\Games\D2xR3 as D2x-RayTracer3.exe (never the real testbenches).
rem Everything below lives only in this process (setlocal). Nothing touches the
rem Windows/user env, global PATH, or the MSYS2 MinGW toolchain used by D1s/D2s.
setlocal
set "ROOT=%~dp0"
set "T=%ROOT%toolchain"
set "PRESET=directx12-win-release"
set "OUT=%ROOT%out\build\%PRESET%\D2Rsp"
set "DEPLOY=C:\Games\D2xR3"
set "EXE=D2x-RayTracer3.exe"

rem Minimal PATH: Windows + Git (vcpkg registry) + local cmake/ninja. No mingw64.
set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\WindowsPowerShell\v1.0;C:\Program Files\Git\cmd;%T%\cmake\bin;%T%\ninja"
call "%T%\msvc\setup_x64.bat" || goto :fail

set "VCPKG_ROOT=%T%\vcpkg"
set "VCPKG_DISABLE_METRICS=1"
set "VCPKG_DOWNLOADS=%T%\vcpkg-cache\downloads"
set "VCPKG_DEFAULT_BINARY_CACHE=%T%\vcpkg-cache\binary"
set "X_VCPKG_REGISTRIES_CACHE=%T%\vcpkg-cache\registries"
set "VCPKG_OVERLAY_TRIPLETS=%T%\triplets"
set "VCPKG_DEFAULT_TRIPLET=x64-windows-d2rs"
set "VCPKG_DEFAULT_HOST_TRIPLET=x64-windows-d2rs"

cd /d "%ROOT%"
cmake --preset %PRESET% -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_MAKE_PROGRAM="%T%\ninja\ninja.exe" -DVCPKG_TARGET_TRIPLET=x64-windows-d2rs -DVCPKG_HOST_TRIPLET=x64-windows-d2rs || goto :fail
cmake --build "out\build\%PRESET%" -- -j10 || goto :fail
if not exist "%OUT%\descent2.exe" (
  echo Build completed, but no expected executable was found. 1>&2
  goto :fail
)

taskkill /F /IM %EXE% >nul 2>&1
timeout /t 1 /nobreak >nul
rem Copy game files only; never deletes anything already in the testbench (game data).
robocopy "%OUT%" "%DEPLOY%" /E /NJH /NJS /NDL /NP /XD CMakeFiles /XF *.cmake *.ninja *.obj *.lib *.exp *.ilk descent2.exe
if errorlevel 8 goto :fail
copy /Y "%OUT%\descent2.exe" "%DEPLOY%\%EXE%" >nul || goto :fail
echo Confirm Complete
exit /b 0

:fail
echo D2x-Raytracer parity build FAILED 1>&2
exit /b 1
