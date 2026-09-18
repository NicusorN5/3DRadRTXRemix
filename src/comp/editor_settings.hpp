#pragma once

namespace comp::editor_settings
{
	constexpr UINT kCmdVideo = 0x7F10;
	constexpr UINT kCmdUI = 0x7F11;

	// Editor-only Settings popup (Video... / UI...). Unowned TOPMOST so it
	// sits above 3DRADCLASS; the editor stays enabled (modeless).
	HMENU popup_menu();

	bool handle_menu_command(UINT id, HWND owner);
	bool filter_message(MSG* msg);
	bool is_settings_hwnd(HWND hwnd);

	void reload();
	void on_editor_wrapped(HWND editor);
	void install_hooks();

	// SetViewport inject for the editor 3D / MAIN pass. Rewrites `vp` in place.
	// Does not Reset. Returns true when the viewport was changed.
	bool inject_editor_viewport(D3DVIEWPORT9& vp);

	// D3DXMatrixOrthoLH is left at engine w/h. buttons.dds quads scale in
	// WORLD (UI x HUD) about the ortho origin; hits are child/S.
	void adjust_ortho(FLOAT& w, FLOAT& h);

	// After ChildClass is sized: write engine mouse/hit viewport (dll3impact
	// 0x100B58B0/B4 + cached RECT). Mouse w/h follow the same WORLD scale as
	// buttons.dds (child/S). PostMessage WM_SIZE on wrap resize only (no SendMessage).
	void sync_hud_hits(HWND viewport);

	void note_texture(DWORD stage, IDirect3DBaseTexture9* tex);
	bool begin_ui_draw(IDirect3DDevice9* dev);
	void end_ui_draw(IDirect3DDevice9* dev, bool scaled);

	bool measure_object_list(MEASUREITEMSTRUCT* mis);
	// After the engine BitBlts 16px item*.bmp skins (DefSubclassProc first),
	// sample the strip (C-suffix / baked tick) and overpaint a scalable
	// gradient + GDI checkbox + text. ODS_SELECTED (crimson) draws no
	// checkbox — stock itemSelected.bmp has no check; itemSelected1C is
	// not the current-selection skin. Gold from Group +0x291C children is
	// applied after that sample; those rows keep GDI checks. Never
	// default-all-checked. Never a process-wide BitBlt hook. List colors
	// are Settings-UI circular pickers as Color*0/1.
	bool restyle_object_list_item(const DRAWITEMSTRUCT* dis);

	// LBN_SELCHANGE on the object list: rebuild the Group child set and
	// invalidate every row so member gold tints update.
	void on_object_list_command(HWND ctl, UINT code);

	// WH_GETMESSAGE: remap a wide checkbox hit to stock x=8 so 3Impact's
	// original ListBox proc still toggles shown. Never remap the
	// ODS_SELECTED / cursel row (that would hide the selected object).
	// Does not eat the message.
	bool rewrite_object_list_click(MSG* msg);

	HWND object_list_hwnd();
	int object_list_cursel();
	bool object_list_item_text(int index, char* buf, int cap);

	using rgb_pick_fn = void(*)(void* ctx, float r, float g, float b);
	void open_rgb_picker(HWND owner, const char* title, float r, float g, float b,
		void* ctx, rgb_pick_fn cb);

	bool match_window();
	int video_width();
	int video_height();
	float ui_scale();
	float hud_scale();
}
