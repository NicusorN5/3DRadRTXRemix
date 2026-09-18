# remix-comp-proxy — 3D Rad 7.22

A DX9 proxy framework for RTX Remix compatibility mods, with built-in fixed-function pipeline (FFP) conversion. Part of the [Vibe Reverse Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering) toolkit.

This tree is the **3D Rad 7.22 (3Impact)** port. The install lives at `C:\3DRadRTX`. The proxy is `d3d9.dll`; RTX Remix is `d3d9_remix.dll`. Never replace the proxy with DXVK as `d3d9.dll`.

## What It Does

Legacy DX9 games use vertex/pixel shaders that RTX Remix can't inject ray-traced lighting into. This proxy sits between 3D Rad and Remix, intercepting D3D9 calls and converting shader-based rendering to fixed-function pipeline calls that Remix understands.

It also captures 3Impact cameras, ParticleSystemEXT particles, PointLights, and Fog v1.03 so Remix sees the same scene the engine is drawing.

### Hosts

| Host | Remix |
|------|--------|
| `3DRad.exe` editor | Launcher, then Remix in the 3D view. Help → **3D Rad Help Files** (`file:///` `3DRad_res/help/`) and **RTX Remix documentation**. |
| Compiled player (`3drad_player` copy) | Native Display Options first. Remix loads on the first real play `CreateDevice` after OK, once a scene camera exists — not on the loading-screen camera. |
| `3DRad_compiler.exe` | Skip Remix and DXVK. System `d3d9` only. |

Launcher and Display Options load **Remix User Graphics Settings** from the **local** folder: `rtx.conf` then `user.conf` (user wins). Saves upsert the same keys; they do not rewrite the whole conf and they keep `rtx.sceneScale`.

### Core Features

- **Full D3D9 proxy** — `d3d9.dll` with every `IDirect3DDevice9` method intercepted; Remix remains `d3d9_remix.dll`
- **FFP conversion** — captures VS constants, parses vertex declarations, transposes matrices, and routes draw calls through the D3D9 fixed-function pipeline
- **Draw routing** — configurable decision trees that classify each draw call (3D geometry, HUD, skinned mesh) and decide whether to convert or pass through
- **Camera detection** — walks **all** `CamChase` / `Cam1StPerson` / `Camera` instances each frame (plugin-pointer identity, host-row ObjectId like Particles — never slot index). Picks the engine realtime Rendering flag (`camera+0x124 == 0`). If several are on, picks the one driving the main viewport. LookAtLH eye is `0,0,0`; View is `VP * inv(P)`; world eye is `C` at `+0x50`, FOV at `+0x78`. Project switch/reload drops the set and rediscovers. No identity View/P after Present, no `enable_constant_world`, no `draws>=160` gate
- **Particles** — ParticleSystemEXT (`sType` 25), ObjectId bind, `{stem}.ini` from editor `3DRad_res\projects` or compiled `<exe>\3DRad_res\projects`. Skip empty overwrite. File-Open wins autoload; same-stem reload does not prune-all
- **PointLight** — D3D9 `SetLight` only (no Remix `CreateLight` / `DrawLightInstance`)
- **Fog v1.03** — Remix fog-remap via `SetConfigVariable` (`rtx.volumetrics.enableFogRemap`). Same project-load drop as Particles. Do not rewrite whole `rtx.conf`; keep `sceneScale`
- **Integrated frame tracer** — captures D3D9 API calls to JSONL with category filtering, delayed capture, and external trigger support
- **INI configuration** — register layouts, albedo stage, skinning toggle, and diagnostics in `remix-comp-proxy.ini` (no recompile needed)
- **ImGui debug overlay** (F4) — live VS constant heatmap, matrix viewer, texture stage bindings, draw stats, FFP enable/disable toggle, tracer controls
- **Diagnostic logging** — timed frame dump to `rtx_comp\diagnostics.log`
- **Optional skinning module** — runtime-toggled vertex skinning with bone matrix upload, vertex buffer expansion, and compressed format decoding
- **DLL chain loading** — pre-load and post-load DLL/ASI injection for additional mods
- **Component module system** — `shared/` (game-agnostic static lib) + `comp/` (3D Rad DLL)

### Architecture

```
src/
  shared/              Game-agnostic static library
    common/
      config.hpp/cpp     INI config reader
      ffp_state.hpp/cpp  FFP state tracking, transforms, lighting, texture stages
      remix_api.hpp/cpp  Remix Bridge (InitializeLibrary after a real scene camera)
      ...
    utils/               Hooking, memory, general utilities
  comp/                3D Rad 7.22 (this tree)
    main.cpp             DLL entry, host_kind (editor / compiled_player / compiler)
    d3d9_proxy.cpp       d3d9.dll export forwarding → d3d9_remix.dll (not DXVK as d3d9.dll)
    project_file.cpp     File-Open / reload; drops particles, fog, cameras
    display_options.cpp  Compiled Display Options; local rtx.conf + user.conf
    launch_dialog.cpp    Editor launcher graphics
    editor_frame.cpp     Help Files + RTX Remix documentation
    remix_graphics.cpp   user.conf overlays rtx.conf; upsert-only saves
    compiler_inject.cpp  Copy remix files into compiledProject; compiler skip Remix
    game/
      camera.cpp         Multi-camera detection, Rendering flag, View/FOV
      particles.cpp      ParticleSystemEXT + ObjectId INI
      lights.cpp         PointLight SetLight-only
      fog.cpp            Fog v1.03 → fog-remap SetConfigVariable
    modules/
      d3d9ex.cpp         D3D9 proxy with FFP + tracer interceptions
      renderer.cpp       Draw routing decision trees
      imgui.cpp          Debug overlay (F4) with FFP tab
      tracer.cpp         Integrated frame tracer
      diagnostics.cpp    Frame logging
      skinning.cpp       Optional skinning
```

## Building

Build from this folder (`patches/3DRad`), not the `rtx_remix_tools` template:

```bat
cd patches\3DRad
build.bat release
```

Requires Visual Studio 2022 (x86 toolset). Output: `build/bin/release/d3d9.dll`. If `C:\3DRadRTX\` exists, `build.bat` also copies that DLL to `C:\3DRadRTX\d3d9.dll`.

## Deploying

1. Copy `d3d9.dll` to `C:\3DRadRTX\` (editor / launcher).
2. Copy the same `d3d9.dll` into **each** `C:\3DRadRTX\3DRad_res\compiledProject\<build>\` folder. Conf, `.ini`, Remix, and `.trex` stay local to that compile.
3. Keep `remix-comp-proxy.ini` next to the host. Edit it for FFP / tracer / diagnostics; do not use it to swap in DXVK as `d3d9.dll`.
4. RTX Remix stays `d3d9_remix.dll` in the same folder as the proxy.
5. Do not wholesale-rewrite `rtx.conf` / `user.conf`. Upsert keys only. Keep `rtx.sceneScale`.

Restart `3DRad.exe` or the compiled player after replacing `d3d9.dll`.

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
