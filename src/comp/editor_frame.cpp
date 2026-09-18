#include "std_include.hpp"
#include "editor_frame.hpp"
#include "editor_settings.hpp"
#include "game/particles.hpp"
#include "project_file.hpp"
#include "shader_cache.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comctl32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

namespace comp::editor_frame
{
	namespace
	{
		constexpr char kWrapperClass[] = "3DRadRTXFrame";
		constexpr char kEditorClass[] = "3DRADCLASS";
		constexpr char kViewportClass[] = "ChildClass";
		constexpr UINT kWrapMsg = WM_APP + 0x71;
		constexpr UINT kOpenMenuMsg = WM_APP + 0x72;
		constexpr UINT kLayoutInnerMsg = WM_APP + 0x73;
		constexpr UINT kSyncHitsMsg = WM_APP + 0x74;
		constexpr UINT_PTR kSyncTimer = 1;
		constexpr UINT_PTR kHitWriteTimer = 2;
		constexpr UINT_PTR kRescaleTimer = 3;
		constexpr UINT kRescaleDebounceMs = 150;
		constexpr int kResizeBorderPx = 8;
		constexpr int kMaxMenus = 8;
		constexpr int kMaxKids = 48;
		constexpr UINT kCmdClearShaders = 0x7F20;
		constexpr UINT kCmdRemixDocs = 0x7F21;
		constexpr wchar_t kRemixDocsUrl[] =
			L"https://docs.omniverse.nvidia.com/kit/docs/rtx_remix/latest/";

		constexpr COLORREF kBg = RGB(0, 0, 0);
		constexpr COLORREF kText = RGB(230, 230, 230);
		constexpr COLORREF kBtnHover = RGB(45, 45, 45);
		constexpr COLORREF kCloseHover = RGB(232, 17, 35);
		constexpr COLORREF kIcon = RGB(230, 230, 230);
		constexpr COLORREF kIconOnClose = RGB(255, 255, 255);

		enum class caption_btn
		{
			none,
			min,
			max,
			close
		};

		struct cap_menu
		{
			wchar_t label[32]{};
			wchar_t mnemonic = 0;
			HMENU popup = nullptr;
			int index = 0;
			RECT rc{};
		};

		struct frame_state
		{
			HWND wrapper = nullptr;
			HWND editor = nullptr;
			int dpi = 96;
			int caption_h = 32;
			int btn_w = 46;
			int border = 8;
			bool can_resize = true;
			bool closing = false;
			bool layout_lock = false;
			bool inner_lock = false;
			bool sizing = false;
			bool inner_posted = false;
			bool wrapped = false;
			caption_btn hover = caption_btn::none;
			caption_btn pressed = caption_btn::none;
			int keep_client_w = 0;
			int keep_client_h = 0;

			HMENU editor_menu = nullptr;
			bool menu_owned = false;
			HACCEL accel = nullptr;
			HHOOK getmsg_hook = nullptr;
			cap_menu menus[kMaxMenus]{};
			int menu_count = 0;
			int menu_hover = -1;
			int menu_open = -1;
			bool inner_metrics = false;
			HWND left_panel = nullptr;
			int left_panel_w = 0;
			int stock_left_panel_w = 0;
			int left_x = 0;
			int pane_gap = 1;
			int view_top = 0;
			int bottom_margin = 0;
			int right_margin = 0;
			int last_inner_w = 0;
			int last_inner_h = 0;
			int last_d3d_vp_w = 0;
			int last_d3d_vp_h = 0;
			int last_rgn_w = -1;
			int last_rgn_h = -1;
			bool hit_posted = false;
			bool editor_active = false;
		};

		static frame_state g_state{};
		static bool g_boot_video_applied = false;
		static std::atomic<long> g_started{ 0 };
		static std::atomic<long> g_create_devices{ 0 };
		static std::atomic<long> g_wrap_result{ 0 };
		static HHOOK g_wnd_hook = nullptr;
		static HWND g_pending_editor = nullptr;
		static std::atomic<long> g_owner_fix_logs{ 0 };

		static void request_editor_close(frame_state& st);

		using GetOpenFileNameA_fn = BOOL(WINAPI*)(LPOPENFILENAMEA);
		using GetOpenFileNameW_fn = BOOL(WINAPI*)(LPOPENFILENAMEW);
		using GetSaveFileNameA_fn = BOOL(WINAPI*)(LPOPENFILENAMEA);
		using GetSaveFileNameW_fn = BOOL(WINAPI*)(LPOPENFILENAMEW);
		using DialogBoxParamA_fn = INT_PTR(WINAPI*)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
		using DialogBoxParamW_fn = INT_PTR(WINAPI*)(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM);
		using DialogBoxIndirectParamA_fn = INT_PTR(WINAPI*)(HINSTANCE, LPCDLGTEMPLATEA, HWND, DLGPROC, LPARAM);
		using DialogBoxIndirectParamW_fn = INT_PTR(WINAPI*)(HINSTANCE, LPCDLGTEMPLATEW, HWND, DLGPROC, LPARAM);
		using CreateDialogParamA_fn = HWND(WINAPI*)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
		using CreateDialogParamW_fn = HWND(WINAPI*)(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM);
		using CreateDialogIndirectParamA_fn = HWND(WINAPI*)(HINSTANCE, LPCDLGTEMPLATEA, HWND, DLGPROC, LPARAM);
		using CreateDialogIndirectParamW_fn = HWND(WINAPI*)(HINSTANCE, LPCDLGTEMPLATEW, HWND, DLGPROC, LPARAM);
		using GetActiveWindow_fn = HWND(WINAPI*)();

		static GetOpenFileNameA_fn GetOpenFileNameA_og = nullptr;
		static GetOpenFileNameW_fn GetOpenFileNameW_og = nullptr;
		static GetSaveFileNameA_fn GetSaveFileNameA_og = nullptr;
		static GetSaveFileNameW_fn GetSaveFileNameW_og = nullptr;
		static DialogBoxParamA_fn DialogBoxParamA_og = nullptr;
		static DialogBoxParamW_fn DialogBoxParamW_og = nullptr;
		static DialogBoxIndirectParamA_fn DialogBoxIndirectParamA_og = nullptr;
		static DialogBoxIndirectParamW_fn DialogBoxIndirectParamW_og = nullptr;
		static CreateDialogParamA_fn CreateDialogParamA_og = nullptr;
		static CreateDialogParamW_fn CreateDialogParamW_og = nullptr;
		static CreateDialogIndirectParamA_fn CreateDialogIndirectParamA_og = nullptr;
		static CreateDialogIndirectParamW_fn CreateDialogIndirectParamW_og = nullptr;
		static GetActiveWindow_fn GetActiveWindow_og = nullptr;

		static int query_dpi(HWND hwnd)
		{
			using get_dpi_fn = UINT(WINAPI*)(HWND);
			static get_dpi_fn fn = reinterpret_cast<get_dpi_fn>(
				GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
			if (fn && hwnd) {
				const UINT dpi = fn(hwnd);
				if (dpi) {
					return static_cast<int>(dpi);
				}
			}
			HDC hdc = GetDC(hwnd ? hwnd : HWND_DESKTOP);
			const int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
			if (hdc) {
				ReleaseDC(hwnd ? hwnd : HWND_DESKTOP, hdc);
			}
			return dpi > 0 ? dpi : 96;
		}

		static int scale_px(int px, int dpi)
		{
			return MulDiv(px, dpi, 96);
		}

		static int i_max(int a, int b)
		{
			return a > b ? a : b;
		}

		static int i_min(int a, int b)
		{
			return a < b ? a : b;
		}

		static int i_abs(int v)
		{
			return v < 0 ? -v : v;
		}

		static bool class_is_object_list(const char* cls)
		{
			if (!cls || !cls[0]) {
				return false;
			}
			return std::strcmp(cls, "SysListView32") == 0 ||
				std::strcmp(cls, "SysTreeView32") == 0 ||
				_stricmp(cls, "ListBox") == 0;
		}

		static BOOL CALLBACK enum_list_child(HWND hwnd, LPARAM lp)
		{
			char cls[80]{};
			GetClassNameA(hwnd, cls, sizeof(cls));
			if (class_is_object_list(cls))
			{
				*reinterpret_cast<HWND*>(lp) = hwnd;
				return FALSE;
			}
			return TRUE;
		}

		static HWND list_in_panel(HWND panel)
		{
			if (!panel || !IsWindow(panel)) {
				return nullptr;
			}
			char cls[80]{};
			GetClassNameA(panel, cls, sizeof(cls));
			if (class_is_object_list(cls)) {
				return panel;
			}
			HWND found = nullptr;
			EnumChildWindows(panel, enum_list_child, reinterpret_cast<LPARAM>(&found));
			return found;
		}

		struct scoped_flag
		{
			bool& flag;
			explicit scoped_flag(bool& f) : flag(f) { flag = true; }
			~scoped_flag() { flag = false; }
			scoped_flag(const scoped_flag&) = delete;
			scoped_flag& operator=(const scoped_flag&) = delete;
		};

		static int resize_border_px(const frame_state& st, HWND hwnd)
		{
			if (!st.can_resize || !hwnd || IsZoomed(hwnd) || IsIconic(hwnd)) {
				return 0;
			}
			return st.border > 0 ? st.border : kResizeBorderPx;
		}

		static void refresh_metrics(frame_state& st, HWND hwnd)
		{
			st.dpi = query_dpi(hwnd);
			st.caption_h = scale_px(32, st.dpi);
			if (st.caption_h < 24) {
				st.caption_h = 24;
			}
			st.btn_w = scale_px(46, st.dpi);
			const int frame = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
			const int want = scale_px(kResizeBorderPx, st.dpi);
			st.border = i_max(frame, want);
			if (st.border < kResizeBorderPx) {
				st.border = kResizeBorderPx;
			}
		}

		static bool class_is(HWND hwnd, const char* name)
		{
			if (!hwnd || !name) {
				return false;
			}
			char cls[256]{};
			if (!GetClassNameA(hwnd, cls, sizeof(cls))) {
				return false;
			}
			return std::strcmp(cls, name) == 0;
		}

		static std::string describe_hwnd(HWND hwnd)
		{
			if (!hwnd) {
				return "hwnd=0x0";
			}
			char cls[256]{};
			char title[256]{};
			GetClassNameA(hwnd, cls, sizeof(cls));
			GetWindowTextA(hwnd, title, sizeof(title));
			RECT wr{};
			RECT cr{};
			GetWindowRect(hwnd, &wr);
			GetClientRect(hwnd, &cr);
			return std::format(
				"hwnd=0x{:X} class={} title='{}' style=0x{:08X} ex=0x{:08X} vis={} iconic={} "
				"parent=0x{:X} owner=0x{:X} root=0x{:X} win={}x{}@{},{} client={}x{}",
				reinterpret_cast<std::uintptr_t>(hwnd),
				cls[0] ? cls : "?",
				title,
				static_cast<unsigned>(GetWindowLongPtrA(hwnd, GWL_STYLE)),
				static_cast<unsigned>(GetWindowLongPtrA(hwnd, GWL_EXSTYLE)),
				IsWindowVisible(hwnd) ? 1 : 0,
				IsIconic(hwnd) ? 1 : 0,
				reinterpret_cast<std::uintptr_t>(GetParent(hwnd)),
				reinterpret_cast<std::uintptr_t>(GetWindow(hwnd, GW_OWNER)),
				reinterpret_cast<std::uintptr_t>(GetAncestor(hwnd, GA_ROOT)),
				wr.right - wr.left, wr.bottom - wr.top, wr.left, wr.top,
				cr.right, cr.bottom);
		}

		static bool is_menu_cmd_msg(UINT msg)
		{
			return msg == WM_COMMAND || msg == WM_ENTERIDLE ||
				msg == WM_INITMENU || msg == WM_INITMENUPOPUP ||
				msg == WM_UNINITMENUPOPUP || msg == WM_MENUSELECT ||
				msg == WM_ENTERMENULOOP || msg == WM_EXITMENULOOP ||
				msg == WM_NEXTMENU || msg == WM_MENUCHAR;
		}

		static bool hwnd_in_wrap_tree(HWND hwnd)
		{
			if (!hwnd || !g_state.wrapped) {
				return false;
			}
			if (hwnd == g_state.wrapper || hwnd == g_state.editor) {
				return true;
			}
			if (g_state.editor && IsWindow(g_state.editor) && IsChild(g_state.editor, hwnd)) {
				return true;
			}
			if (g_state.wrapper && IsWindow(g_state.wrapper) && IsChild(g_state.wrapper, hwnd)) {
				return true;
			}
			return false;
		}

		static HWND walk_off_child(HWND hwnd)
		{
			HWND cur = hwnd;
			for (int i = 0; i < 16 && cur; ++i)
			{
				const LONG_PTR style = GetWindowLongPtrA(cur, GWL_STYLE);
				if ((style & WS_CHILD) == 0) {
					return cur;
				}
				HWND parent = GetParent(cur);
				if (!parent || parent == cur) {
					break;
				}
				cur = parent;
			}
			return cur;
		}

		static HWND preferred_dialog_owner()
		{
			if (g_state.editor && IsWindow(g_state.editor) &&
				(GetWindowLongPtrA(g_state.editor, GWL_STYLE) & WS_CHILD) == 0)
			{
				return g_state.editor;
			}
			if (g_state.wrapper && IsWindow(g_state.wrapper)) {
				return g_state.wrapper;
			}
			return g_state.editor;
		}

		static HWND fix_dialog_owner(HWND owner, const char* api)
		{
			if (!g_state.wrapped || !g_state.wrapper || !g_state.editor) {
				return owner;
			}

			HWND fixed = owner;
			if (!fixed) {
				fixed = preferred_dialog_owner();
			}
			else if (hwnd_in_wrap_tree(fixed))
			{
				HWND top = walk_off_child(fixed);
				if (!top || top == g_state.wrapper ||
					(GetWindowLongPtrA(top, GWL_STYLE) & WS_CHILD) != 0)
				{
					fixed = preferred_dialog_owner();
				}
				else {
					fixed = top;
				}
			}

			if (fixed != owner)
			{
				const long n = g_owner_fix_logs.fetch_add(1, std::memory_order_relaxed);
				if (n < 16)
				{
					shared::common::log("EditorFrame",
						std::format("{} owner {} -> {} (want top-level 3DRADCLASS)",
							api, describe_hwnd(owner), describe_hwnd(fixed)),
						shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				}
			}
			return fixed;
		}

		static HWND WINAPI GetActiveWindow_hk()
		{
			HWND active = GetActiveWindow_og ? GetActiveWindow_og() : nullptr;
			if (g_state.wrapped && g_state.wrapper && g_state.editor &&
				IsWindow(g_state.editor) && active == g_state.wrapper)
			{
				return g_state.editor;
			}
			return active;
		}

		static BOOL WINAPI GetOpenFileNameA_hk(LPOPENFILENAMEA ofn)
		{
			if (ofn) {
				ofn->hwndOwner = fix_dialog_owner(ofn->hwndOwner, "GetOpenFileNameA");
			}
			const BOOL ok = GetOpenFileNameA_og ? GetOpenFileNameA_og(ofn) : FALSE;
			if (ok && ofn && ofn->lpstrFile) {
				comp::game::particles::note_project_file(ofn->lpstrFile);
			}
			return ok;
		}

		static BOOL WINAPI GetOpenFileNameW_hk(LPOPENFILENAMEW ofn)
		{
			if (ofn) {
				ofn->hwndOwner = fix_dialog_owner(ofn->hwndOwner, "GetOpenFileNameW");
			}
			const BOOL ok = GetOpenFileNameW_og ? GetOpenFileNameW_og(ofn) : FALSE;
			if (ok && ofn && ofn->lpstrFile) {
				comp::game::particles::note_project_file_w(ofn->lpstrFile);
			}
			return ok;
		}

		static BOOL WINAPI GetSaveFileNameA_hk(LPOPENFILENAMEA ofn)
		{
			if (ofn) {
				ofn->hwndOwner = fix_dialog_owner(ofn->hwndOwner, "GetSaveFileNameA");
			}
			const BOOL ok = GetSaveFileNameA_og ? GetSaveFileNameA_og(ofn) : FALSE;
			if (ok && ofn && ofn->lpstrFile) {
				comp::game::particles::note_project_file(ofn->lpstrFile);
				comp::game::particles::save_project_ini();
			}
			return ok;
		}

		static BOOL WINAPI GetSaveFileNameW_hk(LPOPENFILENAMEW ofn)
		{
			if (ofn) {
				ofn->hwndOwner = fix_dialog_owner(ofn->hwndOwner, "GetSaveFileNameW");
			}
			const BOOL ok = GetSaveFileNameW_og ? GetSaveFileNameW_og(ofn) : FALSE;
			if (ok && ofn && ofn->lpstrFile) {
				comp::game::particles::note_project_file_w(ofn->lpstrFile);
				comp::game::particles::save_project_ini();
			}
			return ok;
		}

		static INT_PTR WINAPI DialogBoxParamA_hk(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp)
		{
			auto chained = comp::game::particles::chain_dlgproc(proc, lp);
			const INT_PTR r = DialogBoxParamA_og ? DialogBoxParamA_og(inst, tmpl,
				fix_dialog_owner(parent, "DialogBoxParamA"), chained, lp) : 0;
			comp::game::particles::unchain_dlgproc();
			return r;
		}

		static INT_PTR WINAPI DialogBoxParamW_hk(HINSTANCE inst, LPCWSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp)
		{
			auto chained = comp::game::particles::chain_dlgproc(proc, lp);
			const INT_PTR r = DialogBoxParamW_og ? DialogBoxParamW_og(inst, tmpl,
				fix_dialog_owner(parent, "DialogBoxParamW"), chained, lp) : 0;
			comp::game::particles::unchain_dlgproc();
			return r;
		}

		static INT_PTR WINAPI DialogBoxIndirectParamA_hk(HINSTANCE inst, LPCDLGTEMPLATEA tmpl, HWND parent, DLGPROC proc, LPARAM lp)
		{
			auto chained = comp::game::particles::chain_dlgproc(proc, lp);
			const INT_PTR r = DialogBoxIndirectParamA_og ? DialogBoxIndirectParamA_og(inst, tmpl,
				fix_dialog_owner(parent, "DialogBoxIndirectParamA"), chained, lp) : 0;
			comp::game::particles::unchain_dlgproc();
			return r;
		}

		static INT_PTR WINAPI DialogBoxIndirectParamW_hk(HINSTANCE inst, LPCDLGTEMPLATEW tmpl, HWND parent, DLGPROC proc, LPARAM lp)
		{
			auto chained = comp::game::particles::chain_dlgproc(proc, lp);
			const INT_PTR r = DialogBoxIndirectParamW_og ? DialogBoxIndirectParamW_og(inst, tmpl,
				fix_dialog_owner(parent, "DialogBoxIndirectParamW"), chained, lp) : 0;
			comp::game::particles::unchain_dlgproc();
			return r;
		}

		static HWND WINAPI CreateDialogParamA_hk(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp)
		{
			auto chained = comp::game::particles::chain_dlgproc(proc, lp);
			HWND h = CreateDialogParamA_og ? CreateDialogParamA_og(inst, tmpl,
				fix_dialog_owner(parent, "CreateDialogParamA"), chained, lp) : nullptr;
			if (h) {
				comp::game::particles::note_dialog_created(h);
			}
			comp::game::particles::unchain_dlgproc();
			return h;
		}

		static HWND WINAPI CreateDialogParamW_hk(HINSTANCE inst, LPCWSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp)
		{
			auto chained = comp::game::particles::chain_dlgproc(proc, lp);
			HWND h = CreateDialogParamW_og ? CreateDialogParamW_og(inst, tmpl,
				fix_dialog_owner(parent, "CreateDialogParamW"), chained, lp) : nullptr;
			if (h) {
				comp::game::particles::note_dialog_created(h);
			}
			comp::game::particles::unchain_dlgproc();
			return h;
		}

		static HWND WINAPI CreateDialogIndirectParamA_hk(HINSTANCE inst, LPCDLGTEMPLATEA tmpl, HWND parent, DLGPROC proc, LPARAM lp)
		{
			auto chained = comp::game::particles::chain_dlgproc(proc, lp);
			HWND h = CreateDialogIndirectParamA_og ? CreateDialogIndirectParamA_og(inst, tmpl,
				fix_dialog_owner(parent, "CreateDialogIndirectParamA"), chained, lp) : nullptr;
			if (h) {
				comp::game::particles::note_dialog_created(h);
			}
			comp::game::particles::unchain_dlgproc();
			return h;
		}

		static HWND WINAPI CreateDialogIndirectParamW_hk(HINSTANCE inst, LPCDLGTEMPLATEW tmpl, HWND parent, DLGPROC proc, LPARAM lp)
		{
			auto chained = comp::game::particles::chain_dlgproc(proc, lp);
			HWND h = CreateDialogIndirectParamW_og ? CreateDialogIndirectParamW_og(inst, tmpl,
				fix_dialog_owner(parent, "CreateDialogIndirectParamW"), chained, lp) : nullptr;
			if (h) {
				comp::game::particles::note_dialog_created(h);
			}
			comp::game::particles::unchain_dlgproc();
			return h;
		}

		static void hook_export(HMODULE mod, const char* name, void* stub, void** orig)
		{
			if (!mod || !name || !stub || !orig) {
				return;
			}
			FARPROC fn = GetProcAddress(mod, name);
			if (!fn)
			{
				shared::common::log("EditorFrame",
					std::format("dialog hook skip {} — not found", name),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return;
			}
			if (*orig) {
				return;
			}
			if (!shared::utils::hook::detour(reinterpret_cast<DWORD>(fn), stub, orig))
			{
				shared::common::log("EditorFrame",
					std::format("dialog hook failed {}", name),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
			else
			{
				shared::common::log("EditorFrame",
					std::format("dialog hook {}", name),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		}

		static void install_editor_proxy_hooks()
		{
			static std::atomic<long> once{ 0 };
			if (once.exchange(1) != 0) {
				return;
			}

			if (const auto st = MH_Initialize();
				st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
			{
				shared::common::log("EditorFrame",
					std::format("MinHook init failed: {}", static_cast<int>(st)),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return;
			}

			HMODULE user32 = GetModuleHandleA("user32.dll");
			HMODULE comdlg = LoadLibraryA("comdlg32.dll");
			hook_export(user32, "GetActiveWindow", GetActiveWindow_hk,
				reinterpret_cast<void**>(&GetActiveWindow_og));
			hook_export(comdlg, "GetOpenFileNameA", GetOpenFileNameA_hk,
				reinterpret_cast<void**>(&GetOpenFileNameA_og));
			hook_export(comdlg, "GetOpenFileNameW", GetOpenFileNameW_hk,
				reinterpret_cast<void**>(&GetOpenFileNameW_og));
			hook_export(comdlg, "GetSaveFileNameA", GetSaveFileNameA_hk,
				reinterpret_cast<void**>(&GetSaveFileNameA_og));
			hook_export(comdlg, "GetSaveFileNameW", GetSaveFileNameW_hk,
				reinterpret_cast<void**>(&GetSaveFileNameW_og));
			hook_export(user32, "DialogBoxParamA", DialogBoxParamA_hk,
				reinterpret_cast<void**>(&DialogBoxParamA_og));
			hook_export(user32, "DialogBoxParamW", DialogBoxParamW_hk,
				reinterpret_cast<void**>(&DialogBoxParamW_og));
			hook_export(user32, "DialogBoxIndirectParamA", DialogBoxIndirectParamA_hk,
				reinterpret_cast<void**>(&DialogBoxIndirectParamA_og));
			hook_export(user32, "DialogBoxIndirectParamW", DialogBoxIndirectParamW_hk,
				reinterpret_cast<void**>(&DialogBoxIndirectParamW_og));
			hook_export(user32, "CreateDialogParamA", CreateDialogParamA_hk,
				reinterpret_cast<void**>(&CreateDialogParamA_og));
			hook_export(user32, "CreateDialogParamW", CreateDialogParamW_hk,
				reinterpret_cast<void**>(&CreateDialogParamW_og));
			hook_export(user32, "CreateDialogIndirectParamA", CreateDialogIndirectParamA_hk,
				reinterpret_cast<void**>(&CreateDialogIndirectParamA_og));
			hook_export(user32, "CreateDialogIndirectParamW", CreateDialogIndirectParamW_hk,
				reinterpret_cast<void**>(&CreateDialogIndirectParamW_og));
			comp::game::particles::install_ui_hooks();
		}

		static bool has_child_class(HWND parent, const char* name)
		{
			struct ctx { const char* name; bool found; };
			ctx c{ name, false };
			EnumChildWindows(parent, [](HWND child, LPARAM lp) -> BOOL
			{
				auto* c = reinterpret_cast<ctx*>(lp);
				if (class_is(child, c->name)) {
					c->found = true;
					return FALSE;
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&c));
			return c.found;
		}

		static HWND find_editor_frame()
		{
			struct ctx { DWORD pid; HWND hwnd; };
			ctx c{ GetCurrentProcessId(), nullptr };
			EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL
			{
				auto* c = reinterpret_cast<ctx*>(lp);
				DWORD pid = 0;
				GetWindowThreadProcessId(hwnd, &pid);
				if (pid != c->pid || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
					return TRUE;
				}
				if (class_is(hwnd, kEditorClass)) {
					c->hwnd = hwnd;
					return FALSE;
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&c));
			return c.hwnd;
		}

		static void apply_dark_frame(HWND hwnd)
		{
			BOOL dark = TRUE;
			DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
			DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

			COLORREF black = RGB(0, 0, 0);
			COLORREF text = kText;
			DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &black, sizeof(black));
			DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &black, sizeof(black));
			DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));

			using set_wca_fn = BOOL(WINAPI*)(HWND, void*);
			static set_wca_fn set_wca = reinterpret_cast<set_wca_fn>(
				GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
			if (set_wca)
			{
				struct wca_data { DWORD attrib; PVOID pv; SIZE_T cb; };
				BOOL value = TRUE;
				wca_data data{ 26, &value, sizeof(value) };
				set_wca(hwnd, &data);
			}
		}

		static void copy_icons(HWND dst, HWND src)
		{
			auto clone = [src, dst](WPARAM which, int fallback)
			{
				HANDLE icon = reinterpret_cast<HANDLE>(
					SendMessageA(src, WM_GETICON, which, 0));
				if (!icon) {
					icon = reinterpret_cast<HANDLE>(
						GetClassLongPtrA(src, fallback));
				}
				if (icon) {
					SendMessageA(dst, WM_SETICON, which, reinterpret_cast<LPARAM>(icon));
				}
			};
			clone(ICON_SMALL, GCLP_HICONSM);
			clone(ICON_BIG, GCLP_HICON);
		}

		static void copy_title(HWND dst, HWND src)
		{
			char title[512]{};
			if (GetWindowTextA(src, title, sizeof(title)) && title[0]) {
				SetWindowTextA(dst, title);
			}
			else {
				SetWindowTextA(dst, "3D Rad RTX");
			}
		}

		static HFONT make_caption_font(int dpi)
		{
			return CreateFontW(
				scale_px(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
				DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
				CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
		}

		static wchar_t to_upper_w(wchar_t c)
		{
			if (c >= L'a' && c <= L'z') {
				return static_cast<wchar_t>(c - (L'a' - L'A'));
			}
			return c;
		}

		static void strip_ampersand(const wchar_t* src, wchar_t* dst, size_t dst_cch, wchar_t* mnemonic)
		{
			if (mnemonic) {
				*mnemonic = 0;
			}
			if (!src || !dst || dst_cch == 0) {
				return;
			}
			size_t o = 0;
			for (size_t i = 0; src[i] && o + 1 < dst_cch; ++i)
			{
				if (src[i] == L'&' && src[i + 1])
				{
					if (src[i + 1] == L'&')
					{
						dst[o++] = L'&';
						++i;
						continue;
					}
					if (mnemonic && *mnemonic == 0) {
						*mnemonic = to_upper_w(src[i + 1]);
					}
					continue;
				}
				if (src[i] == L'\t') {
					break;
				}
				dst[o++] = src[i];
			}
			dst[o] = 0;
			if (mnemonic && *mnemonic == 0 && dst[0]) {
				*mnemonic = to_upper_w(dst[0]);
			}
		}

		static UINT find_menu_command(HMENU menu, const wchar_t* needle)
		{
			if (!menu || !needle) {
				return 0;
			}
			const int n = GetMenuItemCount(menu);
			for (int i = 0; i < n; ++i)
			{
				wchar_t raw[256]{};
				MENUITEMINFOW mii{};
				mii.cbSize = sizeof(mii);
				mii.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_ID | MIIM_FTYPE;
				mii.dwTypeData = raw;
				mii.cch = 255;
				if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii)) {
					continue;
				}
				if (mii.fType & MFT_SEPARATOR) {
					continue;
				}
				if (mii.hSubMenu)
				{
					const UINT nested = find_menu_command(mii.hSubMenu, needle);
					if (nested) {
						return nested;
					}
					continue;
				}
				wchar_t label[256]{};
				strip_ampersand(raw, label, 256, nullptr);
				if (wcsstr(label, needle)) {
					return mii.wID;
				}
			}
			return 0;
		}

		static HMENU submenu_for_id(HMENU menu, UINT id)
		{
			if (!menu || !id) {
				return nullptr;
			}
			MENUITEMINFOW mii{};
			mii.cbSize = sizeof(mii);
			mii.fMask = MIIM_SUBMENU;
			if (GetMenuItemInfoW(menu, id, FALSE, &mii) && mii.hSubMenu) {
				return mii.hSubMenu;
			}
			const int n = GetMenuItemCount(menu);
			for (int i = 0; i < n; ++i)
			{
				MENUITEMINFOW item{};
				item.cbSize = sizeof(item);
				item.fMask = MIIM_SUBMENU | MIIM_ID;
				if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &item)) {
					continue;
				}
				if (item.hSubMenu)
				{
					HMENU found = submenu_for_id(item.hSubMenu, id);
					if (found) {
						return found;
					}
				}
			}
			return nullptr;
		}

		static bool imgui_wants_keys()
		{
			if (!shared::globals::imgui_menu_open) {
				return false;
			}
			if (ImGui::GetCurrentContext())
			{
				const ImGuiIO& io = ImGui::GetIO();
				if (io.WantCaptureKeyboard || io.WantTextInput) {
					return true;
				}
			}
			return true;
		}

		static bool is_key_msg(UINT msg)
		{
			return msg == WM_KEYDOWN || msg == WM_KEYUP ||
				msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
				msg == WM_CHAR || msg == WM_SYSCHAR ||
				msg == WM_DEADCHAR || msg == WM_SYSDEADCHAR ||
				msg == WM_UNICHAR || msg == WM_APPCOMMAND ||
				msg == WM_IME_CHAR || msg == WM_IME_COMPOSITION;
		}

		static HWND find_named_child(HWND parent, const char* name)
		{
			if (!parent || !name) {
				return nullptr;
			}
			for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
			{
				if (class_is(c, name)) {
					return c;
				}
			}
			struct ctx { const char* name; HWND hwnd; };
			ctx c{ name, nullptr };
			EnumChildWindows(parent, [](HWND child, LPARAM lp) -> BOOL
			{
				auto* c = reinterpret_cast<ctx*>(lp);
				if (class_is(child, c->name)) {
					c->hwnd = child;
					return FALSE;
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&c));
			return c.hwnd;
		}

		static HWND preferred_focus(const frame_state& st)
		{
			if (!st.editor || !IsWindow(st.editor)) {
				return nullptr;
			}
			HWND vp = find_named_child(st.editor, kViewportClass);
			if (vp && IsWindowVisible(vp) && IsWindowEnabled(vp)) {
				return vp;
			}
			return st.editor;
		}

		static void focus_editor(frame_state& st)
		{
			if (st.menu_open >= 0 || st.closing) {
				return;
			}
			if (editor_settings::is_settings_hwnd(GetForegroundWindow()) ||
				editor_settings::is_settings_hwnd(GetFocus()) ||
				comp::game::particles::is_remix_pane_hwnd(GetForegroundWindow()) ||
				comp::game::particles::is_remix_pane_hwnd(GetFocus()))
			{
				return;
			}
			HWND target = preferred_focus(st);
			if (!target) {
				return;
			}
			HWND focus = GetFocus();
			if (focus == target || (st.editor && focus &&
				(focus == st.editor || IsChild(st.editor, focus))))
			{
				return;
			}
			SetFocus(target);
		}

		static void activate_editor_frame(frame_state& st, bool active)
		{
			if (!st.editor || !IsWindow(st.editor)) {
				return;
			}
			if (st.editor_active == active) {
				return;
			}
			st.editor_active = active;
			// Post: SendMessage WM_ACTIVATE into 3DRADCLASS can wait on the
			// wrapper (owned popup) and stall the whole process on refocus.
			PostMessageA(st.editor, WM_NCACTIVATE, active ? TRUE : FALSE, 0);
			PostMessageA(st.editor, WM_ACTIVATE,
				MAKEWPARAM(active ? WA_ACTIVE : WA_INACTIVE, 0),
				reinterpret_cast<LPARAM>(st.wrapper));
		}

		static void dispatch_editor_command(frame_state& st, UINT id, HMENU menu, const char* src)
		{
			if (!id || !st.editor || !IsWindow(st.editor)) {
				return;
			}

			HMENU sub = submenu_for_id(st.editor_menu ? st.editor_menu : menu, id);
			if (sub)
			{
				shared::common::log("EditorFrame",
					std::format("skip WM_COMMAND id={} src={} — MF_POPUP submenu=0x{:X}",
						id, src, reinterpret_cast<std::uintptr_t>(sub)),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				return;
			}

			const WPARAM wp = MAKEWPARAM(id, 0);
			shared::common::log("EditorFrame",
				std::format(
					"dispatch WM_COMMAND id={} src={} PostMessage to {} active=0x{:X} focus=0x{:X}",
					id, src, describe_hwnd(st.editor),
					reinterpret_cast<std::uintptr_t>(GetActiveWindow()),
					reinterpret_cast<std::uintptr_t>(GetFocus())),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			PostMessageA(st.editor, WM_COMMAND, wp, 0);
		}

		static RECT map_child_to_parent(HWND parent, HWND child)
		{
			RECT r{};
			GetWindowRect(child, &r);
			POINT tl{ r.left, r.top };
			POINT br{ r.right, r.bottom };
			ScreenToClient(parent, &tl);
			ScreenToClient(parent, &br);
			return RECT{ tl.x, tl.y, br.x, br.y };
		}

		static std::string describe_menu(HMENU menu, int depth)
		{
			if (!menu) {
				return "(null)";
			}
			const int n = GetMenuItemCount(menu);
			std::string out = std::format("HMENU=0x{:X} items={}",
				reinterpret_cast<std::uintptr_t>(menu), n);
			if (n < 0) {
				return out;
			}
			const int show = (depth == 0) ? n : i_min(n, 12);
			for (int i = 0; i < show; ++i)
			{
				wchar_t raw[256]{};
				MENUITEMINFOW mii{};
				mii.cbSize = sizeof(mii);
				mii.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_ID | MIIM_FTYPE;
				mii.dwTypeData = raw;
				mii.cch = 255;
				if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii)) {
					continue;
				}
				if (mii.fType & MFT_SEPARATOR)
				{
					out += std::format(" | [{}] ---", i);
					continue;
				}
				char utf[256]{};
				WideCharToMultiByte(CP_UTF8, 0, raw, -1, utf, sizeof(utf), nullptr, nullptr);
				out += std::format(" | [{}] '{}' id={} sub=0x{:X}",
					i, utf, static_cast<unsigned>(mii.wID),
					reinterpret_cast<std::uintptr_t>(mii.hSubMenu));
				if (depth == 0 && mii.hSubMenu)
				{
					out += " {";
					out += describe_menu(mii.hSubMenu, 1);
					out += "}";
				}
			}
			if (show < n) {
				out += std::format(" | ... +{} more", n - show);
			}
			return out;
		}

		static void dump_editor_children(HWND editor)
		{
			int i = 0;
			for (HWND c = GetWindow(editor, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT), ++i)
			{
				RECT r = map_child_to_parent(editor, c);
				shared::common::log("EditorFrame",
					std::format("child[{}] {} rect=({},{})-({},{}) {}x{}",
						i, describe_hwnd(c),
						r.left, r.top, r.right, r.bottom,
						r.right - r.left, r.bottom - r.top),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
			if (i == 0)
			{
				shared::common::log("EditorFrame",
					"3DRADCLASS has no immediate children",
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		}

		static HMENU shaders_popup_menu()
		{
			static HMENU popup = nullptr;
			if (!popup)
			{
				popup = CreatePopupMenu();
				AppendMenuW(popup, MF_STRING, kCmdClearShaders, L"Clear Shaders");
			}
			return popup;
		}

		static bool popup_has_clear_shaders(HMENU popup)
		{
			if (!popup) {
				return false;
			}
			const int n = GetMenuItemCount(popup);
			for (int i = 0; i < n; ++i)
			{
				wchar_t raw[256]{};
				MENUITEMINFOW mii{};
				mii.cbSize = sizeof(mii);
				mii.fMask = MIIM_STRING | MIIM_ID;
				mii.dwTypeData = raw;
				mii.cch = 255;
				if (!GetMenuItemInfoW(popup, static_cast<UINT>(i), TRUE, &mii)) {
					continue;
				}
				if (mii.wID == kCmdClearShaders) {
					return true;
				}
				wchar_t label[64]{};
				strip_ampersand(raw, label, 64, nullptr);
				if (_wcsicmp(label, L"Clear Shaders") == 0) {
					return true;
				}
			}
			return false;
		}

		static void ensure_clear_shaders_item(HMENU popup)
		{
			if (!popup || popup_has_clear_shaders(popup)) {
				return;
			}
			if (GetMenuItemCount(popup) > 0) {
				AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
			}
			AppendMenuW(popup, MF_STRING, kCmdClearShaders, L"Clear Shaders");
		}

		static bool handle_clear_shaders(HWND owner, frame_state& st)
		{
			const int confirm = MessageBoxW(owner ? owner : st.wrapper,
				L"Clear NVIDIA shader caches (GLCache, DXCache) and this install's "
				L"Remix/DXVK shader caches (*.dxvk-cache)?\n\n"
				L"3D Rad will close. A hidden helper deletes locked caches after exit, "
				L"then restarts the editor. USD .glslfx and d3d9_dxvk.dll are left alone.",
				L"Clear Shaders",
				MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TASKMODAL);
			if (confirm != IDYES) {
				return true;
			}

			wchar_t root_w[MAX_PATH]{};
			MultiByteToWideChar(CP_ACP, 0, shared::globals::root_path.c_str(), -1,
				root_w, MAX_PATH);
			const auto stw = shader_cache::wipe_now(root_w);
			shared::common::log("EditorFrame",
				std::format(
					"Clear Shaders in-process: deleted={} failed={} dirs_removed={} "
					"(pass 2 after exit via rundll32, no cmd/findstr)",
					stw.deleted, stw.failed, stw.dirs_removed),
				stw.failed ? shared::common::LOG_TYPE::LOG_TYPE_WARN
					: shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

			wchar_t exe[MAX_PATH]{};
			wchar_t cwd[MAX_PATH]{};
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			GetCurrentDirectoryW(MAX_PATH, cwd);
			if (!shader_cache::spawn_post_exit_helper(
				GetCurrentProcessId(), exe, cwd, root_w))
			{
				MessageBoxW(owner ? owner : st.wrapper,
					L"Could not start the post-exit cache helper.\n"
					L"Close 3D Rad and run Clear Shaders again.",
					L"Clear Shaders", MB_OK | MB_ICONWARNING | MB_TASKMODAL);
				return true;
			}

			shared::common::log("EditorFrame",
				"Clear Shaders: helper armed — closing 3DRad.exe, no --rtx-comp-warmed",
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			request_editor_close(st);
			ExitProcess(0);
			return true;
		}

		static bool handle_shaders_command(UINT id, HWND owner, frame_state& st)
		{
			if (id != kCmdClearShaders) {
				return false;
			}
			return handle_clear_shaders(owner, st);
		}

		static UINT g_help_files_cmd = 0;

		static bool shell_open_url(const wchar_t* url)
		{
			if (!url || !url[0]) {
				return false;
			}
			const INT_PTR r = reinterpret_cast<INT_PTR>(
				ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL));
			return r > 32;
		}

		static void path_to_file_url(wchar_t* url, size_t cap, const wchar_t* path)
		{
			if (!url || cap < 10 || !path) {
				return;
			}
			wcscpy_s(url, cap, L"file:///");
			size_t n = 8;
			for (const wchar_t* p = path; *p && n + 1 < cap; ++p)
			{
				url[n++] = (*p == L'\\') ? L'/' : *p;
			}
			url[n] = 0;
		}

		static void open_local_help_in_browser()
		{
			wchar_t exe[MAX_PATH]{};
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			wchar_t* slash = wcsrchr(exe, L'\\');
			if (slash) {
				*slash = 0;
			}

			wchar_t dir[MAX_PATH]{};
			swprintf_s(dir, L"%s\\3DRad_res\\help\\", exe);

			wchar_t url[MAX_PATH * 2]{};
			path_to_file_url(url, _countof(url), dir);
			if (url[0] && url[wcslen(url) - 1] != L'/') {
				wcscat_s(url, L"/");
			}

			if (shell_open_url(url))
			{
				shared::common::log("EditorFrame",
					"Help Files → browser file:/// 3DRad_res/help/",
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				return;
			}

			wchar_t file[MAX_PATH]{};
			swprintf_s(file, L"%sindex.htm", dir);
			path_to_file_url(url, _countof(url), file);
			if (shell_open_url(url)) {
				return;
			}
			swprintf_s(file, L"%shelp.htm", dir);
			path_to_file_url(url, _countof(url), file);
			if (!shell_open_url(url))
			{
				shared::common::log("EditorFrame",
					"Help Files: browser open failed",
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
		}

		static bool label_is_help_files(const wchar_t* label)
		{
			return label && wcsstr(label, L"Help Files");
		}

		static bool popup_has_remix_docs(HMENU popup)
		{
			if (!popup) {
				return false;
			}
			const int n = GetMenuItemCount(popup);
			for (int i = 0; i < n; ++i)
			{
				wchar_t raw[256]{};
				MENUITEMINFOW mii{};
				mii.cbSize = sizeof(mii);
				mii.fMask = MIIM_STRING | MIIM_ID;
				mii.dwTypeData = raw;
				mii.cch = 255;
				if (!GetMenuItemInfoW(popup, static_cast<UINT>(i), TRUE, &mii)) {
					continue;
				}
				if (mii.wID == kCmdRemixDocs) {
					return true;
				}
				wchar_t label[256]{};
				strip_ampersand(raw, label, 256, nullptr);
				if (wcsstr(label, L"RTX Remix documentation")) {
					return true;
				}
			}
			return false;
		}

		static void ensure_help_items(HMENU popup)
		{
			if (!popup) {
				return;
			}
			const int n = GetMenuItemCount(popup);
			for (int i = 0; i < n; ++i)
			{
				wchar_t raw[256]{};
				MENUITEMINFOW mii{};
				mii.cbSize = sizeof(mii);
				mii.fMask = MIIM_STRING | MIIM_ID | MIIM_FTYPE;
				mii.dwTypeData = raw;
				mii.cch = 255;
				if (!GetMenuItemInfoW(popup, static_cast<UINT>(i), TRUE, &mii)) {
					continue;
				}
				if (mii.fType & MFT_SEPARATOR) {
					continue;
				}
				wchar_t label[256]{};
				strip_ampersand(raw, label, 256, nullptr);
				if (label_is_help_files(label)) {
					g_help_files_cmd = mii.wID;
				}
			}
			if (popup_has_remix_docs(popup)) {
				return;
			}
			if (GetMenuItemCount(popup) > 0) {
				AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
			}
			AppendMenuW(popup, MF_STRING, kCmdRemixDocs, L"RTX Remix documentation");
		}

		static bool handle_help_command(UINT id, frame_state& st)
		{
			if (id == kCmdRemixDocs)
			{
				shell_open_url(kRemixDocsUrl);
				shared::common::log("EditorFrame",
					"Help → RTX Remix documentation",
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				return true;
			}
			UINT help_id = g_help_files_cmd;
			if (!help_id) {
				help_id = find_menu_command(st.editor_menu, L"Help Files");
			}
			if (help_id && id == help_id)
			{
				open_local_help_in_browser();
				return true;
			}
			return false;
		}

		static bool adopt_menu(frame_state& st, HMENU menu, bool owned, const char* src)
		{
			if (!menu) {
				return false;
			}
			st.editor_menu = menu;
			st.menu_owned = owned;
			st.menu_count = 0;
			const int n = GetMenuItemCount(menu);
			for (int i = 0; i < n && st.menu_count < kMaxMenus; ++i)
			{
				wchar_t raw[256]{};
				MENUITEMINFOW mii{};
				mii.cbSize = sizeof(mii);
				mii.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_FTYPE;
				mii.dwTypeData = raw;
				mii.cch = 255;
				if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii)) {
					continue;
				}
				if ((mii.fType & MFT_SEPARATOR) || !mii.hSubMenu) {
					continue;
				}
				cap_menu& item = st.menus[st.menu_count];
				item = {};
				strip_ampersand(raw, item.label, 32, &item.mnemonic);
				item.popup = mii.hSubMenu;
				item.index = i;
				++st.menu_count;
			}
			if (st.menu_count < kMaxMenus)
			{
				bool have_settings = false;
				for (int i = 0; i < st.menu_count; ++i)
				{
					if (wcscmp(st.menus[i].label, L"Settings") == 0) {
						have_settings = true;
						break;
					}
				}
				if (!have_settings)
				{
					cap_menu& item = st.menus[st.menu_count];
					item = {};
					wcsncpy_s(item.label, L"Settings", _TRUNCATE);
					item.mnemonic = L'S';
					item.popup = editor_settings::popup_menu();
					item.index = -1;
					++st.menu_count;
				}
			}
			if (st.menu_count < kMaxMenus)
			{
				bool have_shaders = false;
				for (int i = 0; i < st.menu_count; ++i)
				{
					if (wcscmp(st.menus[i].label, L"Shaders") == 0) {
						ensure_clear_shaders_item(st.menus[i].popup);
						have_shaders = true;
						break;
					}
				}
				if (!have_shaders)
				{
					cap_menu& item = st.menus[st.menu_count];
					item = {};
					wcsncpy_s(item.label, L"Shaders", _TRUNCATE);
					item.mnemonic = L'A';
					item.popup = shaders_popup_menu();
					item.index = -1;
					++st.menu_count;
				}
			}
			for (int i = 0; i < st.menu_count; ++i)
			{
				if (wcscmp(st.menus[i].label, L"Help") == 0) {
					ensure_help_items(st.menus[i].popup);
					break;
				}
			}
			shared::common::log("EditorFrame",
				std::format("menu source={} owned={} count={} {}",
					src, owned ? 1 : 0, st.menu_count, describe_menu(menu, 0)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			return st.menu_count > 0;
		}

		static void ensure_caption_menus(frame_state& st)
		{
			if (st.menu_count > 0) {
				return;
			}
			if (!st.editor || !IsWindow(st.editor)) {
				return;
			}
			HMENU live = GetMenu(st.editor);
			if (live) {
				adopt_menu(st, live, false, "GetMenu-paint");
			}
		}

		static void capture_editor_ui(frame_state& st, HWND editor)
		{
			dump_editor_children(editor);

			HMENU live = GetMenu(editor);
			shared::common::log("EditorFrame",
				std::format("GetMenu(3DRADCLASS)={}",
					describe_menu(live, 0)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

			if (live && adopt_menu(st, live, false, "GetMenu"))
			{
				SetMenu(editor, nullptr);
				DrawMenuBar(editor);
			}
			else
			{
				HMODULE exe = GetModuleHandleA(nullptr);
				const int ids[] = { 128, 129, 130, 147, 64, 100, 101, 102, 103, 1, 2 };
				for (int id : ids)
				{
					HMENU loaded = LoadMenuA(exe, MAKEINTRESOURCEA(id));
					if (loaded && adopt_menu(st, loaded, true, std::format("LoadMenu id={}", id).c_str()))
					{
						break;
					}
					if (loaded) {
						DestroyMenu(loaded);
					}
				}
			}

			HMODULE exe = GetModuleHandleA(nullptr);
			const int acc_ids[] = { 128, 129, 130, 147, 64, 100, 101, 102, 103, 1, 2 };
			for (int id : acc_ids)
			{
				HACCEL accel = LoadAcceleratorsA(exe, MAKEINTRESOURCEA(id));
				if (accel)
				{
					st.accel = accel;
					shared::common::log("EditorFrame",
						std::format("LoadAccelerators id={} HACCEL=0x{:X}",
							id, reinterpret_cast<std::uintptr_t>(accel)),
						shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
					break;
				}
			}
			if (!st.accel)
			{
				shared::common::log("EditorFrame",
					"no RT_ACCELERATOR loaded — MFC TranslateAccelerator via hwnd rewrite still applies",
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		}

		static void layout_caption_items(HWND hwnd, frame_state& st)
		{
			RECT crc{};
			GetClientRect(hwnd, &crc);
			const int pad = scale_px(12, st.dpi);
			const int hpad = scale_px(10, st.dpi);
			const int gap = scale_px(16, st.dpi);
			const int btn_area = st.btn_w * 3;

			HDC hdc = GetDC(hwnd);
			HFONT font = make_caption_font(st.dpi);
			HGDIOBJ old = hdc ? SelectObject(hdc, font) : nullptr;

			int menus_w = 0;
			SIZE sizes[kMaxMenus]{};
			for (int i = 0; i < st.menu_count; ++i)
			{
				if (hdc) {
					GetTextExtentPoint32W(hdc, st.menus[i].label,
						static_cast<int>(wcslen(st.menus[i].label)), &sizes[i]);
				}
				else {
					sizes[i].cx = static_cast<LONG>(wcslen(st.menus[i].label) * 7);
				}
				menus_w += sizes[i].cx + hpad * 2;
			}

			if (hdc)
			{
				SelectObject(hdc, old);
				ReleaseDC(hwnd, hdc);
			}
			DeleteObject(font);

			int x = pad;
			wchar_t title[512]{};
			GetWindowTextW(hwnd, title, 512);
			const int title_max = i_max(scale_px(80, st.dpi),
				static_cast<int>(crc.right) - btn_area - menus_w - pad - gap - scale_px(8, st.dpi));
			SIZE title_sz{ scale_px(180, st.dpi), 0 };
			HDC hdc2 = GetDC(hwnd);
			HFONT font2 = make_caption_font(st.dpi);
			HGDIOBJ old2 = hdc2 ? SelectObject(hdc2, font2) : nullptr;
			if (hdc2) {
				GetTextExtentPoint32W(hdc2, title, static_cast<int>(wcslen(title)), &title_sz);
			}
			if (hdc2)
			{
				SelectObject(hdc2, old2);
				ReleaseDC(hwnd, hdc2);
			}
			DeleteObject(font2);
			const int title_w = i_min(static_cast<int>(title_sz.cx), title_max);
			x += title_w + gap;

			for (int i = 0; i < st.menu_count; ++i)
			{
				const int w = sizes[i].cx + hpad * 2;
				st.menus[i].rc = RECT{ x, 0, x + w, st.caption_h };
				if (st.menus[i].rc.right > crc.right - btn_area)
				{
					st.menus[i].rc.right = crc.right - btn_area;
					st.menus[i].rc.left = i_max(pad, static_cast<int>(st.menus[i].rc.right) - w);
				}
				x = st.menus[i].rc.right;
			}
		}

		static int hit_menu(const frame_state& st, POINT client_pt)
		{
			if (client_pt.y < 0 || client_pt.y >= st.caption_h) {
				return -1;
			}
			for (int i = 0; i < st.menu_count; ++i)
			{
				if (PtInRect(&st.menus[i].rc, client_pt)) {
					return i;
				}
			}
			return -1;
		}

		static RECT btn_rect(const frame_state& st, const RECT& client, caption_btn btn)
		{
			const int h = st.caption_h;
			const int w = st.btn_w;
			RECT r{ client.right - w, 0, client.right, h };
			if (btn == caption_btn::close) {
				return r;
			}
			OffsetRect(&r, -w, 0);
			if (btn == caption_btn::max) {
				return r;
			}
			OffsetRect(&r, -w, 0);
			return r;
		}

		static caption_btn hit_button(const frame_state& st, HWND hwnd, POINT client_pt)
		{
			RECT crc{};
			GetClientRect(hwnd, &crc);
			const RECT close_r = btn_rect(st, crc, caption_btn::close);
			const RECT max_r = btn_rect(st, crc, caption_btn::max);
			const RECT min_r = btn_rect(st, crc, caption_btn::min);
			if (PtInRect(&close_r, client_pt)) {
				return caption_btn::close;
			}
			if (PtInRect(&max_r, client_pt)) {
				return caption_btn::max;
			}
			if (PtInRect(&min_r, client_pt)) {
				return caption_btn::min;
			}
			return caption_btn::none;
		}

		static const char* caption_btn_name(caption_btn btn)
		{
			switch (btn)
			{
			case caption_btn::min: return "min";
			case caption_btn::max: return "max";
			case caption_btn::close: return "close";
			default: return "none";
			}
		}

		static RECT wrapper_caption_screen(const frame_state& st)
		{
			RECT r{};
			if (!st.wrapper || !IsWindow(st.wrapper)) {
				return r;
			}
			RECT crc{};
			GetClientRect(st.wrapper, &crc);
			POINT tl{ 0, 0 };
			POINT br{ crc.right, st.caption_h };
			ClientToScreen(st.wrapper, &tl);
			ClientToScreen(st.wrapper, &br);
			r = RECT{ tl.x, tl.y, br.x, br.y };
			return r;
		}

		static bool point_in_wrapper_caption(const frame_state& st, POINT screen)
		{
			const RECT r = wrapper_caption_screen(st);
			return r.right > r.left && PtInRect(&r, screen) != FALSE;
		}

		static bool cursor_in_caption(const frame_state& st)
		{
			POINT pt{};
			GetCursorPos(&pt);
			return point_in_wrapper_caption(st, pt);
		}

		static bool point_in_wrapper_resize_border(const frame_state& st, POINT screen)
		{
			if (!st.wrapper || !IsWindow(st.wrapper)) {
				return false;
			}
			const int b = resize_border_px(st, st.wrapper);
			if (b <= 0) {
				return false;
			}
			RECT wr{};
			GetWindowRect(st.wrapper, &wr);
			if (screen.x < wr.left || screen.x >= wr.right ||
				screen.y < wr.top || screen.y >= wr.bottom)
			{
				return false;
			}
			return screen.x < wr.left + b || screen.x >= wr.right - b ||
				screen.y < wr.top + b || screen.y >= wr.bottom - b;
		}

		static POINT mouse_to_client(HWND hwnd, UINT msg, LPARAM lparam)
		{
			POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
			if (msg == WM_NCMOUSEMOVE || msg == WM_NCMOUSELEAVE ||
				msg == WM_NCLBUTTONDOWN || msg == WM_NCLBUTTONUP ||
				msg == WM_NCLBUTTONDBLCLK)
			{
				ScreenToClient(hwnd, &pt);
			}
			return pt;
		}

		static void clip_editor_to_client(frame_state& st)
		{
			if (!st.editor || !IsWindow(st.editor)) {
				return;
			}
			RECT cr{};
			GetClientRect(st.editor, &cr);
			POINT tl{ cr.left, cr.top };
			POINT br{ cr.right, cr.bottom };
			ClientToScreen(st.editor, &tl);
			ClientToScreen(st.editor, &br);
			RECT wr{};
			GetWindowRect(st.editor, &wr);
			const int x0 = tl.x - wr.left;
			const int y0 = tl.y - wr.top;
			const int x1 = br.x - wr.left;
			const int y1 = br.y - wr.top;
			const int rw = x1 - x0;
			const int rh = y1 - y0;
			const int ww = wr.right - wr.left;
			const int wh = wr.bottom - wr.top;
			if (x0 == 0 && y0 == 0 && ww == rw && wh == rh) {
				return;
			}
			if (rw == st.last_rgn_w && rh == st.last_rgn_h) {
				return;
			}
			HRGN rgn = CreateRectRgn(x0, y0, x1, y1);
			if (!rgn) {
				return;
			}
			if (SetWindowRgn(st.editor, rgn, TRUE))
			{
				st.last_rgn_w = rw;
				st.last_rgn_h = rh;
			}
			else
			{
				DeleteObject(rgn);
			}
		}

		static void log_caption_hit(const char* how, caption_btn btn, int menu, UINT msg)
		{
			shared::common::log("EditorFrame",
				std::format("caption lbutton id={} menu={} how={} msg=0x{:X} hover-path",
					caption_btn_name(btn), menu, how, msg),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		static int menu_index_for_key(const frame_state& st, WPARAM vk)
		{
			wchar_t key = to_upper_w(static_cast<wchar_t>(vk & 0xFF));
			if (vk >= 'a' && vk <= 'z') {
				key = to_upper_w(static_cast<wchar_t>(vk));
			}
			for (int i = 0; i < st.menu_count; ++i)
			{
				if (st.menus[i].mnemonic && st.menus[i].mnemonic == key) {
					return i;
				}
			}
			return -1;
		}

		static void open_caption_menu(HWND hwnd, frame_state& st, int index)
		{
			if (index < 0 || index >= st.menu_count || !st.menus[index].popup) {
				return;
			}
			if (!st.editor || !IsWindow(st.editor)) {
				return;
			}

			layout_caption_items(hwnd, st);
			st.menu_open = index;
			st.menu_hover = index;
			InvalidateRect(hwnd, nullptr, FALSE);
			UpdateWindow(hwnd);

			activate_editor_frame(st, true);

			HMENU popup = st.menus[index].popup;
			const bool ours = st.menus[index].index < 0;
			if (!ours)
			{
				SendMessageA(st.editor, WM_INITMENU, reinterpret_cast<WPARAM>(st.editor_menu), 0);
				SendMessageA(st.editor, WM_INITMENUPOPUP,
					reinterpret_cast<WPARAM>(popup),
					MAKELPARAM(st.menus[index].index, FALSE));
			}

			RECT rc = st.menus[index].rc;
			POINT pt{ rc.left, rc.bottom };
			ClientToScreen(hwnd, &pt);

			// TrackPopupMenu owner is the wrapper (foreground after the caption
			// click). Nested WM_INITMENUPOPUP is forwarded to 3DRADCLASS.
			// TPM_RETURNCMD + PostMessage so the editor runs WM_COMMAND on its
			// own pump. Do not TPM_NONOTIFY (that skips submenu init).
			const UINT cmd = TrackPopupMenuEx(
				popup,
				TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_RIGHTBUTTON |
				TPM_RETURNCMD,
				pt.x, pt.y, hwnd, nullptr);

			if (!ours)
			{
				SendMessageA(st.editor, WM_UNINITMENUPOPUP,
					reinterpret_cast<WPARAM>(popup), 0);
			}

			st.menu_open = -1;
			st.menu_hover = -1;
			if (cmd != 0)
			{
				if (handle_help_command(cmd, st))
				{
					InvalidateRect(hwnd, nullptr, FALSE);
					return;
				}
				if (handle_shaders_command(cmd, hwnd, st))
				{
					InvalidateRect(hwnd, nullptr, FALSE);
					return;
				}
				if (editor_settings::handle_menu_command(cmd, hwnd))
				{
					InvalidateRect(hwnd, nullptr, FALSE);
					return;
				}
				dispatch_editor_command(st, cmd, popup, "TrackPopupMenu");
			}
			InvalidateRect(hwnd, nullptr, FALSE);
			focus_editor(st);
		}

		static void draw_line_icon(HDC hdc, COLORREF color, int x0, int y0, int x1, int y1)
		{
			HPEN pen = CreatePen(PS_SOLID, 1, color);
			HGDIOBJ old = SelectObject(hdc, pen);
			MoveToEx(hdc, x0, y0, nullptr);
			LineTo(hdc, x1, y1);
			SelectObject(hdc, old);
			DeleteObject(pen);
		}

		static void draw_caption_button(HDC hdc, const RECT& r, caption_btn btn,
			bool hovered, bool pressed, bool maximized)
		{
			COLORREF fill = kBg;
			COLORREF icon = kIcon;
			if (hovered || pressed)
			{
				if (btn == caption_btn::close) {
					fill = kCloseHover;
					icon = kIconOnClose;
				}
				else {
					fill = kBtnHover;
				}
			}

			HBRUSH brush = CreateSolidBrush(fill);
			FillRect(hdc, &r, brush);
			DeleteObject(brush);

			const int cx = (r.left + r.right) / 2;
			const int cy = (r.top + r.bottom) / 2;

			if (btn == caption_btn::min)
			{
				draw_line_icon(hdc, icon, cx - 5, cy, cx + 6, cy);
			}
			else if (btn == caption_btn::close)
			{
				draw_line_icon(hdc, icon, cx - 5, cy - 5, cx + 6, cy + 6);
				draw_line_icon(hdc, icon, cx - 5, cy + 5, cx + 6, cy - 6);
			}
			else if (maximized)
			{
				draw_line_icon(hdc, icon, cx - 2, cy - 5, cx + 5, cy - 5);
				draw_line_icon(hdc, icon, cx + 4, cy - 5, cx + 4, cy + 2);
				draw_line_icon(hdc, icon, cx + 4, cy + 1, cx - 3, cy + 1);
				draw_line_icon(hdc, icon, cx - 2, cy + 1, cx - 2, cy - 5);
				draw_line_icon(hdc, icon, cx - 5, cy - 2, cx + 2, cy - 2);
				draw_line_icon(hdc, icon, cx + 1, cy - 2, cx + 1, cy + 5);
				draw_line_icon(hdc, icon, cx + 1, cy + 4, cx - 6, cy + 4);
				draw_line_icon(hdc, icon, cx - 5, cy + 4, cx - 5, cy - 2);
			}
			else
			{
				draw_line_icon(hdc, icon, cx - 5, cy - 5, cx + 6, cy - 5);
				draw_line_icon(hdc, icon, cx + 5, cy - 5, cx + 5, cy + 6);
				draw_line_icon(hdc, icon, cx + 5, cy + 5, cx - 6, cy + 5);
				draw_line_icon(hdc, icon, cx - 5, cy + 5, cx - 5, cy - 6);
			}
		}

		static void paint_caption(HWND hwnd, frame_state& st)
		{
			PAINTSTRUCT ps{};
			HDC hdc = BeginPaint(hwnd, &ps);

			if (st.caption_h < 24) {
				refresh_metrics(st, hwnd);
			}
			ensure_caption_menus(st);

			RECT crc{};
			GetClientRect(hwnd, &crc);
			const int cap_h = i_max(st.caption_h, 24);
			st.caption_h = cap_h;
			RECT cap{ 0, 0, crc.right, cap_h };
			if (crc.right < 1) {
				EndPaint(hwnd, &ps);
				return;
			}

			HDC mem = CreateCompatibleDC(hdc);
			HBITMAP bmp = CreateCompatibleBitmap(hdc, crc.right, cap_h);
			HGDIOBJ old_bmp = SelectObject(mem, bmp);

			HBRUSH bg = CreateSolidBrush(kBg);
			FillRect(mem, &cap, bg);
			DeleteObject(bg);

			layout_caption_items(hwnd, st);

			HFONT font = make_caption_font(st.dpi);
			HGDIOBJ old_font = SelectObject(mem, font);
			SetBkMode(mem, TRANSPARENT);
			SetTextColor(mem, kText);

			wchar_t title[512]{};
			GetWindowTextW(hwnd, title, 512);
			RECT text_r = cap;
			text_r.left = scale_px(12, st.dpi);
			if (st.menu_count > 0) {
				text_r.right = st.menus[0].rc.left - scale_px(8, st.dpi);
			}
			else {
				text_r.right -= st.btn_w * 3 + scale_px(8, st.dpi);
			}
			if (text_r.right < text_r.left + 8) {
				text_r.right = text_r.left + 8;
			}
			DrawTextW(mem, title, -1, &text_r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

			for (int i = 0; i < st.menu_count; ++i)
			{
				const bool hot = (st.menu_hover == i) || (st.menu_open == i);
				if (hot)
				{
					HBRUSH hover = CreateSolidBrush(kBtnHover);
					FillRect(mem, &st.menus[i].rc, hover);
					DeleteObject(hover);
				}
				DrawTextW(mem, st.menus[i].label, -1, &st.menus[i].rc,
					DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			}

			SelectObject(mem, old_font);
			DeleteObject(font);

			const bool zoomed = IsZoomed(hwnd) != FALSE;
			draw_caption_button(mem, btn_rect(st, crc, caption_btn::min),
				caption_btn::min, st.hover == caption_btn::min, st.pressed == caption_btn::min, zoomed);
			draw_caption_button(mem, btn_rect(st, crc, caption_btn::max),
				caption_btn::max, st.hover == caption_btn::max, st.pressed == caption_btn::max, zoomed);
			draw_caption_button(mem, btn_rect(st, crc, caption_btn::close),
				caption_btn::close, st.hover == caption_btn::close, st.pressed == caption_btn::close, zoomed);

			BitBlt(hdc, 0, 0, crc.right, cap_h, mem, 0, 0, SRCCOPY);
			SelectObject(mem, old_bmp);
			DeleteObject(bmp);
			DeleteDC(mem);
			EndPaint(hwnd, &ps);
		}

		static void request_hit_sync(frame_state& st)
		{
			if (!st.wrapper || !IsWindow(st.wrapper) || st.sizing || st.hit_posted) {
				return;
			}
			st.hit_posted = true;
			PostMessageA(st.wrapper, kSyncHitsMsg, 0, 0);
		}

		static void layout_inner_panes(frame_state& st, bool force_log)
		{
			if (!st.editor || !IsWindow(st.editor) || st.inner_lock || st.sizing) {
				return;
			}

			RECT cr{};
			GetClientRect(st.editor, &cr);
			const int cw = cr.right - cr.left;
			const int ch = cr.bottom - cr.top;
			if (cw < 32 || ch < 32) {
				return;
			}

			scoped_flag inner(st.inner_lock);

			struct kid
			{
				HWND hwnd;
				char cls[64];
				RECT r;
				bool vis;
			};
			kid kids[kMaxKids]{};
			int nk = 0;
			for (HWND c = GetWindow(st.editor, GW_CHILD);
				c && nk < kMaxKids;
				c = GetWindow(c, GW_HWNDNEXT))
			{
				kid& k = kids[nk];
				k.hwnd = c;
				GetClassNameA(c, k.cls, sizeof(k.cls));
				k.r = map_child_to_parent(st.editor, c);
				k.vis = IsWindowVisible(c) != FALSE;
				++nk;
			}

			HWND viewport = nullptr;
			HWND status = nullptr;
			HWND splitter = nullptr;
			HWND left = nullptr;
			int left_i = -1;

			for (int i = 0; i < nk; ++i)
			{
				kid& k = kids[i];
				if (!k.vis) {
					continue;
				}
				const int w = k.r.right - k.r.left;
				const int h = k.r.bottom - k.r.top;
				if (std::strcmp(k.cls, kViewportClass) == 0)
				{
					viewport = k.hwnd;
					continue;
				}
				if (std::strstr(k.cls, "status") || std::strstr(k.cls, "Status") ||
					std::strcmp(k.cls, "msctls_statusbar32") == 0)
				{
					status = k.hwnd;
					continue;
				}
				if (w <= 8 && h > 40)
				{
					splitter = k.hwnd;
					continue;
				}
				if (k.r.top >= ch - 36 && h <= 36 && w > cw / 2)
				{
					status = k.hwnd;
					continue;
				}
			}

			if (!viewport) {
				viewport = find_named_child(st.editor, kViewportClass);
			}

			int leftmost_x = cw;
			for (int i = 0; i < nk; ++i)
			{
				kid& k = kids[i];
				if (!k.vis || k.hwnd == viewport || k.hwnd == status || k.hwnd == splitter) {
					continue;
				}
				if (k.r.left < leftmost_x && k.r.left < cw / 2)
				{
					leftmost_x = k.r.left;
					left = k.hwnd;
					left_i = i;
				}
			}

			if (!st.inner_metrics)
			{
				int measured = 0;
				if (left_i >= 0) {
					measured = kids[left_i].r.right - kids[left_i].r.left;
					st.left_x = i_max(0, static_cast<int>(kids[left_i].r.left));
					if (st.left_x > 16) {
						st.left_x = 0;
					}
					if (viewport)
					{
						const RECT vr = map_child_to_parent(st.editor, viewport);
						st.pane_gap = i_max(0, static_cast<int>(vr.left - kids[left_i].r.right));
						if (st.pane_gap > 16) {
							st.pane_gap = 1;
						}
					}
				}
				else if (viewport)
				{
					const RECT vr = map_child_to_parent(st.editor, viewport);
					measured = i_max(0, static_cast<int>(vr.left));
					st.pane_gap = 0;
					st.left_x = 0;
				}
				if (measured < 80) {
					measured = 220;
				}
				if (measured > 640) {
					measured = 640;
				}
				st.stock_left_panel_w = measured;
				if (st.left_panel_w <= 0) {
					st.left_panel_w = measured;
				}
				if (st.left_panel_w > 640) {
					st.left_panel_w = 640;
				}
				// Stock list width only. ChildClass is always pinned to the
				// 3DRADCLASS client right edge — never keep the first-layout
				// gap / right_margin (that left the black strip + small FOV).
				st.left_x = 0;
				st.pane_gap = 0;
				st.view_top = 0;
				st.bottom_margin = 0;
				st.right_margin = 0;
				st.inner_metrics = st.left_panel_w > 0 || viewport != nullptr;
				force_log = true;
			}
			st.left_panel = left;
			st.left_x = 0;
			st.pane_gap = 0;
			st.view_top = 0;
			st.bottom_margin = 0;
			st.right_margin = 0;

			int left_w = st.left_panel_w > 0 ? st.left_panel_w : st.stock_left_panel_w;
			if (left_w < 80) {
				left_w = st.stock_left_panel_w >= 80 ? st.stock_left_panel_w : 220;
			}
			if (left_w > 640) {
				left_w = 640;
			}
			const int max_left = i_max(80, cw - 96);
			if (left_w > max_left) {
				left_w = max_left;
			}
			const int list_x = 0;
			const int view_x = left_w;
			const int view_w = i_max(1, cw - left_w);
			const int view_h = i_max(1, ch);
			const int pane_h = view_h;

			const bool size_changed = (cw != st.last_inner_w || ch != st.last_inner_h);
			// 8px: ChildClass 970 vs 978 (caption/client) must not retrigger
			// DeferWindowPos + posted WM_SIZE + list invalidate every sync tick.
			constexpr int kPaneSlack = 8;
			bool need_force = size_changed || force_log || !viewport;
			if (viewport)
			{
				const RECT vr = map_child_to_parent(st.editor, viewport);
				if (i_abs(vr.left - view_x) > kPaneSlack ||
					i_abs(vr.right - cw) > kPaneSlack ||
					i_abs((vr.right - vr.left) - view_w) > kPaneSlack ||
					i_abs((vr.bottom - vr.top) - view_h) > kPaneSlack ||
					i_abs(vr.top) > kPaneSlack)
				{
					need_force = true;
				}
			}
			if (left)
			{
				const RECT lr = map_child_to_parent(st.editor, left);
				if (i_abs(lr.left - list_x) > kPaneSlack ||
					i_abs((lr.right - lr.left) - left_w) > kPaneSlack ||
					i_abs((lr.bottom - lr.top) - pane_h) > kPaneSlack)
				{
					need_force = true;
				}
			}

			if (force_log || size_changed)
			{
				shared::common::log("EditorFrame",
					std::format(
						"inner layout client={}x{} kids={} list=0..{} ChildClass={}x{}@{},0 "
						"right_pin={} force={} vp=0x{:X} left=0x{:X}",
						cw, ch, nk, left_w, view_w, view_h, view_x,
						view_x + view_w,
						need_force ? 1 : 0,
						reinterpret_cast<std::uintptr_t>(viewport),
						reinterpret_cast<std::uintptr_t>(left)),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				st.last_inner_w = cw;
				st.last_inner_h = ch;
			}

			if (!need_force) {
				return;
			}

			HDWP dwp = BeginDeferWindowPos(nk + 4);
			auto defer = [&](HWND hwnd, int x, int y, int w, int h)
			{
				if (!hwnd) {
					return;
				}
				if (dwp)
				{
					dwp = DeferWindowPos(dwp, hwnd, nullptr, x, y, w, h,
						SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
				}
				else
				{
					SetWindowPos(hwnd, nullptr, x, y, w, h,
						SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
				}
			};

			if (left) {
				defer(left, list_x, 0, left_w, pane_h);
			}
			else
			{
				for (int i = 0; i < nk; ++i)
				{
					kid& k = kids[i];
					if (!k.vis || k.hwnd == viewport || k.hwnd == status || k.hwnd == splitter) {
						continue;
					}
					if (k.r.right <= view_x + 8)
					{
						defer(k.hwnd, list_x, 0, left_w, pane_h);
					}
				}
			}
			if (splitter) {
				defer(splitter, left_w, 0, 1, pane_h);
			}
			if (viewport) {
				defer(viewport, view_x, 0, view_w, view_h);
			}
			for (int i = 0; i < nk; ++i)
			{
				kid& k = kids[i];
				if (!k.vis || k.hwnd == viewport || k.hwnd == left ||
					k.hwnd == status || k.hwnd == splitter)
				{
					continue;
				}
				if (left && (k.hwnd == left || IsChild(left, k.hwnd))) {
					continue;
				}
				if (k.r.left >= view_x - 24 ||
					(k.r.right > view_x + 32 && k.r.left > left_w / 2))
				{
					defer(k.hwnd, view_x, 0, view_w, view_h);
				}
			}
			if (dwp) {
				EndDeferWindowPos(dwp);
			}

			HWND list = left ? list_in_panel(left) : nullptr;
			if (list && list != left)
			{
				RECT cc{};
				GetClientRect(left, &cc);
				SetWindowPos(list, nullptr, 0, 0,
					i_max(1, cc.right - cc.left), i_max(1, cc.bottom - cc.top),
					SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
			}
			if (viewport)
			{
				RECT cc{};
				GetClientRect(viewport, &cc);
				const int pw = i_max(1, cc.right - cc.left);
				const int ph = i_max(1, cc.bottom - cc.top);
				for (HWND c = GetWindow(viewport, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
				{
					if (!IsWindowVisible(c)) {
						continue;
					}
					RECT kidr = map_child_to_parent(viewport, c);
					const int cwnd = kidr.right - kidr.left;
					const int chnd = kidr.bottom - kidr.top;
					if (cwnd * 2 < pw && chnd * 2 < ph) {
						continue;
					}
					SetWindowPos(c, nullptr, 0, 0, pw, ph,
						SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
				}
				// Hang-safe: Post, never SendMessage, into ChildClass (aspect /
				// PerspectiveFovLH / SetViewport follow this size). Skip when
				// the client already matches — posted WM_SIZE flaps 970/978
				// and Remix treats each as a new MAIN.
				if (i_abs(pw - view_w) > 8 || i_abs(ph - view_h) > 8)
				{
					PostMessageA(viewport, WM_SIZE, SIZE_RESTORED, MAKELPARAM(pw, ph));
				}
			}
			if (list) {
				InvalidateRect(list, nullptr, FALSE);
			}
			else if (left) {
				InvalidateRect(left, nullptr, FALSE);
			}
			request_hit_sync(st);
		}

		static void editor_fill_rect(const frame_state& st, int& x, int& y, int& w, int& h)
		{
			RECT crc{};
			GetClientRect(st.wrapper, &crc);
			POINT origin{ 0, st.caption_h };
			ClientToScreen(st.wrapper, &origin);
			x = origin.x;
			y = origin.y;
			w = std::max(1, static_cast<int>(crc.right - crc.left));
			h = std::max(1, static_cast<int>(crc.bottom - crc.top - st.caption_h));
		}

		static void request_inner_layout(frame_state& st)
		{
			if (!st.wrapper || !IsWindow(st.wrapper) || st.sizing || st.inner_posted) {
				return;
			}
			st.inner_posted = true;
			PostMessageA(st.wrapper, kLayoutInnerMsg, 0, 0);
		}

		// After maximize / settle: ChildClass client (inner 3D, not caption /
		// list chrome) drives 3D Rad's WM_SIZE → Reset / SetViewport.
		// Post, never SendMessage. Proxy does not Reset (no 1s sleep).
		static void notify_inner_d3d_size(frame_state& st)
		{
			if (!shared::globals::is_editor_host || !st.editor || !IsWindow(st.editor)) {
				return;
			}
			HWND vp = find_named_child(st.editor, kViewportClass);
			if (!vp || !IsWindow(vp)) {
				return;
			}
			RECT cc{};
			GetClientRect(vp, &cc);
			const int w = i_max(1, static_cast<int>(cc.right - cc.left));
			const int h = i_max(1, static_cast<int>(cc.bottom - cc.top));
			if (w < 64 || h < 64) {
				return;
			}
			if (i_abs(w - st.last_d3d_vp_w) <= 2 && i_abs(h - st.last_d3d_vp_h) <= 2) {
				return;
			}
			st.last_d3d_vp_w = w;
			st.last_d3d_vp_h = h;
			const bool zoomed = st.wrapper && IsZoomed(st.wrapper);
			const WPARAM how = zoomed ? SIZE_MAXIMIZED : SIZE_RESTORED;
			PostMessageA(vp, WM_SIZE, how, MAKELPARAM(w, h));
			editor_settings::sync_hud_hits(vp);
			shared::common::log("EditorFrame",
				std::format(
					"settled inner rescale PostMessage(ChildClass, WM_SIZE, {}) "
					"{}x{} hwnd=0x{:X} (3D Rad Reset/SetViewport; proxy does not Reset)",
					zoomed ? "SIZE_MAXIMIZED" : "SIZE_RESTORED",
					w, h, reinterpret_cast<std::uintptr_t>(vp)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		static void request_settled_rescale(frame_state& st)
		{
			if (!shared::globals::is_editor_host) {
				return;
			}
			if (!st.wrapper || !IsWindow(st.wrapper) || st.sizing) {
				return;
			}
			SetTimer(st.wrapper, kRescaleTimer, kRescaleDebounceMs, nullptr);
		}

		static void layout_editor(frame_state& st, bool relayout_inner = true)
		{
			if (!st.wrapper || !st.editor || !IsWindow(st.editor) || st.layout_lock) {
				return;
			}
			if (IsIconic(st.wrapper)) {
				return;
			}

			int x = 0, y = 0, w = 1, h = 1;
			editor_fill_rect(st, x, y, w, h);

			RECT er{};
			GetWindowRect(st.editor, &er);
			const bool same =
				er.left == x && er.top == y &&
				(er.right - er.left) == w && (er.bottom - er.top) == h;

			scoped_flag lock(st.layout_lock);
			if (!same)
			{
				SetWindowPos(st.editor, HWND_TOP, x, y, w, h,
					SWP_NOACTIVATE | SWP_NOOWNERZORDER);
			}
			clip_editor_to_client(st);
			if (relayout_inner && !st.sizing) {
				request_inner_layout(st);
			}
		}

		static void pin_editor_pos(frame_state& st, WINDOWPOS* wp)
		{
			if (!wp || st.layout_lock || !st.wrapper || !IsWindow(st.wrapper)) {
				return;
			}
			if (IsIconic(st.wrapper)) {
				return;
			}
			int x = 0, y = 0, w = 1, h = 1;
			editor_fill_rect(st, x, y, w, h);
			if (!(wp->flags & SWP_NOMOVE))
			{
				wp->x = x;
				wp->y = y;
			}
			if (!(wp->flags & SWP_NOSIZE))
			{
				wp->cx = w;
				wp->cy = h;
			}
		}

		static void strip_editor_chrome(HWND editor)
		{
			// Keep 3DRADCLASS top-level (WS_POPUP, never WS_CHILD). MFC file
			// dialogs and modal pickers refuse a child hwnd as owner.
			const LONG_PTR style = GetWindowLongPtrA(editor, GWL_STYLE);
			LONG_PTR new_style = style;
			new_style &= ~(WS_CHILD | WS_CAPTION | WS_THICKFRAME | WS_DLGFRAME |
				WS_BORDER | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
			new_style |= WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;

			const LONG_PTR ex = GetWindowLongPtrA(editor, GWL_EXSTYLE);
			LONG_PTR new_ex = ex;
			new_ex &= ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME |
				WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_OVERLAPPEDWINDOW);
			new_ex |= WS_EX_TOOLWINDOW;

			const bool style_changed = new_style != style || new_ex != ex;
			if (style_changed)
			{
				SetWindowLongPtrA(editor, GWL_STYLE, new_style);
				SetWindowLongPtrA(editor, GWL_EXSTYLE, new_ex);
			}

			if (GetMenu(editor)) {
				SetMenu(editor, nullptr);
			}

			if (style_changed)
			{
				SetWindowPos(editor, nullptr, 0, 0, 0, 0,
					SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
			}
		}

		static bool own_editor(HWND editor, HWND wrapper)
		{
			// Owner, not parent: GWLP_HWNDPARENT on a top-level popup. SetParent
			// would force WS_CHILD and break GetOpenFileName / DialogBoxParam.
			SetLastError(0);
			SetWindowLongPtrA(editor, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(wrapper));
			const DWORD err = GetLastError();
			const HWND owner = GetWindow(editor, GW_OWNER);
			const HWND parent = GetParent(editor);
			const HWND root = GetAncestor(editor, GA_ROOT);
			const LONG_PTR style = GetWindowLongPtrA(editor, GWL_STYLE);
			shared::common::log("EditorFrame",
				std::format(
					"own 3DRADCLASS=0x{:X} via GWLP_HWNDPARENT wrapper=0x{:X} "
					"owner=0x{:X} parent=0x{:X} root=0x{:X} WS_CHILD={} WS_POPUP={} err=0x{:X}",
					reinterpret_cast<std::uintptr_t>(editor),
					reinterpret_cast<std::uintptr_t>(wrapper),
					reinterpret_cast<std::uintptr_t>(owner),
					reinterpret_cast<std::uintptr_t>(parent),
					reinterpret_cast<std::uintptr_t>(root),
					(style & WS_CHILD) ? 1 : 0,
					(style & WS_POPUP) ? 1 : 0,
					err),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			if (root != editor)
			{
				shared::common::log("EditorFrame",
					std::format("3DRADCLASS GA_ROOT=0x{:X} (expected self) — dialogs may still work if not WS_CHILD",
						reinterpret_cast<std::uintptr_t>(root)),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
			return owner == wrapper && (style & WS_CHILD) == 0;
		}

		static void request_editor_close(frame_state& st)
		{
			if (st.closing) {
				return;
			}
			st.closing = true;
			if (st.editor && IsWindow(st.editor)) {
				PostMessageA(st.editor, WM_CLOSE, 0, 0);
			}
			else if (st.wrapper) {
				DestroyWindow(st.wrapper);
			}
		}

		static LRESULT CALLBACK editor_subclass_proc(HWND hwnd, UINT msg,
			WPARAM wparam, LPARAM lparam, UINT_PTR /*id*/, DWORD_PTR ref)
		{
			auto* st = reinterpret_cast<frame_state*>(ref);
			if (!st) {
				return DefSubclassProc(hwnd, msg, wparam, lparam);
			}

			if (msg == WM_WINDOWPOSCHANGING)
			{
				pin_editor_pos(*st, reinterpret_cast<WINDOWPOS*>(lparam));
			}
			else if (msg == WM_NCHITTEST)
			{
				POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
				if (point_in_wrapper_caption(*st, pt) ||
					point_in_wrapper_resize_border(*st, pt))
				{
					return HTTRANSPARENT;
				}
			}
			else if (msg == WM_MOUSEACTIVATE)
			{
				POINT pt{};
				GetCursorPos(&pt);
				if (point_in_wrapper_caption(*st, pt)) {
					return MA_NOACTIVATE;
				}
			}
			else if (msg == WM_NCLBUTTONDOWN || msg == WM_NCLBUTTONUP ||
				msg == WM_NCLBUTTONDBLCLK || msg == WM_NCMOUSEMOVE ||
				msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
				msg == WM_LBUTTONDBLCLK || msg == WM_MOUSEMOVE)
			{
				POINT screen{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
				if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
					msg == WM_LBUTTONDBLCLK || msg == WM_MOUSEMOVE)
				{
					ClientToScreen(hwnd, &screen);
				}
				if (st->wrapper && IsWindow(st->wrapper) && point_in_wrapper_caption(*st, screen))
				{
					POINT client = screen;
					ScreenToClient(st->wrapper, &client);
					const UINT wrap_msg =
						(msg == WM_NCLBUTTONDOWN || msg == WM_LBUTTONDOWN) ? WM_LBUTTONDOWN :
						(msg == WM_NCLBUTTONUP || msg == WM_LBUTTONUP) ? WM_LBUTTONUP :
						(msg == WM_NCLBUTTONDBLCLK || msg == WM_LBUTTONDBLCLK) ? WM_LBUTTONDBLCLK :
						WM_MOUSEMOVE;
					const LPARAM lp = MAKELPARAM(client.x, client.y);
					if (wrap_msg == WM_MOUSEMOVE) {
						SendMessageA(st->wrapper, wrap_msg, wparam, lp);
					}
					else {
						PostMessageA(st->wrapper, wrap_msg, wparam, lp);
					}
					return 0;
				}
			}
			else if (msg == WM_MEASUREITEM)
			{
				const LRESULT r = DefSubclassProc(hwnd, msg, wparam, lparam);
				editor_settings::measure_object_list(
					reinterpret_cast<MEASUREITEMSTRUCT*>(lparam));
				return r;
			}
			else if (msg == WM_DRAWITEM)
			{
				// Engine BitBlts the 350x16 item*.bmp (C-suffix = shown) first.
				// Then we sample that strip and overpaint gradients + GDI check.
				const LRESULT r = DefSubclassProc(hwnd, msg, wparam, lparam);
				if (editor_settings::restyle_object_list_item(
					reinterpret_cast<DRAWITEMSTRUCT*>(lparam)))
				{
					return TRUE;
				}
				return r;
			}
			else if (msg == WM_COMMAND)
			{
				if (handle_help_command(LOWORD(wparam), *st)) {
					return 0;
				}
				const LRESULT r = DefSubclassProc(hwnd, msg, wparam, lparam);
				editor_settings::on_object_list_command(
					reinterpret_cast<HWND>(lparam), HIWORD(wparam));
				return r;
			}
			else if (msg == WM_SIZE)
			{
				const LRESULT r = DefSubclassProc(hwnd, msg, wparam, lparam);
				if (!st->layout_lock && !st->inner_lock && !st->sizing) {
					layout_inner_panes(*st, false);
				}
				return r;
			}
			else if (msg == WM_SETTEXT)
			{
				const LRESULT r = DefSubclassProc(hwnd, msg, wparam, lparam);
				if (st->wrapper && IsWindow(st->wrapper)) {
					copy_title(st->wrapper, hwnd);
					InvalidateRect(st->wrapper, nullptr, FALSE);
				}
				return r;
			}
			else if (msg == WM_STYLECHANGING)
			{
				if (wparam == GWL_STYLE)
				{
					auto* ss = reinterpret_cast<STYLESTRUCT*>(lparam);
					if (ss)
					{
						ss->styleNew &= ~(WS_CHILD | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
							WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME);
						ss->styleNew |= WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
					}
				}
				else if (wparam == GWL_EXSTYLE)
				{
					auto* ss = reinterpret_cast<STYLESTRUCT*>(lparam);
					if (ss)
					{
						ss->styleNew &= ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE |
							WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE |
							WS_EX_OVERLAPPEDWINDOW);
						ss->styleNew |= WS_EX_TOOLWINDOW;
					}
				}
			}
			else if (msg == WM_CLOSE)
			{
				st->closing = true;
			}
			else if (msg == WM_NCDESTROY)
			{
				RemoveWindowSubclass(hwnd, editor_subclass_proc, 1);
				st->editor = nullptr;
				if (st->wrapper && IsWindow(st->wrapper)) {
					DestroyWindow(st->wrapper);
				}
			}

			return DefSubclassProc(hwnd, msg, wparam, lparam);
		}

		static LRESULT hit_test_wrapper(HWND hwnd, frame_state& st, LPARAM lparam)
		{
			POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
			RECT wr{};
			GetWindowRect(hwnd, &wr);

			POINT cpt = pt;
			ScreenToClient(hwnd, &cpt);
			if (cpt.y >= 0 && cpt.y < st.caption_h)
			{
				layout_caption_items(hwnd, st);
				if (hit_button(st, hwnd, cpt) != caption_btn::none) {
					return HTCLIENT;
				}
				if (hit_menu(st, cpt) >= 0) {
					return HTCLIENT;
				}
			}

			const int b = resize_border_px(st, hwnd);
			if (b > 0)
			{
				const bool top = pt.y < wr.top + b;
				const bool bottom = pt.y >= wr.bottom - b;
				const bool left = pt.x < wr.left + b;
				const bool right = pt.x >= wr.right - b;
				if (top && left) {
					return HTTOPLEFT;
				}
				if (top && right) {
					return HTTOPRIGHT;
				}
				if (bottom && left) {
					return HTBOTTOMLEFT;
				}
				if (bottom && right) {
					return HTBOTTOMRIGHT;
				}
				if (top) {
					return HTTOP;
				}
				if (bottom) {
					return HTBOTTOM;
				}
				if (left) {
					return HTLEFT;
				}
				if (right) {
					return HTRIGHT;
				}
			}

			if (cpt.y >= 0 && cpt.y < st.caption_h) {
				return HTCAPTION;
			}
			return HTCLIENT;
		}

		static void invalidate_caption(HWND hwnd, const frame_state& st)
		{
			RECT cap{};
			GetClientRect(hwnd, &cap);
			cap.bottom = i_max(st.caption_h, 24);
			InvalidateRect(hwnd, &cap, FALSE);
		}

		static void on_caption_click(HWND hwnd, frame_state& st, caption_btn btn)
		{
			if (btn == caption_btn::min) {
				ShowWindow(hwnd, SW_MINIMIZE);
			}
			else if (btn == caption_btn::max) {
				ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
			}
			else if (btn == caption_btn::close) {
				request_editor_close(st);
			}
		}

		static bool handle_caption_mouse(HWND hwnd, frame_state& st, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			(void)wparam;
			if (msg != WM_MOUSEMOVE && msg != WM_NCMOUSEMOVE &&
				msg != WM_LBUTTONDOWN && msg != WM_NCLBUTTONDOWN &&
				msg != WM_LBUTTONUP && msg != WM_NCLBUTTONUP &&
				msg != WM_LBUTTONDBLCLK && msg != WM_NCLBUTTONDBLCLK &&
				msg != WM_MOUSELEAVE && msg != WM_NCMOUSELEAVE)
			{
				return false;
			}

			if (msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE)
			{
				st.hover = caption_btn::none;
				st.pressed = caption_btn::none;
				if (st.menu_open < 0) {
					st.menu_hover = -1;
				}
				InvalidateRect(hwnd, nullptr, FALSE);
				return true;
			}

			POINT pt = mouse_to_client(hwnd, msg, lparam);
			layout_caption_items(hwnd, st);
			const bool in_cap = pt.y >= 0 && pt.y < st.caption_h;
			const caption_btn btn = in_cap ? hit_button(st, hwnd, pt) : caption_btn::none;
			const int menu = (in_cap && btn == caption_btn::none) ? hit_menu(st, pt) : -1;

			if (msg == WM_MOUSEMOVE || msg == WM_NCMOUSEMOVE)
			{
				if (btn != st.hover || menu != st.menu_hover)
				{
					st.hover = btn;
					st.menu_hover = menu;
					InvalidateRect(hwnd, nullptr, FALSE);
				}
				if (in_cap)
				{
					TRACKMOUSEEVENT tme{};
					tme.cbSize = sizeof(tme);
					tme.dwFlags = TME_LEAVE;
					if (msg == WM_NCMOUSEMOVE) {
						tme.dwFlags |= TME_NONCLIENT;
					}
					tme.hwndTrack = hwnd;
					TrackMouseEvent(&tme);
					return true;
				}
				return false;
			}

			if (msg == WM_LBUTTONDOWN || msg == WM_NCLBUTTONDOWN)
			{
				if (!in_cap) {
					return false;
				}
				if (menu >= 0)
				{
					log_caption_hit("down-menu", caption_btn::none, menu, msg);
					open_caption_menu(hwnd, st, menu);
					return true;
				}
				if (btn != caption_btn::none)
				{
					log_caption_hit("down-btn", btn, -1, msg);
					st.pressed = btn;
					SetCapture(hwnd);
					InvalidateRect(hwnd, nullptr, FALSE);
					return true;
				}
				return false;
			}

			if (msg == WM_LBUTTONUP || msg == WM_NCLBUTTONUP)
			{
				const caption_btn was = st.pressed;
				st.pressed = caption_btn::none;
				if (GetCapture() == hwnd) {
					ReleaseCapture();
				}

				caption_btn fire = caption_btn::none;
				int fire_menu = -1;
				const char* how = "";
				if (was != caption_btn::none && was == btn)
				{
					fire = was;
					how = "up-match";
				}
				else if (was == caption_btn::none && btn != caption_btn::none)
				{
					fire = btn;
					how = "up-hover";
				}
				else if (was == caption_btn::none && menu >= 0)
				{
					fire_menu = menu;
					how = "up-hover-menu";
				}

				if (fire != caption_btn::none)
				{
					log_caption_hit(how, fire, -1, msg);
					on_caption_click(hwnd, st, fire);
					InvalidateRect(hwnd, nullptr, FALSE);
					return true;
				}
				if (fire_menu >= 0)
				{
					log_caption_hit(how, caption_btn::none, fire_menu, msg);
					open_caption_menu(hwnd, st, fire_menu);
					return true;
				}
				if (was != caption_btn::none)
				{
					InvalidateRect(hwnd, nullptr, FALSE);
					return true;
				}
				return false;
			}

			if (msg == WM_LBUTTONDBLCLK || msg == WM_NCLBUTTONDBLCLK)
			{
				if (st.can_resize && in_cap && btn == caption_btn::none && menu < 0)
				{
					log_caption_hit("dblclk-caption", caption_btn::none, -1, msg);
					ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
					return true;
				}
				if (in_cap && (btn != caption_btn::none || menu >= 0)) {
					return true;
				}
				return false;
			}

			return false;
		}

		static bool forward_key_to_editor(frame_state& st, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			if (!st.editor || !IsWindow(st.editor) || !is_key_msg(msg)) {
				return false;
			}
			if (imgui_wants_keys())
			{
				HWND dest = shared::globals::main_window;
				if (dest && IsWindow(dest)) {
					SendMessageA(dest, msg, wparam, lparam);
				}
				return true;
			}

			if ((msg == WM_SYSKEYDOWN || msg == WM_SYSCHAR) &&
				(GetKeyState(VK_MENU) & 0x8000) &&
				!(GetKeyState(VK_CONTROL) & 0x8000))
			{
				const int idx = menu_index_for_key(st, wparam);
				if (idx >= 0 && st.wrapper)
				{
					PostMessageA(st.wrapper, kOpenMenuMsg, static_cast<WPARAM>(idx), 0);
					return true;
				}
			}

			if (st.accel)
			{
				MSG m{};
				m.hwnd = st.editor;
				m.message = msg;
				m.wParam = wparam;
				m.lParam = lparam;
				m.time = GetMessageTime();
				GetCursorPos(&m.pt);
				if (TranslateAcceleratorA(st.editor, st.accel, &m)) {
					return true;
				}
			}

			SendMessageA(st.editor, msg, wparam, lparam);
			return true;
		}

		static LRESULT CALLBACK getmsg_proc(int code, WPARAM wparam, LPARAM lparam)
		{
			if (code >= 0 && wparam == PM_REMOVE && lparam)
			{
				MSG* m = reinterpret_cast<MSG*>(lparam);
				frame_state& st = g_state;
				if (m && st.wrapped && st.editor && IsWindow(st.editor) && st.wrapper)
				{
					HWND hit = nullptr;
					if (m->message >= WM_MOUSEFIRST && m->message <= WM_MOUSELAST) {
						hit = WindowFromPoint(m->pt);
					}

					const bool settings_msg =
						editor_settings::is_settings_hwnd(m->hwnd) ||
						editor_settings::is_settings_hwnd(hit) ||
						comp::game::particles::is_remix_pane_hwnd(m->hwnd) ||
						comp::game::particles::is_remix_pane_hwnd(hit);

					if (settings_msg)
					{
						if (editor_settings::filter_message(m) ||
							comp::game::particles::filter_remix_pane_message(m)) {
							m->message = WM_NULL;
						}
						return CallNextHookEx(nullptr, code, wparam, lparam);
					}

					editor_settings::rewrite_object_list_click(m);

					const bool ours = m->hwnd == st.wrapper || m->hwnd == st.editor ||
						(m->hwnd && (IsChild(st.wrapper, m->hwnd) || IsChild(st.editor, m->hwnd)));

					if (ours && is_menu_cmd_msg(m->message) && m->hwnd == st.wrapper)
					{
						m->hwnd = st.editor;
					}

					if (ours && is_key_msg(m->message))
					{
						if (imgui_wants_keys())
						{
							if (m->hwnd == st.wrapper)
							{
								HWND dest = shared::globals::main_window;
								if (dest && IsWindow(dest)) {
									m->hwnd = dest;
								}
							}
						}
						else
						{
							if ((m->message == WM_SYSKEYDOWN || m->message == WM_SYSCHAR) &&
								(GetKeyState(VK_MENU) & 0x8000) &&
								!(GetKeyState(VK_CONTROL) & 0x8000))
							{
								const int idx = menu_index_for_key(st, m->wParam);
								if (idx >= 0)
								{
									PostMessageA(st.wrapper, kOpenMenuMsg, static_cast<WPARAM>(idx), 0);
									m->message = WM_NULL;
								}
							}

							if (m->message != WM_NULL && m->hwnd == st.wrapper) {
								m->hwnd = st.editor;
							}
						}
					}
				}
			}
			return CallNextHookEx(nullptr, code, wparam, lparam);
		}

		static LRESULT CALLBACK wrapper_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			auto* st = reinterpret_cast<frame_state*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
			if (msg == WM_NCCREATE)
			{
				auto* cs = reinterpret_cast<CREATESTRUCTA*>(lparam);
				st = static_cast<frame_state*>(cs->lpCreateParams);
				SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
			}
			if (!st) {
				return DefWindowProcA(hwnd, msg, wparam, lparam);
			}

			// Caption hit-test and mouse must beat DwmDefWindowProc — DWM
			// otherwise eats clicks on a custom frame while still allowing
			// WM_MOUSEMOVE hover. Size/frame calc must also beat DWM so the
			// 8px resize inset is not replaced by a full-client frame.
			if (msg == WM_NCHITTEST) {
				return hit_test_wrapper(hwnd, *st, lparam);
			}
			if (handle_caption_mouse(hwnd, *st, msg, wparam, lparam)) {
				return 0;
			}
			if (msg == WM_NCCALCSIZE && wparam)
			{
				auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
				if (!IsZoomed(hwnd) && st->can_resize)
				{
					const int b = resize_border_px(*st, hwnd);
					p->rgrc[0].left += b;
					p->rgrc[0].right -= b;
					p->rgrc[0].bottom -= b;
				}
				return 0;
			}
			if (msg == WM_GETMINMAXINFO)
			{
				auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
				const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
				MONITORINFO mi{};
				mi.cbSize = sizeof(mi);
				if (GetMonitorInfoA(mon, &mi))
				{
					mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
					mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
					mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
					mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
					mmi->ptMaxTrackSize = mmi->ptMaxSize;
				}
				mmi->ptMinTrackSize.x = scale_px(320, st->dpi);
				mmi->ptMinTrackSize.y = scale_px(240, st->dpi);
				return 0;
			}
			if (msg == WM_ENTERSIZEMOVE)
			{
				st->sizing = true;
				return 0;
			}
			if (msg == WM_EXITSIZEMOVE)
			{
				st->sizing = false;
				layout_editor(*st, false);
				request_inner_layout(*st);
				request_settled_rescale(*st);
				invalidate_caption(hwnd, *st);
				return 0;
			}
			if (msg == WM_SIZE)
			{
				if (wparam == SIZE_MINIMIZED || st->layout_lock) {
					return 0;
				}
				if (wparam == SIZE_MAXIMIZED)
				{
					shared::common::log("EditorFrame",
						"SIZE_MAXIMIZED — layout editor, debounce inner D3D rescale",
						shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				}
				layout_editor(*st, false);
				if (!st->sizing) {
					request_inner_layout(*st);
					if (wparam == SIZE_MAXIMIZED || wparam == SIZE_RESTORED) {
						request_settled_rescale(*st);
					}
				}
				invalidate_caption(hwnd, *st);
				return 0;
			}

			if (msg == WM_NCLBUTTONDOWN || msg == WM_NCLBUTTONDBLCLK)
			{
				const LRESULT ht = hit_test_wrapper(hwnd, *st, lparam);
				if (ht >= HTLEFT && ht <= HTBOTTOMRIGHT) {
					return DefWindowProcA(hwnd, msg, wparam, lparam);
				}
			}

			LRESULT dwm = 0;
			if (DwmDefWindowProc(hwnd, msg, wparam, lparam, &dwm)) {
				return dwm;
			}

			switch (msg)
			{
			case WM_NCACTIVATE:
				invalidate_caption(hwnd, *st);
				return TRUE;
			case WM_NCPAINT:
				invalidate_caption(hwnd, *st);
				return 0;
			case kLayoutInnerMsg:
				st->inner_posted = false;
				if (!st->sizing && !st->layout_lock) {
					layout_inner_panes(*st, false);
				}
				invalidate_caption(hwnd, *st);
				return 0;
			case kSyncHitsMsg:
				st->hit_posted = false;
				if (!st->sizing)
				{
					HWND vp = find_named_child(st->editor, kViewportClass);
					editor_settings::sync_hud_hits(vp);
				}
				return 0;
			case WM_MOVE:
				if (!st->layout_lock && !st->sizing && !IsIconic(hwnd) && !IsZoomed(hwnd)) {
					layout_editor(*st, false);
				}
				return 0;
			case WM_PAINT:
				paint_caption(hwnd, *st);
				return 0;
			case WM_ERASEBKGND:
			{
				HDC hdc = reinterpret_cast<HDC>(wparam);
				RECT crc{};
				GetClientRect(hwnd, &crc);
				crc.bottom = st->caption_h;
				HBRUSH bg = CreateSolidBrush(kBg);
				FillRect(hdc, &crc, bg);
				DeleteObject(bg);
				return 1;
			}
			case WM_MOUSEACTIVATE:
				if (cursor_in_caption(*st)) {
					return MA_ACTIVATE;
				}
				break;
			case kOpenMenuMsg:
				open_caption_menu(hwnd, *st, static_cast<int>(wparam));
				return 0;
			case WM_INITMENU:
			case WM_INITMENUPOPUP:
			case WM_UNINITMENUPOPUP:
			case WM_MENUSELECT:
			case WM_ENTERMENULOOP:
			case WM_EXITMENULOOP:
			case WM_ENTERIDLE:
			case WM_NEXTMENU:
			case WM_MENUCHAR:
				if (st->editor && IsWindow(st->editor)) {
					return SendMessageA(st->editor, msg, wparam, lparam);
				}
				break;
			case WM_COMMAND:
				if (handle_help_command(LOWORD(wparam), *st)) {
					return 0;
				}
				if (handle_shaders_command(LOWORD(wparam), hwnd, *st)) {
					return 0;
				}
				if (editor_settings::handle_menu_command(LOWORD(wparam), hwnd)) {
					return 0;
				}
				dispatch_editor_command(*st, LOWORD(wparam), nullptr, "wrapper WM_COMMAND");
				return 0;
			case WM_SYSCOMMAND:
				if ((wparam & 0xFFF0) == SC_CLOSE) {
					request_editor_close(*st);
					return 0;
				}
				if ((wparam & 0xFFF0) == SC_KEYMENU)
				{
					const int idx = menu_index_for_key(*st, lparam);
					open_caption_menu(hwnd, *st, idx >= 0 ? idx : 0);
					return 0;
				}
				break;
			case WM_SETFOCUS:
				if (editor_settings::is_settings_hwnd(GetForegroundWindow()) ||
					editor_settings::is_settings_hwnd(GetFocus()) ||
					comp::game::particles::is_remix_pane_hwnd(GetForegroundWindow()) ||
					comp::game::particles::is_remix_pane_hwnd(GetFocus()))
				{
					return 0;
				}
				if (!cursor_in_caption(*st) && st->pressed == caption_btn::none &&
					st->menu_open < 0)
				{
					focus_editor(*st);
				}
				return 0;
			case WM_ACTIVATE:
				if (LOWORD(wparam) != WA_INACTIVE)
				{
					if (editor_settings::is_settings_hwnd(GetForegroundWindow()) ||
						editor_settings::is_settings_hwnd(GetFocus()) ||
						comp::game::particles::is_remix_pane_hwnd(GetForegroundWindow()) ||
						comp::game::particles::is_remix_pane_hwnd(GetFocus()))
					{
						break;
					}
					if (!cursor_in_caption(*st) && st->pressed == caption_btn::none &&
						st->menu_open < 0)
					{
						activate_editor_frame(*st, true);
						focus_editor(*st);
					}
				}
				else {
					activate_editor_frame(*st, false);
				}
				break;
			case WM_SHOWWINDOW:
				if (st->editor && IsWindow(st->editor))
				{
					ShowWindow(st->editor, wparam ? SW_SHOW : SW_HIDE);
					if (wparam) {
						layout_editor(*st, false);
					}
				}
				break;
			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
			case WM_CHAR:
			case WM_SYSCHAR:
			case WM_DEADCHAR:
			case WM_SYSDEADCHAR:
			case WM_APPCOMMAND:
				if (forward_key_to_editor(*st, msg, wparam, lparam)) {
					return 0;
				}
				break;
			case WM_CLOSE:
				request_editor_close(*st);
				return 0;
			case WM_TIMER:
				if (wparam == kHitWriteTimer)
				{
					KillTimer(hwnd, kHitWriteTimer);
					if (!st->sizing && st->editor && IsWindow(st->editor))
					{
						HWND vp = find_named_child(st->editor, kViewportClass);
						editor_settings::sync_hud_hits(vp);
					}
					return 0;
				}
				if (wparam == kRescaleTimer)
				{
					KillTimer(hwnd, kRescaleTimer);
					if (!st->sizing && !st->layout_lock && st->editor &&
						IsWindow(st->editor) && !IsIconic(hwnd))
					{
						layout_inner_panes(*st, true);
						notify_inner_d3d_size(*st);
					}
					return 0;
				}
				if (wparam == kSyncTimer)
				{
					if (!st->editor || !IsWindow(st->editor))
					{
						KillTimer(hwnd, kSyncTimer);
						DestroyWindow(hwnd);
						return 0;
					}
					if (st->layout_lock || st->sizing) {
						return 0;
					}
					copy_title(hwnd, st->editor);
					strip_editor_chrome(st->editor);
					game::particles::install_ui_hooks();
					project_file::poll();
					if (GetWindow(st->editor, GW_OWNER) != hwnd) {
						own_editor(st->editor, hwnd);
					}
					int x = 0, y = 0, w = 1, h = 1;
					editor_fill_rect(*st, x, y, w, h);
					RECT er{};
					GetWindowRect(st->editor, &er);
					auto away = [](int a, int b) { return a > b ? a - b : b - a; };
					if (away(static_cast<int>(er.left), x) > 4 ||
						away(static_cast<int>(er.top), y) > 4 ||
						away(static_cast<int>(er.right - er.left), w) > 4 ||
						away(static_cast<int>(er.bottom - er.top), h) > 4)
					{
						layout_editor(*st, false);
					}
					if (!st->inner_lock) {
						layout_inner_panes(*st, false);
					}
				}
				return 0;
			case WM_DESTROY:
				KillTimer(hwnd, kSyncTimer);
				KillTimer(hwnd, kHitWriteTimer);
				KillTimer(hwnd, kRescaleTimer);
				if (st->getmsg_hook)
				{
					UnhookWindowsHookEx(st->getmsg_hook);
					st->getmsg_hook = nullptr;
				}
				if (st->menu_owned && st->editor_menu)
				{
					DestroyMenu(st->editor_menu);
					st->editor_menu = nullptr;
					st->menu_owned = false;
				}
				if (st->wrapper == hwnd) {
					st->wrapper = nullptr;
					st->wrapped = false;
				}
				return 0;
			default:
				break;
			}

			return DefWindowProcA(hwnd, msg, wparam, lparam);
		}

		static bool register_wrapper_class()
		{
			static bool done = false;
			if (done) {
				return true;
			}

			WNDCLASSEXA wc{};
			wc.cbSize = sizeof(wc);
			wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
			wc.lpfnWndProc = wrapper_proc;
			wc.hInstance = shared::globals::dll_hmodule;
			wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
			wc.hbrBackground = CreateSolidBrush(kBg);
			wc.lpszClassName = kWrapperClass;
			if (!RegisterClassExA(&wc))
			{
				if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
				{
					shared::common::log("EditorFrame",
						std::format("RegisterClassEx failed (0x{:X})", GetLastError()),
						shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
					return false;
				}
			}
			done = true;
			return true;
		}

		static bool install_wrapper(HWND editor)
		{
			if (!editor || !IsWindow(editor) || g_state.wrapped) {
				return g_state.wrapped;
			}
			if (!class_is(editor, kEditorClass)) {
				shared::common::log("EditorFrame",
					std::format("skip wrap — not 3DRADCLASS ({})", describe_hwnd(editor)),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return false;
			}
			if (GetAncestor(editor, GA_ROOT) != editor) {
				shared::common::log("EditorFrame",
					std::format("skip wrap — 3DRADCLASS is not a top-level root ({})",
						describe_hwnd(editor)),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return false;
			}

			RECT outer{};
			RECT client{};
			GetWindowRect(editor, &outer);
			GetClientRect(editor, &client);
			POINT client_origin{ 0, 0 };
			ClientToScreen(editor, &client_origin);

			const int keep_w = client.right - client.left;
			const int keep_h = client.bottom - client.top;
			if (keep_w < 64 || keep_h < 64)
			{
				shared::common::log("EditorFrame",
					std::format("skip wrap — editor client {}x{} is too small ({})",
						keep_w, keep_h, describe_hwnd(editor)),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return false;
			}

			const LONG_PTR old_style = GetWindowLongPtrA(editor, GWL_STYLE);
			const LONG_PTR old_ex = GetWindowLongPtrA(editor, GWL_EXSTYLE);
			g_state.can_resize = (old_style & WS_THICKFRAME) != 0;
			g_state.keep_client_w = keep_w;
			g_state.keep_client_h = keep_h;
			g_state.editor = editor;
			g_state.closing = false;
			g_state.layout_lock = false;
			g_state.inner_lock = false;
			g_state.sizing = false;
			g_state.inner_posted = false;
			g_state.hit_posted = false;
			g_state.last_rgn_w = -1;
			g_state.last_rgn_h = -1;

			shared::common::log("EditorFrame",
				std::format("wrap start on tid={} (editor tid={}): {}",
					GetCurrentThreadId(), GetWindowThreadProcessId(editor, nullptr),
					describe_hwnd(editor)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

			capture_editor_ui(g_state, editor);

			if (!register_wrapper_class()) {
				g_state.editor = nullptr;
				return false;
			}

			refresh_metrics(g_state, editor);

			DWORD wrap_style = WS_POPUP | WS_VISIBLE | WS_SYSMENU | WS_MINIMIZEBOX |
				WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
			if (g_state.can_resize) {
				wrap_style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
			}
			const DWORD wrap_ex = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;

			char title[512]{};
			GetWindowTextA(editor, title, sizeof(title));

			SetLastError(0);
			HWND wrapper = CreateWindowExA(
				wrap_ex, kWrapperClass,
				title[0] ? title : "3D Rad RTX",
				wrap_style,
				outer.left, outer.top,
				std::max(1, static_cast<int>(outer.right - outer.left)),
				std::max(1, static_cast<int>(outer.bottom - outer.top)),
				nullptr, nullptr, shared::globals::dll_hmodule, &g_state);
			if (!wrapper)
			{
				shared::common::log("EditorFrame",
					std::format("CreateWindowEx 3DRadRTXFrame failed (GetLastError=0x{:X})", GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				g_state.editor = nullptr;
				return false;
			}

			g_state.wrapper = wrapper;
			if (title[0]) {
				SetWindowTextA(wrapper, title);
			}
			else {
				copy_title(wrapper, editor);
			}
			copy_icons(wrapper, editor);
			apply_dark_frame(wrapper);
			refresh_metrics(g_state, wrapper);

			shared::common::log("EditorFrame",
				std::format("created 3DRadRTXFrame {} (editor style before strip 0x{:08X} ex=0x{:08X})",
					describe_hwnd(wrapper),
					static_cast<unsigned>(old_style),
					static_cast<unsigned>(old_ex)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

			// Preserve the CreateDevice / Remix hwnd. Keep 3DRADCLASS top-level
			// (owned popup in the wrapper client) so MFC dialogs have a
			// top-level owner. Do not SetParent / WS_CHILD. Do not replace
			// GWLP_WNDPROC — DefSubclassProc only.
			ShowWindow(editor, SW_HIDE);
			strip_editor_chrome(editor);
			if (!own_editor(editor, wrapper))
			{
				shared::common::log("EditorFrame",
					std::format("owner wrap failed — restoring original chrome ({})",
						describe_hwnd(editor)),
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				SetWindowLongPtrA(editor, GWLP_HWNDPARENT, 0);
				SetWindowLongPtrA(editor, GWL_STYLE, old_style);
				SetWindowLongPtrA(editor, GWL_EXSTYLE, old_ex);
				SetWindowPos(editor, nullptr, 0, 0, 0, 0,
					SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
				ShowWindow(editor, SW_SHOW);
				g_state.editor = nullptr;
				g_state.wrapper = nullptr;
				DestroyWindow(wrapper);
				return false;
			}
			strip_editor_chrome(editor);

			if (!SetWindowSubclass(editor, editor_subclass_proc, 1,
				reinterpret_cast<DWORD_PTR>(&g_state)))
			{
				shared::common::log("EditorFrame",
					std::format("SetWindowSubclass failed (0x{:X}) — wrap continues without subclass",
						GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}

			RECT wrap_rc{ 0, 0, keep_w, keep_h + g_state.caption_h };
			AdjustWindowRectEx(&wrap_rc, wrap_style, FALSE, wrap_ex);
			int wrap_w = wrap_rc.right - wrap_rc.left;
			int wrap_h = wrap_rc.bottom - wrap_rc.top;
			if (wrap_w < keep_w) {
				wrap_w = keep_w;
			}
			if (wrap_h < keep_h + g_state.caption_h) {
				wrap_h = keep_h + g_state.caption_h;
			}

			SetWindowPos(wrapper, HWND_TOP, outer.left, outer.top, wrap_w, wrap_h,
				SWP_FRAMECHANGED | SWP_SHOWWINDOW);
			ShowWindow(wrapper, SW_SHOW);
			layout_editor(g_state);

			POINT new_origin{ 0, 0 };
			ClientToScreen(editor, &new_origin);
			RECT wr{};
			GetWindowRect(wrapper, &wr);
			SetWindowPos(wrapper, nullptr,
				wr.left + (client_origin.x - new_origin.x),
				wr.top + (client_origin.y - new_origin.y),
				0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

			ShowWindow(editor, SW_SHOW);
			SetWindowPos(editor, HWND_TOP, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
			layout_editor(g_state);
			ShowWindow(wrapper, SW_SHOW);
			UpdateWindow(wrapper);
			BringWindowToTop(wrapper);
			activate_editor_frame(g_state, true);
			focus_editor(g_state);

			install_editor_proxy_hooks();

			g_state.getmsg_hook = SetWindowsHookExA(
				WH_GETMESSAGE, getmsg_proc, nullptr, GetCurrentThreadId());
			if (!g_state.getmsg_hook)
			{
				shared::common::log("EditorFrame",
					std::format("SetWindowsHookEx(WH_GETMESSAGE) failed (0x{:X}) — key forward uses WndProc only",
						GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
			else
			{
				shared::common::log("EditorFrame",
					"WH_GETMESSAGE installed — wrapper keys/commands rewrite hwnd to 3DRADCLASS",
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}

			const UINT open_id = find_menu_command(g_state.editor_menu, L"Open");
			const UINT add_id = find_menu_command(g_state.editor_menu, L"Add");
			shared::common::log("EditorFrame",
				std::format(
					"command path Open id={} Add id={} — Project/Object dispatch uses "
					"PostMessage(3DRADCLASS, WM_COMMAND, MAKEWPARAM(id,0), 0)",
					open_id, add_id),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

			RECT now{};
			GetClientRect(editor, &now);
			RECT cap = wrapper_caption_screen(g_state);
			RECT er{};
			GetWindowRect(editor, &er);
			shared::common::log("EditorFrame",
				std::format(
					"wrapped 3DRADCLASS as owned popup of 3DRadRTXFrame "
					"(client {}x{} -> {}x{}, caption={}, resize={}, menus={}) editor-after={} wrapper-after={}",
					keep_w, keep_h, now.right, now.bottom,
					g_state.caption_h, g_state.can_resize ? 1 : 0, g_state.menu_count,
					describe_hwnd(editor), describe_hwnd(wrapper)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			shared::common::log("EditorFrame",
				std::format(
					"caption screen=({},{})-({},{}) editor win=({},{})-({},{}) overlap_top={}",
					cap.left, cap.top, cap.right, cap.bottom,
					er.left, er.top, er.right, er.bottom,
					(er.top < cap.bottom) ? 1 : 0),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

			SetTimer(wrapper, kSyncTimer, 750, nullptr);
			g_state.wrapped = true;
			editor_settings::on_editor_wrapped(editor);
			return true;
		}

		static LRESULT CALLBACK callwnd_hook(int code, WPARAM wparam, LPARAM lparam)
		{
			if (code >= 0)
			{
				const auto* msg = reinterpret_cast<CWPSTRUCT*>(lparam);
				if (msg && msg->message == kWrapMsg && msg->hwnd &&
					msg->hwnd == g_pending_editor)
				{
					const bool ok = install_wrapper(msg->hwnd);
					g_wrap_result.store(ok ? 1 : -1, std::memory_order_relaxed);
				}
			}
			return CallNextHookEx(nullptr, code, wparam, lparam);
		}

		static void unhook_wrap()
		{
			if (g_wnd_hook)
			{
				UnhookWindowsHookEx(g_wnd_hook);
				g_wnd_hook = nullptr;
			}
		}

		static bool request_wrap_on_ui_thread(HWND editor)
		{
			if (!editor || !IsWindow(editor)) {
				return false;
			}
			if (g_state.wrapped) {
				return true;
			}

			g_pending_editor = editor;
			g_wrap_result.store(0, std::memory_order_relaxed);

			const DWORD editor_tid = GetWindowThreadProcessId(editor, nullptr);
			shared::common::log("EditorFrame",
				std::format(
					"request wrap via SendMessageTimeout on editor tid={} (watcher tid={}): {}",
					editor_tid, GetCurrentThreadId(), describe_hwnd(editor)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

			if (editor_tid == GetCurrentThreadId()) {
				return install_wrapper(editor);
			}

			// WH_CALLWNDPROC only sees SendMessage, never PostMessage. The previous
			// wrap posted WM_APP and returned — the hook never fired, so 3DRadRTXFrame
			// was never created and 3DRADCLASS kept WS_CAPTION.
			unhook_wrap();
			SetLastError(0);
			g_wnd_hook = SetWindowsHookExA(WH_CALLWNDPROC, callwnd_hook, nullptr, editor_tid);
			if (!g_wnd_hook)
			{
				shared::common::log("EditorFrame",
					std::format("SetWindowsHookEx(WH_CALLWNDPROC) failed (0x{:X}) — retry later, not wrapping from watcher thread",
						GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return false;
			}

			DWORD_PTR sent = 0;
			SetLastError(0);
			const BOOL delivered = SendMessageTimeoutA(
				editor, kWrapMsg, 0, 0, SMTO_ABORTIFHUNG, 5000, &sent);
			const DWORD send_err = GetLastError();
			unhook_wrap();

			if (!delivered)
			{
				shared::common::log("EditorFrame",
					std::format("SendMessageTimeout wrap msg failed (GetLastError=0x{:X}) — will retry",
						send_err),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return false;
			}

			if (g_state.wrapped || g_wrap_result.load(std::memory_order_relaxed) == 1) {
				return true;
			}

			shared::common::log("EditorFrame",
				std::format("wrap SendMessage returned but 3DRadRTXFrame was not installed (result={}) — will retry",
					g_wrap_result.load(std::memory_order_relaxed)),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return false;
		}

		static bool editor_ready_to_wrap(HWND editor)
		{
			if (!editor || !IsWindow(editor) || !IsWindowVisible(editor) || IsIconic(editor)) {
				return false;
			}

			RECT client{};
			GetClientRect(editor, &client);
			if (client.right < 64 || client.bottom < 64) {
				return false;
			}

			return true;
		}

		static bool wrap_delay_elapsed(HWND editor, DWORD seen_at)
		{
			const DWORD waited = GetTickCount() - seen_at;
			const bool remix = shared::globals::editor_backend ==
				shared::globals::launch_backend::remix;
			const long n = g_create_devices.load(std::memory_order_relaxed);
			if (remix)
			{
				// Prefer waiting for both boot CreateDevices so Remix can hook
				// WndProc on a still-top-level tree. Do not wait forever: wrap
				// after one device + 1.2s, or after 4s even if CreateDevice is late.
				if (n >= 2) {
					return true;
				}
				if (n >= 1 && waited >= 1200) {
					return true;
				}
				if (waited >= 4000) {
					return true;
				}
				return false;
			}
			return has_child_class(editor, kViewportClass) || waited >= 800;
		}

		static DWORD WINAPI watcher_thread(LPVOID)
		{
			shared::common::log("EditorFrame",
				std::format(
					"watcher start backend={} — waiting for visible 3DRADCLASS (picker #32770 skipped).",
					shared::globals::launch_backend_name(shared::globals::editor_backend)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

			HWND editor = nullptr;
			DWORD seen_at = 0;
			DWORD last_wait_log = GetTickCount();
			for (;;)
			{
				if (g_state.wrapped) {
					return 0;
				}

				HWND found = find_editor_frame();
				if (found)
				{
					if (found != editor) {
						editor = found;
						seen_at = GetTickCount();
						shared::common::log("EditorFrame",
							std::format("found 3DRADCLASS — waiting for device/viewport before wrap: {}",
								describe_hwnd(editor)),
							shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
						game::particles::install_ui_hooks();
						project_file::poll();
					}

					if (editor_ready_to_wrap(editor) && wrap_delay_elapsed(editor, seen_at))
					{
						if (request_wrap_on_ui_thread(editor)) {
							return 0;
						}
						Sleep(250);
						continue;
					}

					if (GetTickCount() - last_wait_log >= 10000)
					{
						last_wait_log = GetTickCount();
						shared::common::log("EditorFrame",
							std::format(
								"still waiting to wrap devices={} d3d_device={} waited_ms={} ready={} {}",
								g_create_devices.load(std::memory_order_relaxed),
								shared::globals::d3d_device ? 1 : 0,
								GetTickCount() - seen_at,
								editor_ready_to_wrap(editor) ? 1 : 0,
								describe_hwnd(editor)),
							shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
					}
				}

				Sleep(1);
			}
		}
	}

	void start()
	{
		if (!shared::globals::is_editor_host) {
			return;
		}
		if (g_started.exchange(1) != 0) {
			return;
		}

		INITCOMMONCONTROLSEX icc{};
		icc.dwSize = sizeof(icc);
		icc.dwICC = ICC_WIN95_CLASSES;
		InitCommonControlsEx(&icc);

		shared::common::log("EditorFrame",
			std::format("start wrap watcher for editor backend={}",
				shared::globals::launch_backend_name(shared::globals::editor_backend)),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

		// CreateFile of lastProject.3dr runs in WinMain. Hook it here, at EXE
		// entry, so autoload is remembered the same as File-Open.
		install_editor_proxy_hooks();
		game::particles::install_ui_hooks();
		project_file::poll();

		if (HANDLE t = CreateThread(nullptr, 0, watcher_thread, nullptr, 0, nullptr)) {
			CloseHandle(t);
		}
	}

	void note_create_device()
	{
		if (!shared::globals::is_editor_host) {
			return;
		}
		g_create_devices.fetch_add(1, std::memory_order_relaxed);
		game::particles::install_ui_hooks();
		project_file::poll();
	}

	HWND wrapper_hwnd()
	{
		return g_state.wrapper;
	}

	HWND editor_hwnd()
	{
		return g_state.editor;
	}

	HWND viewport_hwnd()
	{
		if (!g_state.editor || !IsWindow(g_state.editor)) {
			return nullptr;
		}
		return find_named_child(g_state.editor, kViewportClass);
	}

	HWND left_panel_hwnd()
	{
		if (g_state.left_panel && IsWindow(g_state.left_panel)) {
			return g_state.left_panel;
		}
		return nullptr;
	}

	int left_panel_width()
	{
		return g_state.left_panel_w;
	}

	int stock_left_panel_width()
	{
		return g_state.stock_left_panel_w;
	}

	void set_left_panel_width(int w)
	{
		if (w < 80) {
			w = 80;
		}
		if (w > 640) {
			w = 640;
		}
		if (g_state.left_panel_w == w && g_state.inner_metrics) {
			return;
		}
		g_state.left_panel_w = w;
		if (!g_state.editor || !IsWindow(g_state.editor) || g_state.sizing) {
			return;
		}
		layout_inner_panes(g_state, true);
	}

	void request_delayed_hit_write()
	{
		if (!g_state.wrapper || !IsWindow(g_state.wrapper) || g_state.sizing) {
			return;
		}
		SetTimer(g_state.wrapper, kHitWriteTimer, 0, nullptr);
	}

	SIZE viewport_client_size()
	{
		SIZE s{ 0, 0 };
		HWND vp = viewport_hwnd();
		if (vp)
		{
			RECT r{};
			GetClientRect(vp, &r);
			s.cx = r.right - r.left;
			s.cy = r.bottom - r.top;
		}
		return s;
	}

	static void resize_wrapper_to_editor_client(int need_w, int need_h, bool exact)
	{
		if (!g_state.wrapper || !g_state.editor || !IsWindow(g_state.wrapper) ||
			!IsWindow(g_state.editor))
		{
			return;
		}
		if (need_w < 64 || need_h < 64) {
			return;
		}
		if (IsZoomed(g_state.wrapper) || IsIconic(g_state.wrapper) || g_state.sizing) {
			return;
		}

		RECT crc{};
		GetClientRect(g_state.wrapper, &crc);
		const int cap = i_max(g_state.caption_h, 0);
		const int cur_w = i_max(1, static_cast<int>(crc.right - crc.left));
		const int cur_h = i_max(1, static_cast<int>(crc.bottom - crc.top - cap));
		int dw = need_w - cur_w;
		int dh = need_h - cur_h;
		if (!exact)
		{
			if (dw < 0) {
				dw = 0;
			}
			if (dh < 0) {
				dh = 0;
			}
		}
		if (i_abs(dw) <= 2 && i_abs(dh) <= 2)
		{
			request_hit_sync(g_state);
			return;
		}

		RECT wr{};
		GetWindowRect(g_state.wrapper, &wr);
		const int new_w = (wr.right - wr.left) + dw;
		const int new_h = (wr.bottom - wr.top) + dh;
		static int last_logged_w = 0;
		static int last_logged_h = 0;
		if (exact || last_logged_w != new_w || last_logged_h != new_h)
		{
			last_logged_w = new_w;
			last_logged_h = new_h;
			shared::common::log("EditorFrame",
				std::format(
					"{} wrapper {}x{} -> {}x{} for 3DRADCLASS {}x{} "
					"(caption={} client was {}x{} no Reset)",
					exact ? "boot rescale" : "grow",
					wr.right - wr.left, wr.bottom - wr.top, new_w, new_h,
					need_w, need_h, cap, cur_w, cur_h),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}
		SetWindowPos(g_state.wrapper, nullptr, 0, 0, new_w, new_h,
			SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		if (exact)
		{
			layout_editor(g_state, false);
			layout_inner_panes(g_state, true);
		}
		request_hit_sync(g_state);
	}

	void request_viewport_client_size(int want_w, int want_h)
	{
		if (want_w < 64 || want_h < 64) {
			return;
		}
		const int left_w = g_state.left_panel_w > 0 ? g_state.left_panel_w : 0;
		resize_wrapper_to_editor_client(left_w + want_w, want_h, false);
	}

	void apply_boot_editor_client_size(int w, int h)
	{
		if (g_boot_video_applied) {
			return;
		}
		g_boot_video_applied = true;
		resize_wrapper_to_editor_client(w, h, true);
	}
}
