# CLAUDE.md
- ALWAYS use Caveman skill.
- ALWAYS use Ponytail skill.

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

D2X Raytracer: a Descent 2 fork of DXX-Raytracer (C, with C++ in the renderer), with a DirectX 12 path tracer. This git repo (`rethinkalot/D2x-Raytracer`) is only the `d2` folder. The build lives in the parent, `C:/Games/D2Rs`: its root `CMakeLists.txt` pulls in `sdl-master`, `sdl-mixer-master`, `physfs-main`, `RT` (the renderer) and `d2`; `d1` is disabled. The sibling MinGW ports live in `C:/Games/D1s` and `C:/Games/D2s`, each with its own `CLAUDE.md`.

## Working rules (from RETHINK)

- Plan, confirm, execute, build, then wait for the next task. Raise likely problems and choices before deciding.
- Make only the code changes that were asked for. No unrequested refactors or style cleanups.
- If an attempt fails or you get stuck, stop and ask how to proceed.
- Don't run git commands or scripts on your own initiative. Don't commit untested builds; RETHINK tests first.
- Ask before destructive commands, and don't treat one approval as standing permission. No `rm -rf`-style cleans; builds stay incremental.
- Don't install or reconfigure toolchains.
- Use targeted reads and greps. Many files here are very large.

## Toolchain isolation (hard rule)

D2Rs must never affect the D1s/D2s MinGW builds:
- Never set Windows/user/machine env vars, never edit the global PATH, and never install to Program Files.
- Never copy DLLs into shared folders.
- Everything D2Rs needs lives in `C:/Games/D2Rs/toolchain/` (portable MSVC + Windows SDK, CMake, Ninja, vcpkg + its caches). That folder is not in git.
- All env setup happens only inside `C:/Games/D2Rs/build.bat`'s own process (`setlocal`). MSYS2 `mingw64` is deliberately kept off its PATH.

## Build and deploy

From `C:/Games/D2Rs`:

    build.bat

This configures the `directx12-win-release` preset and builds incrementally into `out/build/directx12-win-release`. On success it closes any running `descent2.exe`, copies the game to the testbench `C:/Games/D2xRT`, and prints "Confirm Complete". The same command is the "Build D2x-Raytracer" task in `C:/Games/D2Rs/.vscode/tasks.json`.

The global F5 keybind is the MinGW D1s/D2s build. Don't press it in a terminal at `C:/Games/D2Rs`.
