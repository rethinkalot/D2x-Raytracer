#!/bin/bash
# PARITY TEST BUILD: D2X-Redux SLikeNet (D2sp) from C:\Games\D2sRestore.
# Deploys only to C:\Games\D2x3 as D2x-Redux-Slikenet3.exe (never the real testbenches).
# PATH change lives only in this script's process. Run from Git Bash: ./build-d2s.sh
cd /c/Games/D2sRestore/D2sp || exit 1
export PATH=/c/Programs/msys64/mingw64/bin:$PATH
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release || exit 1
cmake --build build -- -j10 || exit 1
[ -f build/main/d2x-redux.exe ] || { echo "Build completed, but no expected executable was found." >&2; exit 1; }
mkdir -p /c/Games/D2x3
taskkill //F //IM D2x-Redux-Slikenet3.exe >/dev/null 2>&1; sleep 1
cp -v build/main/d2x-redux.exe /c/Games/D2x3/D2x-Redux-Slikenet3.exe || exit 1
cp -v build/main/*.dll /c/Games/D2x3/ 2>/dev/null
echo "Confirm Complete"
