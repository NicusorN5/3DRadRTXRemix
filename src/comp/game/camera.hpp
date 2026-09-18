#pragma once

namespace comp::game::camera
{
	/*
	 * Camera recovery for the 3Impact engine.
	 *
	 * 3Impact hands its vertex shaders one matrix per draw — declared as
	 * `float4x4 mxViewProj : VIEWPROJECTION`, but actually holding
	 * World * View * Projection for rigid meshes, since nothing else transforms
	 * position. Remix needs World, View and Projection separately, so the proxy has
	 * to reach the operands before the engine multiplies them together.
	 *
	 * The engine builds every one of those matrices through a single
	 * D3DXMatrixMultiplyTranspose(out, world, viewProjection) call, which is the one
	 * place the camera still exists on its own.
	 */

	// Redirects the engine's D3DXMatrixMultiplyTranspose import. Must run after
	// dll3impact.dll is loaded and its imports are resolved.
	bool init();

	// Back buffer / CreateDevice / Reset size. Compiled-player MAIN is this
	// swapchain size. The editor 3D view is a *separate* offscreen RT (log:
	// 1332×614) while the window swapchain stays ~827×620 — locking MAIN to
	// the backbuffer is wrong for the editor. Scene sizes are NOT registered
	// during picker / windowed-boot / loading. After live, a leftover
	// windowed/client rect (e.g. 1519×824 on a 1920×1080 buffer) is not a
	// second scene size. First live scene VP records the chosen size.
	void set_viewport(UINT width, UINT height);
	void note_surface_size(UINT width, UINT height);
	// Editor only: 3D pane RT from SetRenderTarget. Not the swapchain blit.
	void note_editor_scene_rt(UINT width, UINT height);
	void on_viewport(UINT width, UINT height);
	bool size_is_scene_viewport(UINT width, UINT height);
	UINT viewport_width();
	UINT viewport_height();

	// Compiled player: #32770 picker → windowed-boot (small Windowed
	// CreateDevice) → loading (fullscreen Reset / exclusive / Fullscreen
	// Window at the chosen size) → live (first scene VP, same frame).
	// No FFP, no scene size, no camera publish until live. Editor
	// (3DRADCLASS) is live immediately.
	void on_device_created(HWND hwnd, UINT width, UINT height, BOOL windowed);
	void on_device_reset(IDirect3DDevice9* dev, UINT width, UINT height,
		HWND hwnd, BOOL windowed);
	bool scene_conversion_allowed();
	bool is_compiled_player();
	// InitializeLibrary only after a scene camera exists. Editor CreateDevice
	// then Reset (and compiled picker CreateDevice #2) AVed NvRemixBridge
	// when the API was already exposed on the dying device.
	// Editor does not auto-relaunch (project load publishes the first camera
	// and was exiting like a compile). Compiled player keeps rtx_comp_boot.ok
	// + --rtx-comp-warmed.
	bool remix_api_init_allowed();
	const char* init_phase_name();

	// Pregame dummy inject was removed: XYZRHW DrawPrimitiveUP + toggling
	// rtx.enableRaytracing crashed NvRemixBridge (0xc0000005) on the compiled
	// player's second CreateDevice. Pregame still skips scene size / FFP /
	// camera and never writes identity View/P. This is a no-op.
	void mark_pregame_swapchain_ui(IDirect3DDevice9* dev);

	// Pregame draw: log GetViewport / GetBackBuffer / GetRenderTarget /
	// client rect vs HUD vertex AABB so 1080×810 leftover vs 1920×1080 is visible.
	void note_pregame_draw(IDirect3DDevice9* dev);

	// Pin leftover compiled-player viewports to the tracked backbuffer.
	// Boot leftovers (1080×810 on 1920×1080) and live windowed/client rects
	// (1519×824) are rewritten so a second SetViewport cannot rescale the
	// camera/HUD. Editor 3D RT (1332×614) must NOT be pinned to the 827
	// swapchain, and 827 must NOT be pinned to 1332. Cubemap squares pass
	// through. Returns true when `vp` was rewritten.
	// One log line per distinct size per frame.
	bool adjust_pregame_viewport(IDirect3DDevice9* dev, D3DVIEWPORT9& viewport);

	// Recovers the world transform for the draw that is about to be issued, from the
	// WVP the engine left in the vertex shader constants. Returns false when the
	// result is not a usable world matrix, in which case the draw must keep its
	// shaders instead of going through the FFP.
	// Camera and world transform for one draw, identified by the mxViewProj constant
	// it was given. Fails when the draw's constant was not one the engine built
	// through D3DXMatrixMultiplyTranspose, in which case it keeps its own shaders.
	bool resolve_draw(IDirect3DVertexShader9* shader, const float* vs_constants,
		D3DMATRIX& view, D3DMATRIX& proj, D3DMATRIX& world);

	// Remix treats a *camera identity* jump as a cut. Within a Present the shown
	// handle stays put; View and C still refresh. Detection walks ALL CamChase /
	// Cam1StPerson / Camera plugins each frame (plugin-pointer identity, ObjectId
	// like Particles — never slot index). Pick uses engine +0x124 (0=rendering).
	// Several rendering: the one whose FOV matches the main VP. No plugin
	// rendering: editor list-cam (shown 60°). Not sticky-forever, not
	// draws>=160, not sim-start/stop. Never identity View/P after Present.
	void on_begin_scene();
	void on_present();
	void note_non_scene_skip();
	bool remix_camera(D3DMATRIX& view, D3DMATRIX& proj);
	void on_project_before();
	void on_project_after();
}
