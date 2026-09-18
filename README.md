# remix-comp-proxy — 3D Rad 7.22

A DX9 proxy framework for RTX Remix compatibility mods, with built-in fixed-function pipeline (FFP) conversion. Part of the [Vibe Reverse Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering) toolkit.

[![Watch the video](https://img.youtube.com/vi/GXJ3-TsTYsg/maxresdefault.jpg)](https://youtu.be/GXJ3-TsTYsg)

This folder is the **3D Rad 7.22** port (the 3Impact engine). A typical install is `C:\3DRadRTX`.

## What It Does

3D Rad draws with old Direct3D 9 shaders. RTX Remix cannot put ray-traced lighting into those shaders, so this proxy sits in the middle: it presents itself as `d3d9.dll`, turns the game’s draws into the fixed-function pipeline Remix understands, and hands the real Remix runtime a second DLL named `d3d9_remix.dll`.

On top of that conversion, it reads the live scene so Remix tracks the same camera, lights, particles, and fog that 3D Rad is using.

## Core functionality

### What you copy where

After a build you have one proxy DLL: `d3d9.dll`.

| Put a copy here | Used by |
|-----------------|--------|
| `C:\3DRadRTX\d3d9.dll` | The editor (`3DRad.exe`) |
| `C:\3DRadRTX\3DRad_res\compiledProject\<your-game>\d3d9.dll` | That compiled game (each build has its own folder) |

Keep `d3d9_remix.dll` (Remix from update function) and `remix-comp-proxy.ini` next to the proxy. Graphics settings live in that same folder as `rtx.conf` and `user.conf` — your user file wins if both exist.

Example: you compile a project named *scary*. It copies the proxy and remix files into `compiledProject\scary_20260918113742\`

### Editor, compiled game, compiler

**Editor** — Run `3DRad.exe`. A launcher lets you pick Remix graphics. The 3D view then runs through Remix. Help → **3D Rad Help Files** opens `3DRad_res\help\` in your browser; **RTX Remix documentation** opens NVIDIA’s docs.

**Compiled game** — Run the exe. You get 3D Rad’s Display Options first (resolution, and the same Remix graphics lists as the launcher). After OK, the game starts and Remix comes up once the real play camera is there.

**Compiler** (`3DRad_compiler.exe`) — Builds the game only. It does not load Remix.

### What Remix sees from the scene

A project can have many cameras (several chase cams, first-person cams, and others). The proxy finds all of them and follows whichever one is actually rendering the main view. Open a different project, or reload the same one, and it starts that search over.

Lights, particle systems, and Fog objects in the scene are forwarded the same way, so a PointLight, a ParticleSystem, or Fog in the editor list shows up in Remix. Per-object particle settings are stored in a `{project}.ini` next to the project: editor installs use `C:\3DRadRTX\3DRad_res\projects\`, compiled games use `<that-exe>\3DRad_res\projects\`.

### Graphics and the F4 overlay

Launcher (editor) and Display Options (compiled) both edit Remix’s User Graphics Settings — presets like Ultra / High / Medium / Low / Custom — and save them locally.

In a Remix session, press **F4** for the ImGui overlay: draw stats, FFP on/off, matrix/texture views, and tracer controls. Timed dumps go to `rtx_comp\diagnostics.log`. Press **ALT + X** for Remix settings.

### Other toolkit pieces

- **Draw routing** — 3D meshes go through FFP; HUD and similar draws can pass through
- **Frame tracer** — optional JSONL capture of D3D9 calls
- **`remix-comp-proxy.ini`** — register layouts, albedo stage, skinning, diagnostics (no recompile)
- **Skinning module** — optional bone-matrix path for skinned meshes
- **DLL chain** — extra DLL/ASI mods can load before or after the proxy

## Architecture

```
src/
  shared/              Game-agnostic static library
    common/
      config.hpp/cpp     INI config reader
      ffp_state.hpp/cpp  FFP state tracking, transforms, lighting, texture stages
      remix_api.hpp/cpp  Remix Bridge
      ...
    utils/               Hooking, memory, general utilities
  comp/                3D Rad 7.22 (this tree)
    main.cpp             DLL entry; editor vs compiled vs compiler
    d3d9_proxy.cpp       Forwards to d3d9_remix.dll
    project_file.cpp     Project open / reload
    display_options.cpp  Compiled Display Options
    launch_dialog.cpp    Editor launcher graphics
    editor_frame.cpp     Help menu
    remix_graphics.cpp   rtx.conf + user.conf
    compiler_inject.cpp  Copies remix files into a compiled build
    game/
      camera.cpp         Scene cameras
      particles.cpp      Particle systems
      lights.cpp         Point lights
      fog.cpp            Fog
    modules/
      d3d9ex.cpp         D3D9 proxy with FFP + tracer
      renderer.cpp       Draw routing
      imgui.cpp          F4 overlay
      tracer.cpp         Frame tracer
      diagnostics.cpp    Frame logging
      skinning.cpp       Optional skinning
```

## Building

From this folder (`patches/3DRad`):

```bat
cd patches\3DRad
build.bat release
```

Requires Visual Studio 2022 (x86 toolset). Output: `build/bin/release/d3d9.dll`. If `C:\3DRadRTX\` exists, the script also copies the DLL there. Compiled games still need their own copy (see above).

## Deploying

1. Copy `d3d9.dll` to `C:\3DRadRTX\` for the editor.
2. Copy the same file into each `compiledProject\<your-game>\` folder.
3. Leave `d3d9_remix.dll` and `remix-comp-proxy.ini` beside the proxy.
4. Restart `3DRad.exe` or the compiled exe.

## Contributors

| Who | What | Support |
|-----|------|---------|
| [xoxor4d](https://github.com/xoxor4d) | Original [remix-comp-base](https://github.com/xoxor4d/remix-comp-base) framework, D3D9 proxy architecture, ImGui integration, module system | [Ko-Fi](https://ko-fi.com/xoxor4d) / [Patreon](https://patreon.com/xoxor4d) |
| [kim2091](https://github.com/kim2091) | FFP conversion system, skinning module, diagnostic logging, tracer integration, INI config, toolkit integration | [Ko-Fi](https://ko-fi.com/kim20913944) |
| [momo5502](https://github.com/momo5502) | Initial codebase that remix-comp-base was built on | |

## Dependencies

- [Dear ImGui](https://github.com/ocornut/imgui) — debug overlay
- [MinHook](https://github.com/TsudaKageyu/minhook) — function hooking
- [RTX Remix Bridge API](https://github.com/NVIDIAGameWorks/rtx-remix) — Remix integration

## License

MIT. See [LICENSE](LICENSE).
