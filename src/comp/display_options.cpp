#include "std_include.hpp"
#include "display_options.hpp"
#include "remix_graphics.hpp"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace comp::display_options
{
	namespace
	{
		constexpr int k_id_device = 1036;
		constexpr int k_id_res = 1038;
		constexpr int k_id_aa = 1039;
		constexpr int k_id_full = 1040;
		constexpr int k_id_vsync = 1041;
		constexpr int k_id_noshow = 1072;
		constexpr UINT k_subclass = 0x444F;
		constexpr char k_prop[] = "vreDispOpt";
		constexpr char k_prop_font[] = "vreDispFnt";
		constexpr char k_prop_fontb[] = "vreDispFtb";
		constexpr int k_id_host = 4011;
		constexpr char k_host_class[] = "RtxCompDispGfxHost";

		HHOOK g_cbt = nullptr;
		HHOOK g_wndret = nullptr;
		DWORD g_tid = 0;
		bool g_started = false;
		bool g_injecting = false;
		bool g_remix_armed = false;

		HWND find_child_text(HWND dlg, const char* cls, const char* needle)
		{
			struct ctx { const char* cls; const char* needle; HWND found; };
			ctx c{ cls, needle, nullptr };
			EnumChildWindows(dlg, [](HWND child, LPARAM lp) -> BOOL
			{
				auto* c = reinterpret_cast<ctx*>(lp);
				char kn[32]{};
				GetClassNameA(child, kn, 32);
				if (c->cls && _stricmp(kn, c->cls) != 0) {
					return TRUE;
				}
				char text[128]{};
				GetWindowTextA(child, text, 128);
				if (text[0] && std::strstr(text, c->needle)) {
					c->found = child;
					return FALSE;
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&c));
			return c.found;
		}

		void hide_stock_groups(HWND dlg)
		{
			EnumChildWindows(dlg, [](HWND child, LPARAM) -> BOOL
			{
				const LONG style = GetWindowLongA(child, GWL_STYLE);
				if ((style & BS_TYPEMASK) != BS_GROUPBOX) {
					return TRUE;
				}
				char text[80]{};
				GetWindowTextA(child, text, 80);
				if (std::strstr(text, "Rendering Device") ||
					std::strstr(text, "Display Mode") ||
					std::strstr(text, "Antialiasing"))
				{
					ShowWindow(child, SW_HIDE);
				}
				return TRUE;
			}, 0);
		}

		void move_ctrl(HWND hwnd, int x, int y, int w, int h)
		{
			if (!hwnd) {
				return;
			}
			UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
			if (w <= 0 || h <= 0) {
				flags |= SWP_NOSIZE;
				w = 0;
				h = 0;
			}
			SetWindowPos(hwnd, nullptr, x, y, w, h, flags);
		}

		void apply_font(HWND dlg, HFONT font)
		{
			if (!dlg || !font) {
				return;
			}
			EnumChildWindows(dlg, [](HWND child, LPARAM lp) -> BOOL
			{
				SendMessageA(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
				return TRUE;
			}, reinterpret_cast<LPARAM>(font));
		}

		int imax(int a, int b) { return a > b ? a : b; }
		int imin(int a, int b) { return a < b ? a : b; }

		void host_apply_scroll(HWND host, int np)
		{
			RECT cr{};
			GetClientRect(host, &cr);
			const int content = static_cast<int>(GetWindowLongPtrA(host, GWLP_USERDATA));
			const int maxp = imax(0, content - static_cast<int>(cr.bottom));
			if (np < 0) {
				np = 0;
			}
			if (np > maxp) {
				np = maxp;
			}
			const int pos = GetScrollPos(host, SB_VERT);
			if (np == pos) {
				return;
			}
			const int dy = pos - np;
			struct move_ctx { HWND host; int dy; };
			move_ctx ctx{ host, dy };
			EnumChildWindows(host, [](HWND child, LPARAM lp) -> BOOL
			{
				const auto* m = reinterpret_cast<move_ctx*>(lp);
				RECT r{};
				GetWindowRect(child, &r);
				MapWindowPoints(HWND_DESKTOP, m->host, reinterpret_cast<POINT*>(&r), 2);
				SetWindowPos(child, nullptr, r.left, r.top + m->dy, 0, 0,
					SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
				return TRUE;
			}, reinterpret_cast<LPARAM>(&ctx));
			SetScrollPos(host, SB_VERT, np, TRUE);
			InvalidateRect(host, nullptr, TRUE);
		}

		void host_update_bar(HWND host)
		{
			if (!host) {
				return;
			}
			RECT cr{};
			GetClientRect(host, &cr);
			const int content = static_cast<int>(GetWindowLongPtrA(host, GWLP_USERDATA));
			SCROLLINFO si{};
			si.cbSize = sizeof(si);
			si.fMask = SIF_RANGE | SIF_PAGE | SIF_DISABLENOSCROLL;
			si.nMin = 0;
			si.nMax = imax(0, content - 1);
			si.nPage = static_cast<UINT>(imax(1, static_cast<int>(cr.bottom)));
			SetScrollInfo(host, SB_VERT, &si, TRUE);
			ShowScrollBar(host, SB_VERT, content > cr.bottom ? TRUE : FALSE);
			const int pos = GetScrollPos(host, SB_VERT);
			const int maxp = imax(0, content - static_cast<int>(cr.bottom));
			if (pos > maxp) {
				host_apply_scroll(host, maxp);
			}
		}

		LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			switch (msg)
			{
			case WM_ERASEBKGND:
			{
				RECT rc{};
				GetClientRect(hwnd, &rc);
				FillRect(reinterpret_cast<HDC>(wparam), &rc,
					GetSysColorBrush(COLOR_3DFACE));
				return 1;
			}
			case WM_COMMAND:
			case WM_CTLCOLORSTATIC:
			case WM_CTLCOLORBTN:
			case WM_NOTIFY:
				return SendMessageA(GetParent(hwnd), msg, wparam, lparam);
			case WM_MOUSEWHEEL:
			{
				const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
				host_apply_scroll(hwnd, GetScrollPos(hwnd, SB_VERT) - delta / 2);
				return 0;
			}
			case WM_VSCROLL:
			{
				RECT cr{};
				GetClientRect(hwnd, &cr);
				int np = GetScrollPos(hwnd, SB_VERT);
				switch (LOWORD(wparam))
				{
				case SB_LINEUP: np -= 32; break;
				case SB_LINEDOWN: np += 32; break;
				case SB_PAGEUP: np -= cr.bottom; break;
				case SB_PAGEDOWN: np += cr.bottom; break;
				case SB_TOP: np = 0; break;
				case SB_BOTTOM: np = 0x7fff; break;
				case SB_THUMBTRACK:
				case SB_THUMBPOSITION: np = HIWORD(wparam); break;
				default: break;
				}
				host_apply_scroll(hwnd, np);
				return 0;
			}
			case WM_SIZE:
				host_update_bar(hwnd);
				return 0;
			case WM_HSCROLL:
				remix_graphics::on_host_hscroll(hwnd);
				return 0;
			default:
				break;
			}
			return DefWindowProcA(hwnd, msg, wparam, lparam);
		}

		void register_host()
		{
			static bool once = false;
			if (once) {
				return;
			}
			once = true;
			WNDCLASSA wc{};
			wc.style = CS_HREDRAW | CS_VREDRAW;
			wc.lpfnWndProc = host_proc;
			wc.hInstance = shared::globals::dll_hmodule;
			wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
			wc.hbrBackground = GetSysColorBrush(COLOR_3DFACE);
			wc.lpszClassName = k_host_class;
			RegisterClassA(&wc);
		}

		HANDLE activate_v6(ULONG_PTR* cookie)
		{
			if (!cookie) {
				return INVALID_HANDLE_VALUE;
			}
			*cookie = 0;
			ACTCTXA ctx{};
			ctx.cbSize = sizeof(ctx);
			ctx.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID;
			ctx.hModule = shared::globals::dll_hmodule;
			ctx.lpResourceName = MAKEINTRESOURCEA(2);
			HANDLE act = CreateActCtxA(&ctx);
			if (act == INVALID_HANDLE_VALUE) {
				return INVALID_HANDLE_VALUE;
			}
			if (!ActivateActCtx(act, cookie))
			{
				ReleaseActCtx(act);
				return INVALID_HANDLE_VALUE;
			}
			return act;
		}

		void deactivate_v6(HANDLE act, ULONG_PTR cookie)
		{
			if (act != INVALID_HANDLE_VALUE && cookie) {
				DeactivateActCtx(0, cookie);
			}
			if (act != INVALID_HANDLE_VALUE) {
				ReleaseActCtx(act);
			}
		}

		LRESULT CALLBACK disp_subclass(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
			UINT_PTR id, DWORD_PTR)
		{
			if (msg == WM_MOUSEWHEEL)
			{
				HWND host = GetDlgItem(hwnd, k_id_host);
				if (host) {
					SendMessageA(host, WM_MOUSEWHEEL, wparam, lparam);
					return 0;
				}
			}
			const int cid = LOWORD(wparam);
			const int note = HIWORD(wparam);
			if (msg == WM_COMMAND && cid >= 0x7F00 && cid < 0x7FA0 &&
				(note == CBN_SELCHANGE || note == BN_CLICKED))
			{
				remix_graphics::update_dependent_enables(hwnd);
			}
			if (msg == WM_COMMAND && cid == IDOK &&
				(note == BN_CLICKED || note == 0))
			{
				remix_graphics::write_from_dialog(hwnd);
				remix_graphics::commit_conf_before_remix();
				g_remix_armed = true;
				shared::common::log("DisplayOpt",
					"OK — Remix armed for next valid CreateDevice (picker may still exist)",
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				return DefSubclassProc(hwnd, msg, wparam, lparam);
			}
			if (msg == WM_COMMAND && cid == IDCANCEL &&
				(note == BN_CLICKED || note == 0))
			{
				g_remix_armed = true;
				return DefSubclassProc(hwnd, msg, wparam, lparam);
			}
			if (msg == WM_NCDESTROY)
			{
				g_remix_armed = true;
				HFONT f = static_cast<HFONT>(GetPropA(hwnd, k_prop_font));
				HFONT fb = static_cast<HFONT>(GetPropA(hwnd, k_prop_fontb));
				RemovePropA(hwnd, k_prop);
				RemovePropA(hwnd, k_prop_font);
				RemovePropA(hwnd, k_prop_fontb);
				RemovePropA(hwnd, "vreRtxGfx");
				RemoveWindowSubclass(hwnd, disp_subclass, id);
				if (f) {
					DeleteObject(f);
				}
				if (fb) {
					DeleteObject(fb);
				}
			}
			return DefSubclassProc(hwnd, msg, wparam, lparam);
		}

		void modernize_and_inject(HWND dlg)
		{
			if (!dlg || !IsWindow(dlg)) {
				return;
			}
			if (shared::globals::is_editor_host ||
				shared::globals::skip_remix ||
				!shared::globals::is_compiled_host)
			{
				return;
			}
			if (!is_display_options(dlg)) {
				return;
			}
			if (GetPropA(dlg, k_prop) || g_injecting) {
				return;
			}
			g_injecting = true;
			SetPropA(dlg, k_prop, reinterpret_cast<HANDLE>(1));
			remix_graphics::load_from_disk();

			INITCOMMONCONTROLSEX icc{};
			icc.dwSize = sizeof(icc);
			icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
			InitCommonControlsEx(&icc);

			ULONG_PTR cookie = 0;
			HANDLE act = activate_v6(&cookie);

			const int dpi = remix_graphics::window_dpi(dlg);
			HFONT font = remix_graphics::segoe_ui(9, false, dpi);
			HFONT fontb = remix_graphics::segoe_ui(9, true, dpi);
			if (font) {
				SetPropA(dlg, k_prop_font, font);
			}
			if (fontb) {
				SetPropA(dlg, k_prop_fontb, fontb);
			}

			HWND device = GetDlgItem(dlg, k_id_device);
			HWND res = GetDlgItem(dlg, k_id_res);
			HWND aa = GetDlgItem(dlg, k_id_aa);
			HWND full = GetDlgItem(dlg, k_id_full);
			HWND vsync = GetDlgItem(dlg, k_id_vsync);
			HWND noshow = GetDlgItem(dlg, k_id_noshow);
			HWND ok = GetDlgItem(dlg, IDOK);
			HWND cancel = GetDlgItem(dlg, IDCANCEL);
			if (!full) {
				full = find_child_text(dlg, "Button", "Full Screen Mode");
			}
			if (!vsync) {
				vsync = find_child_text(dlg, "Button", "Vertical Sync");
			}
			if (!noshow) {
				noshow = find_child_text(dlg, "Button", "Do not show this panel");
			}
			if (!ok) {
				ok = find_child_text(dlg, "Button", "OK");
			}
			if (!cancel) {
				cancel = find_child_text(dlg, "Button", "Cancel");
			}

			hide_stock_groups(dlg);

			HMONITOR mon = MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST);
			MONITORINFO mi{};
			mi.cbSize = sizeof(mi);
			GetMonitorInfoA(mon, &mi);
			const int work_w = mi.rcWork.right - mi.rcWork.left;
			const int work_h = mi.rcWork.bottom - mi.rcWork.top;
			const bool stack = work_w < 900;

			auto px = [dpi](int v) { return MulDiv(v, dpi > 0 ? dpi : 96, 96); };
			const int pad = px(14);
			const int row = px(22);
			const int left_w = px(320);
			const int right_w = stack ? left_w : px(470);
			const int gap = px(12);
			// Combo *window* height is the closed edit row (22â€“24px at 96dpi).
			// 160px was the dropdown list height and overlapped every row below.
			const int combo_h = px(22);
			const int btn_w = px(88);
			const int btn_h = px(26);

			const int client_w = stack
				? (pad * 2 + left_w)
				: (pad * 2 + left_w + gap + right_w);

			int y = pad;
			HWND disp_g = CreateWindowExA(0, "BUTTON", "Display",
				WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
				pad, y, left_w, px(168), dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(0x7EFF)),
				shared::globals::dll_hmodule, nullptr);
			if (disp_g && fontb) {
				SendMessageA(disp_g, WM_SETFONT, reinterpret_cast<WPARAM>(fontb), TRUE);
			}

			int iy = y + px(18);
			const int ix = pad + px(10);
			const int iw = left_w - px(20);

			HWND dev_l = CreateWindowExA(0, "STATIC", "Rendering Device",
				WS_CHILD | WS_VISIBLE | SS_LEFT,
				ix, iy, iw, px(16), dlg, nullptr, shared::globals::dll_hmodule, nullptr);
			if (dev_l && font) {
				SendMessageA(dev_l, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
			}
			iy += px(18);
			move_ctrl(device, ix, iy, iw, combo_h);
			iy += row + px(4);

			move_ctrl(full, ix, iy, px(150), px(18));
			move_ctrl(vsync, ix + px(160), iy, px(130), px(18));
			iy += row;

			HWND res_l = CreateWindowExA(0, "STATIC", "Resolution",
				WS_CHILD | WS_VISIBLE | SS_LEFT,
				ix, iy, px(150), px(16), dlg, nullptr, shared::globals::dll_hmodule, nullptr);
			HWND aa_l = CreateWindowExA(0, "STATIC", "Antialiasing",
				WS_CHILD | WS_VISIBLE | SS_LEFT,
				ix + px(168), iy, px(120), px(16), dlg, nullptr, shared::globals::dll_hmodule, nullptr);
			if (font)
			{
				if (res_l) {
					SendMessageA(res_l, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
				}
				if (aa_l) {
					SendMessageA(aa_l, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
				}
			}
			iy += px(18);
			move_ctrl(res, ix, iy, px(160), combo_h);
			move_ctrl(aa, ix + px(168), iy, px(112), combo_h);
			iy += row + px(2);
			move_ctrl(noshow, ix, iy, iw, px(18));
			iy += px(24);

			const int disp_h = iy - y + px(6);
			if (disp_g) {
				SetWindowPos(disp_g, HWND_BOTTOM, pad, y, left_w, disp_h, SWP_NOACTIVATE);
			}

			RECT wr{}, cr{};
			GetWindowRect(dlg, &wr);
			GetClientRect(dlg, &cr);
			const int chrome_w = (wr.right - wr.left) - (cr.right - cr.left);
			const int chrome_h = (wr.bottom - wr.top) - (cr.bottom - cr.top);

			const int btn_row = btn_h + px(10) + pad;
			const int min_host = px(140);
			int client_h;
			if (stack)
			{
				const int top = pad + disp_h + gap;
				const int max_client = imax(top + min_host + btn_row,
					work_h * 90 / 100 - chrome_h);
				client_h = imin(max_client, work_h * 90 / 100 - chrome_h);
				client_h = imax(client_h, top + min_host + btn_row);
			}
			else
			{
				const int need = pad + imax(disp_h, min_host) + btn_row;
				client_h = imin(work_h * 90 / 100 - chrome_h, imax(need, px(420)));
			}

			SetWindowPos(dlg, nullptr, 0, 0,
				client_w + chrome_w, client_h + chrome_h,
				SWP_NOMOVE | SWP_NOZORDER);

			GetClientRect(dlg, &cr);
			client_h = static_cast<int>(cr.bottom);
			const int by = client_h - pad - btn_h;
			const int bx = static_cast<int>(cr.right) - pad - btn_w;
			move_ctrl(cancel, bx, by, btn_w, btn_h);
			move_ctrl(ok, bx - btn_w - px(8), by, btn_w, btn_h);

			int remix_x = stack ? pad : (pad + left_w + gap);
			int remix_y = stack ? (y + disp_h + gap) : y;
			int host_w = stack
				? (static_cast<int>(cr.right) - pad * 2)
				: right_w;
			int host_h = imax(min_host, by - px(8) - remix_y);

			register_host();
			HWND host = CreateWindowExA(
				WS_EX_CLIENTEDGE, k_host_class, "",
				WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
				remix_x, remix_y, host_w, host_h,
				dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(k_id_host)),
				shared::globals::dll_hmodule, nullptr);
			int remix_h = 0;
			if (host)
			{
				remix_graphics::bind_controls_host(dlg, host);
				RECT hc{};
				GetClientRect(host, &hc);
				remix_h = remix_graphics::create_controls(
					host, 0, 0, imax(120, static_cast<int>(hc.right)), font);
				SetWindowLongPtrA(host, GWLP_USERDATA, remix_h);
				host_update_bar(host);
			}

			if (font) {
				apply_font(dlg, font);
			}
			if (disp_g) {
				SetWindowPos(disp_g, HWND_BOTTOM, 0, 0, 0, 0,
					SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			}

			SetWindowTheme(dlg, L"Explorer", nullptr);
			EnumChildWindows(dlg, [](HWND child, LPARAM) -> BOOL
			{
				char kn[32]{};
				GetClassNameA(child, kn, 32);
				if (_stricmp(kn, k_host_class) == 0) {
					return TRUE;
				}
				SetWindowTheme(child, L"Explorer", nullptr);
				return TRUE;
			}, 0);

			SetWindowSubclass(dlg, disp_subclass, k_subclass, 0);
			deactivate_v6(act, cookie);
			remix_graphics::clamp_to_work_area(dlg);
			g_injecting = false;

			char title[80]{};
			GetWindowTextA(dlg, title, 80);
			shared::common::log("DisplayOpt",
				std::format("hooked '{}' hwnd=0x{:X} class=#32770 device={} res={} aa={} "
					"stack={} host={}x{} remix_h={} â€” OK/Cancel pinned, conf on OK",
					title[0] ? title : "Display Options",
					reinterpret_cast<std::uintptr_t>(dlg),
					device ? 1 : 0, res ? 1 : 0, aa ? 1 : 0,
					stack ? 1 : 0, host_w, host_h, remix_h),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		LRESULT CALLBACK cbt_proc(int code, WPARAM wparam, LPARAM lparam)
		{
			if (code == HCBT_ACTIVATE && wparam)
			{
				modernize_and_inject(reinterpret_cast<HWND>(wparam));
			}
			return CallNextHookEx(g_cbt, code, wparam, lparam);
		}

		LRESULT CALLBACK wndret_proc(int code, WPARAM wparam, LPARAM lparam)
		{
			if (code >= 0)
			{
				const auto* msg = reinterpret_cast<CWPRETSTRUCT*>(lparam);
				if (msg && msg->hwnd &&
					(msg->message == WM_INITDIALOG || msg->message == WM_SHOWWINDOW))
				{
					if (is_display_options(msg->hwnd)) {
						modernize_and_inject(msg->hwnd);
					}
				}
			}
			return CallNextHookEx(g_wndret, code, wparam, lparam);
		}
	}

	bool is_display_options(HWND hwnd)
	{
		if (!hwnd || !IsWindow(hwnd)) {
			return false;
		}
		if (shared::globals::is_editor_host ||
			shared::globals::skip_remix ||
			!shared::globals::is_compiled_host)
		{
			return false;
		}
		char cls[32]{};
		GetClassNameA(hwnd, cls, 32);
		if (std::strcmp(cls, "#32770") != 0) {
			return false;
		}
		char title[80]{};
		GetWindowTextA(hwnd, title, 80);
		if (!title[0] || !std::strstr(title, "Display Options")) {
			return false;
		}
		if (std::strstr(title, "launch editor") ||
			std::strstr(title, "Particles") ||
			std::strstr(title, "Properties"))
		{
			return false;
		}
		// All original DXUT Display Options IDs as a group. Particles v1.16
		// and other object dialogs are also #32770 and may reuse a subset.
		return GetDlgItem(hwnd, k_id_device) &&
			GetDlgItem(hwnd, k_id_res) &&
			GetDlgItem(hwnd, k_id_aa) &&
			GetDlgItem(hwnd, k_id_full) &&
			GetDlgItem(hwnd, k_id_vsync) &&
			GetDlgItem(hwnd, k_id_noshow);
	}

	void start()
	{
		if (shared::globals::skip_remix ||
			shared::globals::is_editor_host ||
			!shared::globals::is_compiled_host)
		{
			return;
		}
		g_started = true;
		const DWORD tid = GetCurrentThreadId();
		if (g_tid == tid && g_cbt && g_wndret) {
			return;
		}
		if (g_cbt) {
			UnhookWindowsHookEx(g_cbt);
			g_cbt = nullptr;
		}
		if (g_wndret) {
			UnhookWindowsHookEx(g_wndret);
			g_wndret = nullptr;
		}
		g_tid = tid;
		HMODULE mod = shared::globals::dll_hmodule;
		g_cbt = SetWindowsHookExA(WH_CBT, cbt_proc, mod, tid);
		g_wndret = SetWindowsHookExA(WH_CALLWNDPROCRET, wndret_proc, mod, tid);
		shared::common::log("DisplayOpt",
			std::format("hooks tid={} cbt={} wndret={} host={}",
				tid, g_cbt ? 1 : 0, g_wndret ? 1 : 0,
				shared::globals::host_kind_name()));
	}

	bool picker_visible()
	{
		if (!shared::globals::is_compiled_host ||
			shared::globals::is_editor_host)
		{
			return false;
		}
		bool found = false;
		EnumThreadWindows(GetCurrentThreadId(),
			[](HWND w, LPARAM lp) -> BOOL
			{
				if (is_display_options(w))
				{
					*reinterpret_cast<bool*>(lp) = true;
					return FALSE;
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&found));
		return found;
	}

	bool is_picker_hwnd(HWND hwnd)
	{
		if (!hwnd || !IsWindow(hwnd)) {
			return false;
		}
		if (is_display_options(hwnd)) {
			return true;
		}
		char cls[64]{};
		GetClassNameA(hwnd, cls, sizeof(cls));
		if (std::strcmp(cls, "#32770") == 0 ||
			std::strcmp(cls, "Rendering Window") == 0)
		{
			return true;
		}
		if (std::strcmp(cls, "Fullscreen Window") == 0)
		{
			RECT cr{};
			if (!GetClientRect(hwnd, &cr) ||
				(cr.right - cr.left) < 64 ||
				(cr.bottom - cr.top) < 64)
			{
				return true;
			}
		}
		return false;
	}

	bool is_real_play_hwnd(HWND hwnd)
	{
		if (!hwnd || !IsWindow(hwnd) || is_picker_hwnd(hwnd)) {
			return false;
		}
		char cls[64]{};
		GetClassNameA(hwnd, cls, sizeof(cls));
		if (std::strcmp(cls, "Fullscreen Window") != 0 &&
			std::strcmp(cls, "3DRADCLASS") != 0)
		{
			return false;
		}
		RECT cr{};
		if (!GetClientRect(hwnd, &cr)) {
			return false;
		}
		return (cr.right - cr.left) >= 64 && (cr.bottom - cr.top) >= 64;
	}

	bool remix_load_allowed()
	{
		if (!shared::globals::is_compiled_host ||
			shared::globals::is_editor_host ||
			shared::globals::skip_remix)
		{
			return false;
		}
		return g_remix_armed;
	}

	void note_picker_finished()
	{
		g_remix_armed = true;
	}
}