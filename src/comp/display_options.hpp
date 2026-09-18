#pragma once

namespace comp::display_options
{
	// Compiled-player only (scary.exe / 3drad_player). Never the editor
	// (3DRad.exe uses the launch dialog) or compiler. CBT + CALLWNDPROCRET
	// match DXUT "Display Options": title + IDs 1036/1038/1039/1040/1041/1072
	// as a group. Other #32770 dialogs (Particles, Properties) are ignored.
	// Remix graphics sit in a scroll host; OK/Cancel stay pinned outside it.
	void start();

	bool is_display_options(HWND hwnd);
	bool picker_visible();
	bool is_picker_hwnd(HWND hwnd);
	bool is_real_play_hwnd(HWND hwnd);

	// True after Display Options OK/Cancel/destroy. The post-OK CreateDevice
	// is still Rendering Window 588x441 while the dialog may exist — that is
	// the 3Impact device that Reset()s to the play size. Do not wait for a
	// later Fullscreen Window CreateDevice; DXUT never issues one.
	bool remix_load_allowed();
	void note_picker_finished();
}
