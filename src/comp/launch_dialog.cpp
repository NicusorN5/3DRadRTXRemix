#include "std_include.hpp"
#include "launch_dialog.hpp"
#include "runtime_update.hpp"
#include "d3d9_proxy.hpp"
#include "editor_frame.hpp"
#include "editor_settings.hpp"
#include "remix_graphics.hpp"
#include "shared/common/config.hpp"
#include "comp.hpp"
#include "resource.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <objidl.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <wincon.h>

#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))
#define COMP_LAUNCH_MINMAX 1
#endif
#include <gdiplus.h>
#ifdef COMP_LAUNCH_MINMAX
#undef min
#undef max
#undef COMP_LAUNCH_MINMAX
#endif

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace comp::launch
{
	namespace fs = std::filesystem;

	namespace
	{
		constexpr UINT WM_UPDATE_DONE = WM_APP + 40;

		constexpr wchar_t kLinkRemix[] = L"https://github.com/NVIDIAGameWorks/rtx-remix";
		constexpr wchar_t kLinkDxvk[] = L"https://github.com/doitsujin/dxvk";
		constexpr wchar_t kLinkVibe[] = L"https://github.com/Ekozmaster/Vibe-Reverse-Engineering";

		constexpr wchar_t kSysLinkText[] =
			L"Repos: <a href=\"https://github.com/NVIDIAGameWorks/rtx-remix\">RTX Remix</a>"
			L"    <a href=\"https://github.com/doitsujin/dxvk\">DXVK</a>"
			L"    <a href=\"https://github.com/Ekozmaster/Vibe-Reverse-Engineering\">Vibe</a>";

		constexpr COLORREF kDlgBg = RGB(0, 0, 0);
		constexpr COLORREF kDlgText = RGB(245, 245, 245);
		constexpr COLORREF kDlgCheckText = RGB(255, 255, 255);
		constexpr COLORREF kDlgLink = RGB(0x7E, 0xB8, 0xFF);

		static HBRUSH dlg_bg_brush()
		{
			static HBRUSH brush = CreateSolidBrush(kDlgBg);
			return brush;
		}

		struct dialog_state
		{
			shared::globals::launch_backend backend = shared::globals::launch_backend::remix;
			HWND hwnd = nullptr;
			HWND banner = nullptr;
			HWND banner_bottom = nullptr;
			HWND links = nullptr;
			HWND gfx_host = nullptr;
			HANDLE worker = nullptr;
			HBITMAP banner_bmp = nullptr;
			HBITMAP banner_bottom_bmp = nullptr;
			ULONG_PTR gdiplus_token = 0;
			std::atomic<bool> updating{ false };
			comp::runtime_update::result update{};
		};

		static HANDLE g_chain_ready = nullptr;
		static std::atomic<bool> g_launch_aborted{ false };
		static BYTE* g_exe_entry = nullptr;
		static BYTE g_exe_entry_orig[5]{};
		static bool g_exe_entry_patched = false;

		static fs::path dxvk_path()
		{
			return fs::path(shared::globals::root_path) / "d3d9_dxvk.dll";
		}

		static fs::path remix_path()
		{
			auto& cfg = shared::common::config::get();
			fs::path p = cfg.remix.dll_name.empty() ? "d3d9_remix.dll" : cfg.remix.dll_name;
			if (!p.is_absolute()) {
				p = fs::path(shared::globals::root_path) / p;
			}
			return p;
		}

		static bool file_exists(const fs::path& p)
		{
			std::error_code ec;
			return fs::exists(p, ec);
		}

		static std::wstring utf8_to_wide(const std::string& s)
		{
			if (s.empty()) {
				return {};
			}
			const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
			if (n <= 0) {
				return std::wstring(s.begin(), s.end());
			}
			std::wstring out(static_cast<size_t>(n - 1), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
			return out;
		}

		static fs::path banner_jpeg_path(const char* filename)
		{
			if (!filename || !filename[0]) {
				return {};
			}
			const fs::path primary = fs::path(shared::globals::root_path) /
				"3DRad_res" / "help" / "img" / filename;
			if (file_exists(primary)) {
				return primary;
			}
			const fs::path fallback = fs::path("C:\\3DRadRTX\\3DRad_res\\help\\img") / filename;
			if (file_exists(fallback)) {
				return fallback;
			}
			return {};
		}

		static HBITMAP load_banner_bitmap(const fs::path& path, int dest_w, int* out_h)
		{
			if (out_h) {
				*out_h = 0;
			}
			if (dest_w <= 0 || path.empty()) {
				return nullptr;
			}

			const std::wstring wpath = utf8_to_wide(path.string());
			Gdiplus::Image src(wpath.c_str());
			if (src.GetLastStatus() != Gdiplus::Ok || src.GetWidth() == 0 || src.GetHeight() == 0)
			{
				shared::common::log("Launch",
					std::format("Banner JPEG unreadable: {}", path.string()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, false);
				return nullptr;
			}

			const UINT src_w = src.GetWidth();
			const UINT src_h = src.GetHeight();
			const int dest_h = std::max(1, static_cast<int>(
				(static_cast<long long>(src_h) * dest_w) / src_w));

			Gdiplus::Bitmap scaled(dest_w, dest_h, PixelFormat32bppPARGB);
			if (scaled.GetLastStatus() != Gdiplus::Ok) {
				return nullptr;
			}

			{
				Gdiplus::Graphics g(&scaled);
				g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
				g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
				g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
				if (g.DrawImage(&src, 0, 0, dest_w, dest_h) != Gdiplus::Ok) {
					return nullptr;
				}
			}

			HBITMAP hbmp = nullptr;
			if (scaled.GetHBITMAP(Gdiplus::Color(255, 255, 255), &hbmp) != Gdiplus::Ok) {
				return nullptr;
			}
			if (out_h) {
				*out_h = dest_h;
			}
			return hbmp;
		}

		static void set_status(HWND hwnd, const char* text)
		{
			if (hwnd && text) {
				SetDlgItemTextA(hwnd, IDC_LAUNCH_STATUS, text);
			}
		}

		static void set_busy(HWND hwnd, bool busy)
		{
			EnableWindow(GetDlgItem(hwnd, IDC_LAUNCH_GO), busy ? FALSE : TRUE);
			EnableWindow(GetDlgItem(hwnd, IDC_LAUNCH_SAVE), busy ? FALSE : TRUE);
			EnableWindow(GetDlgItem(hwnd, IDC_LAUNCH_UPDATE), busy ? FALSE : TRUE);
			EnableWindow(GetDlgItem(hwnd, IDCANCEL), busy ? FALSE : TRUE);
			EnableWindow(GetDlgItem(hwnd, IDC_LAUNCH_REMIX), busy ? FALSE : TRUE);
			EnableWindow(GetDlgItem(hwnd, IDC_LAUNCH_DX9), busy ? FALSE : TRUE);
			EnableWindow(GetDlgItem(hwnd, IDC_LAUNCH_DXVK), busy ? FALSE : TRUE);
			if (HWND links = GetDlgItem(hwnd, IDC_LAUNCH_LINKS)) {
				EnableWindow(links, busy ? FALSE : TRUE);
			}
		}

		static shared::globals::launch_backend selected_backend(HWND hwnd)
		{
			if (IsDlgButtonChecked(hwnd, IDC_LAUNCH_DX9) == BST_CHECKED) {
				return shared::globals::launch_backend::dx9;
			}
			if (IsDlgButtonChecked(hwnd, IDC_LAUNCH_DXVK) == BST_CHECKED) {
				return shared::globals::launch_backend::dxvk;
			}
			return shared::globals::launch_backend::remix;
		}

		static BOOL CALLBACK set_child_font(HWND child, LPARAM lp)
		{
			SendMessageA(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
			return TRUE;
		}

		struct shift_ctx
		{
			HWND parent = nullptr;
			HWND skip = nullptr;
			int dy = 0;
		};

		static BOOL CALLBACK shift_child(HWND child, LPARAM lp)
		{
			const auto* ctx = reinterpret_cast<shift_ctx*>(lp);
			if (!ctx || child == ctx->skip) {
				return TRUE;
			}
			RECT r{};
			GetWindowRect(child, &r);
			MapWindowPoints(HWND_DESKTOP, ctx->parent, reinterpret_cast<POINT*>(&r), 2);
			SetWindowPos(child, nullptr, r.left, r.top + ctx->dy, 0, 0,
				SWP_NOSIZE | SWP_NOZORDER);
			return TRUE;
		}

		static void open_url(HWND hwnd, const wchar_t* url)
		{
			if (!url || !url[0]) {
				return;
			}
			ShellExecuteW(hwnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
		}

		static void create_repo_links(dialog_state* st, HWND hwnd)
		{
			HWND placeholder = GetDlgItem(hwnd, IDC_LAUNCH_LINKS);
			RECT r{};
			if (placeholder)
			{
				GetWindowRect(placeholder, &r);
				MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<POINT*>(&r), 2);
				DestroyWindow(placeholder);
			}
			else
			{
				r.left = 10;
				r.top = 82;
				r.right = 270;
				r.bottom = 98;
			}

			st->links = CreateWindowExW(
				0, WC_LINK, kSysLinkText,
				WS_CHILD | WS_VISIBLE | WS_TABSTOP,
				r.left, r.top, r.right - r.left, r.bottom - r.top,
				hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LAUNCH_LINKS)),
				shared::globals::dll_hmodule, nullptr);

			if (!st->links)
			{
				st->links = CreateWindowExW(
					0, L"BUTTON", L"RTX Remix  |  DXVK  |  Vibe",
					WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
					r.left, r.top, r.right - r.left, r.bottom - r.top,
					hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LAUNCH_LINKS)),
					shared::globals::dll_hmodule, nullptr);
			}
			if (st->links) {
				SetWindowTheme(st->links, L"", L"");
			}
		}

		static HWND create_banner_static(HWND hwnd, int id, HBITMAP bmp, int x, int y, int w, int h)
		{
			HWND banner = CreateWindowExA(
				0, "STATIC", nullptr,
				WS_CHILD | WS_VISIBLE | SS_BITMAP | SS_CENTERIMAGE,
				x, y, w, h,
				hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
				shared::globals::dll_hmodule, nullptr);
			if (!banner) {
				return nullptr;
			}
			SendMessageA(banner, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(bmp));
			return banner;
		}

		static void grow_dialog_width(HWND hwnd, int extra_w)
		{
			if (extra_w <= 0) {
				return;
			}
			RECT wr{};
			GetWindowRect(hwnd, &wr);
			const int ww = wr.right - wr.left + extra_w;
			const int wh = wr.bottom - wr.top;
			const int sw = GetSystemMetrics(SM_CXSCREEN);
			const int sh = GetSystemMetrics(SM_CYSCREEN);
			SetWindowPos(hwnd, nullptr,
				std::max(0, (sw - ww) / 2), std::max(0, (sh - wh) / 2),
				ww, wh, SWP_NOZORDER);
		}

		static void grow_dialog_height(HWND hwnd, int extra_h)
		{
			if (extra_h <= 0) {
				return;
			}
			RECT wr{};
			GetWindowRect(hwnd, &wr);
			const int ww = wr.right - wr.left;
			const int wh = wr.bottom - wr.top + extra_h;
			const int sw = GetSystemMetrics(SM_CXSCREEN);
			const int sh = GetSystemMetrics(SM_CYSCREEN);
			SetWindowPos(hwnd, nullptr,
				std::max(0, (sw - ww) / 2), std::max(0, (sh - wh) / 2),
				ww, wh, SWP_NOZORDER);
		}

		static int imax(int a, int b) { return a > b ? a : b; }
		static int imin(int a, int b) { return a < b ? a : b; }

		static const char k_gfx_host_class[] = "RtxCompGfxHost";

		static void gfx_host_apply_scroll(HWND host, int np)
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

		static void gfx_host_update_bar(HWND host)
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
				gfx_host_apply_scroll(host, maxp);
			}
		}

		static LRESULT CALLBACK gfx_host_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			switch (msg)
			{
			case WM_ERASEBKGND:
			{
				RECT rc{};
				GetClientRect(hwnd, &rc);
				FillRect(reinterpret_cast<HDC>(wparam), &rc, dlg_bg_brush());
				return 1;
			}
			case WM_HSCROLL:
				comp::remix_graphics::on_host_hscroll(hwnd);
				return 0;
			case WM_COMMAND:
			case WM_CTLCOLORSTATIC:
			case WM_CTLCOLORBTN:
			case WM_NOTIFY:
				return SendMessageA(GetParent(hwnd), msg, wparam, lparam);
			case WM_MOUSEWHEEL:
			{
				const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
				gfx_host_apply_scroll(hwnd, GetScrollPos(hwnd, SB_VERT) - delta / 2);
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
				gfx_host_apply_scroll(hwnd, np);
				return 0;
			}
			case WM_SIZE:
				gfx_host_update_bar(hwnd);
				return 0;
			default:
				break;
			}
			return DefWindowProcA(hwnd, msg, wparam, lparam);
		}

		static void register_gfx_host()
		{
			static bool once = false;
			if (once) {
				return;
			}
			once = true;
			WNDCLASSA wc{};
			wc.style = CS_HREDRAW | CS_VREDRAW;
			wc.lpfnWndProc = gfx_host_proc;
			wc.hInstance = shared::globals::dll_hmodule;
			wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
			wc.hbrBackground = dlg_bg_brush();
			wc.lpszClassName = k_gfx_host_class;
			RegisterClassA(&wc);
		}

		static void layout_action_row(HWND hwnd, int y, int btn_w, int btn_h, int gap, int pad)
		{
			RECT client{};
			GetClientRect(hwnd, &client);
			HWND go = GetDlgItem(hwnd, IDC_LAUNCH_GO);
			HWND save = GetDlgItem(hwnd, IDC_LAUNCH_SAVE);
			HWND upd = GetDlgItem(hwnd, IDC_LAUNCH_UPDATE);
			HWND cancel = GetDlgItem(hwnd, IDCANCEL);
			int x = pad;
			if (go) {
				SetWindowPos(go, nullptr, x, y, btn_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
			}
			x += btn_w + gap;
			if (save) {
				SetWindowPos(save, nullptr, x, y, btn_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
			}
			x += btn_w + gap;
			if (upd) {
				SetWindowPos(upd, nullptr, x, y, btn_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
			}
			if (cancel) {
				SetWindowPos(cancel, nullptr, client.right - pad - btn_w, y, btn_w, btn_h,
					SWP_NOZORDER | SWP_NOACTIVATE);
			}
		}

		static void layout_gfx_host(HWND hwnd)
		{
			auto* st = reinterpret_cast<dialog_state*>(GetWindowLongPtrA(hwnd, DWLP_USER));
			if (!st || !st->gfx_host) {
				return;
			}
			RECT client{};
			GetClientRect(hwnd, &client);
			RECT br{};
			HWND go = GetDlgItem(hwnd, IDC_LAUNCH_GO);
			if (go)
			{
				GetWindowRect(go, &br);
				MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<POINT*>(&br), 2);
			}
			const int pad = 10;
			const int y = br.bottom + pad;
			const int h = imax(80, static_cast<int>(client.bottom) - y - pad);
			SetWindowPos(st->gfx_host, nullptr, pad, y, client.right - pad * 2, h,
				SWP_NOZORDER | SWP_NOACTIVATE);
			gfx_host_update_bar(st->gfx_host);
		}

		static void install_remix_graphics(HWND hwnd)
		{
			remix_graphics::load_from_disk();
			register_gfx_host();
			const int dpi = remix_graphics::window_dpi(hwnd);
			auto px = [dpi](int v) { return MulDiv(v, dpi > 0 ? dpi : 96, 96); };

			HWND status = GetDlgItem(hwnd, IDC_LAUNCH_STATUS);
			RECT sr{};
			if (status)
			{
				GetWindowRect(status, &sr);
				MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<POINT*>(&sr), 2);
				SetWindowPos(status, nullptr, sr.left, sr.top, sr.right - sr.left, px(22),
					SWP_NOZORDER | SWP_NOACTIVATE);
				GetWindowRect(status, &sr);
				MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<POINT*>(&sr), 2);
			}

			const int pad = px(10);
			const int btn_w = px(84);
			const int btn_h = px(28);
			const int gap = px(8);
			const int by = (status ? sr.bottom : px(90)) + px(8);
			layout_action_row(hwnd, by, btn_w, btn_h, gap, pad);

			RECT wr{}, cr{};
			GetWindowRect(hwnd, &wr);
			GetClientRect(hwnd, &cr);
			HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
			MONITORINFO mi{};
			mi.cbSize = sizeof(mi);
			GetMonitorInfoA(mon, &mi);
			const int work_h = mi.rcWork.bottom - mi.rcWork.top;
			const int chrome = (wr.bottom - wr.top) - (cr.bottom - cr.top);
			const int want_client = by + btn_h + px(12) + px(360);
			const int max_client = work_h * 85 / 100 - chrome;
			const int new_client = imin(want_client, imax(px(420), max_client));
			SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, new_client + chrome,
				SWP_NOMOVE | SWP_NOZORDER);

			GetClientRect(hwnd, &cr);
			const int host_y = by + btn_h + px(12);
			auto* st = reinterpret_cast<dialog_state*>(GetWindowLongPtrA(hwnd, DWLP_USER));
			const int host_h = imax(px(80), static_cast<int>(cr.bottom) - host_y - pad);
			st->gfx_host = CreateWindowExA(
				WS_EX_CLIENTEDGE, k_gfx_host_class, "",
				WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | WS_CLIPCHILDREN,
				pad, host_y, static_cast<int>(cr.right) - pad * 2, host_h,
				hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LAUNCH_GFX_HOST)),
				shared::globals::dll_hmodule, nullptr);
			if (!st->gfx_host) {
				return;
			}
			SetWindowTheme(st->gfx_host, L"", L"");
			remix_graphics::bind_controls_host(hwnd, st->gfx_host);

			HFONT font = remix_graphics::segoe_ui(9, false, dpi);
			RECT hc{};
			GetClientRect(st->gfx_host, &hc);
			const int content_h = remix_graphics::create_controls(
				st->gfx_host, 0, 0, imax(120, static_cast<int>(hc.right)), font);
			SetWindowLongPtrA(st->gfx_host, GWLP_USERDATA, content_h);
			gfx_host_update_bar(st->gfx_host);
			layout_gfx_host(hwnd);
			remix_graphics::set_enabled(hwnd,
				selected_backend(hwnd) == shared::globals::launch_backend::remix);
		}

		static void apply_banner(dialog_state* st, HWND hwnd)
		{
			RECT client{};
			GetClientRect(hwnd, &client);
			const int dest_w = client.right - client.left;
			int dest_h = 0;
			st->banner_bmp = load_banner_bitmap(banner_jpeg_path("main_logo.jpg"), dest_w, &dest_h);
			if (!st->banner_bmp || dest_h <= 0) {
				return;
			}

			st->banner = create_banner_static(hwnd, IDC_LAUNCH_BANNER, st->banner_bmp,
				0, 0, dest_w, dest_h);
			if (!st->banner)
			{
				DeleteObject(st->banner_bmp);
				st->banner_bmp = nullptr;
				return;
			}

			shift_ctx ctx{ hwnd, st->banner, dest_h };
			EnumChildWindows(hwnd, shift_child, reinterpret_cast<LPARAM>(&ctx));
			grow_dialog_height(hwnd, dest_h);
		}

		static void apply_bottom_banner(dialog_state* st, HWND hwnd)
		{
			RECT client{};
			GetClientRect(hwnd, &client);
			const int dest_w = client.right - client.left;
			int dest_h = 0;
			st->banner_bottom_bmp = load_banner_bitmap(
				banner_jpeg_path("main_logo_bottom.jpg"), dest_w, &dest_h);
			if (!st->banner_bottom_bmp || dest_h <= 0) {
				return;
			}

			const int y = client.bottom - client.top;
			st->banner_bottom = create_banner_static(hwnd, IDC_LAUNCH_BANNER_BOTTOM,
				st->banner_bottom_bmp, 0, y, dest_w, dest_h);
			if (!st->banner_bottom)
			{
				DeleteObject(st->banner_bottom_bmp);
				st->banner_bottom_bmp = nullptr;
				return;
			}

			grow_dialog_height(hwnd, dest_h);
		}

		static BOOL CALLBACK untheme_text_child(HWND child, LPARAM)
		{
			const int id = GetDlgCtrlID(child);
			if (id == IDC_LAUNCH_BANNER || id == IDC_LAUNCH_BANNER_BOTTOM) {
				return TRUE;
			}

			char cls[32]{};
			GetClassNameA(child, cls, sizeof(cls));
			if (_stricmp(cls, "Button") == 0)
			{
				const LONG type = GetWindowLongA(child, GWL_STYLE) & BS_TYPEMASK;
				if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) {
					return TRUE;
				}
			}

			SetWindowTheme(child, L"", L"");
			InvalidateRect(child, nullptr, TRUE);
			return TRUE;
		}

		static void apply_dark_theme(HWND hwnd)
		{
			SetWindowTheme(hwnd, L"", L"");
			EnumChildWindows(hwnd, untheme_text_child, 0);
		}

		static INT_PTR color_dialog_ctl(dialog_state* st, HDC hdc, HWND child)
		{
			const bool link = st && st->links && child == st->links;
			char cls[32]{};
			GetClassNameA(child, cls, 32);
			const bool button = _stricmp(cls, "Button") == 0;
			LONG type = 0;
			if (button) {
				type = GetWindowLongA(child, GWL_STYLE) & BS_TYPEMASK;
			}
			const bool check_or_radio = button &&
				(type == BS_AUTOCHECKBOX || type == BS_CHECKBOX ||
					type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON);
			SetBkMode(hdc, (button || check_or_radio) ? OPAQUE : TRANSPARENT);
			SetBkColor(hdc, kDlgBg);
			SetTextColor(hdc, link ? kDlgLink :
				(check_or_radio ? kDlgCheckText : kDlgText));
			return reinterpret_cast<INT_PTR>(dlg_bg_brush());
		}

		static DWORD WINAPI update_thread(LPVOID param)
		{
			auto* st = static_cast<dialog_state*>(param);
			const HWND hwnd = st->hwnd;
			st->update = comp::runtime_update::run(
				[hwnd](const char* msg)
				{
					if (hwnd && IsWindow(hwnd) && msg) {
						SetDlgItemTextA(hwnd, IDC_LAUNCH_STATUS, msg);
					}
				});
			if (hwnd && IsWindow(hwnd)) {
				PostMessageA(hwnd, WM_UPDATE_DONE, 0, 0);
			}
			return 0;
		}

		static void draw_link_button(const DRAWITEMSTRUCT* dis)
		{
			if (!dis) {
				return;
			}
			FillRect(dis->hDC, &dis->rcItem, dlg_bg_brush());
			SetBkMode(dis->hDC, TRANSPARENT);
			SetTextColor(dis->hDC, kDlgLink);
			const wchar_t label[] = L"RTX Remix    DXVK    Vibe";
			DrawTextW(dis->hDC, label, -1, const_cast<RECT*>(&dis->rcItem),
				DT_LEFT | DT_VCENTER | DT_SINGLELINE);
		}

		static INT_PTR CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			auto* st = reinterpret_cast<dialog_state*>(GetWindowLongPtrA(hwnd, DWLP_USER));

			switch (msg)
			{
			case WM_INITDIALOG:
			{
				st = reinterpret_cast<dialog_state*>(lparam);
				SetWindowLongPtrA(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(st));
				st->hwnd = hwnd;
				SetWindowTextW(hwnd, L"3D Rad RTX \x2014 launch editor");

				int radio = IDC_LAUNCH_REMIX;
				if (st->backend == shared::globals::launch_backend::dx9) {
					radio = IDC_LAUNCH_DX9;
				}
				else if (st->backend == shared::globals::launch_backend::dxvk) {
					radio = IDC_LAUNCH_DXVK;
				}
				CheckRadioButton(hwnd, IDC_LAUNCH_REMIX, IDC_LAUNCH_DXVK, radio);

				create_repo_links(st, hwnd);
				apply_banner(st, hwnd);
				install_remix_graphics(hwnd);
				apply_dark_theme(hwnd);

				if (HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)))
				{
					EnumChildWindows(hwnd, set_child_font, reinterpret_cast<LPARAM>(font));
				}

				std::string status = "Enter launches RTX Remix. Update downloads vanilla DXVK (d3d9_dxvk.dll) "
					"and the RTX Remix 1.5.2 runtime. The proxy d3d9.dll is never replaced.";
				if (!file_exists(dxvk_path())) {
					status += " d3d9_dxvk.dll is missing — hit Update, or place a 32-bit DXVK d3d9.dll next to the proxy.";
				}
				set_status(hwnd, status.c_str());

				SetForegroundWindow(hwnd);
				return TRUE;
			}
			case WM_SIZE:
				layout_gfx_host(hwnd);
				return FALSE;
			case WM_GETMINMAXINFO:
			{
				auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
				if (mmi)
				{
					mmi->ptMinTrackSize.x = 560;
					mmi->ptMinTrackSize.y = 420;
				}
				SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, 0);
				return TRUE;
			}
			case WM_DRAWITEM:
			{
				const auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
				if (dis && dis->CtlID == IDC_LAUNCH_LINKS) {
					draw_link_button(dis);
					return TRUE;
				}
				break;
			}
			case WM_SETCURSOR:
				if (st && st->links && reinterpret_cast<HWND>(wparam) == st->links)
				{
					SetCursor(LoadCursorA(nullptr, IDC_HAND));
					SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, TRUE);
					return TRUE;
				}
				break;
			case WM_CTLCOLORDLG:
				SetBkColor(reinterpret_cast<HDC>(wparam), kDlgBg);
				return reinterpret_cast<INT_PTR>(dlg_bg_brush());
			case WM_CTLCOLORSTATIC:
			case WM_CTLCOLORBTN:
			{
				HWND child = reinterpret_cast<HWND>(lparam);
				const int id = GetDlgCtrlID(child);
				if (msg == WM_CTLCOLORBTN &&
					(id == IDC_LAUNCH_GO || id == IDC_LAUNCH_SAVE ||
						id == IDC_LAUNCH_UPDATE || id == IDCANCEL))
				{
					break;
				}
				return color_dialog_ctl(st, reinterpret_cast<HDC>(wparam), child);
			}
			case WM_NOTIFY:
			{
				const auto* hdr = reinterpret_cast<NMHDR*>(lparam);
				if (hdr && hdr->idFrom == IDC_LAUNCH_LINKS && hdr->code == NM_CUSTOMDRAW)
				{
					const auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(lparam);
					if (cd->dwDrawStage == CDDS_PREPAINT)
					{
						SetBkMode(cd->hdc, TRANSPARENT);
						SetBkColor(cd->hdc, kDlgBg);
						SetTextColor(cd->hdc, kDlgText);
						SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
						return TRUE;
					}
					if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
					{
						SetBkMode(cd->hdc, TRANSPARENT);
						SetTextColor(cd->hdc, kDlgLink);
						SetWindowLongPtrA(hwnd, DWLP_MSGRESULT, CDRF_NEWFONT);
						return TRUE;
					}
				}
				if (hdr && hdr->idFrom == IDC_LAUNCH_LINKS &&
					(hdr->code == NM_CLICK || hdr->code == NM_RETURN))
				{
					const auto* link = reinterpret_cast<NMLINK*>(lparam);
					if (link && link->item.szUrl[0]) {
						open_url(hwnd, link->item.szUrl);
					}
					return TRUE;
				}
				break;
			}
			case WM_UPDATE_DONE:
			{
				if (!st) {
					return TRUE;
				}
				st->updating = false;
				if (st->worker)
				{
					WaitForSingleObject(st->worker, 1000);
					CloseHandle(st->worker);
					st->worker = nullptr;
				}
				set_busy(hwnd, false);

				std::string status_text = st->update.status;
				if (status_text.empty()) {
					status_text = st->update.error.empty() ? "Update finished." : st->update.error;
				}
				if (!st->update.warning.empty())
				{
					status_text += "\n";
					status_text += st->update.warning;
				}
				set_status(hwnd, status_text.c_str());

				if (!st->update.error.empty() && !st->update.dxvk_ok && !st->update.remix_ok)
				{
					MessageBoxA(hwnd, st->update.error.c_str(), "3D Rad RTX - update failed",
						MB_OK | MB_ICONWARNING);
				}
				else if (!st->update.warning.empty() && st->update.remix_skipped)
				{
					MessageBoxA(hwnd, st->update.warning.c_str(), "3D Rad RTX - Remix update",
						MB_OK | MB_ICONINFORMATION);
				}
				return TRUE;
			}
			case WM_COMMAND:
			{
				if (!st) {
					return FALSE;
				}
				const int id = LOWORD(wparam);
				if (id >= 0x7F00 && id < 0x8100)
				{
					remix_graphics::update_dependent_enables(hwnd);
					return FALSE;
				}
				if (id == IDC_LAUNCH_REMIX || id == IDC_LAUNCH_DX9 || id == IDC_LAUNCH_DXVK)
				{
					remix_graphics::set_enabled(hwnd,
						selected_backend(hwnd) == shared::globals::launch_backend::remix);
					return FALSE;
				}
				if (id == IDC_LAUNCH_LINKS)
				{
					const int x = static_cast<short>(LOWORD(GetMessagePos()));
					RECT r{};
					GetWindowRect(st->links ? st->links : hwnd, &r);
					const int w = std::max(1, static_cast<int>(r.right - r.left));
					const int rel = x - r.left;
					if (rel < w / 3) {
						open_url(hwnd, kLinkRemix);
					}
					else if (rel < (2 * w) / 3) {
						open_url(hwnd, kLinkDxvk);
					}
					else {
						open_url(hwnd, kLinkVibe);
					}
					return TRUE;
				}
				if (id == IDC_LAUNCH_SAVE)
				{
					if (st->updating) {
						return TRUE;
					}
					if (remix_graphics::write_from_dialog(hwnd)) {
						set_status(hwnd, "Saved rtx.conf and user.conf.");
					}
					else {
						set_status(hwnd, "Nothing to save — pick RTX Remix to edit graphics.");
					}
					return TRUE;
				}
				if (id == IDC_LAUNCH_UPDATE)
				{
					if (st->updating) {
						return TRUE;
					}
					st->updating = true;
					st->update = {};
					set_busy(hwnd, true);
					set_status(hwnd, "Starting update…");
					st->worker = CreateThread(nullptr, 0, update_thread, st, 0, nullptr);
					if (!st->worker)
					{
						st->updating = false;
						set_busy(hwnd, false);
						set_status(hwnd, "Could not start update thread.");
						MessageBoxA(hwnd, "Could not start the update worker thread.",
							"3D Rad RTX - update failed", MB_OK | MB_ICONERROR);
					}
					return TRUE;
				}
				if (id == IDC_LAUNCH_GO || id == IDOK)
				{
					if (st->updating) {
						return TRUE;
					}
					st->backend = selected_backend(hwnd);
					if (st->backend == shared::globals::launch_backend::dxvk && !file_exists(dxvk_path()))
					{
						MessageBoxA(hwnd,
							"d3d9_dxvk.dll is missing.\n\n"
							"Hit Update to download vanilla DXVK, or copy a 32-bit DXVK d3d9.dll "
							"next to this proxy as d3d9_dxvk.dll.\n\n"
							"Remix will not be loaded instead.",
							"3D Rad RTX - DXVK missing", MB_OK | MB_ICONWARNING);
						return TRUE;
					}
					if (st->backend == shared::globals::launch_backend::remix && !file_exists(remix_path()))
					{
						MessageBoxA(hwnd,
							"d3d9_remix.dll is missing.\n\n"
							"Hit Update to download the RTX Remix runtime, or place the 32-bit "
							"Remix bridge next to this proxy as d3d9_remix.dll.",
							"3D Rad RTX - Remix missing", MB_OK | MB_ICONWARNING);
						return TRUE;
					}
					if (st->backend == shared::globals::launch_backend::remix) {
						remix_graphics::write_from_dialog(hwnd);
						set_status(hwnd, "Wrote rtx.conf / user.conf — starting Remix.");
						UpdateWindow(hwnd);
						remix_graphics::commit_conf_before_remix();
					}
					EndDialog(hwnd, IDOK);
					return TRUE;
				}
				if (id == IDCANCEL)
				{
					if (st->updating) {
						return TRUE;
					}
					EndDialog(hwnd, IDCANCEL);
					return TRUE;
				}
				return FALSE;
			}
			case WM_CLOSE:
				if (st && st->updating) {
					return TRUE;
				}
				EndDialog(hwnd, IDCANCEL);
				return TRUE;
			case WM_DESTROY:
				if (st)
				{
					if (st->banner_bmp)
					{
						if (st->banner) {
							SendMessageA(st->banner, STM_SETIMAGE, IMAGE_BITMAP, 0);
						}
						DeleteObject(st->banner_bmp);
						st->banner_bmp = nullptr;
					}
					if (st->banner_bottom_bmp)
					{
						if (st->banner_bottom) {
							SendMessageA(st->banner_bottom, STM_SETIMAGE, IMAGE_BITMAP, 0);
						}
						DeleteObject(st->banner_bottom_bmp);
						st->banner_bottom_bmp = nullptr;
					}
				}
				return FALSE;
			default:
				break;
			}
			return FALSE;
		}

		static void init_common_controls_and_gdiplus(dialog_state& st)
		{
			INITCOMMONCONTROLSEX icc{};
			icc.dwSize = sizeof(icc);
			icc.dwICC = ICC_STANDARD_CLASSES | ICC_LINK_CLASS;
			InitCommonControlsEx(&icc);

			Gdiplus::GdiplusStartupInput gdip_in;
			Gdiplus::GdiplusStartup(&st.gdiplus_token, &gdip_in, nullptr);
		}

		[[noreturn]] static void abort_editor_process()
		{
			g_launch_aborted = true;

			shared::common::log("Launch",
				"Launch cancelled — closing console and exiting 3DRad.exe.",
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

			if (shared::common::log_file.is_open()) {
				shared::common::log_file.flush();
				shared::common::log_file.close();
			}
			fflush(stdout);
			fflush(stderr);

			HWND con = GetConsoleWindow();
			if (con && IsWindow(con))
			{
				ShowWindow(con, SW_HIDE);
				PostMessageA(con, WM_CLOSE, 0, 0);
			}
			FreeConsole();
			shared::common::g_external_console_created = false;

			if (g_chain_ready) {
				SetEvent(g_chain_ready);
			}

			ExitProcess(0);
		}

		// true = Launch with out_backend. false = Cancel / window X (caller aborts).
		static bool show_dialog(shared::globals::launch_backend& out_backend)
		{
			dialog_state st;
			st.backend = shared::globals::editor_backend;
			init_common_controls_and_gdiplus(st);

			const INT_PTR ret = DialogBoxParamA(
				shared::globals::dll_hmodule,
				MAKEINTRESOURCEA(IDD_LAUNCH_EDITOR),
				nullptr,
				dlg_proc,
				reinterpret_cast<LPARAM>(&st));

			if (st.worker)
			{
				WaitForSingleObject(st.worker, 15000);
				CloseHandle(st.worker);
				st.worker = nullptr;
			}
			if (st.gdiplus_token) {
				Gdiplus::GdiplusShutdown(st.gdiplus_token);
				st.gdiplus_token = 0;
			}

			if (ret == IDCANCEL)
			{
				shared::common::log("Launch",
					"User cancelled the launch dialog — not loading a D3D backend.",
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				return false;
			}

			if (ret == 0 || ret == -1)
			{
				shared::common::log("Launch",
					std::format("Launch dialog failed (0x{:X}) — defaulting to RTX Remix.", GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				out_backend = shared::globals::launch_backend::remix;
				return true;
			}

			out_backend = st.backend;
			return true;
		}

		static void apply_backend_and_load(shared::globals::launch_backend backend)
		{
			shared::globals::editor_backend = backend;
			shared::common::config::get().save_launch_backend(
				shared::globals::launch_backend_name(backend));

			shared::common::log("Launch",
				std::format("Editor backend: {}", shared::globals::launch_backend_name(backend)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

			if (!d3d9_proxy::init())
			{
				MessageBoxA(nullptr,
					"Could not load a Direct3D 9 implementation.\n"
					"For DXVK, hit Update on the next start or place d3d9_dxvk.dll next to the proxy.\n"
					"For Remix, place d3d9_remix.dll next to the proxy.",
					"3D Rad RTX - d3d9 load failed", MB_OK | MB_ICONERROR);
				return;
			}

			if (backend == shared::globals::launch_backend::remix) {
				comp::start_game_hooks();
			}
			else
			{
				shared::common::log("Launch",
					"Passthrough mode — no MinHook game hooks, no NvRemixBridge, no FFP wrapper.",
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}

			comp::editor_frame::start();
			comp::editor_settings::install_hooks();
		}

		static bool cmdline_is_warmed()
		{
			wchar_t buf[8]{};
			return GetEnvironmentVariableW(L"RTX_COMP_EDITOR_WARMED", buf, 8) > 0 &&
				buf[0] == L'1';
		}

		static void run_dialog_and_load()
		{
			if (g_launch_aborted) {
				abort_editor_process();
			}
			if (d3d9_proxy::get_Direct3DCreate9()) {
				return;
			}

			shared::globals::launch_backend backend = shared::globals::editor_backend;
			if (cmdline_is_warmed())
			{
				shared::common::log("Launch",
					"editor warmed relaunch (RTX_COMP_EDITOR_WARMED) — skip launch dialog, reuse last backend.",
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				apply_backend_and_load(backend);
				if (g_chain_ready) {
					SetEvent(g_chain_ready);
				}
				return;
			}

			shared::common::log("Launch", "3DRad.exe detected — showing launch dialog.");
			if (!show_dialog(backend)) {
				abort_editor_process();
			}
			apply_backend_and_load(backend);
			if (g_chain_ready) {
				SetEvent(g_chain_ready);
			}
		}

		static void restore_exe_entry()
		{
			if (!g_exe_entry_patched || !g_exe_entry) {
				return;
			}
			DWORD old = 0;
			if (VirtualProtect(g_exe_entry, 5, PAGE_EXECUTE_READWRITE, &old))
			{
				memcpy(g_exe_entry, g_exe_entry_orig, 5);
				VirtualProtect(g_exe_entry, 5, old, &old);
				FlushInstructionCache(GetCurrentProcess(), g_exe_entry, 5);
			}
			g_exe_entry_patched = false;
		}

		static void __stdcall editor_entry_thunk()
		{
			restore_exe_entry();
			run_dialog_and_load();
		}

		__declspec(naked) static void hijacked_exe_entry()
		{
			__asm {
				call editor_entry_thunk
				mov eax, g_exe_entry
				jmp eax
			}
		}

		static bool install_exe_entry_hijack()
		{
			HMODULE exe = GetModuleHandleA(nullptr);
			if (!exe) {
				return false;
			}

			auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
				return false;
			}
			auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
				reinterpret_cast<BYTE*>(exe) + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) {
				return false;
			}

			g_exe_entry = reinterpret_cast<BYTE*>(exe) + nt->OptionalHeader.AddressOfEntryPoint;
			memcpy(g_exe_entry_orig, g_exe_entry, 5);

			const INT32 rel = static_cast<INT32>(
				reinterpret_cast<BYTE*>(reinterpret_cast<void*>(&hijacked_exe_entry))
				- (g_exe_entry + 5));
			BYTE jmp[5];
			jmp[0] = 0xE9;
			memcpy(jmp + 1, &rel, 4);

			DWORD old = 0;
			if (!VirtualProtect(g_exe_entry, 5, PAGE_EXECUTE_READWRITE, &old)) {
				return false;
			}
			memcpy(g_exe_entry, jmp, 5);
			VirtualProtect(g_exe_entry, 5, old, &old);
			FlushInstructionCache(GetCurrentProcess(), g_exe_entry, 5);
			g_exe_entry_patched = true;
			return true;
		}

		static DWORD WINAPI dialog_fallback_thread(LPVOID)
		{
			run_dialog_and_load();
			return 0;
		}
	}

	void start_from_dllmain()
	{
		if (!shared::globals::is_editor_host) {
			return;
		}
		if (!g_chain_ready) {
			g_chain_ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
		}

		if (install_exe_entry_hijack())
		{
			shared::common::log("Launch",
				"editor detected — dialog at EXE entry (before WinMain) so Remix hooks CreateWindow.",
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			return;
		}

		shared::common::log("Launch",
			"EXE entry hijack failed — dialog thread, Direct3DCreate9 will wait (no message pump).",
			shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		if (HANDLE t = CreateThread(nullptr, 0, dialog_fallback_thread, nullptr, 0, nullptr)) {
			CloseHandle(t);
		}
	}

	bool aborted()
	{
		return g_launch_aborted;
	}

	void ensure_ready()
	{
		if (!shared::globals::is_editor_host) {
			return;
		}
		if (g_launch_aborted) {
			abort_editor_process();
		}
		if (d3d9_proxy::get_Direct3DCreate9()) {
			return;
		}

		static std::mutex mu;
		std::lock_guard<std::mutex> lock(mu);
		if (g_launch_aborted) {
			abort_editor_process();
		}
		if (d3d9_proxy::get_Direct3DCreate9()) {
			return;
		}

		if (g_chain_ready)
		{
			WaitForSingleObject(g_chain_ready, INFINITE);
			if (g_launch_aborted) {
				abort_editor_process();
			}
			return;
		}

		run_dialog_and_load();
	}
}
