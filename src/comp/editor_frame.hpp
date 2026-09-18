#pragma once

namespace comp::editor_frame
{
	// Editor-only (3DRad.exe). Starts a watcher that wraps 3DRADCLASS in a
	// modern dark caption after the editor frame exists. The wrapper owns an
	// 8px resize border (WM_NCHITTEST) and paints Project/Object/Edit/Help in
	// the caption, including when maximized. No-op for compiler / compiled
	// players. Safe to call more than once.
	void start();

	// Remix CreateDevice completed (same hwnd — do not replace it). Lets the
	// wrapper wait until both boot devices exist before wrapping the frame.
	void note_create_device();

	HWND wrapper_hwnd();
	HWND editor_hwnd();
	HWND viewport_hwnd();
	HWND left_panel_hwnd();
	SIZE viewport_client_size();
	int left_panel_width();
	int stock_left_panel_width();
	// Resize the MFC object list to Settings/stock width; ChildClass fills
	// the rest, right-aligned to the 3DRADCLASS client (DeferWindowPos, no
	// gap / right_margin). Maximize keeps that width. No SendMessage into
	// ChildClass. ListBox rows are owner-drawn gradients (not LVSIL_SMALL).
	void set_left_panel_width(int w);
	// After a posted ChildClass WM_SIZE, rewrite scaled HUD mouse globals.
	void request_delayed_hit_write();
	// Grow the wrapper so ChildClass client is at least w×h. Left list width
	// unchanged. No D3D Reset. No nested SendMessage WM_SIZE.
	void request_viewport_client_size(int w, int h);
	// Once after wrap: set 3DRadRTXFrame so the 3DRADCLASS client (below the
	// caption) is exactly w×h. Resize-border chrome stays; the outer HWND is
	// larger than Video. Does not run again after the user drags the frame.
	void apply_boot_editor_client_size(int w, int h);
}
