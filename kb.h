// Knowledge base — 3D Rad 7.22 (3Impact engine), RTX Remix FFP port
//
// Target install: C:\3DRadRTX
// Renderer lives in dll3impact.dll, NOT in 3DRad.exe. The exe (editor),
// 3drad_player.exe (runtime) and 3DRad_compiler.exe are all thin hosts that
// import the engine DLL; none of them touch d3d9 directly. A d3d9.dll proxy
// therefore covers every host. Host kind is classified once in
// setup_exe_module() from the EXE leaf (plus GetModuleHandle self-check):
//   editor          = 3DRad.exe / 3DRadRT.exe (exact leaf, not 3drad_player)
//   compiler        = 3DRad_compiler.exe (skip Remix AND DXVK)
//   compiled_player = everything else (scary.exe, 3drad_player.exe, client.exe)
// #32770 is NOT a host-kind signal (launch dialog, Particles, Properties, and
// compiled Display Options all use it). Camera must not flip compiled_player
// from a dialog hwnd while is_editor_host. Display Options CBT is compiled
// player only and requires title "Display Options" plus IDs
// 1036/1038/1039/1040/1041/1072 as a group.
//
// 3DRad_compiler.exe is detected by exe name
// and skips Remix/DXVK (system d3d9 passthrough, no FFP wrapper). The
// RTX-Comp console is still shown so inject/process logs are visible.
// MinHook is still used in that process, but only to watch SHFileOperationW /
// MessageBox so compiled games get the Remix proxy runtime copied in.
//
// Editor launch dialog (3DRad.exe only — never compiler or compiled player):
//   First Direct3DCreate9 / Direct3DCreate9Ex (NOT DllMain) shows a native
//   Win32 dialog: RTX Remix / Base DX9 / DXVK, plus Update and Cancel.
//   Enter = Launch with the selected radio (Remix is the default). The same
//   dialog hosts the Remix 1.5 Graphics-tab set (preset, lighting, upscaler,
//   reflex, denoiser, post, replacements) in a scrollable panel. Launch / Save /
//   Update sit above that slider; Cancel stays on the action row. Checkbox
//   captions are unthemed + WM_CTLCOLORBTN (white on #000). Launch writes
//   rtx.conf + user.conf
//   (C:\3DRadRTX\ and .trex\ if present), flushes, THEN LoadLibrary(d3d9_remix.dll).
//   Save writes without starting Remix. Cancel / window X abort: no conf write,
//   no Remix/DXVK.
//
// Compiled-player DXUT "Display Options" (#32770) — never the editor:
//   Live IDs (do not change): IDOK=1, IDCANCEL=2, device combo=1036,
//   resolution=1038, antialiasing=1039, Full Screen=1040, VSync=1041,
//   Do not show again=1072. Proxy subclasses, restyles (Segoe UI, Explorer
//   theme). Compiled player only. Original Display controls + OK/Cancel stay pinned outside a
//   scroll host; only the Remix graphics panel scrolls (EnumChildWindows
//   MOVE, not ScrollWindowEx). Combo window height 22–24px, never 160.
//   Prefills on show: load user.conf then rtx.conf from the compiled EXE dir
//   (rtx wins). .trex copies fill missing keys only so a stale
//   .trex\rtx.conf cannot pin Graphics Preset to Ultra. Editor launcher uses
//   C:\3DRadRTX, never a compiled project folder. OK writes both files, flushes,
//   then DefSubclassProc so native DXUT applies the device in THIS process.
//   First CreateDevice is Rendering Window 588x441 on system d3d9 (picker
//   not dismissed yet). OK arms Remix; the post-OK CreateDevice is still
//   #32770 / Rendering Window 588x441 (not Fullscreen Window, not 1920)
//   then Reset to the play size. LoadLibrary d3d9_remix on that post-OK
//   CreateDevice when hwnd != 0 and backbuffer >= 64 (not 10x10 dummy).
//   Do not wait for a later Fullscreen Window CreateDevice — DXUT never
//   issues one. Do not LoadLibrary while Display Options is up before OK.
//   No --rtx-comp-gfx-ok, no CreateDialog skip, no ExitProcess on OK.
//   Graphics inject is User Graphics Settings only: General (brightness,
//   DLSS preset/type/RR/RR model/DLSS mode/DLFG/Reflex) and Graphics
//   (preset, bounces, particle light, NRC, volumetrics + quality froxel
//   32/16/8/4/3, post FX). No NIS/XeSS/TAA-U, no gfx-ok relaunch.
//   Editor Help (3DRad.exe caption): native "3D Rad Help Files" opens
//   file:/// <exe>\3DRad_res\help\ in the default browser (not Explorer,
//   not the local .htm association). Extra item "RTX Remix documentation"
//   opens https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/ .
//   Compiler skipped. Editor
//   Shaders → Clear Shaders on 3DRadRTXFrame is a separate path (editor_frame.cpp).
//   Last Launch choice is written to
//   remix-comp-proxy.ini [Launch] Backend=remix|dx9|dxvk and preselected
//   next time; the dialog still appears every editor launch.
//   Remix: LoadLibrary d3d9_remix.dll, FFP wrapper, camera hooks, MinHook,
//   NvRemixBridge / ImGui overlay as before.
//   Base DX9: GetSystemDirectory d3d9.dll, no FFP, no MinHook game hooks,
//   no NvRemixBridge.
//   DXVK: LoadLibrary d3d9_dxvk.dll (vanilla 32-bit DXVK). Missing file →
//   tell the user to hit Update; never silently load Remix.
//   Update (same dialog, WinHTTP worker): GitHub doitsujin/dxvk latest
//   win32/x32 d3d9.dll → d3d9_dxvk.dll; NVIDIAGameWorks rtx-remix 1.5.2
//   runtime zip → d3d9_remix.dll + .trex\ + NvRemixLauncher32.exe.
//   NEVER overwrite game-folder proxy d3d9.dll with Remix or DXVK.
//   .trex\d3d9.dll (64-bit renderer) is OK to update; backed up as
//   d3d9.dll.1.5.2.bak / d3d9.dll.bak. Abort Remix extract if the zip is
//   64-bit-only or would mix remix-main 152MB d3d9 into a .trex without
//   usd_*.dll. Close 3DRad.exe fully and reopen to see the dialog.
//
// Compiler output (runtime-confirmed 2026-09-14, project scary.3dr):
//   Staging / default dest: 3DRad_res\compiledProject\<name>_<YYYYMMDDHHMMSS>\
//   Example: C:\3DRadRTX\3DRad_res\compiledProject\scary_20260914172531
//   User may pick another parent ("Select destination folder for the compiled
//   project sub-folder"); Desktop client_YYYYMMDDHHMMSS is the same layout
//   when the compiled name is "client".
//   Exe is 3drad_player.exe renamed to <name>.exe (110592 bytes), not always
//   client.exe. Stock copy (SHFileOperationW): dll3impact.dll, 3DRad_res,
//   VC80 CRT/MFC/ATL/OPENMP, PhysX*, AntTweakBar, cudart32_30_9, icon.ico,
//   windowed.ini. No d3d9.dll / remix. Success dialog:
//   "Stand-alone executable generation completed successfully. Do you want
//   to open the compiled project folder?"
//   compiler_inject copies into that folder (never overwrites the stock exe
//   / dll3impact / 3DRad_res): d3d9.dll (this proxy), d3d9_remix.dll,
//   remix-comp-proxy.ini, dxvk.conf, rtx.conf, user.conf,
//   NvRemixLauncher32.exe, .trex\ (skip *.dmp, *dlss5*, d3d9.dll.* backups),
//   rtx-remix\ (skip captures/logs and directory symlinks — mods\3dradRTX is
//   a link into Documents and must not be followed). Particles UI hooks
//   (CBT / kernel32 CreateFile / DialogBoxParam) must not run in
//   3DRad_compiler.exe — they aborted VC80 with "Runtime Error" after inject.
//   Re-inject refreshes d3d9.dll even if the dest already has Remix; rtx.conf
//   / user.conf are copied only when missing (never overwritten).
//
// Compiled-player load abort (scary_20260918113742, 2026-09-18): VC80
// "Runtime to terminate in an unusual way" after Display Options OK, Remix
// bridge loaded, Reset 1920x1080, Frame 5 still phase=loading. Cause:
// try_enter_live_from_scene_vp called remixapi_InitializeLibrary on the first
// loading-screen scene VP (no published camera). RemixApi init now waits for
// remix_api_init_allowed (BeginScene / CreateDevice). ObjectRun hook is
// editor-only (exe leaf 3DRad.exe + prologue 8B 15 60 04 45 00). Fog capture
// / offsets / keys unchanged.
//   Project .ini files are copied from the compiler project ListBox (same
//   5-digit prefix + name as the editor object list) into
//   dest\3DRad_res\projects\{stem}.ini and dest\{stem}.ini. Restart the
//   compiler after deploying a new d3d9.dll for inject to be loaded.
//
// Binaries:
//   3DRad.exe          208896 bytes, 2011-09-09, MSVC8 (VC80) + MFC80U   <- editor
//   3drad_player.exe   110592 bytes, 2011-09-09                          <- player
//   dll3impact.dll     897024 bytes, 2011-09-09, ImageBase 0x10000000    <- engine/renderer
//
// Engine identity strings: "3Impact", "3Impact Engine", "3Impact.com - Fernando Zanini".
// The D3D device/window code is DXUT-derived (reference-rasterizer fallback text,
// display-mode dialog, "Could not find any compatible Direct3D devices").

// ---------------------------------------------------------------------------
// Graphics API surface
// ---------------------------------------------------------------------------
// dll3impact.dll imports d3d9.dll!Direct3DCreate9 and the whole D3DX9 helper
// set from d3dx9_27.dll. Shading is done through the D3DX Effect framework
// (D3DXCreateEffect / D3DXCreateEffectFromFileA), so all matrix uploads reach
// the device as SetVertexShaderConstantF issued from inside d3dx9_27.
//
// d3d8to9.dll shipping in the game folder is unused: the engine is native D3D9.

// IAT slots in dll3impact.dll. Preferred-base addresses — rebase by the module's
// actual load address before use, the DLL is relocatable.
$ 0x1005840C void* iat_Direct3DCreate9
$ 0x1005847C void* iat_D3DXMatrixLookAtLH          // source of the View matrix
$ 0x10058484 void* iat_D3DXMatrixPerspectiveFovLH  // source of the Projection matrix
$ 0x100584D4 void* iat_D3DXMatrixOrthoLH           // 2D/offscreen passes, not the main camera
$ 0x10058488 void* iat_D3DXMatrixMultiplyTranspose // concatenates matrices for VS constants
$ 0x100584F4 void* iat_D3DXMatrixMultiply
$ 0x100584F8 void* iat_D3DXMatrixInverse
$ 0x10058504 void* iat_D3DXMatrixTranspose
$ 0x10058438 void* iat_D3DXCreateEffect
$ 0x10058440 void* iat_D3DXCreateEffectFromFileA

// ---------------------------------------------------------------------------
// Shader transform model  (the critical finding for the Remix port)
// ---------------------------------------------------------------------------
// Shaders ship in 3DRad_res\system\shaders as D3DX effects: a handful of .fx
// sources plus ~370 precompiled .fxo. Every effect declares the same uniforms:
//
//   float4x3 amPalette[26];            // bone palette
//   float4x4 mxWorld      : WORLD;
//   float4x4 mxWorldIT    : amPalette; // inverse-transpose, transforms normals
//   float4x4 mxViewProj   : VIEWPROJECTION;
//
// Both vertex shaders transform position with mxViewProj ALONE:
//
//   VertNoSkinning:  OUT.Pos = mul(float4(IN.Pos.xyz,1), mxViewProj);
//                    N       = normalize(mul(IN.Normal, mxWorldIT).xyz);
//   VertSkinning:    world   = sum(mul(IN.Pos, amPalette[idx]) * weight);
//                    OUT.Pos = mul(float4(world,1), mxViewProj);
//
// mxViewProj is therefore OVERLOADED despite its name and semantic:
//   - rigid draws   -> it holds World * View * Projection (a full WVP).
//                      Proof: position gets no other transform, yet normals
//                      still need mxWorldIT, so vertices are in MODEL space.
//   - skinned draws -> it holds View * Projection only, because the bone
//                      palette already did model -> world (as the .fx comment
//                      in VS_Skin states).
//
// Consequence: the engine never hands the GPU a separate View or Projection.
// This is the classic concatenated-matrix case that Remix cannot decompose,
// and it is the core problem this port has to solve.

// ---------------------------------------------------------------------------
// VS constant register layout — VARIES PER SHADER, do not hardcode
// ---------------------------------------------------------------------------
// Extracted from the CTAB of all 16008 shader blobs via analysis/parse_ctab.py.
// A fixed register layout in ffp_state.hpp cannot work for this engine.
//
//   skinned, vs_1_1     amPalette c0-c77,  mxViewProj c78-c81
//   skinned, vs_2_0     amPalette c0-c77,  mxViewProj c83-c86 or c84-c87
//   rigid               mxViewProj c0-c3,  mxWorldIT c4-c6
//   rigid (+ world)     mxViewProj c0-c3,  mxWorld c4-c6, mxWorldIT c7-c9
//   rare variant        mxViewProj c4-c7
//
// mxWorld / mxWorldIT are float4x3 (3 registers, no translation row), so the
// object translation cannot be recovered from them — World has to come from
// the WVP instead.
//
// Robust approach: read the bound vertex shader's own CTAB at runtime
// (IDirect3DVertexShader9::GetFunction -> "CTAB" block) and look mxViewProj up
// by name. Same parser logic as analysis/parse_ctab.py.

// D3DX effect constant table, as embedded in shader bytecode after the CTAB fourcc.
struct D3DXSHADER_CONSTANTTABLE {
    unsigned long Size;         // 28 for a valid table
    unsigned long Creator;      // byte offset to creator string, from struct start
    unsigned long Version;      // high word 0xFFFE = vs, 0xFFFF = ps
    unsigned long Constants;    // element count of the ConstantInfo array
    unsigned long ConstantInfo; // byte offset to D3DXSHADER_CONSTANTINFO[]
    unsigned long Flags;
    unsigned long Target;       // byte offset to target string, eg "vs_2_0"
};

struct D3DXSHADER_CONSTANTINFO {
    unsigned long Name;          // byte offset to ASCIIZ name, eg "mxViewProj"
    unsigned short RegisterSet;  // 0=BOOL 1=INT4 2=FLOAT4 3=SAMPLER
    unsigned short RegisterIndex;
    unsigned short RegisterCount;
    unsigned short Reserved;
    unsigned long TypeInfo;
    unsigned long DefaultValue;
};

// ---------------------------------------------------------------------------
// Camera recovery — SOLVED and verified against Remix itself
// ---------------------------------------------------------------------------
// Remix's developer menu (Types > Main) confirms the result:
//   Vertical FOV: 60.0     Near / Far plane: 0.2 / 9986.6
//   Direction matches the recovered forward axis; handedness left-handed both.
// 0.2 / 10000 are the engine's genuine planes, recovered to within 0.13%.
//
// The method, in the order it runs:
//
// 1. Projection — captured exactly from d3dx9_27!D3DXMatrixPerspectiveFovLH.
//    Its arguments state the camera outright, so nothing is inferred. Store
//    *pOut plus fov/aspect/zn/zf. KEEP ONLY DISTINCT MATRICES: the engine
//    builds one projection far more often than the camera's, and a plain ring
//    buffer ends up holding N copies of a near-miss with the camera's gone.
//
// 2. Which projection built a given ViewProjection — matched by SCALE, not by
//    aspect ratio. ViewProjection = R * P with R a rotation, and a rotation
//    preserves length, so the product's first two column lengths are exactly
//    the projection's own w and h:
//        w = length(VP column 0 over rows 0..2)   (excludes the translation row)
//        h = length(VP column 1 over rows 0..2)
//    This is ESSENTIAL here: the scene contains two camera objects,
//    Cam 1StPerson and CamChase, which BOTH render at aspect 1.3339 — one at
//    fov 60 (w=1.29844, h=1.73205), the other at fov 75 (w=0.97697, h=1.30323).
//    An aspect-ratio filter cannot separate them; these scales can. Requiring a
//    0.1% relative match picks the right one unambiguously.
//
// 3. View = ViewProjection * inverse(Projection). One well-conditioned multiply.
//    Both factors are exact, so there is no decomposition and no precision loss.
//    D3DXMatrixInverse on the exact projection is fine; the verified analytic
//    form, for P = {w 0 0 0; 0 h 0 0; 0 0 q 1; 0 0 -q*zn 0}, is
//        invP = {1/w 0 0 0; 0 1/h 0 0; 0 0 0 -1/(q*zn); 0 0 1 1/zn}
//    confirmed numerically as 0.77016 / 0.57735 / -4.99990 / 5.00000.
//
// 4. Validation, which doubles as the positive identification. A view matrix is
//    a rigid transform: last column (0,0,0,1) and an orthonormal 3x3. That
//    rejects pairing the wrong projection (the mismatch survives as scale) and
//    rejects a World*ViewProjection (the object's scale survives). Correct
//    pairings land within 0.0002 of unit rows, so a 0.01 tolerance is both safe
//    and discriminating. Plus std::isfinite on all 16 elements — a NaN camera
//    took the Remix runtime down repeatedly.
//
// 5. Which camera is the scene's — the ViewProjection shared by the bulk of the
//    frame's draws. Steady state logs "30 calls of 30 accepted, across 1
//    distinct ViewProjection(s)", so one matrix serves every object while a
//    preview or cubemap face accounts for a handful. Remix reads one camera per
//    frame at Present, so only the dominant one is applied.
//
// 6. Independent cross-check — normalize(at - eye) from D3DXMatrixLookAtLH
//    equals the recovered rotation's third column (the forward axis). Two
//    unrelated derivations agreeing is the strongest identification available.
//    Measured error: 0.00000.
//
// REGRESSION ANCHOR. camera.cpp::self_test() reproduces this captured frame;
// it recovers the view to 0.000085 max element error. Do not change the
// recovery without re-running it.
//   ViewProj row0: 1.16152 -0.24261 -0.42445 -0.42445
//   ViewProj row1: 0.00000  1.64480 -0.31339 -0.31339
//   ViewProj row2: 0.58035  0.48557  0.84949  0.84949
//   ViewProj row3: 0.00000  0.00000 -0.20000  0.00000
//   Proj = {1.29844,1.73205,q=1.00002,-0.20000}  (fov 60, aspect 1.3339)
//   View row0: 0.89455 -0.14007 -0.42445 0   <- orthonormal, zero translation
//   View row1: 0.00000  0.94962 -0.31339 0
//   View row2: 0.44696  0.28034  0.84949 0
//
// DO NOT recover View and Projection by algebraically splitting their product.
// Tried and abandoned: the far plane is numerically unrecoverable, because
// q = zf/(zf-zn) sits within rounding error of 1.0 for any distant far plane —
// it produced 104857.8 for a true 10000 — and the recovered translation was
// wrong. Its shape test also accepted a World*ViewProjection as a camera.

// ---------------------------------------------------------------------------
// 3Impact renders CAMERA-RELATIVE  (explains Remix's Position 0,0,0)
// ---------------------------------------------------------------------------
// D3DXMatrixLookAtLH is called with eye=(0,0,0) EVERY time. The engine pins the
// camera at the origin and folds the camera offset into each object's world
// matrix instead — a standard large-world precision technique. So:
//   - the view matrix legitimately has NO translation (confirmed: recovered
//     view row3 is exactly (0,0,0,1), and VP row3 is (0,0,-q*zn,0));
//   - object world matrices carry the offset (observed translation
//     (2.838, -1.913, -2.039) while the editor HUD showed the camera at
//     C(1.834, -5.792, 33.887));
//   - Remix reporting "Position: 0.00 0.00 0.00" is FAITHFUL, not a bug.
// LookAtLH's `at` is the aim point in the same camera-relative space.
//
// Cost of leaving it: the camera never moves in Remix's world space, which
// undermines temporal accumulation/denoising and world-space anchoring for mesh
// replacements. Fix (WorldSpacePosition=1): read the SHOWN 60deg camera's
// iCameraLocation and put C into D3DTS_VIEW; World_true = World_rel * T(C)
// so World*View is unchanged.
//
// Gold camera/jitter (2026-09-16 ~21:21): no DLL bytes survived (C:\3DRadRTX
// d3d9.dll at 21:26 recycle is the 19:12 1.dll SHA256 D3FAAAB6… that this
// deploy replaced). Source is 2026-09-16_2140_editor-first-boot-restart
// camera.cpp (21:17, restore-play-cam result; PDB 21:19). 2116 backup is the
// pre-edit 2107 camera.
//
// Editor sim gate (2026-09-18): 3DRad.exe ObjectRun loop @ 0x414F20 still
// logs `sim start` / `sim stop`. Camera pick does NOT use that gate.
//
// Camera detection (2026-09-18): walk ALL CamChase / Cam1StPerson / Camera
// host plugins each Present (plugin-pointer identity, ObjectId = host row
// like Particles — never plugin+0x00 slot as live identity). Link each
// instance to dll3impact camera* (pointer scan in plugin 0x600, or plugin
// itself). Live Rendering flag is engine camera+0x124 (0=shown/rendering),
// same offset on CamChase and Cam1StPerson. Plugin +0x04 shown, +0x08
// Rendering At Start (checkbox), +0x0C Active (checkbox). Pick among
// rendering==1; if several, the instance whose FOV matches the main VP.
// No plugin rendering: editor list-cam (shown 60°). Log
// `cam pick n=N rendering=R id=OID name=CamChase fov=… shown=…`.
// Project switch/reload drops the whole set (project_file apply_folder_stem).
//
// Camera object (dll3impact.dll, preferred base 0x10000000):
// $ 0x100B7A88 int camera_count
// $ 0x100B7A28 void** camera_list   // array of camera*
//   camera+0x50  float3 location    // iCameraLocation
//   camera+0x78  float fov_radians  // pi/3 = editor 60deg, 1.309 = 75deg
//   camera+0x124 int hidden         // 0=shown (iCameraShown returns xor 1)
//
// Live 2026-09-13: shown 60deg camera is the editor viewport and can sit
// hundreds of units from the scene (postage-stamp view). Shown 75deg is
// Cam 1StPerson / CamChase in play. Both are valid scene cameras; applying
// both in one frame (or falling back to origin when the list read misses)
// looks like a Remix camera cut and grains the denoiser.
// Within a Present: lock one shown handle+FOV band; refresh View/C as the
// camera moves; reuse last-good View/P/C on a miss (no origin snap).
// Across frames: a real shown-camera switch (handle or 60↔75) is accepted
// after 2 confirming frames, or at once when a scene VP matches the new FOV.
// Cache the shown camera* and read C/FOV/shown from +0x50/+0x78/+0x124;
// rescan 0x100B7A28 only when that handle is stale. SetTransform VIEW/PROJ
// once per Present when the composed camera hash changes, not per draw.
// Cubemap/preview RTs (size != viewport) must not write D3DTS_VIEW/PROJECTION.
// HUD C() is the SELECTED object, not necessarily the shown camera.
//
// Script help (Script_reference.htm + CamChase.htm / Cam1StPerson.htm) never
// names iCamera* or a "current camera" getter. Public script API treats
// cameras as objects: iObjectShow/Hide/ShowHideSwitch toggles rendering
// ("Rendering At Start" + show/hide). iObjectLocation(OBJ, Vector3) is the
// documented world-space read (same field iCameraLocation reads at +0x50).
// FOV is a CamChase write-only internal parameter / property in degrees —
// no documented getter. Look-at is a CamChase relationship, not iCameraLookAt.
// Multiple cameras may be shown at once (split-screen); the foremost shown
// camera is the 3D-sound listener. iObjectPicked uses "the current camera
// (viewport)" implicitly; every other camera query takes an explicit handle.
// iCameraSet is an undocumented 3Impact export (likely a current-handle
// setter for later iCamera* calls), NOT a documented render-camera poll.
// Do not replace the list walk + shown(+0x124) + FOV match with "last
// iCameraSet": last Set is editor/script context, same trap as HUD C().

// ---------------------------------------------------------------------------
// Projections the engine builds (do not mistake these for the camera)
// ---------------------------------------------------------------------------
//   fov 90.0, aspect 1.0000, zn 0.2, zf 10000  x6  -> CUBEMAP FACES. Emitted
//        during engine init, BEFORE Direct3DCreate9/CreateDevice.
//   fov 60.0, aspect 1.3339, zn 0.2, zf 10000      -> main scene camera
//   fov 75.0, aspect 1.3339, zn 0.2, zf 10000      -> the second camera object
// 0.2 appears to be the engine-wide near plane. 75.19 deg is the horizontal
// equivalent of 60 vertical at aspect 1.3339, so the two may be one camera
// expressed under two conventions; either way the scale match separates them.

// ---------------------------------------------------------------------------
// TRAP: redirect() recursion on double install
// ---------------------------------------------------------------------------
// camera::init()'s redirect() takes the IAT slot's CURRENT value as the
// original to call through to. Installing twice therefore records our own stub
// as the original and recurses until the stack dies. init() runs from the
// renderer constructor and this game calls CreateDevice TWICE, so it is
// reachable. Guarded by a static flag in init(). Keep the guard.
//
// Remaining hook hazards, not yet addressed:
//   - registers_for()'s function-local static unordered_map is mutated per
//     draw and is not thread-safe.
//   - it is keyed on the raw IDirect3DVertexShader9*, so a freed-and-
//     reallocated shader at the same address gives a wrong register. Currently
//     self-correcting: a wrong register cannot satisfy the forward-multiply
//     check below, so the draw is rejected rather than mistransformed.
//   - the IAT is never unhooked, so if this DLL unloads before dll3impact.dll,
//     later calls jump into freed memory.
//   - an IAT hook would not catch calls resolved via GetProcAddress.

// ---------------------------------------------------------------------------
// World matrix per draw  (the current blocker)
// ---------------------------------------------------------------------------
// Two sources, both implemented:
//
// A. RECORDED (shipping, stable). The MultiplyTranspose hook sees the world,
//    the ViewProjection and its own output — the exact value the shader will
//    receive — all at once, so recording the three together lets a draw find
//    its own entry by the constant it was given. No inference. But the engine
//    concatenates only ~30 times for a frame of 230 draws, so most draws reuse
//    a constant, find no record, and keep their own shaders. Coverage is
//    therefore very low: 6 of 11 draws in one scene, 1 of 230 in another.
//
// B. mxWorld DIRECT READ (implemented, DISABLED behind enable_constant_world).
//    mxWorld is its own uniform, so the world can be read straight out of the
//    VS constants at the register the CTAB reports — no inversion, nothing
//    ill-conditioned. Validated by FORWARD multiply, which is well conditioned
//    and genuinely discriminating:
//        mxWorld * dominant_ViewProjection  ==  mxViewProj constant
//    That also proves the constants read belong to THIS draw and that the draw
//    belongs to the frame's camera. Note mxWorld may be allocated only 3
//    registers (each register is one COLUMN; the omitted 4th is the constant
//    (0,0,0,1)), and skinned shaders omit it entirely — identity is then
//    correct, since the bone palette already moved vertices to world space.
//
// DO NOT recover the world as WVP * inverse(ViewProjection). Tried twice, and
// it crashed the Remix runtime both times. Affine-after-division is NOT a
// discriminating test: matrices belonging to other cameras pass it.
//
// OPEN, and important: Remix crashes when conversion volume rises (1 -> ~228
// draws) REGARDLESS of how the world matrix is obtained. Since A and B derive
// it by entirely different means, this implicates THE FFP DRAW PATH ITSELF, not
// the transforms. Every stable configuration converted at most 6 draws, so the
// FFP path has only ever been exercised on a tiny sample of geometry. Bisect by
// conversion count to find the offending draw before re-enabling either.
//
// Measured draw routing (diagnostics.log "Draw routing"):
//   frame  580, 11 draws:   ffp=6  skinned=0 noNormal=0 worldUnresolved=4
//   frame 1530, 230 draws:  ffp=1  skinned=1 noNormal=0 worldUnresolved=228
//   frame  446, 26 draws:   ffp=0  skinned=0 noNormal=0 worldUnresolved=26
// Nothing is ever rejected for lacking a camera or normals.
//
// Vertex declarations seen in capture are all FFP-friendly — every one is
// [s0 +0] POSITION[0] FLOAT3, [s0 +12] NORMAL[0] FLOAT3, [s0 +24] TEXCOORD[0]
// FLOAT2, stream 0, default method. No FLOAT16, UBYTE4, TANGENT/BINORMAL or
// multi-stream layouts. NOT yet confirmed for the 230-draw scene specifically.

// ---------------------------------------------------------------------------
// Lighting — why converted geometry went black below Ultra
// ---------------------------------------------------------------------------
// 3Impact lights only in shaders. Converting a draw nulls VS/PS. Previously
// setup_lighting() also set D3DRS_LIGHTING FALSE and never SetLight, so Remix
// overlay Lights: 0. Ultra still showed meshes via sky GI (pathMinBounces=1
// in rtx_options.cpp). High/Medium/Low set pathMinBounces=0 — unlit FFP is
// black. Graphics presets do NOT turn RT off; they only drop bounces / GI.
// No fake skybox in this proxy. Remix sky autodetection (rtx.skyAutoDetect,
// skyBoxTextures) already runs on every preset.
//
// Capture: PointLight / SunLight are plugins (3DRad_res\objects\*\object.dll).
// Host list is in the THIN EXE (not dll3impact). Same host+0 / +0x291A layout;
// rebase off the actual module base. NEVER apply 3DRad.exe VAs to scary.exe —
// compiled log 2026-09-14 had no "host base=" bind and emitted 3 empty
// iLightLocal* slots (origin, rgb=0, range=25).
//   3DRad.exe (preferred 0x400000):
//     $ 0x450460 int object_count
//     $ 0x454468 void** object_list   // inline array, plugin* at host+0
//     $ 0x44AE58 HMODULE[]            // plugin module per slot
//   3drad_player.exe / renamed compiled exe (110592 bytes, preferred 0x400000):
//     $ 0x44445C int object_count     // cmp eax,[count] then [eax*4+list]
//     $ 0x448460 void** object_list
//     $ 0x43EE58 HMODULE[]            // GetProcAddress ObjectRun
// PointLight instance (malloc 0x584):
//   +0x04 shown, +0x0C active, +0x484 quat xyzw, +0x494 LOCAL pos xyz,
//   +0x52C gizmo mesh*, +0x530 rgb, +0x53C range factor
// ObjectRun calls transform helper then iLightLocal* from +0x494 (local).
// Parent follow (PointLight.htm): host+0x2928 linked / child list at
// host+0x291C count + host+0x2920 stride-8 {slot, boneId}. World =
// parent_world + R(parent_quat) * local, or gizmo iMeshLocation +0xCF8.
// Parent 3Impact is the *light's* host+0x2924 (not the parent's +0x2924).
// Parent world: SkinMesh +0x6DC mesh +0x15A8, camera +0x50, body +0x5B4.
// Skip origin padding when a longer sane vector exists. Editor 3D MAIN is
// the 1332 SetViewport (SetRenderTarget never reported that RT).
// Re-read every Present. Last-good color/range/pos kept so Show/Hide and
// RGB pulses do not drop Remix lights — never store insane coords
// (gizmo/mesh +0x52C on a small plugin read heap garbage ~1e21; Remix
// drops those). Parent resolved every frame (Character / SkinMesh,
// origin allowed). Do NOT probe SkinMesh +0x15A8 on small plugins.
// SetLight every Present. Remix CreateLight is OFF (SetLight-only like
// the working overlay Sphere:4 Distant:2). CreateLight on Reset AVed
// the bridge; CreateLight with NaN/huge pos zeroed lights. Title
// "sphere|rectangle|disk|cylinder" is a shape token only; ignore 'ht'.
// Radius constant (~0.1); range factor is radiance/attenuation (Remix
// SetLight sphere radius is fixed ~4). ObjectRun writes iLightLocal*
// only when +0x550 != -1 (SkinMesh assigned a slot). BSS 0x101B3C80
// stays empty while PointLight objects exist. Do not SetLight those
// empty template slots (rgb=0 origin) — compiled player fell into that
// path when the editor host table missed.
// iLightDirectional* 0x100ED650 is the one sun (default or last SunLight).
// SunLight ObjectRun: parent rotation aims +0x50C dir; SetLight every Present.
//
// Capture: Particles v1.16 (3DRad_res\objects\Particles\object.dll).
// ObjectPreInit malloc 0x1E5C (not PointLight 0x584). Host tables same as
// PointLight. Plugin at host+0. Dialog GET 0x10006F80 + live attach
// (slot 32 / 00032Particles, host 0x1A2EA898, plugin 0x1BE42AD8):
//   +0x04 shown, +0x08 Visible At Start, +0x0C active, +0x10=1
//   +0x1C / +0x1E54 Animation FPS (dialog 0x418)
//   +0x24 dir path, +0x424 name
//   +0x488 quat xyzw (n2≈1). LOCAL pos is +0x498/+0x49C/+0x4A0 — NOT
//   PointLight +0x494 (that dword is quat.w on this plugin).
//   World = parent_world + R(parent_quat)*local, parent via host+0x2924
//   / child list; +0xDD0 Parent bone (dialog 0x419), +0x5C8 ObjectInit bone.
//   +0x4D8 live CPU particle count, +0x4DC array stride 0x70, particle+0x6C
//   alive. Sim/play: count and +0x4DC heap fill; +0xE44 ObjectRun warmup
//   (reset after >5). Play can zero +0x04 shown while +0x0C active still
//   simulates — ObjectRun then skips the CPU billboard body; Remix still
//   spawns from dialog params. +0x564/+0x568 heap during emission.
//   +0x56C Working At Start
//   Dialog GET (DIALOGEX y-order + ObjectPropertiesWrite fstp), 0x40A–0x416:
//     0x40A Gravity X → +0x570
//     0x40B Gravity Y → +0x574   (NOT Opacity; old kb swapped these)
//     0x40C Gravity Z → +0x578
//     0x40D Air resistance → +0x57C  (GET: 0 → -0.001)
//     0x40E Lifetime (s) → +0x598
//     0x40F Speed min → +0x59C
//     0x410 Speed max → +0x5A0
//     0x411 Emission max deg → +0x5A8
//     0x412 Timer (s) → +0xE34   (NOT +0x570)
//     0x413 Scale init → +0x5AC
//     0x414 Scale final → +0x5B0
//     0x415 Frequency → +0x5B4
//     0x416 Opacity 0–1 → +0x5BC  (clamped [0,1] in GET)
//     0x417 Emission min deg → +0x5A4
//   Gravity is an absolute XYZ vector on the Particles dialog (not world
//   G-Force). Earth Y ≈ -9.8; X/Z are wind. Remix gravityForce = Y × 100
//   (m/s² → cm/s²), sign as stored. Air stays +0x57C.
//   Properties DIALOGEX 104 (resource title "Properties") is shown as
//   "Particles v1.16" (#32770). Live hook: owner 3DRADCLASS, Gravity
//   edits 0x40A/0x40B/0x40C at DLU y=218, Air 0x40D at y=236, Scale
//   0x413/0x414 at y=252. Detect by class + those IDs + static
//   "Gravity (abs. vector)", not the window title. DialogBoxParam is
//   chained; subclass injects Collide checkbox id 0x7E80 at DLU 17,268
//   (below Gravity/Air). Bind Collide + Advanced + color from dialog
//   lParam / host+plugin pointers stashed at WM_INITDIALOG (vrePHost /
//   vrePPlug). Never live LB_GETCURSEL, never title, never 5-digit list
//   prefix, never slot as identity. Identity = 0x50544C0000000000 |
//   (host32 XOR rotl(plugin32,1)). Mesh/mat hash = identity^1/^2 mixed
//   with albedo path and sheet rows/cols/fps. Ini `[Particles_<hex>]`
//   keyed by Hash=; Slot= is unused.
//   Persist `{stem}.ini` next to the loaded .3dr (any basename — File Open,
//   recent, CreateFileW/A / FindFirstFile on *.3dr, caption
//   `{stem} - 3D Rad v7.22 (hw) - www.3DRad.com`). Switching A→B drops A's
//   Particles/Remix maps and loads B.ini. Migrate rtx-particles.ini once if
//   `{stem}.ini` is missing. Sections are `[Particles_%05d]` keyed by list
//   ObjectId (Hash= is a field, not the section name). Delete of any particle
//   (first / middle / last) remaps remaining IDs ASC then prunes every
//   `[Particles_%05d]` (and g_ext / g_collide / Hash maps) whose oid has no
//   live plugin — the vacated highest slot must not remain for the next add.
//   Shift+Q clone remaps DESC and inherits params onto the vacated oid; do
//   not prune the last section on insert. write_ini only the open
//   `{scene}\{stem}.ini`. Section `%05d%s` e.g. [00032Particles] — slot
//   + plugin+0x424 name. compiler_inject copies `{stem}.ini` (and
//   `<exename>.ini` if different) into the compiled output. DrawInstance
//   loops every Particles plugin like PointLights.
//   Remix ParticleSystemEXT sType 25; hideEmitter; no CreateLight.
//   Emitter: quat +0x488 as world XYZ columns, then one RH Rx(+90°) about
//   local X (x′=x, y′=−z, z′=y). Remix +Z = pitched axis.
//   Lifetime GET 0x40E → +0x598 (TTL). Timer GET 0x412 → +0xE34 is the
//   emit window. spawnBurstDuration is 0 (screenshot-era: each frame
//   DrawInstance restarts USD's 1s default so fountains stay continuous).
//   spawnRate stays at Frequency unless Advanced saved an override.
//   Speed GET 0x40F/0x410 → +0x59C/+0x5A0 → initialVelocityFromNormal
//   = avg × 0.5 × 100 cm/s. Gravity +0x570/574/578 Y×100 unless that
//   slot's Advanced GravOverride=1. Unique instance: CreateMesh/CreateMaterial
//   hash = identity^1 / identity^2 from host pointer mixed with plugin and
//   host slot (0x50544C0000000000 | (host32 XOR rotl(plugin32,1) XOR
//   (slot+1)*0x9E3779B9)). Shared plugin after Duplicate Object stays unique
//   via host+slot. Clone rescan invalidates cache when host count or *host
//   plugin ptr changes. Color/Advanced/Collide/sprite picker bind the
//   stashed vrePSlot from WM_INITDIALOG (handle, else list 5-digit prefix
//   at open only). Later clicks never live LB_GETCURSEL.
//   ObjectPickingValue is the low 32 bits of that identity (not list title
//   digits). ParticleSystemEXT is InstanceInfo.pNext; picking on ps.pNext.
//   Category PARTICLE_EMITTER, never HIDDEN. Color/Advanced/Collide bind
//   the stashed 5-digit host slot, not the live list highlight; ini stays
//   `[00032Particles]` with Slot= and Hash=. Apply/Collide never DestroyMesh
//   of other emitters. Sprite albedo is the plugin texture, or a packed
//   sheet from data\<leaf>\0001.dds… plus data\<leaf>.dds (3D Rad sequence).
//   Default animation folder: objects\Particles\data\animation\ (0001.dds,
//   0002.dds) with parent animation.dds as first frame — sequential 256²
//   DXT5, packed 1×N left-to-right. Advanced "Pick frames..." multi-select
//   (flame_0001…) generates a NEW CreateMaterial per host identity
//   (hash mixes identity + sheet path + slot); Animation=/AnimationFiles=/SheetFile=
//   persist on that slot only. Color picker ctx is identity+field so 00033
//   cannot retint 00032. Apply/OK/EN_CHANGE/picker WritePrivateProfile that
//   section.
//   Bridge reads: gravity vec3 +0x570, air +0x57C, opacity +0x5BC,
//   scale +0x580/+0x584 (14/20 cm), RGB +0x588/+0x594, lifetime +0x598
//   (GET 0x40E), emit +0x5A4/+0x5A8, speed +0x59C/+0x5A0 (GET 0x40F/0x410)
//   × 0.5 × 100 cm/s → initialVelocityFromNormal, freq +0x5B4.
//   +0x5B8 runtime freq copy, +0x5C0 Visible in reflections
//   +0x5C4 Render mode (1=Burn-out), +0x5CC texture leaf (data\*.dds)
// Idle selected dump: shown=1, live count 0, buffers NULL, pos matches
// dialog Location. Live sim: live count / +0x4DC / +0xE44 / +0xDF4+0xDF8
// change; Frequency/Lifetime/colors/pos stay at dialog values.
// Remix 1.5.2 has no CreateParticleSystem. Publish DrawInstance +
// InstanceInfoParticleSystemEXT (sType 24) after scene camera exists.
// hideEmitter=true on a tiny CreateMesh quad + CreateMaterial albedo=
// particle_default.dds. spawnRate=+0x5B4, TTL=+0x5A0, cone=+0x5A8,
// velocity=avg(speed min/max), maxSpeed=speed max, colors+opacity,
// size=10*scale, Burn-out → motion trail + higher emissive. Cap 16.
// DestroyMesh/DestroyMaterial BEFORE device Reset (imgui pattern).
// Never CreateLight. After Reset only null handles.
//
// Capture: Fog v1.03 (3DRad_res\objects\Fog\object.dll). Host tables same
// as PointLight / Particles. Plugin at host+0. Identity = 0x464F470000000000
// | plugin32 (not host slot). +0x04 shown, +0x08 Enable at start, +0x0C
// Active, +0x424 name. Event bus (Fog.dll rdata): S-VAL 01–03 RGB 0–1,
// S-VAL 10 Fog start, S-VAL 11 Fog end. DLL image has default end=100.0 at
// RVA 0x123CC; instance RGB/start/end live on the plugin heap (scan prefers
// 1.0/100.0, then RGB 3 floats or COLORREF before start). Each frame: D3D9
// FOGENABLE/FOGCOLOR/FOGSTART/FOGEND (linear table fog) plus Remix
// SetConfigVariable rtx.volumetrics.enableFogRemap / enableFogColorRemap /
// enableFogMaxDistanceRemap / fogRemapMaxDistanceMinMeters / MaxMeters /
// transmittanceColor / rtx.enableFog. Does not set rtx.maxFogDistance or
// rewrite rtx.conf (keep rtx.sceneScale). Compiler skips.
// Log: Fog live n=… start=… end=… color=…
//
// Compiled player mouse: do not ClipCursor (that blocked Cam1StPerson wrap
// and could clip the debug console, same PID). Click-to-capture on the
// Fullscreen/Rendering Window only when live, client >= 64×64, and
// foreground is not ConsoleWindowClass / #32770. Never capture the 10×10
// picker Fullscreen Window. Wrap cursor at client edges, ShowCursor(FALSE).
// Editor HUD/orbit unchanged.
//
// Compiled first boot (scary.exe): NvRemixBridge AV on the first live frame
// after InitializeLibrary + SetLight + particle CreateMaterial/EXT. Sentinel
// rtx_comp_boot.ok next to the exe + --rtx-comp-warmed. Write sentinel at
// first live, InitializeLibrary, relaunch once, ExitProcess. Compiler skips.
// Editor does not auto-relaunch on first camera / project load (that looked
// like a compile-and-quit). Compiled player still uses rtx_comp_boot.ok.
//
// Editor Shaders menu (3DRadRTXFrame caption, 3DRADCLASS wrap): Shaders →
// Clear Shaders. Pass 1: in-process Win32 delete of %LOCALAPPDATA%\NVIDIA\
// GLCache and DXCache, plus <install>\*.dxvk-cache and .trex\*.dxvk-cache.
// Pass 2: hidden rundll32 of this d3d9.dll export RtxCompClearShadersWipe
// (CREATE_NO_WINDOW | DETACHED). Helper waits for 3DRad.exe PID via
// OpenProcess/WaitForSingleObject (no cmd.exe / findstr), retries wipe on
// sharing violations, then relaunches 3DRad.exe without --rtx-comp-warmed.
// DllMain returns immediately for rundll32.exe (not a compiled player).
// Env RTX_COMP_CACHE_WIPED=1. Clear Shaders is the only editor ExitProcess.
// Does not wipe D3DSCache, USD .glslfx, or d3d9_dxvk.dll. Native Shaders
// popup items still dispatch to 3DRADCLASS; Clear Shaders is appended if
// missing. Editor does not start Display Options CBT.
//
// Editor autoload INI (3DRad.exe only — not compiler, not compiled player):
// 3D Rad opens lastProject.txt (3DRad_res\system\) / windowed.ini recent /
// a command-line .3dr before File-Open. CreateFile of that .3dr runs in
// WinMain; proxy hooks are installed at EXE entry (editor_frame::start) so
// the same remember+apply path as GetOpenFileName runs. If CreateFile is
// missed, poll() still binds from lastProject.txt, caption stem under
// 3DRad_res\projects\<stem>.3dr, then cwd/exe. Autoload same stem+folder is
// a no-op. Explicit File-Open of the SAME project reloads from disk: drop
// in-memory maps, load {stem}.ini, never write empty in between. compact
// skips while live plugin count is 0. skip empty overwrite always applies
// when live==0 or maps empty and disk still has [Particles_%05d] even if
// loaded_ok from the first load. File-Open (explicit) wins over a later
// autoload of A while caption lags.
// Project switch: on_before does NOT write (empty live maps must never flush
// onto A.ini or B.ini). Stem changes, then drop_overrides_and_load reads
// {newstem}.ini only. write_ini owns only that stem's file. If live/maps are
// empty and the target INI still has [Particles_%05d] sections, skip the
// write (log "skip empty overwrite"). Empty persist only after a successful
// load of THIS stem and a real user delete of all particles. Prune vacated
// ObjectIds only when live plugins overlap this stem's maps (host ready).
// One-time {stem}.ini.bak in the project folder, not every frame.
// Autoload is not compiler-quit and does not set --rtx-comp-warmed.
//
// Compiled player project-local tree (this exe dir, never C:\3DRadRTX except
// a shared proxy d3d9.dll if that is how they launched):
//   data_root = GetModuleFileName(exe) directory. remix-comp-proxy.ini,
//   rtx.conf / user.conf, .trex, d3d9_remix.dll, bridge.conf,
//   rtx_comp\console.log, particle sheets, Particles animation, and
//   3DRad_res\projects\{stem}.ini all resolve here. Stem is discovered
//   like the editor (caption / CreateFile / lastProject STEM / lone
//   .3dr/.ini in THAT local projects folder) — never a hardcoded
//   scary/scary2/scary_YYYYMMDD name, never <exe>\{exeleaf}.ini, never
//   the editor install projects file. persist + skip empty overwrite
//   only that local INI. Editor stays on C:\3DRadRTX. Compiler skip Remix.
// Bind [Particles_%05d] by host slot ObjectId — editor Hash= is a plugin
// pointer and will not match.
// After Display Options OK, first live scene LookAtLH / Frame phase=live:
// compiled player ShowWindow(SW_SHOW/SW_RESTORE)+SetWindowPos(HWND_TOP) the
// D3D hwnd even if it is 10x10 / iconic / off the taskbar, then SetFocus /
// SetActiveWindow (never skip because GetForegroundWindow already matches).
// AllocConsole is WS_EX_TOOLWINDOW + SHOWNOACTIVATE so it is not the only
// taskbar button. If Fullscreen Window stays tiny, restore its owner/parent
// (not #32770 picker, not the console).
//
// setup_lighting() SetLight captured D3DLIGHT9 (cap 8) when count>0,
// else fallback directional + ambient so below-Ultra is not black.
// No API distant — SetLight + Remix sky already stacked Distant.
// Re-submit on every FFP engage so DirtyLights fires after HUD disengage.
// Do not SetConfigVariable for graphicsPreset / fallbackLightMode.
// Launcher / Display Options: rtx.conf is defaults, user.conf overlays
// (user wins — Remix runtime order). Editor root C:\3DRadRTX, compiled
// <exe>. Never .trex-only. rtx.graphicsPreset Ultra=0 High=1 Medium=2
// Low=3 Custom=4 is a separate key from rtx.dlssPreset.
//
// Note when submitting D3DLIGHT9: zero the struct and set every field;
// Range > 0 and Attenuation0 = 1.0, or Remix's radius derivation can divide
// by zero. Directional lights ignore position (world-space, not camera-relative).

// ---------------------------------------------------------------------------
// Remix gives us no crash diagnostics
// ---------------------------------------------------------------------------
// All logs under rtx-remix/logs are 0 bytes, and remix-dxvk.log ends cleanly at
// D3D9DeviceEx::ResetSwapChain with nothing after it — never flushed past the
// crash. DXVK_LOG_LEVEL=info added nothing. Dumps do appear as
// .trex\NvRemixBridge.exe_<timestamp>.dmp. Do not expect the runtime to explain
// itself; bisect instead.

// ---------------------------------------------------------------------------
// Build dependency — d3dx9.lib
// ---------------------------------------------------------------------------
// deps\dxsdk\Lib\x86\d3dx9.lib comes from the OFFICIAL Microsoft NuGet package
// Microsoft.DXSDK.D3DX 9.29.952.8 (Microsoft's redistribution of the June 2010
// DirectX SDK D3DX libraries, ~20 MB rather than the 572 MB SDK installer):
//   https://www.nuget.org/api/v2/package/Microsoft.DXSDK.D3DX/9.29.952.8
// DO NOT regenerate it from a hand-written .def. Doing so produced import
// descriptors naming the wrong DLL, so the loader looked for D3DX exports in
// OUR proxy and the game died at startup with
//   "The procedure entry point D3DXMatrixRotationYawPitchRoll@16 could not be
//    found in the dynamic link library C:\3DRadRTX\d3d9.dll"
// The official lib attributes its imports to d3dx9_43.dll, which is what fixed
// it. Verify with: dumpbin /linkermember:1 d3dx9.lib (expect _D3DXMatrix*@NN),
// and check the built proxy has no self-import of d3d9.dll.

// ---------------------------------------------------------------------------
// Window / proxy integration
// ---------------------------------------------------------------------------
// RUNTIME-CONFIRMED: the class name is "3DRADCLASS" (not "3Impact"), with a
// separate "ChildClass" viewport window. Set WINDOW_CLASS_NAME to 3DRADCLASS.
//
// Editor chrome (3DRad.exe only): after 3DRADCLASS is visible and CreateDevice
// has used that same hwnd, d3d9.dll parents it under a new top-level
// "3DRadRTXFrame" with a dark custom caption (min / max / close). Project /
// Object / Edit / Help are the original GetMenu(3DRADCLASS) bar (UTF-16
// "&Project" / "Object" / "Edit" / "&Help" in 3DRad.exe). They vanish with
// WS_CAPTION, so they are painted on the caption and TrackPopupMenu the same
// HMENU submenus (WM_COMMAND back to 3DRADCLASS). WH_GETMESSAGE rewrites
// wrapper key MSG.hwnd to 3DRADCLASS so MFC TranslateAccelerator still sees
// Ctrl+S etc.; Alt+P/O/E/H opens the caption menus. On wrap WM_SIZE, pin
// 3DRADCLASS below the caption and stretch ChildClass (keep left-panel width)
// so maximize/drag fills like stock 3D Rad. After ChildClass is sized, a
// 150ms debounce (kRescaleTimer) PostMessages WM_SIZE SIZE_MAXIMIZED|RESTORED
// to ChildClass with the inner client (3D pane, not caption/list chrome) so
// 3D Rad Reset/SetViewport matches the new resolution. Never SendMessage
// (deadlock). Proxy does not Reset (no 1s sleep). Layout ticks still skip
// posting when the pane already matches (8px slack) to avoid 970/978 MAIN
// flap; maximize/restore/exit-size-move always schedule the settled pass.
// Then write dll3impact
// mouse viewport 0x100B58B0/B4 + cached RECT 0x100B70A0 so HUD hits (buttons.dds
// atlas quads) match the drawn widgets. buttons.dds WORLD scale S (ui*hud)
// about the ortho origin; OrthoLH is not shrunk (that doubled S). Hits use
// engine mouse space child/S. LINEAR mag-filter on that atlas. Object list
// width is Settings/stock on maximize (ChildClass takes the rest). The left
// pane is a ListBox owner-drawn with 350x16 item*.bmp skins (LoadImageW +
// 1:1 BitBlt); row height is LB_SETITEMHEIGHT / WM_MEASUREITEM. The ListBox
// subclass DefSubclassProc on WM_PAINT / WM_DRAWITEM first so 3Impact BitBlts
// the C-suffix skin; we then sample that strip (local BitBlt-to-DIB) and
// overpaint gradients + GDI checks + text in the same paint. One frame of
// stock skins may flash; working checks over flicker-free. Parent DRAWITEM
// restyles after DefSubclassProc. itemSelectedg.bmp is the yellow/tan
// group-linked member row when a Group is selected; gold from Group +0x291C
// children is painted after the check sample. LBS_OWNERDRAWFIXED (style
// 0x50000553 / 0x50200553) means the dest DC often never holds itemSelectedg
// pixels — host graph gold still applies after sample. Live 2026-09-15
// (scary.3dr, Group selected): ListBox LB_GETITEMDATA is the host slot;
// cursel row 13 data=7 is objects\Group\object.dll; Group+0x291C children
// [10,11,9,12,6] = SkinMesh, RigidBody, Character, Joint, Wheel. Gold those
// rows. Host tables 3DRad.exe 0x450460 / 0x454468 / 0x44AE58 (same as
// PointLight). +0x2924/+0x2928 are null in this project — do not treat 0 as
// slot 0. Check state is C-suffix skin / baked tick on the blit strip, not
// plugin+0x04 as the only source. Do not default-all-checked. Hit strip ~36px
// remapped to x=8 and forwarded to the original ListBox WndProc — except the
// ODS_SELECTED / cursel row (itemSelected.bmp has no check; remapping x=8
// there would toggle hide on the selected object). Clicks on the selected
// row stay stock selection/drag. Owner-draw: if ODS_SELECTED, do not draw
// the GDI checkbox; unselected and gold group-member rows still get checks.
// itemSelected1C.bmp is not the current-selection skin.
// Never a process-wide GDI32 BitBlt hook.
// ODS_SELECTED (the Group / current object row) stays crimson. Settings on the caption: Video
// SetViewport inject; UI font/scale (system\\ui + buttons.dds) and object
// list width/row/checkbox plus circular ColorSelected/Unselected/Group/Hidden
// wheels (custom GDI hue, not ChooseColor). Slider ticks and color drags
// apply live (no reboot); Apply/OK/release write remix-comp-proxy.ini [Video]
// MatchWindow/Width/Height/Scale and [UI]
// Scale/HudScale/FontSize/FontName/ListWidth/RowHeight/CheckSize/
// ShowObjectIds (1 = 00032Particles prefix, 0 = name only on object rows)/
// ColorSelected0/1 ColorUnselected0/1 ColorGroup0/1 ColorHidden0/1. Reload
// after config load; re-apply at wrap. Reset on both dialogs restores
// defaults and saves (no D3D Reset, editor stays enabled).
// Do not wrap the #32770 picker,
// 3DRad_compiler.exe, or compiled players. Do not replace the CreateDevice
// hwnd or GWLP_WNDPROC of 3DRADCLASS (Remix hooks CreateWindow;
// SetWindowSubclass chains). Strip WS_CAPTION/WS_THICKFRAME on the editor frame
// and keep its client size so the 3D pane is not smashed to the leftover
// 827×620 swapchain size.
//
// Startup order, from console.log — this matters for initialisation:
//   1. the window exists and is found          <- well before any D3D9 call
//   2. Direct3DCreate9
//   3. CreateDevice  hwnd=... 723x542 windowed=1   then Reset, on the SAME device
//   4. CreateDevice AGAIN (same hwnd, same device pointer), then Reset again
// So the device is created twice and reset twice at 723x542. Anything installed
// or initialised per-CreateDevice must be idempotent (see the redirect()
// recursion trap above).
//
// "Failed to initialize the remixApi - Code: 11" (REMIXAPI_ERROR_CODE_NOT
// _INITIALIZED) is caused by this ordering: remix_api::initialize() runs from
// comp::main() when the window is found, which is step 1 — before a D3D9 device
// exists. It must be deferred to after CreateDevice, and made idempotent across
// the two calls. STILL OPEN.
//
// The editor draws its 3D viewport into an OFFSCREEN RENDER TARGET, not the
// swap chain: GetRenderTarget(0) is D3DUSAGE_RENDERTARGET and is NOT the
// back buffer. Historically that RT matched CreateDevice (~723×542). The
// 3D pane can also be a *different* size (log: 1332×614 vs swapchain
// 827×620). MAIN is that 3D RT, not the window blit. Cubemap/preview
// targets are other sizes (especially squares). Compiled player MAIN is
// the swapchain; leftover client rects (1519×824 on 1920×1080) are not.
//
// The install already runs bridge-remix: game-dir d3d9.dll (863856 bytes) is
// the 32-bit bridge client, .trex\ holds NvRemixBridge.exe and the 190 MB
// Remix runtime d3d9.dll. Installing this proxy means renaming the bridge
// client to d3d9_remix.dll and letting the proxy load it via [Remix] DLLName.
//
// Albedo: every technique binds the diffuse map as Texture[0] and sets
// PixelShader = NULL already, so AlbedoStage=0 and the FFP pixel path is
// already what the engine expects.
