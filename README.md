# D2X Parity: Raytracer × Redux SLikeNet

**Two Descent 2 engines. One battlefield.**

This branch holds two Descent 2 builds side by side, and for the first time they can play in the same match:

- **D2X Raytracer (DMAX 12)** (`D2Rsp/` + `RT/`): Descent 2 running on a real-time **DirectX 12 path tracer**, with soft shadows, path-traced global illumination, bloom, TAA, motion blur and physically based materials. The mines have never looked like this.
- **D2X-Redux SLikeNet** (`D2sp/`): the fast, tight, competitive OpenGL build with SLikeNet networking, observers, demos and speedrun tools.

One pilot can fly path-traced and the other classic OpenGL, in the same game. Both see each other's shield bubbles, both use the same field of view, and the co-op robots behave the same in both builds. That's what *parity* means here.

---

## 🤝 Crossplay: how the two builds fit together

| | D2X Raytracer | D2X-Redux SLikeNet |
|---|---|---|
| Multiplayer protocol | **30107** | **30107** |
| Reported game version | 1.1.0 | 1.1.0 |
| Networking | SLikeNet | SLikeNet |
| Shield bubble packet (`MULTI_DMAX_SHIELDS`) | sends + draws | sends + draws |
| FOV math | tan(vertical/2), vertical worked out from horizontal FOV and screen shape | identical |
| Co-op robot timeout | once per frame | once per frame |

- **One protocol.** Both builds moved to protocol 30107; before, the raytracer was on 30106 and D2s on 30006. They find and join each other's games.
- **One version string.** The raytracer now reports 1.1.0 like D2s, instead of DXX-Retro 0.58.1, and the old define is gone from both CMakeLists.
- **Shield packets understood on both sides.** D2s reads `MULTI_DMAX_SHIELDS` (same id, same length), so the rest of the packet is never thrown away, and it now draws the bubbles too.
- **Same view.** 90° at 16:9 is stock Descent in both builds, so neither player gets a wider view than the other.
- **Same robots.** A leftover duplicate `multi_check_robot_timeout()` was removed from the raytracer, so co-op robots act the same for everyone.

---

## ✨ What's new in the D2X Raytracer

### Speed
- **Shader cache.** Compiled shaders are saved in `shader_cache/<version>/` next to the game. The version is a hash of every file under `assets/shaders`, so any shader edit triggers a recompile, and old caches clean themselves up on the next launch.
  - Renderer start drops from **~2.3 s to ~0.1 s** after the first run.
  - Main menu appears in **3.3 s instead of 6.4 s**.
- **No console window** in Release and RelWithDebInfo builds (`-debug` still opens one when you want it).

### Your look, your way
- **Texture packs.** Every folder in `assets/textures` is a pack layered over the original textures. Pick one in the Graphics menu.
- **Render profiles.** Shared by all pilots and packs, with their own lights and light levels. Each pack remembers its profile, and the renderer window auto-saves to the current profile.
- **Pilots remember everything.** Pack, profile and last-used settings are saved per pilot in `Players/<pilot>.rtvars`. The first run starts from a shipped profile.
- **Raytracing Options rebuilt from scratch.** They use the renderer's own values over their full range, with compact sliders that update live while the menu is open. The layout follows the renderer window, and profile values are no longer rounded off.

### Light and shadow
- **Smooth Transparency**, with energy effects (sparks, explosions, blobs, orbs) drawn as glowing additive light.
- **Shadow fixes:** "No casting shadow" is now honoured on the inline path, shadow rays are two-sided, and there are new **Lights Cast Shadows** and **Projectiles Cast Shadows** toggles. Material flags are set per texture type.
- **New "flicker" material.** Reactor walls (ctrl01-04) glow red with whitish flashes.
- **The Z monitor sign** (misc060) is now a flashing green/white light that goes dark when you shoot it.
- **Packaged textures** got updated material flags, so orbs and energy01 glow as effects.
- Per-weapon light names and the flash multiplier now save correctly.

### Game feel
- **3D briefing robots.** D2 briefings show the path-traced 3D robot whenever 3D Briefing Robots is on, or whenever the movie can't play.
- Proximity bombs, cloak, invulnerability and extra life spin at half speed (visual only).
- **A custom Pyro.** `assets/ship.pof` (SHIP1-1..5 + GLOW04) ships with the build.
- Window title: **Dmax 12 D2x-RayTracer3**.

---

## 🛡️ What's new in D2X-Redux SLikeNet

- **Shield bubbles, ported from the raytracer.** `shieldfx.c` is shared as-is and drawn by a new OpenGL renderer (`arch/ogl/shieldgl.c`), a GLSL port of the raytracer's `ShadeShieldBubble` / `ShieldSweep` shaders. You get:
  - bubbles on players with hit ripples and flicker
  - collapse and power-up animations
  - invulnerability arcs and energy-transfer effects
  - the red low-shield gauge pulse
  - a **Shield Effects** checkbox, saved in the pilot file
- **FOV slider.** 90-120 in steps of 2, live, under 3D Briefing Robots, saved as `FOV=` in `descent.cfg`. It uses exactly the raytracer's math.
- **Tidier Graphics menu.** Colored Dynamic Light, Classic Depth Ordering, Disable Cockpit View and 4x multisampling are hidden from the menu, but their values stay in the config and pilot files.

---

## 📜 Full update history

### 2026-11-27: Parity branch (this branch)
- **Parity setup.** Copies of both games side by side: the raytracer updated to **DMAX 12** (RT renderer + game code, plus the D2s merge files it was missing) in `D2Rsp/`, and Redux SLikeNet in `D2sp/`.
  - The `.dxa` packs (`scoresb.dxa`, `XD1D2_HUD.DXA`) ship in `D2Rsp/Data` and are copied next to the exe on build.
  - Each game has its own build script and testbench.
- **Raytracer:** shader cache, no console window, window title.
- **Raytracer:** texture packs, render profiles, pilot settings, new Raytracing Options.
- **Crossplay:** D2s and the D2X Raytracer join each other's games.
- **D2s:** FOV slider and a tidier Graphics menu. The raytracer ships its Pyro model.
- **Shield bubbles in D2s,** plus briefing robots, reactor and sign lighting, and texture fixes.

### 2026-11-26: Menu boxes, briefings and texture loading
- **Menu boxes show again.** Menu and title PNGs are read through PhysFS, so PNGs inside the game's `.dxa` packs load, with a plain file on disk as fallback (`2d/pcx.c`).
- **Briefing robots are textured again.**
- **Raytracer texture loading.** Materials and textures moved to PhysFS so they can come from packs, and the material editor no longer saves blank texture paths.

### 2026-11-25: D2X-Redux SLikeNet merged into the raytracer
The raytracer now runs on the D2X-Redux SLikeNet game code, with the raytracer's DirectX 12 hooks and DMAX features carried over.

**Brought over from D2X-Redux SLikeNet**
- SLikeNet multiplayer:
  - in-game traffic on the game port, up to 90 packets per second
  - token-checked handshake and better NAT behaviour
  - `proxy.cfg` for running several copies on one PC
  - the HAM-hash anti-cheat
  - the SLikeNet source itself, in `thirdparty/SLikeNet`
- Smooth remote ships, and co-op robots that move only by network updates.
- Observer mode: its own controls, a wall-aware camera, fading ships, observer chat and ping.
- Demos recorded and played at your frame rate (demo version 16).
- HUD and gameplay extras: speedrun timers (RTA/IGT), a velocity meter, longer HUD messages, Suppress HUD Messages, Original D1 Weapons, and team and ship colours.
- The resolution menu, menus and options, and the frame limiter.

**Kept from the raytracer**
- DirectX 12 path tracing, the 3D cockpit, VR frame pacing and free camera.
- Shield bubbles for you and other players, the shield packet, and the red low-shield pulse.
- Enhanced turning with mouse and joystick turn limits, uncapped turning, and Automap XL.
- Player ship colours that find the recolourable ship textures by name.

**Changed or left out**
- Frame rate cap is 360, the same as D2X-Redux SLikeNet.
- D2X-Redux's OpenGL-only 3D model replacements are switched off in the raytracer (`main/rt_d2s_stubs.c`), because the raytracer has its own model and material system.
- VSync is the raytracer's own setting in its render menu.

### 2026-11-25: Repository start
- The `d2` folder of DXX-Raytracer-D2 imported as the D2x-Raytracer repository.

---

## 🔧 Building

| Game | Source | Build | Testbench | Exe |
|---|---|---|---|---|
| D2X Raytracer | `D2Rsp/` + `RT/` | `build.bat` | `C:/Games/D2xR3` | `D2x-RayTracer3.exe` |
| D2X-Redux SLikeNet | `D2sp/` | `./build-d2s.sh` (Git Bash) | `C:/Games/D2x3` | `D2x-Redux-Slikenet3.exe` |

- **Raytracer:** MSVC + Windows SDK, CMake, Ninja and vcpkg, using the `directx12-win-release` preset. The root `CMakeLists.txt` pulls in `sdl-master`, `sdl-mixer-master`, `physfs-main`, `RT` and `D2Rsp`. All environment setup stays inside `build.bat`'s own process.
- **Redux SLikeNet:** MSYS2 MinGW64, `cmake -G Ninja -B build` in `D2sp`, then the exe and its DLLs are copied to the testbench.

To play, add the `.hog`, `.ham`, `.s22`, `.pig` and `.mvl` files from your own Descent 2 install to the testbench folder.

---

## 🎮 Raytracer controls
- **SHIFT + ALT + F1:** debug menus (see below)
- **SHIFT + ALT + F2:** toggle depth testing for debug lines
- **SHIFT + ALT + F:** toggle free camera
- **F1:** important keybindings
- **ALT + F2 / ALT + F3:** save / load game

### Debug menus
- **Render Settings:** graphics settings and render debug tools
- **GPU Profiler:** GPU time per render pass
- **Material Editor:** edit material properties in any level
- **Polymodel Viewer:** view a polymodel in the current scene
- **Dynamic Lights:** tune weapon, explosion and muzzle-flash lights
- **Light Explorer:** tune and debug-view individual level lights

## 🖼️ Texture compression: making DDS textures
Download [NVIDIA Texture Tools](https://developer.nvidia.com/gpu-accelerated-texture-compression).

1. Run `python PNGToDDS.py <path>` on a folder of PNGs. It writes an `.nvtt` batch file.
2. Run `nvtt_export --batch "DXX_Raytracer_Convert_PNG_To_DDS.nvtt"`.

Name your PNGs `filename_basecolor.png`, `_normal`, `_metallic`, `_roughness` and `_emissive`, and each type gets the right settings. If you make DDS files another way, include mips and pre-multiply emissive textures by their alpha.

---

## 🏆 Credits: a three-man job

This one took three of us, and every one of them shows in the game:

- **RETHINK:** the vision, the driver and the tester. RETHINK set the goal of two engines in one match, tested every build and every texture by hand, and judged when each fix was right.
- **DMAX:** the raytracer muscle. DMAX 12 is DMAX's build, with the shield bubbles, the 3D cockpit, the VR pacing, the enhanced turning and the renderer work this whole branch stands on.
- **Claude:** the code partner. Claude ported the shield shaders to GLSL, aligned the protocols, built the shader cache, texture packs and render profiles, did the FOV math and wrote these notes.

Three people, two engines, one Descent 2. **See you in the mines, pilot.** 🚀

### Standing on the shoulders of
- [DXX-Raytracer](https://github.com/BredaUniversityGames/DXX-Raytracer) by Breda University of Applied Sciences students: the original path-traced Descent.
- [DXX-Raytracer-D2](https://github.com/arbruijn/DXX-Raytracer-D2) by arbruijn: the Descent 2 port.
- D2X-Rebirth, D2X-Redux and the SLikeNet project: the game code and networking underneath it all.
- Parallax Software and Interplay: Descent 2.
