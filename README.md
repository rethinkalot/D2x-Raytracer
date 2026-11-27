## Update History

### 2026-11-26: Menu boxes, briefings and texture loading
- **Menu boxes show again.** Menu and title PNGs are now read through PhysFS, so PNGs inside the game's `.dxa` packs load, with a plain file on disk as fallback (`2d/pcx.c`).
- **Briefing robots are textured again.**
- **Raytracer texture loading (work in progress, not in this repo).** Materials and textures are being moved to PhysFS so they can come from packs, and the material editor no longer saves blank texture paths. These changes are in `RT/`, which sits outside this repo, and haven't been tested in game yet.

### 2026-11-25: D2X-Redux SLikeNet merged into the raytracer
The raytracer now runs on the D2X-Redux SLikeNet game code, with the raytracer's DirectX 12 hooks and DMAX features carried over.

**Brought over from D2X-Redux SLikeNet**
- SLikeNet multiplayer: in-game traffic on the game port, up to 90 packets per second, token-checked handshake, better NAT behaviour, `proxy.cfg` for several copies on one PC, and the HAM-hash anti-cheat. The SLikeNet source is in `thirdparty/SLikeNet`.
- Smooth remote ships, and co-op robots that move only by network updates.
- Observer mode: own controls, wall-aware camera, fading ships, observer chat and ping.
- Demos recorded and played at your frame rate (demo version 16).
- Speedrun timers (RTA/IGT), velocity meter, longer HUD messages, Suppress HUD Messages, Original D1 Weapons, team and ship colours.
- The resolution menu, menus and options, and the frame limiter.

**Kept from the raytracer**
- DirectX 12 path tracing, the 3D cockpit, VR frame pacing and free camera.
- Shield bubbles for you and other players, the shield packet, and the red low-shield pulse on the ship gauge.
- Enhanced turning with mouse and joystick turn limits (in the sensitivity menu), uncapped turning, and Automap XL (Misc menu).
- Player ship colours that find the recolourable ship textures by name.

**Changed or left out**
- Multiplayer protocol is **30106**. Raytracer builds play each other, but not D2X-Redux SLikeNet builds (30006), because the shield packet would break their games.
- Frame rate cap is 360, the same as D2X-Redux SLikeNet.
- D2X-Redux's OpenGL-only 3D model replacements are switched off in this build (`main/rt_d2s_stubs.c`). The raytracer has its own model and material system.
- VSync is the raytracer's own setting in its render menu.

**Build**
- Builds with Visual Studio 2022's compiler (MSVC), the Windows SDK, CMake and vcpkg, using the `directx12-win-release` preset from the parent DXX-Raytracer folder.

### 2026-11-25: Repository start
- The `d2` folder of DXX-Raytracer-D2 imported as this repository.
