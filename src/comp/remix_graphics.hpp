#pragma once

namespace comp::remix_graphics
{
	// Compiled player: hold LoadLibrary(d3d9_remix.dll) until the first
	// CreateDevice whose hwnd is the real game window (not Display Options /
	// Rendering Window). Same process — no gfx-ok relaunch. Editor loads Remix
	// from the launch dialog. Compiler never reaches this.
	bool compiled_should_defer_remix();
	void on_host_hscroll(HWND host);

	// Compiled: directory of this player EXE, never C:\3DRadRTX.
	// Editor: C:\3DRadRTX. Per tree: user.conf then rtx.conf (rtx wins).
	// .trex copies fill missing keys only — they must not stomp the project conf.
	void load_from_disk();

	// Native combo/checkbox set matching the Remix 1.5 Graphics overlay.
	// Combo CreateWindow height is 22–24px (not 160). Returns pixel height used.
	// Idempotent (refreshes values if already created). Parent may be a scroll host.
	int create_controls(HWND dlg, int x, int y, int width, HFONT font);

	void apply_to_dialog(HWND dlg);
	void set_enabled(HWND dlg, bool on);
	void update_dependent_enables(HWND dlg);

	// Upsert rtx.conf + user.conf in the Remix read locations. Returns false
	// if the dialog has no remix controls. Does not sleep.
	bool write_from_dialog(HWND dlg);

	// FlushFileBuffers on the written conf files. No Sleep — a 1s stall on
	// the D3D / dialog thread AVs NvRemixBridge on compiled OK/relaunch.
	void commit_conf_before_remix();

	HWND controls_root(HWND dlg);
	void bind_controls_host(HWND dlg, HWND host);

	HFONT segoe_ui(int point_size, bool bold, int dpi);
	int window_dpi(HWND hwnd);
	void clamp_to_work_area(HWND hwnd);
}
