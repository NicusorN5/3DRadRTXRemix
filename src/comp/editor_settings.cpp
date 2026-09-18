#include "std_include.hpp"
#include "editor_settings.hpp"
#include "editor_frame.hpp"

#include "shared/common/config.hpp"

#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "msimg32.lib")

#ifndef TVS_CHECKBOXES
#define TVS_CHECKBOXES 0x0100
#endif

namespace comp::editor_settings
{
	namespace
	{
		constexpr char kDlgClass[] = "3DRadRTXSettings";
		constexpr char kPickerClass[] = "3DRadRTXColorPicker";
		constexpr DWORD kEnginePref = 0x10000000;
		constexpr DWORD kOffMouseW = 0x100B58B0;
		constexpr DWORD kOffMouseH = 0x100B58B4;
		constexpr DWORD kOffRectL = 0x100B70A0;
		constexpr DWORD kOffRectT = 0x100B70A4;
		constexpr DWORD kOffRectR = 0x100B70A8;
		constexpr DWORD kOffRectB = 0x100B70AC;
		constexpr DWORD kOffHwnd = 0x100E38FC;

		constexpr COLORREF kBg = RGB(0, 0, 0);
		constexpr COLORREF kText = RGB(230, 230, 230);
		constexpr COLORREF kMuted = RGB(160, 160, 160);
		constexpr COLORREF kFill = RGB(32, 32, 32);
		constexpr COLORREF kThumb = RGB(90, 90, 90);
		constexpr COLORREF kAccent = RGB(0x7E, 0xB8, 0xFF);

		constexpr int kIdOk = 1;
		constexpr int kIdCancel = 2;
		constexpr int kIdApply = 3;
		constexpr int kIdReset = 4;
		constexpr int kIdMatch = 10;
		constexpr int kDefaultFontSize = 13;
		constexpr const char kDefaultFontName[] = "Segoe UI";
		constexpr int kIdWEdit = 11;
		constexpr int kIdHEdit = 12;
		constexpr int kIdUiScaleEdit = 20;
		constexpr int kIdHudScaleEdit = 21;
		constexpr int kIdFontSizeEdit = 22;
		constexpr int kIdFontList = 23;
		constexpr int kIdAssetList = 24;
		constexpr int kIdPreview = 25;
		constexpr int kIdListWidthEdit = 26;
		constexpr int kIdRowHeightEdit = 27;
		constexpr int kIdCheckSizeEdit = 28;
		constexpr int kIdShowObjectIds = 29;
		constexpr int kDefaultListWidth = 220;
		constexpr int kDefaultRowHeight = 18;
		constexpr int kDefaultCheckSize = 13;
		constexpr COLORREF kDefSel0 = RGB(0x8A, 0x1C, 0x1C);
		constexpr COLORREF kDefSel1 = RGB(0xC4, 0x38, 0x38);
		constexpr COLORREF kDefUns0 = RGB(0x12, 0x12, 0x14);
		constexpr COLORREF kDefUns1 = RGB(0x28, 0x28, 0x2C);
		constexpr COLORREF kDefGrp0 = RGB(224, 144, 50);
		constexpr COLORREF kDefGrp1 = RGB(187, 115, 29);
		constexpr COLORREF kDefHid0 = RGB(0x0C, 0x0C, 0x0E);
		constexpr COLORREF kDefHid1 = RGB(0x1A, 0x1A, 0x1E);

		enum class dlg_kind
		{
			video,
			ui
		};

		struct live_state
		{
			bool match_window = true;
			int width = 0;
			int height = 0;
			int scale = 100;
			int ui_scale = 100;
			int hud_scale = 100;
			int font_size = kDefaultFontSize;
			char font_name[64] = "Segoe UI";
			int list_width = 0;
			int row_height = 0;
			int check_size = 0;
			bool show_object_ids = true;
			COLORREF sel0 = kDefSel0;
			COLORREF sel1 = kDefSel1;
			COLORREF uns0 = kDefUns0;
			COLORREF uns1 = kDefUns1;
			COLORREF grp0 = kDefGrp0;
			COLORREF grp1 = kDefGrp1;
			COLORREF hid0 = kDefHid0;
			COLORREF hid1 = kDefHid1;
		};

		struct dlg_state
		{
			dlg_kind kind = dlg_kind::video;
			HWND hwnd = nullptr;
			HWND owner = nullptr;
			live_state draft{};
			HFONT font = nullptr;
			int dpi = 96;
			int drag = 0; // 1=w 2=h 3=pct 4=ui 5=hud 6=font 7=listw 8=row 9=chk
			RECT rc_w{}, rc_h{}, rc_pct{};
			RECT rc_ui{}, rc_hud{}, rc_fsz{};
			RECT rc_listw{}, rc_row{}, rc_chk{}, rc_ids{};
			RECT rc_c_sel{}, rc_c_uns{}, rc_c_grp{}, rc_c_hid{};
		};

		static live_state g_live{};
		static HWND g_video_dlg = nullptr;
		static HWND g_ui_dlg = nullptr;
		static HMENU g_popup = nullptr;
		static HFONT g_ui_font = nullptr;
		static HFONT g_ui_font_prev = nullptr;
		static std::atomic<long> g_hooks{ 0 };
		static IDirect3DBaseTexture9* g_buttons_tex = nullptr;
		static bool g_buttons_bound = false;
		static D3DMATRIX g_saved_world{};
		static DWORD g_samp_mag = D3DTEXF_POINT;
		static DWORD g_samp_min = D3DTEXF_POINT;
		static DWORD g_samp_mip = D3DTEXF_NONE;
		static bool g_samp_saved = false;
		static bool g_world_scaled = false;
		static int g_last_hit_w = 0;
		static int g_last_hit_h = 0;
		static int g_last_child_w = 0;
		static int g_last_child_h = 0;
		static int g_boot_font_passes = 0;
		static int g_stock_row_h = 0;
		static int g_stock_check_sz = 0;
		static HWND g_object_list = nullptr;
		static HIMAGELIST g_orig_state_iml = nullptr;
		static HIMAGELIST g_orig_tv_state = nullptr;
		static HIMAGELIST g_our_state_iml = nullptr;
		static bool g_list_orig_captured = false;
		static bool g_list_enum_logged = false;
		static bool g_list_subclassed = false;
		static WNDPROC g_list_orig_proc = nullptr;
		static bool g_check_click_armed = false;
		static int g_check_click_item = 0;
		static int g_fwd_mouse = 0;
		static int g_applied_list_w = -1;
		static int g_applied_row = -1;
		static int g_applied_chk = -1;
		static int g_applied_font_size = -1;
		static char g_applied_font_name[64]{};
		static std::atomic<long> g_gdi_hooks{ 0 };
		static int g_skin_load_log = 0;
		static int g_list_sel = -2;
		static int g_grp_log = 0;
		static int g_chk_log = 0;
		static int g_click_log = 0;
		static thread_local int g_in_row_paint = 0;
		static bool g_loadimage_saw_gold = false;
		static bool g_grp_from_ini = false;
		static signed char g_row_last[4096]{};
		static DWORD g_shown_poll_tick = 0;
		static unsigned g_shown_hash = 0;
		static HWND g_picker_hwnd = nullptr;

		struct picker_state
		{
			dlg_state* owner = nullptr;
			int role = 0;
			int stop = 0;
			float hue = 0.f;
			float sat = 1.f;
			float val = 1.f;
			int drag = 0;
			RECT rc_wheel{};
			RECT rc_val{};
			RECT rc_top{};
			RECT rc_bot{};
			bool rgb_mode = false;
			char title[64]{};
			float rgb[3]{ 1.f, 1.f, 1.f };
			HWND notify = nullptr;
			void* rgb_ctx = nullptr;
			void (*rgb_cb)(void*, float, float, float) = nullptr;
		};
		static picker_state g_picker{};

		// Group-member gold comes from the host object graph (same tables as
		// PointLight parent follow). Owner-draw never leaves itemSelectedg
		// pixels in the dest DC — do not hash GradientFill output.
		constexpr std::uintptr_t k_host_pref = 0x00400000;
		constexpr std::uintptr_t k_host_count_va = 0x00450460;
		constexpr std::uintptr_t k_host_list_va = 0x00454468;
		constexpr std::uintptr_t k_host_hmod_va = 0x0044AE58;
		constexpr int k_host_max = 4096;
		constexpr int k_host_header = 0x2930;
		constexpr int k_host_off_child_n = 0x291C;
		constexpr int k_host_off_child_arr = 0x2920;
		constexpr int k_host_off_parent = 0x2924;
		constexpr int k_host_off_linked = 0x2928;
		constexpr int k_host_child_stride = 8;
		constexpr int k_host_child_cap = 256;
		constexpr int k_plugin_off_shown = 0x04;
		constexpr int k_plugin_off_active = 0x0C;
		static const int* g_host_count_at = nullptr;
		static void* const* g_host_list_at = nullptr;
		static HMODULE const* g_host_hmod_at = nullptr;
		static bool g_host_ok = false;
		static bool g_gold_slot[k_host_max]{};
		static int g_gold_n = 0;
		static int g_gold_group_slot = -1;
		static int g_gold_sel_slot = -1;
		static int g_gold_built_sel = -999;
		static int g_gold_built_count = -1;
		static char g_gold_sel_text[128]{};

		enum class skin_row
		{
			unselected,
			selected,
			hidden,
			disabled,
			group
		};

		struct skin_rec
		{
			HBITMAP bmp = nullptr;
			skin_row row = skin_row::unselected;
			bool zebra = false;
			bool checked = false;
			bool has_check = true;
			bool sampled = false;
			COLORREF mid = CLR_INVALID;
			COLORREF chk = CLR_INVALID;
			COLORREF chk2 = CLR_INVALID;
			char name[32]{};
		};
		static skin_rec g_skins[48]{};
		static int g_skin_n = 0;
		constexpr int kStripW = 56;
		constexpr int kStripH = 16;
		// Engine HBITMAPs from 3DRad.exe LoadImageW when the file is
		// itemSelectedg.bmp (logged only — gold rows use the host graph).
		static HBITMAP g_gold_bmp[8]{};
		static int g_gold_bmp_n = 0;
		static COLORREF g_gold_ref[kStripW * kStripH]{};
		static bool g_gold_ref_ok = false;
		static COLORREF g_gold_top_c = RGB(224, 144, 50);
		static COLORREF g_gold_mid_c = RGB(203, 124, 31);
		static COLORREF g_gold_bot_c = RGB(187, 115, 29);
		static char g_gold_path[MAX_PATH]{};
		static int g_gold_w = 350;
		static int g_gold_h = 16;

		using load_image_w_t = HANDLE(WINAPI*)(HINSTANCE, LPCWSTR, UINT, int, int, UINT);
		using load_image_a_t = HANDLE(WINAPI*)(HINSTANCE, LPCSTR, UINT, int, int, UINT);
		using load_bitmap_w_t = HBITMAP(WINAPI*)(HINSTANCE, LPCWSTR);
		using load_bitmap_a_t = HBITMAP(WINAPI*)(HINSTANCE, LPCSTR);
		static load_image_w_t LoadImageW_og = nullptr;
		static load_image_a_t LoadImageA_og = nullptr;
		static load_bitmap_w_t LoadBitmapW_og = nullptr;
		static load_bitmap_a_t LoadBitmapA_og = nullptr;

		static void preview_from_draft(dlg_state& st);
		static void apply_ui_now(bool persist);
		static void apply_video_now(bool persist);

		using create_tex_a_t = HRESULT(WINAPI*)(LPDIRECT3DDEVICE9, LPCSTR, LPDIRECT3DTEXTURE9*);
		using create_tex_ex_a_t = HRESULT(WINAPI*)(LPDIRECT3DDEVICE9, LPCSTR, UINT, UINT, UINT,
			DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, void*, void*, LPDIRECT3DTEXTURE9*);
		static create_tex_a_t CreateTextureFromFileA_og = nullptr;
		static create_tex_ex_a_t CreateTextureFromFileExA_og = nullptr;

		static HBRUSH bg_brush()
		{
			static HBRUSH b = CreateSolidBrush(kBg);
			return b;
		}

		static int clamp_i(int v, int lo, int hi)
		{
			return v < lo ? lo : (v > hi ? hi : v);
		}

		static int query_dpi(HWND hwnd)
		{
			using fn_t = UINT(WINAPI*)(HWND);
			static fn_t fn = reinterpret_cast<fn_t>(
				GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
			if (fn && hwnd)
			{
				const UINT d = fn(hwnd);
				if (d) {
					return static_cast<int>(d);
				}
			}
			return 96;
		}

		static int px(int v, int dpi)
		{
			return MulDiv(v, dpi, 96);
		}

		static std::string ui_dir()
		{
			std::string p = shared::globals::root_path + "\\3DRad_res\\system\\ui";
			if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
				return p;
			}
			return "C:\\3DRadRTX\\3DRad_res\\system\\ui";
		}

		static std::string ini_path()
		{
			if (!shared::globals::root_path.empty()) {
				return shared::globals::root_path + "\\remix-comp-proxy.ini";
			}
			auto& cfg = shared::common::config::get();
			if (!cfg.ini_file().empty()) {
				return cfg.ini_file();
			}
			return "C:\\3DRadRTX\\remix-comp-proxy.ini";
		}

		static void flush_ini()
		{
			WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini_path().c_str());
		}

		static bool write_ini(const char* section, const char* key, const char* value)
		{
			const std::string p = ini_path();
			if (!WritePrivateProfileStringA(section, key, value, p.c_str()))
			{
				shared::common::log("Settings",
					std::format("INI write failed {} [{}] {}={} err=0x{:X}",
						p, section, key, value ? value : "", GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				return false;
			}
			return true;
		}

		static void write_ini_int(const char* section, const char* key, int value)
		{
			char buf[32]{};
			sprintf_s(buf, "%d", value);
			write_ini(section, key, buf);
		}

		static void write_ini_rgb(const char* key, COLORREF c)
		{
			char buf[32]{};
			sprintf_s(buf, "%d,%d,%d", GetRValue(c), GetGValue(c), GetBValue(c));
			write_ini("UI", key, buf);
		}

		static bool parse_rgb(const char* s, COLORREF& out)
		{
			if (!s || !s[0]) {
				return false;
			}
			int r = 0, g = 0, b = 0;
			if (sscanf_s(s, "%d,%d,%d", &r, &g, &b) != 3) {
				return false;
			}
			out = RGB(clamp_i(r, 0, 255), clamp_i(g, 0, 255), clamp_i(b, 0, 255));
			return true;
		}

		static bool read_ini_rgb(const char* key, COLORREF& out)
		{
			char buf[32]{};
			GetPrivateProfileStringA("UI", key, "", buf, sizeof(buf), ini_path().c_str());
			return parse_rgb(buf, out);
		}

		static void stock_list_colors(live_state& s)
		{
			s.sel0 = kDefSel0;
			s.sel1 = kDefSel1;
			s.uns0 = kDefUns0;
			s.uns1 = kDefUns1;
			s.grp0 = kDefGrp0;
			s.grp1 = kDefGrp1;
			s.hid0 = kDefHid0;
			s.hid1 = kDefHid1;
		}

		static void role_colors(const live_state& s, int role, COLORREF& top, COLORREF& bot)
		{
			switch (role)
			{
			case 0: top = s.sel0; bot = s.sel1; break;
			case 1: top = s.uns0; bot = s.uns1; break;
			case 2: top = s.grp0; bot = s.grp1; break;
			default: top = s.hid0; bot = s.hid1; break;
			}
		}

		static void set_role_stop(live_state& s, int role, int stop, COLORREF c)
		{
			COLORREF* p = nullptr;
			if (role == 0) {
				p = stop ? &s.sel1 : &s.sel0;
			}
			else if (role == 1) {
				p = stop ? &s.uns1 : &s.uns0;
			}
			else if (role == 2) {
				p = stop ? &s.grp1 : &s.grp0;
			}
			else {
				p = stop ? &s.hid1 : &s.hid0;
			}
			if (p) {
				*p = c;
			}
		}

		static COLORREF hsv_to_rgb(float h, float s, float v)
		{
			while (h < 0.f) {
				h += 360.f;
			}
			while (h >= 360.f) {
				h -= 360.f;
			}
			s = s < 0.f ? 0.f : (s > 1.f ? 1.f : s);
			v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
			const float c = v * s;
			const float hp = h / 60.f;
			const float x = c * (1.f - fabsf(fmodf(hp, 2.f) - 1.f));
			const float m = v - c;
			float r = 0.f, g = 0.f, b = 0.f;
			if (hp < 1.f) {
				r = c; g = x;
			}
			else if (hp < 2.f) {
				r = x; g = c;
			}
			else if (hp < 3.f) {
				g = c; b = x;
			}
			else if (hp < 4.f) {
				g = x; b = c;
			}
			else if (hp < 5.f) {
				r = x; b = c;
			}
			else {
				r = c; b = x;
			}
			return RGB(
				clamp_i(static_cast<int>((r + m) * 255.f + 0.5f), 0, 255),
				clamp_i(static_cast<int>((g + m) * 255.f + 0.5f), 0, 255),
				clamp_i(static_cast<int>((b + m) * 255.f + 0.5f), 0, 255));
		}

		static void rgb_to_hsv(COLORREF c, float& h, float& s, float& v)
		{
			const float r = static_cast<float>(GetRValue(c)) / 255.f;
			const float g = static_cast<float>(GetGValue(c)) / 255.f;
			const float b = static_cast<float>(GetBValue(c)) / 255.f;
			const float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
			const float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
			const float d = mx - mn;
			v = mx;
			s = (mx <= 0.0001f) ? 0.f : d / mx;
			if (d < 0.0001f) {
				h = 0.f;
			}
			else if (mx == r) {
				h = 60.f * fmodf((g - b) / d, 6.f);
			}
			else if (mx == g) {
				h = 60.f * ((b - r) / d + 2.f);
			}
			else {
				h = 60.f * ((r - g) / d + 4.f);
			}
			if (h < 0.f) {
				h += 360.f;
			}
		}

		static void repaint_object_list()
		{
			if (g_object_list && IsWindow(g_object_list)) {
				InvalidateRect(g_object_list, nullptr, FALSE);
			}
		}

		static BYTE* engine_at(DWORD preferred)
		{
			HMODULE eng = GetModuleHandleA("dll3impact.dll");
			if (!eng) {
				return nullptr;
			}
			return reinterpret_cast<BYTE*>(eng) + (preferred - kEnginePref);
		}

		static bool writable(void* p, size_t n)
		{
			if (!p) {
				return false;
			}
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(p, &mbi, sizeof(mbi))) {
				return false;
			}
			if (mbi.State != MEM_COMMIT) {
				return false;
			}
			const DWORD prot = mbi.Protect & 0xFF;
			if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE) {
				return false;
			}
			(void)n;
			return true;
		}

		static void apply_dark(HWND hwnd)
		{
			BOOL dark = TRUE;
			DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
			DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
			COLORREF black = RGB(0, 0, 0);
			COLORREF text = kText;
			DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &black, sizeof(black));
			DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &black, sizeof(black));
			DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
			SetWindowTheme(hwnd, L"", L"");
		}

		static HFONT make_font(int dpi, int pt, const char* name)
		{
			LOGFONTA lf{};
			lf.lfHeight = -MulDiv(pt > 0 ? pt : 9, dpi, 72);
			lf.lfWeight = FW_NORMAL;
			lf.lfQuality = CLEARTYPE_QUALITY;
			lf.lfCharSet = DEFAULT_CHARSET;
			strncpy_s(lf.lfFaceName, name && name[0] ? name : "Segoe UI", _TRUNCATE);
			return CreateFontIndirectA(&lf);
		}

		static void path_has_buttons(const char* path, IDirect3DTexture9* tex)
		{
			if (!path || !tex) {
				return;
			}
			std::string s(path);
			for (char& c : s)
			{
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				}
				if (c == '/') {
					c = '\\';
				}
			}
			if (s.find("buttons.dds") != std::string::npos ||
				s.find("system\\ui\\buttons") != std::string::npos)
			{
				g_buttons_tex = tex;
				shared::common::log("Settings",
					std::format("buttons.dds texture 0x{:X} from {}",
						reinterpret_cast<std::uintptr_t>(tex), path),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		}

		static HRESULT WINAPI CreateTextureFromFileA_hk(LPDIRECT3DDEVICE9 dev, LPCSTR file, LPDIRECT3DTEXTURE9* out)
		{
			const HRESULT hr = CreateTextureFromFileA_og
				? CreateTextureFromFileA_og(dev, file, out) : E_FAIL;
			if (SUCCEEDED(hr) && out && *out) {
				path_has_buttons(file, *out);
			}
			return hr;
		}

		static HRESULT WINAPI CreateTextureFromFileExA_hk(LPDIRECT3DDEVICE9 dev, LPCSTR file,
			UINT w, UINT h, UINT mips, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
			DWORD filter, DWORD mipfilter, D3DCOLOR key, void* info, void* pal,
			LPDIRECT3DTEXTURE9* out)
		{
			const HRESULT hr = CreateTextureFromFileExA_og
				? CreateTextureFromFileExA_og(dev, file, w, h, mips, usage, fmt, pool,
					filter, mipfilter, key, info, pal, out)
				: E_FAIL;
			if (SUCCEEDED(hr) && out && *out) {
				path_has_buttons(file, *out);
			}
			return hr;
		}

		static void hook_d3dx_tex()
		{
			if (g_hooks.exchange(1) != 0) {
				return;
			}
			HMODULE eng = GetModuleHandleA("dll3impact.dll");
			if (!eng) {
				g_hooks.store(0);
				return;
			}
			auto redirect = [eng](const char* name, void* stub, void** orig)
			{
				const auto slot = shared::utils::mem::find_import_addr(eng, "d3dx9_27.dll", name);
				if (!slot || *orig) {
					return;
				}
				auto** entry = reinterpret_cast<void**>(slot);
				DWORD prot = 0;
				if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &prot)) {
					return;
				}
				*orig = *entry;
				*entry = stub;
				VirtualProtect(entry, sizeof(void*), prot, &prot);
				shared::common::log("Settings",
					std::format("Hooked {} for buttons.dds", name),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			};
			redirect("D3DXCreateTextureFromFileA", CreateTextureFromFileA_hk,
				reinterpret_cast<void**>(&CreateTextureFromFileA_og));
			redirect("D3DXCreateTextureFromFileExA", CreateTextureFromFileExA_hk,
				reinterpret_cast<void**>(&CreateTextureFromFileExA_og));
		}

		static void lower_copy(char* dst, size_t dst_n, const char* src)
		{
			if (!dst || dst_n == 0) {
				return;
			}
			size_t i = 0;
			for (; src && src[i] && i + 1 < dst_n; ++i)
			{
				char c = src[i];
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				}
				if (c == '/') {
					c = '\\';
				}
				dst[i] = c;
			}
			dst[i] = 0;
		}

		static const char* file_name_of(const char* path)
		{
			if (!path) {
				return "";
			}
			const char* s = path;
			for (const char* p = path; *p; ++p)
			{
				if (*p == '\\' || *p == '/') {
					s = p + 1;
				}
			}
			return s;
		}

		static bool name_is_list_skin(const char* lower_path)
		{
			if (!lower_path || !lower_path[0]) {
				return false;
			}
			const char* n = file_name_of(lower_path);
			return std::strstr(n, "itemselected") != nullptr ||
				std::strstr(n, "itemunselected") != nullptr ||
				std::strstr(n, "itemhidden") != nullptr;
		}

		static skin_rec parse_skin_name(const char* lower_path)
		{
			skin_rec r{};
			const char* n = file_name_of(lower_path);
			const bool has_d = std::strstr(n, "d.bmp") != nullptr ||
				std::strstr(n, "dg.bmp") != nullptr ||
				std::strstr(n, "selectedd") != nullptr;
			const bool has_g = std::strstr(n, "g.bmp") != nullptr;
			const bool has_c = std::strstr(n, "c.bmp") != nullptr ||
				std::strstr(n, "1c") != nullptr ||
				std::strstr(n, "0c") != nullptr;
			if (std::strstr(n, "itemhidden")) {
				r.row = has_d ? skin_row::disabled : skin_row::hidden;
				r.zebra = std::strstr(n, "itemhidden1") != nullptr;
				// C = checked glyph baked into the 350x16 skin (visibility on).
				r.checked = has_c;
				r.has_check = true;
			}
			else if (std::strstr(n, "itemselected"))
			{
				// itemSelectedg.bmp = yellow/tan group-linked members of the
				// currently selected Group. itemSelected.bmp = crimson.
				// itemSelectedDg.bmp is the dim disabled+group skin, not gold.
				if (has_d) {
					r.row = skin_row::disabled;
				}
				else if (has_g || std::strstr(n, "selectedg") != nullptr) {
					r.row = skin_row::group;
				}
				else {
					r.row = skin_row::selected;
				}
				r.zebra = std::strstr(n, "selected1") != nullptr;
				r.checked = has_c;
				r.has_check = true;
			}
			else
			{
				r.row = has_d ? skin_row::disabled : skin_row::unselected;
				r.zebra = std::strstr(n, "unselected1") != nullptr;
				r.checked = has_c;
				r.has_check = true;
			}
			return r;
		}

		static void remember_skin(HBITMAP bmp, const char* lower_path)
		{
			if (!bmp || g_skin_n >= static_cast<int>(sizeof(g_skins) / sizeof(g_skins[0]))) {
				return;
			}
			for (int i = 0; i < g_skin_n; ++i)
			{
				if (g_skins[i].bmp == bmp) {
					return;
				}
			}
			g_skins[g_skin_n] = parse_skin_name(lower_path);
			g_skins[g_skin_n].bmp = bmp;
			strncpy_s(g_skins[g_skin_n].name, file_name_of(lower_path), _TRUNCATE);
			++g_skin_n;
		}

		static bool color_is_red_skin(COLORREF c)
		{
			return GetRValue(c) > 130 && GetGValue(c) < 90 && GetBValue(c) < 90;
		}

		static bool color_is_gold_skin(COLORREF c)
		{
			const int r = GetRValue(c);
			const int g = GetGValue(c);
			const int b = GetBValue(c);
			return r > 160 && g > 90 && g < 230 && b < 110 &&
				(r - b) > 60 && (g - b) > 30;
		}

		static bool name_is_gold_skin(const char* lower_path)
		{
			const char* n = file_name_of(lower_path);
			// itemSelectedg.bmp / itemSelectedgC.bmp. Not itemSelectedDg.bmp
			// ("itemselecteddg" does not contain "itemselectedg").
			return n && n[0] && std::strstr(n, "itemselectedg") != nullptr;
		}

		static bool is_gold_handle(HBITMAP bmp)
		{
			if (!bmp) {
				return false;
			}
			for (int i = 0; i < g_gold_bmp_n; ++i)
			{
				if (g_gold_bmp[i] == bmp) {
					return true;
				}
			}
			return false;
		}

		static void remember_gold_bmp(HBITMAP bmp)
		{
			if (!bmp || is_gold_handle(bmp)) {
				return;
			}
			if (g_gold_bmp_n >= static_cast<int>(sizeof(g_gold_bmp) / sizeof(g_gold_bmp[0]))) {
				return;
			}
			g_gold_bmp[g_gold_bmp_n++] = bmp;
			g_loadimage_saw_gold = true;
		}

		static int lum_of(COLORREF c)
		{
			return (GetRValue(c) * 3 + GetGValue(c) * 6 + GetBValue(c)) / 10;
		}

		static int color_dist(COLORREF a, COLORREF b)
		{
			if (a == CLR_INVALID || b == CLR_INVALID) {
				return 1000;
			}
			return abs(GetRValue(a) - GetRValue(b)) +
				abs(GetGValue(a) - GetGValue(b)) +
				abs(GetBValue(a) - GetBValue(b));
		}

		// Copy dest pixels with BitBlt-to-DIB (not a process-wide hook).
		// DRAWITEM GetPixel returns CLR_INVALID on this engine DC.
		static bool grab_item_strip(HDC hdc, int x, int y, int w, int h, COLORREF* px)
		{
			if (!hdc || !px || w < 1 || h < 1) {
				return false;
			}
			BITMAPINFO bi{};
			bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bi.bmiHeader.biWidth = w;
			bi.bmiHeader.biHeight = -h;
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			bi.bmiHeader.biCompression = BI_RGB;
			void* bits = nullptr;
			HDC mem = CreateCompatibleDC(hdc);
			if (!mem) {
				return false;
			}
			HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
			if (!dib || !bits)
			{
				DeleteDC(mem);
				return false;
			}
			HGDIOBJ old = SelectObject(mem, dib);
			const BOOL ok = BitBlt(mem, 0, 0, w, h, hdc, x, y, SRCCOPY);
			SelectObject(mem, old);
			if (ok)
			{
				const auto* src = static_cast<const unsigned char*>(bits);
				const int stride = w * 4;
				for (int j = 0; j < h; ++j)
				{
					const unsigned char* row = src + j * stride;
					for (int i = 0; i < w; ++i)
					{
						px[j * w + i] = RGB(row[i * 4 + 2], row[i * 4 + 1], row[i * 4]);
					}
				}
			}
			DeleteObject(dib);
			DeleteDC(mem);
			return ok != 0;
		}

		static bool strip_has_check_glyph(const COLORREF* px, int w, int h)
		{
			if (!px || w < 16 || h < 8) {
				return false;
			}
			const int cy = h / 2;
			const COLORREF fill = px[cy * w + 4];
			const int fl = lum_of(fill);
			int ink = 0;
			int n = 0;
			for (int y = cy - 3; y <= cy + 3; ++y)
			{
				if (y < 1 || y >= h - 1) {
					continue;
				}
				for (int x = 4; x <= 11; ++x)
				{
					++n;
					if (abs(lum_of(px[y * w + x]) - fl) > 40) {
						++ink;
					}
				}
			}
			return n > 0 && ink >= 6;
		}

		static bool strip_mostly_black(const COLORREF* px, int w, int h)
		{
			if (!px || w < 1 || h < 1) {
				return true;
			}
			const int n = w * h;
			int dark = 0;
			for (int i = 0; i < n; ++i)
			{
				if (lum_of(px[i]) < 18) {
					++dark;
				}
			}
			return dark > n * 3 / 4;
		}

		static void sample_skin_bmp(skin_rec& sk)
		{
			if (sk.sampled || !sk.bmp) {
				return;
			}
			sk.sampled = true;
			HDC mem = CreateCompatibleDC(nullptr);
			if (!mem) {
				return;
			}
			HGDIOBJ old = SelectObject(mem, sk.bmp);
			sk.chk = GetPixel(mem, 8, 8);
			sk.chk2 = GetPixel(mem, 10, 5);
			sk.mid = GetPixel(mem, 48, 8);
			COLORREF box[kStripW * kStripH]{};
			if (grab_item_strip(mem, 0, 0, kStripW, 16, box))
			{
				if (sk.mid == CLR_INVALID)
				{
					sk.chk = box[8 * kStripW + 8];
					sk.chk2 = box[5 * kStripW + 10];
					sk.mid = box[8 * kStripW + 48];
				}
				if (strip_has_check_glyph(box, kStripW, 16)) {
					sk.checked = true;
				}
			}
			SelectObject(mem, old);
			DeleteDC(mem);
		}

		static const skin_rec* match_strip_skin(const COLORREF* px, int w, int h)
		{
			if (!px || w < 16 || h < 4 || g_skin_n < 1) {
				return nullptr;
			}
			const int cy = clamp_i(h / 2, 1, h - 1);
			const COLORREF mid = px[cy * w + clamp_i(48, 0, w - 1)];
			const COLORREF chk = px[cy * w + clamp_i(8, 0, w - 1)];
			const COLORREF chk2 = px[clamp_i(cy - 2, 0, h - 1) * w + clamp_i(10, 0, w - 1)];
			int best_i = -1;
			int best_d = 1 << 30;
			for (int i = 0; i < g_skin_n; ++i)
			{
				sample_skin_bmp(g_skins[i]);
				if (g_skins[i].mid == CLR_INVALID) {
					continue;
				}
				const int d = color_dist(mid, g_skins[i].mid) +
					color_dist(chk, g_skins[i].chk) +
					color_dist(chk2, g_skins[i].chk2);
				if (d < best_d)
				{
					best_d = d;
					best_i = i;
				}
			}
			if (best_i < 0 || best_d > 160) {
				return nullptr;
			}
			return &g_skins[best_i];
		}

		static void sample_gold_from_bmp(HBITMAP bmp, const char* path)
		{
			if (!bmp) {
				return;
			}
			remember_gold_bmp(bmp);
			if (g_gold_ref_ok)
			{
				if (path && path[0] && !g_gold_path[0]) {
					strncpy_s(g_gold_path, path, _TRUNCATE);
				}
				return;
			}
			BITMAP bm{};
			if (GetObjectA(bmp, sizeof(bm), &bm))
			{
				if (bm.bmWidth > 0) {
					g_gold_w = bm.bmWidth;
				}
				if (bm.bmHeight > 0) {
					g_gold_h = bm.bmHeight;
				}
			}
			HDC mem = CreateCompatibleDC(nullptr);
			if (!mem) {
				return;
			}
			HGDIOBJ old = SelectObject(mem, bmp);
			COLORREF box[kStripW * kStripH]{};
			const bool ok = grab_item_strip(mem, 0, 0, kStripW, kStripH, box);
			SelectObject(mem, old);
			DeleteDC(mem);
			if (!ok) {
				return;
			}
			std::memcpy(g_gold_ref, box, sizeof(g_gold_ref));
			g_gold_ref_ok = true;
			g_gold_top_c = box[0 * kStripW + 48];
			g_gold_mid_c = box[8 * kStripW + 48];
			g_gold_bot_c = box[15 * kStripW + 48];
			if (!g_grp_from_ini)
			{
				g_live.grp0 = g_gold_top_c;
				g_live.grp1 = g_gold_bot_c;
			}
			if (path && path[0]) {
				strncpy_s(g_gold_path, path, _TRUNCATE);
			}
			shared::common::log("Settings",
				std::format(
					"itemSelectedg '{}' {}x{} handle=0x{:X} mid=({},{},{}) top=({},{},{}) bot=({},{},{})",
					g_gold_path[0] ? g_gold_path : (path ? path : ""),
					g_gold_w, g_gold_h,
					reinterpret_cast<std::uintptr_t>(bmp),
					GetRValue(g_gold_mid_c), GetGValue(g_gold_mid_c), GetBValue(g_gold_mid_c),
					GetRValue(g_gold_top_c), GetGValue(g_gold_top_c), GetBValue(g_gold_top_c),
					GetRValue(g_gold_bot_c), GetGValue(g_gold_bot_c), GetBValue(g_gold_bot_c)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		static bool strip_matches_gold_ref(const COLORREF* px, int w, int h)
		{
			if (!px || w < 24 || h < 4) {
				return false;
			}
			if (g_gold_ref_ok)
			{
				int hit = 0;
				int n = 0;
				const int y0 = clamp_i(h / 8, 1, h - 2);
				const int y1 = clamp_i(h - h / 8, y0 + 1, h - 1);
				for (int y = y0; y < y1; ++y)
				{
					const int yr = clamp_i(y * kStripH / (h < 1 ? 1 : h), 0, kStripH - 1);
					for (int x = 20; x < w && x < kStripW; ++x)
					{
						++n;
						if (color_dist(px[y * w + x], g_gold_ref[yr * kStripW + x]) <= 48) {
							++hit;
						}
					}
				}
				if (n > 8 && hit * 4 >= n * 3) {
					return true;
				}
				const int cy = clamp_i(h / 2, 0, h - 1);
				const COLORREF mid = px[cy * w + clamp_i(48, 0, w - 1)];
				const COLORREF top = px[clamp_i(2, 0, h - 1) * w + clamp_i(48, 0, w - 1)];
				if (color_dist(mid, g_gold_mid_c) <= 40 && color_dist(top, g_gold_top_c) <= 56) {
					return true;
				}
			}
			const int cy = clamp_i(h / 2, 0, h - 1);
			return color_is_gold_skin(px[cy * w + clamp_i(48, 0, w - 1)]) ||
				color_is_gold_skin(px[clamp_i(2, 0, h - 1) * w + clamp_i(48, 0, w - 1)]);
		}

		static void ensure_gold_skin_files()
		{
			const char* src = "C:\\3D Rad\\3DRad_res\\system\\ui\\itemSelectedg.bmp";
			const char* dst_dir_fixed = "C:\\3DRadRTX\\3DRad_res\\system\\ui";
			const std::string live = ui_dir();
			auto copy_into = [src](const char* dir)
			{
				if (!dir || !dir[0]) {
					return;
				}
				char dst[MAX_PATH]{};
				sprintf_s(dst, "%s\\itemSelectedg.bmp", dir);
				if (GetFileAttributesA(dst) != INVALID_FILE_ATTRIBUTES) {
					if (!g_gold_path[0]) {
						strncpy_s(g_gold_path, dst, _TRUNCATE);
					}
					return;
				}
				if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) {
					return;
				}
				CreateDirectoryA(dir, nullptr);
				if (CopyFileA(src, dst, TRUE))
				{
					strncpy_s(g_gold_path, dst, _TRUNCATE);
					shared::common::log("Settings",
						std::format("copied itemSelectedg.bmp -> '{}' (engine LoadImage path)", dst),
						shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				}
			};
			copy_into(live.c_str());
			if (live != dst_dir_fixed) {
				copy_into(dst_dir_fixed);
			}
			if (!g_gold_path[0] && GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) {
				return;
			}
			if (!g_gold_path[0] && GetFileAttributesA(src) != INVALID_FILE_ATTRIBUTES) {
				strncpy_s(g_gold_path, src, _TRUNCATE);
			}
			if (g_gold_path[0])
			{
				shared::common::log("Settings",
					std::format("itemSelectedg found on disk '{}'", g_gold_path),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		}

		static void sample_gold_from_file()
		{
			if (g_gold_ref_ok) {
				return;
			}
			ensure_gold_skin_files();
			const std::string live = ui_dir() + "\\itemSelectedg.bmp";
			const char* cands[] = {
				g_gold_path[0] ? g_gold_path : live.c_str(),
				live.c_str(),
				"C:\\3DRadRTX\\3DRad_res\\system\\ui\\itemSelectedg.bmp",
				"C:\\3D Rad\\3DRad_res\\system\\ui\\itemSelectedg.bmp",
			};
			auto load = LoadImageW_og;
			if (!load)
			{
				load = reinterpret_cast<load_image_w_t>(
					GetProcAddress(GetModuleHandleA("user32.dll"), "LoadImageW"));
			}
			for (const char* p : cands)
			{
				if (!p || !p[0] || GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
					continue;
				}
				wchar_t wpath[MAX_PATH]{};
				MultiByteToWideChar(CP_ACP, 0, p, -1, wpath, MAX_PATH);
				HANDLE img = load
					? load(nullptr, wpath, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE)
					: nullptr;
				if (!img) {
					continue;
				}
				sample_gold_from_bmp(static_cast<HBITMAP>(img), p);
				if (g_gold_ref_ok) {
					return;
				}
			}
			shared::common::log("Settings",
				"itemSelectedg.bmp not loaded - group gold will use hardcoded amber",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}

		static void preload_list_skins()
		{
			static bool once = false;
			if (once) {
				return;
			}
			once = true;
			const std::string dir = ui_dir();
			const std::string glob = dir + "\\item*.bmp";
			WIN32_FIND_DATAA fd{};
			HANDLE find = FindFirstFileA(glob.c_str(), &fd);
			if (find == INVALID_HANDLE_VALUE) {
				return;
			}
			auto load = LoadImageW_og;
			if (!load)
			{
				load = reinterpret_cast<load_image_w_t>(
					GetProcAddress(GetModuleHandleA("user32.dll"), "LoadImageW"));
			}
			int n = 0;
			do
			{
				char full[MAX_PATH]{};
				char lower[MAX_PATH]{};
				sprintf_s(full, "%s\\%s", dir.c_str(), fd.cFileName);
				lower_copy(lower, sizeof(lower), full);
				if (!name_is_list_skin(lower)) {
					continue;
				}
				wchar_t wpath[MAX_PATH]{};
				MultiByteToWideChar(CP_ACP, 0, full, -1, wpath, MAX_PATH);
				HANDLE img = load
					? load(nullptr, wpath, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE)
					: nullptr;
				if (img) {
					remember_skin(static_cast<HBITMAP>(img), lower);
					if (name_is_gold_skin(lower)) {
						sample_gold_from_bmp(static_cast<HBITMAP>(img), full);
					}
					++n;
				}
			} while (FindNextFileA(find, &fd));
			FindClose(find);
			shared::common::log("Settings",
				std::format("preloaded {} item*.bmp list skins (check from C suffix + glyph)", n),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		static COLORREF lerp_c(COLORREF a, COLORREF b, int t, int n)
		{
			if (n <= 1) {
				return a;
			}
			const int r = GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t / (n - 1);
			const int g = GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t / (n - 1);
			const int bl = GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t / (n - 1);
			return RGB(r, g, bl);
		}

		static void fill_vgrad(HDC hdc, const RECT& r, COLORREF top, COLORREF bot)
		{
			if (r.right <= r.left || r.bottom <= r.top) {
				return;
			}
			TRIVERTEX v[2]{};
			v[0].x = r.left;
			v[0].y = r.top;
			v[0].Red = static_cast<COLOR16>(GetRValue(top) << 8);
			v[0].Green = static_cast<COLOR16>(GetGValue(top) << 8);
			v[0].Blue = static_cast<COLOR16>(GetBValue(top) << 8);
			v[1].x = r.right;
			v[1].y = r.bottom;
			v[1].Red = static_cast<COLOR16>(GetRValue(bot) << 8);
			v[1].Green = static_cast<COLOR16>(GetGValue(bot) << 8);
			v[1].Blue = static_cast<COLOR16>(GetBValue(bot) << 8);
			GRADIENT_RECT gr{ 0, 1 };
			if (!GradientFill(hdc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V))
			{
				const int h = r.bottom - r.top;
				for (int y = 0; y < h; ++y)
				{
					RECT line{ r.left, r.top + y, r.right, r.top + y + 1 };
					HBRUSH br = CreateSolidBrush(lerp_c(top, bot, y, h));
					FillRect(hdc, &line, br);
					DeleteObject(br);
				}
			}
		}

		static void fill_circle_grad(HDC hdc, const RECT& r, COLORREF top, COLORREF bot)
		{
			if (r.right - r.left < 4 || r.bottom - r.top < 4) {
				return;
			}
			HRGN rgn = CreateEllipticRgn(r.left, r.top, r.right, r.bottom);
			if (rgn)
			{
				const int saved = SaveDC(hdc);
				SelectClipRgn(hdc, rgn);
				fill_vgrad(hdc, r, top, bot);
				RestoreDC(hdc, saved);
				DeleteObject(rgn);
			}
			HPEN pen = CreatePen(PS_SOLID, 1, RGB(210, 210, 214));
			HGDIOBJ oldp = SelectObject(hdc, pen);
			HGDIOBJ oldb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
			Ellipse(hdc, r.left, r.top, r.right, r.bottom);
			SelectObject(hdc, oldp);
			SelectObject(hdc, oldb);
			DeleteObject(pen);
		}

		static void row_colors(skin_row row, bool zebra, COLORREF& top, COLORREF& bot, COLORREF& text)
		{
			switch (row)
			{
			case skin_row::selected:
				top = g_live.sel0;
				bot = g_live.sel1;
				text = RGB(255, 255, 255);
				break;
			case skin_row::group:
				top = g_live.grp0;
				bot = g_live.grp1;
				text = RGB(0x22, 0x14, 0x08);
				break;
			case skin_row::hidden:
				top = g_live.hid0;
				bot = g_live.hid1;
				if (zebra)
				{
					top = lerp_c(top, RGB(40, 40, 44), 1, 5);
					bot = lerp_c(bot, RGB(48, 48, 52), 1, 5);
				}
				text = RGB(150, 150, 155);
				break;
			case skin_row::disabled:
				top = RGB(0x08, 0x08, 0x08);
				bot = RGB(0x14, 0x14, 0x14);
				text = RGB(110, 110, 110);
				break;
			default:
				top = g_live.uns0;
				bot = g_live.uns1;
				if (zebra)
				{
					top = lerp_c(top, RGB(40, 40, 44), 1, 4);
					bot = lerp_c(bot, RGB(56, 56, 60), 1, 4);
				}
				text = RGB(224, 224, 228);
				break;
			}
		}

		static HBITMAP replace_skin_bitmap(HBITMAP orig, const char* lower_path)
		{
			(void)lower_path;
			return orig;
		}

		static void note_skin_load(const char* src, const char* path, HANDLE img, UINT type)
		{
			if (type != IMAGE_BITMAP || !img || !path) {
				return;
			}
			char lower[MAX_PATH]{};
			lower_copy(lower, sizeof(lower), path);
			if (!name_is_list_skin(lower)) {
				return;
			}
			remember_skin(static_cast<HBITMAP>(img), lower);
			if (name_is_gold_skin(lower)) {
				g_loadimage_saw_gold = true;
				sample_gold_from_bmp(static_cast<HBITMAP>(img), path);
			}
			if (g_skin_load_log < 24)
			{
				++g_skin_load_log;
				BITMAP bm{};
				GetObjectA(img, sizeof(bm), &bm);
				shared::common::log("Settings",
					std::format("list skin {} '{}' via {} {}x{} handle=0x{:X}",
						file_name_of(lower), path, src, bm.bmWidth, bm.bmHeight,
						reinterpret_cast<std::uintptr_t>(img)),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		}

		static HANDLE WINAPI LoadImageW_hk(HINSTANCE inst, LPCWSTR name, UINT type, int cx, int cy, UINT flags)
		{
			HANDLE img = LoadImageW_og
				? LoadImageW_og(inst, name, type, cx, cy, flags) : nullptr;
			if (img && type == IMAGE_BITMAP && name && !IS_INTRESOURCE(name))
			{
				char path[MAX_PATH]{};
				WideCharToMultiByte(CP_ACP, 0, name, -1, path, sizeof(path), nullptr, nullptr);
				char lower[MAX_PATH]{};
				lower_copy(lower, sizeof(lower), path);
				if (name_is_list_skin(lower))
				{
					HBITMAP neu = replace_skin_bitmap(static_cast<HBITMAP>(img), lower);
					img = neu;
					note_skin_load("LoadImageW", path, img, type);
				}
			}
			return img;
		}

		static HANDLE WINAPI LoadImageA_hk(HINSTANCE inst, LPCSTR name, UINT type, int cx, int cy, UINT flags)
		{
			HANDLE img = LoadImageA_og
				? LoadImageA_og(inst, name, type, cx, cy, flags) : nullptr;
			if (img && type == IMAGE_BITMAP && name && !IS_INTRESOURCE(name))
			{
				char lower[MAX_PATH]{};
				lower_copy(lower, sizeof(lower), name);
				if (name_is_list_skin(lower))
				{
					HBITMAP neu = replace_skin_bitmap(static_cast<HBITMAP>(img), lower);
					img = neu;
					note_skin_load("LoadImageA", name, img, type);
				}
			}
			return img;
		}

		static HBITMAP WINAPI LoadBitmapW_hk(HINSTANCE inst, LPCWSTR name)
		{
			HBITMAP img = LoadBitmapW_og ? LoadBitmapW_og(inst, name) : nullptr;
			if (img && name && !IS_INTRESOURCE(name))
			{
				char path[MAX_PATH]{};
				WideCharToMultiByte(CP_ACP, 0, name, -1, path, sizeof(path), nullptr, nullptr);
				char lower[MAX_PATH]{};
				lower_copy(lower, sizeof(lower), path);
				if (name_is_list_skin(lower))
				{
					img = replace_skin_bitmap(img, lower);
					note_skin_load("LoadBitmapW", path, img, IMAGE_BITMAP);
				}
			}
			return img;
		}

		static HBITMAP WINAPI LoadBitmapA_hk(HINSTANCE inst, LPCSTR name)
		{
			HBITMAP img = LoadBitmapA_og ? LoadBitmapA_og(inst, name) : nullptr;
			if (img && name && !IS_INTRESOURCE(name))
			{
				char lower[MAX_PATH]{};
				lower_copy(lower, sizeof(lower), name);
				if (name_is_list_skin(lower))
				{
					img = replace_skin_bitmap(img, lower);
					note_skin_load("LoadBitmapA", name, img, IMAGE_BITMAP);
				}
			}
			return img;
		}

		static void iat_redirect(HMODULE mod, const char* dll, const char* name, void* stub, void** orig)
		{
			if (!mod || !dll || !name || !stub || !orig) {
				return;
			}
			const auto slot = shared::utils::mem::find_import_addr(mod, dll, name);
			if (!slot) {
				return;
			}
			auto** entry = reinterpret_cast<void**>(slot);
			if (*entry == stub) {
				return;
			}
			DWORD prot = 0;
			if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &prot)) {
				return;
			}
			if (!*orig) {
				*orig = *entry;
			}
			*entry = stub;
			VirtualProtect(entry, sizeof(void*), prot, &prot);
			char mod_name[MAX_PATH]{};
			GetModuleFileNameA(mod, mod_name, MAX_PATH);
			shared::common::log("Settings",
				std::format("Hooked {}!{} in {} for object-list skins",
					dll, name, file_name_of(mod_name)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		static void hook_list_skins()
		{
			if (g_gdi_hooks.exchange(1) != 0) {
				return;
			}
			auto hook_mod = [](HMODULE mod)
			{
				if (!mod) {
					return;
				}
				iat_redirect(mod, "USER32.dll", "LoadImageW", LoadImageW_hk,
					reinterpret_cast<void**>(&LoadImageW_og));
				iat_redirect(mod, "USER32.dll", "LoadImageA", LoadImageA_hk,
					reinterpret_cast<void**>(&LoadImageA_og));
				iat_redirect(mod, "USER32.dll", "LoadBitmapW", LoadBitmapW_hk,
					reinterpret_cast<void**>(&LoadBitmapW_og));
				iat_redirect(mod, "USER32.dll", "LoadBitmapA", LoadBitmapA_hk,
					reinterpret_cast<void**>(&LoadBitmapA_og));
				// Never IAT-hook GDI32 BitBlt/StretchBlt. A process-wide stub
				// deadlocks GDI or blacks out the wrapper caption. Group rows
				// are detected from the host object graph on LBN_SELCHANGE.
			};
			// LoadImage IAT on 3DRad.exe only. Never hook GDI32 BitBlt
			// (that blacked out the caption). Never MinHook user32
			// LoadImageW (process-wide). dll3impact is not hooked here.
			ensure_gold_skin_files();
			hook_mod(GetModuleHandleA(nullptr));
			shared::common::log("Settings",
				"object-list skins: 3DRad.exe LoadImage IAT only (no BitBlt hook; group gold via host object graph)",
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			preload_list_skins();
			sample_gold_from_file();
		}

		static int scale_from_pane(int width, int pane_w)
		{
			if (pane_w < 1) {
				return 100;
			}
			return clamp_i(MulDiv(width, 100, pane_w), 25, 200);
		}

		static void save_video()
		{
			if (g_live.match_window) {
				g_live.scale = 100;
			}
			write_ini_int("Video", "Width", g_live.match_window && g_live.width < 64 ? 0 : g_live.width);
			write_ini_int("Video", "Height", g_live.match_window && g_live.height < 64 ? 0 : g_live.height);
			write_ini("Video", "MatchWindow", g_live.match_window ? "1" : "0");
			write_ini_int("Video", "Scale", g_live.scale);
			flush_ini();
			auto& cfg = shared::common::config::get();
			cfg.video.match_window = g_live.match_window;
			cfg.video.width = g_live.width;
			cfg.video.height = g_live.height;
			cfg.video.scale = g_live.scale;
			shared::common::log("Settings",
				std::format("saved {} [Video] MatchWindow={} Width={} Height={} Scale={}",
					ini_path(), g_live.match_window ? 1 : 0,
					g_live.width, g_live.height, g_live.scale),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		static void save_ui()
		{
			write_ini_int("UI", "Scale", g_live.ui_scale);
			write_ini_int("UI", "HudScale", g_live.hud_scale);
			write_ini_int("UI", "FontSize", g_live.font_size);
			write_ini("UI", "FontName", g_live.font_name);
			write_ini_int("UI", "ListWidth", g_live.list_width);
			write_ini_int("UI", "RowHeight", g_live.row_height);
			write_ini_int("UI", "CheckSize", g_live.check_size);
			write_ini("UI", "ShowObjectIds", g_live.show_object_ids ? "1" : "0");
			write_ini_rgb("ColorSelected0", g_live.sel0);
			write_ini_rgb("ColorSelected1", g_live.sel1);
			write_ini_rgb("ColorUnselected0", g_live.uns0);
			write_ini_rgb("ColorUnselected1", g_live.uns1);
			write_ini_rgb("ColorGroup0", g_live.grp0);
			write_ini_rgb("ColorGroup1", g_live.grp1);
			write_ini_rgb("ColorHidden0", g_live.hid0);
			write_ini_rgb("ColorHidden1", g_live.hid1);
			flush_ini();
			auto& cfg = shared::common::config::get();
			cfg.ui.scale = g_live.ui_scale;
			cfg.ui.hud_scale = g_live.hud_scale;
			cfg.ui.font_size = g_live.font_size;
			cfg.ui.font_name = g_live.font_name;
			cfg.ui.list_width = g_live.list_width;
			cfg.ui.row_height = g_live.row_height;
			cfg.ui.check_size = g_live.check_size;
			cfg.ui.show_object_ids = g_live.show_object_ids;
			shared::common::log("Settings",
				std::format(
					"saved {} [UI] Scale={} HudScale={} FontSize={} FontName={} "
					"ListWidth={} RowHeight={} CheckSize={} ShowObjectIds={} colors",
					ini_path(), g_live.ui_scale, g_live.hud_scale,
					g_live.font_size, g_live.font_name,
					g_live.list_width, g_live.row_height, g_live.check_size,
					g_live.show_object_ids ? 1 : 0),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		static BOOL CALLBACK set_font_child_post(HWND child, LPARAM lp)
		{
			char cls[64]{};
			GetClassNameA(child, cls, sizeof(cls));
			if (std::strcmp(cls, "ChildClass") == 0 ||
				std::strcmp(cls, "3DRADCLASS") == 0 ||
				std::strcmp(cls, kDlgClass) == 0)
			{
				return TRUE;
			}
			// Post, never SendMessage into 3DRADCLASS / ChildClass (deadlock).
			PostMessageA(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
			InvalidateRect(child, nullptr, TRUE);
			return TRUE;
		}

		static void apply_fonts(HWND editor)
		{
			if (!editor || !IsWindow(editor) || !g_live.font_name[0]) {
				return;
			}
			if (g_ui_font_prev) {
				DeleteObject(g_ui_font_prev);
				g_ui_font_prev = nullptr;
			}
			g_ui_font_prev = g_ui_font;
			g_ui_font = nullptr;
			const int dpi = query_dpi(editor);
			g_ui_font = make_font(dpi, clamp_i(g_live.font_size, 8, 32), g_live.font_name);
			if (!g_ui_font) {
				return;
			}
			EnumChildWindows(editor, set_font_child_post, reinterpret_cast<LPARAM>(g_ui_font));
			g_applied_font_size = clamp_i(g_live.font_size, 8, 32);
			strncpy_s(g_applied_font_name, g_live.font_name, _TRUNCATE);
			shared::common::log("Settings",
				std::format("MFC UI font '{}' size={} posted WM_SETFONT (skip ChildClass)",
					g_live.font_name, g_live.font_size),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		static bool class_is_list(const char* cls)
		{
			if (!cls || !cls[0]) {
				return false;
			}
			return std::strcmp(cls, "SysListView32") == 0 ||
				std::strcmp(cls, "SysTreeView32") == 0 ||
				_stricmp(cls, "ListBox") == 0;
		}

		static int list_score(HWND hwnd, HWND left)
		{
			char cls[80]{};
			GetClassNameA(hwnd, cls, sizeof(cls));
			int score = 0;
			if (std::strcmp(cls, "SysListView32") == 0) {
				score = 30;
			}
			else if (std::strcmp(cls, "SysTreeView32") == 0) {
				score = 28;
			}
			else if (_stricmp(cls, "ListBox") == 0) {
				score = 20;
			}
			else if (_strnicmp(cls, "Afx:", 4) == 0)
			{
				if (IsWindow(reinterpret_cast<HWND>(SendMessageA(hwnd, LVM_GETHEADER, 0, 0)))) {
					score = 25;
				}
				else {
					score = 4;
				}
			}
			if (score <= 0) {
				return 0;
			}
			if (left && (hwnd == left || IsChild(left, hwnd))) {
				score += 50;
			}
			RECT r{};
			GetWindowRect(hwnd, &r);
			const int w = r.right - r.left;
			const int h = r.bottom - r.top;
			if (w > 40 && h > 80) {
				score += 10;
			}
			return score;
		}

		struct list_enum
		{
			HWND left = nullptr;
			HWND best = nullptr;
			int best_score = 0;
			char log[1024]{};
			int log_n = 0;
		};

		static BOOL CALLBACK enum_object_list(HWND hwnd, LPARAM lp)
		{
			auto* ctx = reinterpret_cast<list_enum*>(lp);
			char cls[80]{};
			GetClassNameA(hwnd, cls, sizeof(cls));
			if (std::strcmp(cls, "ChildClass") == 0) {
				return TRUE;
			}
			if (!g_list_enum_logged && ctx->log_n < 8)
			{
				char line[80]{};
				sprintf_s(line, "  kid 0x%p %s\n", static_cast<void*>(hwnd), cls);
				strcat_s(ctx->log, line);
				++ctx->log_n;
			}
			const int s = list_score(hwnd, ctx->left);
			if (s > ctx->best_score)
			{
				ctx->best_score = s;
				ctx->best = hwnd;
			}
			return TRUE;
		}

		static HWND find_object_list(HWND editor)
		{
			if (g_object_list && IsWindow(g_object_list)) {
				return g_object_list;
			}
			if (!editor || !IsWindow(editor)) {
				editor = editor_frame::editor_hwnd();
			}
			if (!editor || !IsWindow(editor)) {
				return nullptr;
			}

			list_enum ctx{};
			ctx.left = editor_frame::left_panel_hwnd();
			if (ctx.left) {
				char lcls[80]{};
				GetClassNameA(ctx.left, lcls, sizeof(lcls));
				if (class_is_list(lcls)) {
					ctx.best = ctx.left;
					ctx.best_score = 80;
				}
			}
			EnumChildWindows(editor, enum_object_list, reinterpret_cast<LPARAM>(&ctx));
			if (!g_list_enum_logged)
			{
				g_list_enum_logged = true;
				char best_cls[80]{};
				if (ctx.best) {
					GetClassNameA(ctx.best, best_cls, sizeof(best_cls));
				}
				shared::common::log("Settings",
					std::format(
						"3DRADCLASS object-list scan left=0x{:X} best=0x{:X} class='{}' score={}\n{}",
						reinterpret_cast<std::uintptr_t>(ctx.left),
						reinterpret_cast<std::uintptr_t>(ctx.best),
						best_cls, ctx.best_score, ctx.log),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
			g_object_list = (ctx.best_score >= 20) ? ctx.best : nullptr;
			return g_object_list;
		}

		// LVS_EX_CHECKBOXES / TVS_CHECKBOXES: index 0 unused, 1 unchecked, 2 checked.
		static HIMAGELIST make_glyph_iml(int cx, int cy)
		{
			cx = clamp_i(cx, 8, 64);
			cy = clamp_i(cy, 8, 64);
			HIMAGELIST himl = ImageList_Create(cx, cy, ILC_COLOR32 | ILC_MASK, 3, 1);
			if (!himl) {
				return nullptr;
			}
			HDC screen = GetDC(nullptr);
			HDC mem = CreateCompatibleDC(screen);
			for (int i = 0; i < 3; ++i)
			{
				HBITMAP bmp = CreateCompatibleBitmap(screen, cx, cy);
				HGDIOBJ old = SelectObject(mem, bmp);
				RECT r{ 0, 0, cx, cy };
				FillRect(mem, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
				if (i > 0)
				{
					const int side = cx < cy ? cx : cy;
					RECT box{
						(cx - side) / 2 + 1,
						(cy - side) / 2 + 1,
						(cx - side) / 2 + side - 1,
						(cy - side) / 2 + side - 1
					};
					UINT dfcs = DFCS_BUTTONCHECK | DFCS_FLAT;
					if (i == 2) {
						dfcs |= DFCS_CHECKED;
					}
					DrawFrameControl(mem, &box, DFC_BUTTON, dfcs);
				}
				SelectObject(mem, old);
				ImageList_AddMasked(himl, bmp, RGB(255, 255, 255));
				DeleteObject(bmp);
			}
			DeleteDC(mem);
			ReleaseDC(nullptr, screen);
			return himl;
		}

		static HIMAGELIST scale_state_iml(HIMAGELIST src, int cx, int cy)
		{
			cx = clamp_i(cx, 8, 64);
			cy = clamp_i(cy, 8, 64);
			const int n = src ? ImageList_GetImageCount(src) : 0;
			if (!src || n < 2)
			{
				return make_glyph_iml(cx, cy);
			}

			HIMAGELIST neu = ImageList_Create(cx, cy, ILC_COLOR32 | ILC_MASK, n, 1);
			if (!neu) {
				return make_glyph_iml(cx, cy);
			}
			HDC screen = GetDC(nullptr);
			HDC mem = CreateCompatibleDC(screen);
			for (int i = 0; i < n; ++i)
			{
				HBITMAP bmp = CreateCompatibleBitmap(screen, cx, cy);
				HGDIOBJ old = SelectObject(mem, bmp);
				RECT r{ 0, 0, cx, cy };
				FillRect(mem, &r, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
				ImageList_DrawEx(src, i, mem, 0, 0, cx, cy,
					RGB(255, 255, 255), CLR_NONE, ILD_TRANSPARENT);
				SelectObject(mem, old);
				ImageList_AddMasked(neu, bmp, RGB(255, 255, 255));
				DeleteObject(bmp);
			}
			DeleteDC(mem);
			ReleaseDC(nullptr, screen);
			return neu;
		}

		static void replace_state_iml(HWND list, bool tree, HIMAGELIST src, int cx, int cy)
		{
			HIMAGELIST neu = scale_state_iml(src, cx, cy);
			if (!neu) {
				return;
			}
			if (tree) {
				SendMessageA(list, TVM_SETIMAGELIST, TVSIL_STATE, reinterpret_cast<LPARAM>(neu));
			}
			else {
				SendMessageA(list, LVM_SETIMAGELIST, LVSIL_STATE, reinterpret_cast<LPARAM>(neu));
			}
			if (g_our_state_iml && g_our_state_iml != neu) {
				ImageList_Destroy(g_our_state_iml);
			}
			g_our_state_iml = neu;
		}

		static void restore_state_iml(HWND list, bool tree)
		{
			if (tree)
			{
				SendMessageA(list, TVM_SETIMAGELIST, TVSIL_STATE,
					reinterpret_cast<LPARAM>(g_orig_tv_state));
			}
			else
			{
				SendMessageA(list, LVM_SETIMAGELIST, LVSIL_STATE,
					reinterpret_cast<LPARAM>(g_orig_state_iml));
			}
			if (g_our_state_iml)
			{
				ImageList_Destroy(g_our_state_iml);
				g_our_state_iml = nullptr;
			}
		}

		static void capture_list_stock(HWND list)
		{
			if (!list || g_list_orig_captured) {
				return;
			}
			char cls[80]{};
			GetClassNameA(list, cls, sizeof(cls));
			if (std::strcmp(cls, "SysTreeView32") == 0)
			{
				g_stock_row_h = static_cast<int>(SendMessageA(list, TVM_GETITEMHEIGHT, 0, 0));
				g_orig_tv_state = reinterpret_cast<HIMAGELIST>(
					SendMessageA(list, TVM_GETIMAGELIST, TVSIL_STATE, 0));
				if (g_orig_tv_state)
				{
					int cx = 0, cy = 0;
					ImageList_GetIconSize(g_orig_tv_state, &cx, &cy);
					g_stock_check_sz = cy > 0 ? cy : cx;
				}
			}
			else if (std::strcmp(cls, "SysListView32") == 0
				|| _strnicmp(cls, "Afx:", 4) == 0)
			{
				RECT ir{};
				if (SendMessageA(list, LVM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&ir))) {
					g_stock_row_h = ir.bottom - ir.top;
				}
				g_orig_state_iml = reinterpret_cast<HIMAGELIST>(
					SendMessageA(list, LVM_GETIMAGELIST, LVSIL_STATE, 0));
				if (g_orig_state_iml)
				{
					int cx = 0, cy = 0;
					ImageList_GetIconSize(g_orig_state_iml, &cx, &cy);
					g_stock_check_sz = cy > 0 ? cy : cx;
				}
			}
			else if (_stricmp(cls, "ListBox") == 0)
			{
				g_stock_row_h = static_cast<int>(SendMessageA(list, LB_GETITEMHEIGHT, 0, 0));
				const LONG style = GetWindowLongA(list, GWL_STYLE);
				shared::common::log("Settings",
					std::format(
						"object list hwnd=0x{:X} ListBox style=0x{:08X} ownerdraw={} hasstrings={} "
						"itemh={} (skins are 350x16 item*.bmp via LoadImageW/BitBlt)",
						reinterpret_cast<std::uintptr_t>(list),
						static_cast<unsigned>(style),
						(style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) ? 1 : 0,
						(style & LBS_HASSTRINGS) ? 1 : 0,
						g_stock_row_h),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
			if (g_stock_row_h < 8) {
				g_stock_row_h = kDefaultRowHeight;
			}
			if (g_stock_check_sz < 8) {
				g_stock_check_sz = kDefaultCheckSize;
			}
			g_list_orig_captured = true;
		}

		static bool lv_has_checks(HWND list, HIMAGELIST orig)
		{
			if (orig) {
				return true;
			}
			const DWORD ex = static_cast<DWORD>(
				SendMessageA(list, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0));
			return (ex & LVS_EX_CHECKBOXES) != 0;
		}

		static bool tv_has_checks(HWND list, HIMAGELIST orig)
		{
			if (orig) {
				return true;
			}
			const LONG style = GetWindowLongA(list, GWL_STYLE);
			return (style & TVS_CHECKBOXES) != 0;
		}

		static bool paint_object_row(const DRAWITEMSTRUCT* dis);
		static void paint_list_client(HWND hwnd, HDC hdc);
		static int display_check_size();
		static void rebuild_group_gold(HWND list, bool log);
		static bool bind_editor_host();
		static int host_count();
		static int plugin_shown_at(const int slot, const int count);
		static int list_item_slot(HWND list, int item, const int count);
		static bool hwnd_is_object_list(HWND hwnd);
		static void log_check_click(HWND hwnd, int item);
		static void invalidate_after_shown_toggle(HWND hwnd, int item);
		static void poll_shown_bits();
		static LRESULT forward_list_mouse(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

		static bool remap_checkbox_click(HWND hwnd, UINT msg, LPARAM& lparam)
		{
			char cls[80]{};
			GetClassNameA(hwnd, cls, sizeof(cls));
			if (_stricmp(cls, "ListBox") != 0) {
				return false;
			}
			if (msg != WM_LBUTTONDOWN && msg != WM_LBUTTONDBLCLK &&
				msg != WM_LBUTTONUP)
			{
				return false;
			}

			const int x = GET_X_LPARAM(lparam);
			const int y = GET_Y_LPARAM(lparam);

			if (msg == WM_LBUTTONUP)
			{
				if (!g_check_click_armed) {
					return false;
				}
				RECT ir{};
				if (SendMessageA(hwnd, LB_GETITEMRECT, g_check_click_item,
					reinterpret_cast<LPARAM>(&ir)) == LB_ERR)
				{
					g_check_click_armed = false;
					return false;
				}
				const int gy = ir.top + (ir.bottom - ir.top) / 2;
				lparam = MAKELPARAM(ir.left + 8, gy);
				g_check_click_armed = false;
				return true;
			}

			const LRESULT ip = SendMessageA(hwnd, LB_ITEMFROMPOINT, 0,
				MAKELPARAM(x, y));
			if (HIWORD(ip)) {
				g_check_click_armed = false;
				return false;
			}
			const int item = static_cast<int>(LOWORD(ip));
			RECT ir{};
			if (SendMessageA(hwnd, LB_GETITEMRECT, item,
				reinterpret_cast<LPARAM>(&ir)) == LB_ERR)
			{
				return false;
			}

			const int chk = display_check_size();
			const int row_h = ir.bottom - ir.top;
			int hit_w = 5 + chk + 12;
			if (hit_w < 36) {
				hit_w = 36;
			}
			if (hit_w > 56) {
				hit_w = 56;
			}

			if (x < ir.left || x >= ir.left + hit_w ||
				y < ir.top || y >= ir.bottom)
			{
				g_check_click_armed = false;
				return false;
			}

			// Stock itemSelected.bmp has no check. Remapping the selected
			// row's left strip to x=8 would toggle hide on the current
			// object. Leave those clicks as selection/drag.
			if (SendMessageA(hwnd, LB_GETSEL, item, 0) > 0 ||
				item == static_cast<int>(SendMessageA(hwnd, LB_GETCURSEL, 0, 0)))
			{
				g_check_click_armed = false;
				return false;
			}

			// Visual glyph stays CheckSize; hit strip is the left pad + full
			// row height. Engine still toggles from the stock ~16px bmp column
			// (item*C.bmp = shown). Forward x=8 to the original ListBox proc.
			const int gy = ir.top + row_h / 2;
			lparam = MAKELPARAM(ir.left + 8, gy);
			g_check_click_armed = true;
			g_check_click_item = item;
			return true;
		}

		static LRESULT forward_list_mouse(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			++g_fwd_mouse;
			LRESULT r = 0;
			if (g_list_orig_proc) {
				r = CallWindowProcA(g_list_orig_proc, hwnd, msg, wparam, lparam);
			}
			else {
				r = DefSubclassProc(hwnd, msg, wparam, lparam);
			}
			--g_fwd_mouse;
			return r;
		}

		static LRESULT CALLBACK list_subclass_proc(HWND hwnd, UINT msg,
			WPARAM wparam, LPARAM lparam, UINT_PTR /*id*/, DWORD_PTR)
		{
			if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK ||
				msg == WM_LBUTTONUP)
			{
				if (g_fwd_mouse)
				{
					return forward_list_mouse(hwnd, msg, wparam, lparam);
				}
				const int armed = g_check_click_item;
				LPARAM lp = lparam;
				const bool check_hit = remap_checkbox_click(hwnd, msg, lp);
				const LPARAM send = check_hit ? lp : lparam;
				// Never skip the original proc on mouse. WM_PAINT now
				// DefSubclassProc first so the engine blit runs.
				const LRESULT r = forward_list_mouse(hwnd, msg, wparam, send);
				if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)
				{
					const int item = (msg == WM_LBUTTONUP && check_hit)
						? armed : (check_hit ? g_check_click_item
							: static_cast<int>(LOWORD(SendMessageA(hwnd,
								LB_ITEMFROMPOINT, 0, lparam))));
					if (check_hit) {
						log_check_click(hwnd, item);
					}
					invalidate_after_shown_toggle(hwnd, item);
				}
				return r;
			}
			if (msg == WM_ERASEBKGND)
			{
				RECT rc{};
				GetClientRect(hwnd, &rc);
				HBRUSH br = CreateSolidBrush(g_live.uns0);
				FillRect(reinterpret_cast<HDC>(wparam), &rc, br);
				DeleteObject(br);
				return 1;
			}
			if (msg == WM_PAINT)
			{
				// DefSubclassProc first so 3Impact BitBlts item*.bmp (C-suffix
				// = shown). Parent DRAWITEM then samples that strip and
				// overpaints in the same paint, before EndPaint.
				return DefSubclassProc(hwnd, msg, wparam, lparam);
			}
			if (msg == WM_PRINTCLIENT)
			{
				return DefSubclassProc(hwnd, msg, wparam, lparam);
			}
			if (msg == WM_NCDESTROY)
			{
				RemoveWindowSubclass(hwnd, list_subclass_proc, 2);
				g_list_subclassed = false;
				g_list_orig_proc = nullptr;
				g_list_sel = -2;
				g_gold_built_sel = -999;
				g_gold_built_count = -1;
				if (g_object_list == hwnd) {
					g_object_list = nullptr;
				}
			}
			const LRESULT r = DefSubclassProc(hwnd, msg, wparam, lparam);
			return r;
		}

		static void attach_list_subclass(HWND list)
		{
			if (!list || !IsWindow(list) || g_list_subclassed) {
				return;
			}
			if (!g_list_orig_proc) {
				g_list_orig_proc = reinterpret_cast<WNDPROC>(
					GetWindowLongA(list, GWL_WNDPROC));
			}
			if (SetWindowSubclass(list, list_subclass_proc, 2, 0))
			{
				g_list_subclassed = true;
				SetWindowTheme(list, L"", L"");
				const LONG ex = GetWindowLongA(list, GWL_EXSTYLE);
				SetWindowLongA(list, GWL_EXSTYLE, ex | WS_EX_COMPOSITED);
			}
		}

		static void apply_list_metrics(HWND list, int row_h, int chk)
		{
			if (!list || !IsWindow(list)) {
				return;
			}
			attach_list_subclass(list);
			char cls[80]{};
			GetClassNameA(list, cls, sizeof(cls));
			const bool tree = std::strcmp(cls, "SysTreeView32") == 0;
			const bool lv = std::strcmp(cls, "SysListView32") == 0 ||
				_strnicmp(cls, "Afx:", 4) == 0;
			const bool lb = _stricmp(cls, "ListBox") == 0;
			const bool restore = row_h <= 0 && chk <= 0;
			const int h = restore ? g_stock_row_h : (row_h > 0 ? row_h : g_stock_row_h);
			const int box = restore ? g_stock_check_sz : (chk > 0 ? chk : g_stock_check_sz);

			if (tree)
			{
				if (h >= 8) {
					SendMessageA(list, TVM_SETITEMHEIGHT, static_cast<WPARAM>(h), 0);
				}
				if (restore) {
					restore_state_iml(list, true);
				}
				else if (tv_has_checks(list, g_orig_tv_state) && box >= 8)
				{
					replace_state_iml(list, true, g_orig_tv_state, box, box);
				}
			}
			else if (lv)
			{
				if (restore) {
					restore_state_iml(list, false);
				}
				else if (lv_has_checks(list, g_orig_state_iml) && (box >= 8 || h >= 8))
				{
					const int cy = h > box ? h : box;
					const int cx = box >= 8 ? box : cy;
					replace_state_iml(list, false, g_orig_state_iml, cx, cy);
				}
			}
			else if (lb && h >= 8)
			{
				SendMessageA(list, LB_SETITEMHEIGHT, 0, h);
			}

			InvalidateRect(list, nullptr, FALSE);
		}

		static int display_list_width()
		{
			if (g_live.list_width >= 80) {
				return clamp_i(g_live.list_width, 80, 640);
			}
			int w = editor_frame::left_panel_width();
			if (w < 80) {
				w = editor_frame::stock_left_panel_width();
			}
			if (w < 80) {
				w = kDefaultListWidth;
			}
			return clamp_i(w, 80, 640);
		}

		static int display_row_height()
		{
			if (g_live.row_height >= 12) {
				return clamp_i(g_live.row_height, 12, 48);
			}
			if (g_stock_row_h >= 12) {
				return clamp_i(g_stock_row_h, 12, 48);
			}
			return kDefaultRowHeight;
		}

		static int display_check_size()
		{
			if (g_live.check_size >= 8) {
				return clamp_i(g_live.check_size, 8, 40);
			}
			if (g_stock_check_sz >= 8) {
				return clamp_i(g_stock_check_sz, 8, 40);
			}
			return kDefaultCheckSize;
		}

		static bool host_readable(const void* p, const SIZE_T bytes)
		{
			MEMORY_BASIC_INFORMATION info{};
			if (!p || bytes == 0 || !VirtualQuery(p, &info, sizeof(info))) {
				return false;
			}
			const auto* start = static_cast<const std::uint8_t*>(p);
			const auto* region = static_cast<const std::uint8_t*>(info.BaseAddress);
			if (start + bytes > region + info.RegionSize) {
				return false;
			}
			if (info.State != MEM_COMMIT) {
				return false;
			}
			const auto protect = info.Protect & 0xFF;
			return protect == PAGE_READONLY || protect == PAGE_READWRITE ||
				protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READ ||
				protect == PAGE_EXECUTE_READWRITE;
		}

		static bool bind_editor_host()
		{
			if (g_host_ok && g_host_count_at && host_readable(g_host_count_at, sizeof(int)))
			{
				const int n = *g_host_count_at;
				if (n > 0 && n <= k_host_max &&
					host_readable(g_host_list_at, static_cast<SIZE_T>(n) * sizeof(void*)) &&
					host_readable(g_host_hmod_at, static_cast<SIZE_T>(n) * sizeof(void*)))
				{
					return true;
				}
			}

			HMODULE exe = GetModuleHandleA("3DRad.exe");
			if (!exe) {
				exe = GetModuleHandleA(nullptr);
			}
			if (!exe) {
				return false;
			}

			const auto rebase = [exe](const std::uintptr_t preferred)
			{
				return reinterpret_cast<std::uintptr_t>(exe) + (preferred - k_host_pref);
			};
			const auto* count_at = reinterpret_cast<const int*>(rebase(k_host_count_va));
			const auto* list_at = reinterpret_cast<void* const*>(rebase(k_host_list_va));
			const auto* hmod_at = reinterpret_cast<HMODULE const*>(rebase(k_host_hmod_va));
			if (!host_readable(count_at, sizeof(int))) {
				g_host_ok = false;
				return false;
			}
			const int n = *count_at;
			if (n <= 0 || n > k_host_max) {
				g_host_ok = false;
				return false;
			}
			const auto bytes = static_cast<SIZE_T>(n) * sizeof(void*);
			if (!host_readable(list_at, bytes) || !host_readable(hmod_at, bytes)) {
				g_host_ok = false;
				return false;
			}

			g_host_count_at = count_at;
			g_host_list_at = list_at;
			g_host_hmod_at = hmod_at;
			g_host_ok = true;
			return true;
		}

		static int host_count()
		{
			return (g_host_ok && g_host_count_at) ? *g_host_count_at : 0;
		}

		static int slot_of_host_ptr(const void* p, const int count)
		{
			if (!p || !g_host_list_at) {
				return -1;
			}
			const auto value = reinterpret_cast<std::uintptr_t>(p);
			if (value > 0 && value < static_cast<std::uintptr_t>(count)) {
				return static_cast<int>(value);
			}
			for (int i = 0; i < count; ++i)
			{
				if (g_host_list_at[i] == p) {
					return i;
				}
			}
			return -1;
		}

		static bool path_has_folder(const char* path, const char* folder)
		{
			if (!path || !folder || !folder[0]) {
				return false;
			}
			const char* p = path;
			const size_t n = std::strlen(folder);
			while (*p)
			{
				if (_strnicmp(p, folder, n) == 0)
				{
					const char before = (p == path) ? '\\' : p[-1];
					const char after = p[n];
					if ((before == '\\' || before == '/' || before == ':') &&
						(after == '\\' || after == '/' || after == 0 || after == '.'))
					{
						return true;
					}
				}
				++p;
			}
			return false;
		}

		static bool host_slot_is_group(const int slot, const int count, char* path_out, int path_cap)
		{
			if (path_out && path_cap > 0) {
				path_out[0] = 0;
			}
			if (slot < 0 || slot >= count || !g_host_hmod_at) {
				return false;
			}
			char path[MAX_PATH]{};
			if (!GetModuleFileNameA(g_host_hmod_at[slot], path, MAX_PATH) || !path[0]) {
				return false;
			}
			if (path_out && path_cap > 0) {
				strncpy_s(path_out, static_cast<size_t>(path_cap), path, _TRUNCATE);
			}
			return path_has_folder(path, "Group");
		}

		static const std::uint8_t* plugin_ptr_at(const int slot, const int count)
		{
			if (slot < 0 || slot >= count || !g_host_list_at) {
				return nullptr;
			}
			const auto* host = static_cast<const std::uint8_t*>(g_host_list_at[slot]);
			if (!host || !host_readable(host, sizeof(void*))) {
				return nullptr;
			}
			const auto* plugin = *reinterpret_cast<const std::uint8_t* const*>(host);
			if (!plugin || !host_readable(plugin + k_plugin_off_shown, sizeof(int))) {
				return nullptr;
			}
			return plugin;
		}

		static int plugin_shown_at(const int slot, const int count)
		{
			const auto* plugin = plugin_ptr_at(slot, count);
			if (!plugin) {
				return -1;
			}
			const int shown = *reinterpret_cast<const int*>(plugin + k_plugin_off_shown);
			return shown != 0 ? 1 : 0;
		}

		static int plugin_active_at(const int slot, const int count)
		{
			const auto* plugin = plugin_ptr_at(slot, count);
			if (!plugin || !host_readable(plugin + k_plugin_off_active, sizeof(int))) {
				return -1;
			}
			return *reinterpret_cast<const int*>(plugin + k_plugin_off_active);
		}

		static void log_check_click(HWND hwnd, int item)
		{
			bind_editor_host();
			const int n = host_count();
			const int slot = list_item_slot(hwnd, item, n);
			const auto* plugin = plugin_ptr_at(slot, n);
			const int shown = plugin_shown_at(slot, n);
			const int active = plugin_active_at(slot, n);
			char flags[160]{};
			if (plugin && host_readable(plugin, 0x20))
			{
				char* p = flags;
				int left = static_cast<int>(sizeof(flags));
				for (int off = 0; off <= 0x1C && left > 8; off += 4)
				{
					const int v = *reinterpret_cast<const int*>(plugin + off);
					if (v == 0 || v == 1)
					{
						const int w = sprintf_s(p, static_cast<size_t>(left), "+0x%02X=%d ", off, v);
						if (w > 0) {
							p += w;
							left -= w;
						}
					}
				}
			}
			if (g_click_log < 32)
			{
				++g_click_log;
				shared::common::log("Settings",
					std::format(
						"check-click item={} slot={} plugin+0x04 shown={} +0x0C active={} {}",
						item, slot, shown, active, flags),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		}

		static void invalidate_after_shown_toggle(HWND hwnd, int /*item*/)
		{
			// Group hide also flips children — refresh every row from +0x04.
			if (hwnd && IsWindow(hwnd)) {
				InvalidateRect(hwnd, nullptr, FALSE);
			}
		}

		static void poll_shown_bits()
		{
			HWND list = g_object_list;
			if (!list || !IsWindow(list)) {
				return;
			}
			const DWORD now = GetTickCount();
			if (g_shown_poll_tick && now - g_shown_poll_tick < 100) {
				return;
			}
			g_shown_poll_tick = now;
			if (!bind_editor_host()) {
				return;
			}
			const int n = host_count();
			const int rows = static_cast<int>(SendMessageA(list, LB_GETCOUNT, 0, 0));
			unsigned hash = 2166136261u;
			for (int i = 0; i < rows && i < k_host_max; ++i)
			{
				const int slot = list_item_slot(list, i, n);
				const int shown = plugin_shown_at(slot, n);
				hash ^= static_cast<unsigned>(shown + 3);
				hash *= 16777619u;
			}
			if (hash != g_shown_hash)
			{
				g_shown_hash = hash;
				InvalidateRect(list, nullptr, FALSE);
			}
		}

		static void collect_group_children(const int group_slot, const int count)
		{
			if (group_slot < 0 || group_slot >= count || !g_host_list_at) {
				return;
			}
			const auto* host = static_cast<const std::uint8_t*>(g_host_list_at[group_slot]);
			if (!host || !host_readable(host, static_cast<SIZE_T>(k_host_header))) {
				return;
			}
			const int nchild = *reinterpret_cast<const int*>(host + k_host_off_child_n);
			if (nchild > 0 && nchild <= k_host_child_cap)
			{
				const auto* arr = *reinterpret_cast<const std::uint8_t* const*>(
					host + k_host_off_child_arr);
				if (arr && host_readable(arr, static_cast<SIZE_T>(nchild * k_host_child_stride)))
				{
					for (int k = 0; k < nchild; ++k)
					{
						const auto* entry = arr + k * k_host_child_stride;
						int idx = *reinterpret_cast<const int*>(entry);
						if (idx < 0 || idx >= count) {
							idx = slot_of_host_ptr(
								*reinterpret_cast<void* const*>(entry), count);
						}
						if (idx >= 0 && idx < count && idx != group_slot && !g_gold_slot[idx])
						{
							g_gold_slot[idx] = true;
							++g_gold_n;
						}
					}
				}
			}

			// Extra: anyone whose linked/parent handle names this Group.
			// Skip 0 — every host reads +0x2924 as null, which is not slot 0.
			for (int i = 0; i < count; ++i)
			{
				if (i == group_slot) {
					continue;
				}
				const auto* ch = static_cast<const std::uint8_t*>(g_host_list_at[i]);
				if (!ch || !host_readable(ch, static_cast<SIZE_T>(k_host_header))) {
					continue;
				}
				const void* linked = *reinterpret_cast<void* const*>(ch + k_host_off_linked);
				const void* parent = *reinterpret_cast<void* const*>(ch + k_host_off_parent);
				const int ls = slot_of_host_ptr(linked, count);
				const int ps = slot_of_host_ptr(parent, count);
				if ((ls == group_slot || ps == group_slot) && !g_gold_slot[i])
				{
					g_gold_slot[i] = true;
					++g_gold_n;
				}
			}
		}

		static int parse_list_id(const char* text, const int count)
		{
			if (!text || !text[0] || count <= 0) {
				return -1;
			}
			const char* p = text;
			while (*p == ' ' || *p == '\t') {
				++p;
			}
			if (*p < '0' || *p > '9') {
				return -1;
			}
			int v = 0;
			int digits = 0;
			while (*p >= '0' && *p <= '9' && digits < 8)
			{
				v = v * 10 + (*p - '0');
				++p;
				++digits;
			}
			if (digits < 1) {
				return -1;
			}
			if (v >= 0 && v < count) {
				return v;
			}
			if (v >= 1 && v - 1 < count) {
				return v - 1;
			}
			return -1;
		}

		// Display-only: engine strings are "00032Particles" / "00005PointLight - rectangle".
		// Slot mapping still uses LB_GETITEMDATA / the full LB_GETTEXT prefix.
		static const char* object_row_label(const char* text, bool header)
		{
			if (!text || !*text || header || g_live.show_object_ids) {
				return text ? text : "";
			}
			const char* p = text;
			while (*p == ' ' || *p == '\t') {
				++p;
			}
			int digits = 0;
			const char* q = p;
			while (*q >= '0' && *q <= '9' && digits < 8)
			{
				++q;
				++digits;
			}
			if (digits == 5 && *q) {
				return q;
			}
			return text;
		}

		static int list_item_slot(HWND list, int item, const int count)
		{
			if (!list || item < 0 || count <= 0) {
				return -1;
			}
			const LRESULT data = SendMessageA(list, LB_GETITEMDATA, item, 0);
			if (data != LB_ERR)
			{
				const int slot = static_cast<int>(data);
				if (slot >= 0 && slot < count) {
					return slot;
				}
			}
			char text[512]{};
			if (SendMessageA(list, LB_GETTEXT, item, reinterpret_cast<LPARAM>(text)) > 0) {
				return parse_list_id(text, count);
			}
			return -1;
		}

		static int list_row_slot(const DRAWITEMSTRUCT* dis, const int count)
		{
			if (!dis || count <= 0) {
				return -1;
			}
			const int from_data = static_cast<int>(dis->itemData);
			if (from_data >= 0 && from_data < count) {
				return from_data;
			}
			return list_item_slot(dis->hwndItem, static_cast<int>(dis->itemID), count);
		}

		static void plugin_leaf(const char* path, char* dest, int cap)
		{
			if (!dest || cap < 2) {
				return;
			}
			dest[0] = 0;
			if (!path || !path[0]) {
				return;
			}
			// C:\...\objects\<Type>\object.dll → Type
			const char* end = path + std::strlen(path);
			const char* last = end;
			const char* prev = end;
			for (const char* p = path; *p; ++p)
			{
				if (*p == '\\' || *p == '/')
				{
					prev = last;
					last = p + 1;
				}
			}
			const char* start = prev;
			int n = static_cast<int>(last - prev);
			if (n > 0 && last > prev && (last[-1] == '\\' || last[-1] == '/')) {
				n -= 1;
			}
			if (n < 1)
			{
				start = last;
				n = static_cast<int>(end - last);
			}
			if (n >= cap) {
				n = cap - 1;
			}
			if (n > 0) {
				std::memcpy(dest, start, static_cast<size_t>(n));
				dest[n] = 0;
			}
		}

		static void rebuild_group_gold(HWND list, bool log)
		{
			for (int i = 0; i < k_host_max; ++i) {
				g_gold_slot[i] = false;
			}
			g_gold_n = 0;
			g_gold_group_slot = -1;
			g_gold_sel_slot = -1;
			g_gold_sel_text[0] = 0;

			if (!list || !IsWindow(list)) {
				g_gold_built_sel = -1;
				g_gold_built_count = -1;
				return;
			}

			bind_editor_host();
			const int count = host_count();
			const int cur = static_cast<int>(SendMessageA(list, LB_GETCURSEL, 0, 0));
			const LONG style = GetWindowLongA(list, GWL_STYLE);
			g_gold_built_sel = cur;
			g_gold_built_count = count;

			if (cur >= 0)
			{
				SendMessageA(list, LB_GETTEXT, cur, reinterpret_cast<LPARAM>(g_gold_sel_text));
				g_gold_sel_slot = list_item_slot(list, cur, count);
				if (g_gold_sel_slot < 0) {
					g_gold_sel_slot = parse_list_id(g_gold_sel_text, count);
				}
			}

			char group_path[MAX_PATH]{};
			if (g_gold_sel_slot >= 0 &&
				host_slot_is_group(g_gold_sel_slot, count, group_path, MAX_PATH))
			{
				g_gold_group_slot = g_gold_sel_slot;
				collect_group_children(g_gold_group_slot, count);
			}

			if (!log) {
				return;
			}

			static int logged_sel = -999;
			static int logged_slot = -999;
			static int logged_group = -999;
			static int logged_count = -999;
			if (cur == logged_sel && g_gold_sel_slot == logged_slot &&
				g_gold_group_slot == logged_group && count == logged_count)
			{
				return;
			}
			logged_sel = cur;
			logged_slot = g_gold_sel_slot;
			logged_group = g_gold_group_slot;
			logged_count = count;

			shared::common::log("Settings",
				std::format(
					"group-gold sel_row={} sel_text='{}' sel_slot={} group_slot={} "
					"gold_n={} host_n={} ownerdraw=0x{:08X}",
					cur, g_gold_sel_text, g_gold_sel_slot, g_gold_group_slot,
					g_gold_n, count, static_cast<unsigned>(style)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		static void ensure_group_gold(HWND list)
		{
			if (!list) {
				return;
			}
			bind_editor_host();
			const int cur = static_cast<int>(SendMessageA(list, LB_GETCURSEL, 0, 0));
			const int count = host_count();
			if (cur == g_gold_built_sel && count == g_gold_built_count) {
				return;
			}
			rebuild_group_gold(list, false);
		}

		static bool hwnd_is_object_list(HWND hwnd)
		{
			if (!hwnd || !IsWindow(hwnd)) {
				return false;
			}
			if (g_object_list && hwnd == g_object_list) {
				return true;
			}
			char cls[80]{};
			GetClassNameA(hwnd, cls, sizeof(cls));
			const bool listish = _stricmp(cls, "ListBox") == 0 ||
				std::strcmp(cls, "SysListView32") == 0 ||
				std::strcmp(cls, "SysTreeView32") == 0;
			if (!listish) {
				return false;
			}
			HWND left = editor_frame::left_panel_hwnd();
			if (left && (hwnd == left || IsChild(left, hwnd))) {
				return true;
			}
			HWND editor = editor_frame::editor_hwnd();
			if (!editor || !IsChild(editor, hwnd)) {
				return false;
			}
			RECT r{};
			GetWindowRect(hwnd, &r);
			POINT pt{ r.left, r.top };
			ScreenToClient(editor, &pt);
			return pt.x < 48;
		}

		static void draw_check_box(HDC hdc, RECT box, bool on, skin_row row)
		{
			if (box.right - box.left < 6 || box.bottom - box.top < 6) {
				return;
			}
			COLORREF border = RGB(190, 190, 196);
			COLORREF fill = RGB(18, 18, 20);
			COLORREF tick = RGB(255, 255, 255);
			if (row == skin_row::selected)
			{
				border = RGB(255, 220, 220);
				fill = RGB(70, 16, 16);
			}
			else if (row == skin_row::group)
			{
				border = RGB(90, 52, 16);
				fill = RGB(62, 34, 10);
				tick = RGB(255, 236, 200);
			}
			HPEN pen = CreatePen(PS_SOLID, 1, border);
			HBRUSH br = CreateSolidBrush(fill);
			HGDIOBJ oldp = SelectObject(hdc, pen);
			HGDIOBJ oldb = SelectObject(hdc, br);
			RoundRect(hdc, box.left, box.top, box.right, box.bottom, 2, 2);
			if (on)
			{
				const int w = box.right - box.left;
				const int h = box.bottom - box.top;
				HPEN tk = CreatePen(PS_SOLID, clamp_i(w / 8, 2, 8), tick);
				SelectObject(hdc, tk);
				MoveToEx(hdc, box.left + w * 2 / 10, box.top + h / 2, nullptr);
				LineTo(hdc, box.left + w * 42 / 100, box.top + h * 78 / 100);
				LineTo(hdc, box.left + w * 82 / 100, box.top + h * 22 / 100);
				SelectObject(hdc, pen);
				DeleteObject(tk);
			}
			SelectObject(hdc, oldp);
			SelectObject(hdc, oldb);
			DeleteObject(pen);
			DeleteObject(br);
		}

		static bool row_checked(int item, bool cskin_on, bool glyph_on)
		{
			// C-suffix skin (itemUnselected0C / itemSelected1C / itemSelectedg
			// with tick) or baked tick pixels in the left of the blit strip.
			// Never plugin+0x04 as the only source. Never default-all-checked.
			const bool checked = cskin_on || glyph_on;
			if (item >= 0 && item < k_host_max) {
				g_row_last[item] = checked ? 1 : 0;
			}
			return checked;
		}

		static bool paint_object_row(const DRAWITEMSTRUCT* dis)
		{
			if (!dis || !dis->hDC || dis->itemID == static_cast<UINT>(-1)) {
				return false;
			}
			if (dis->CtlType != ODT_LISTBOX && dis->CtlType != ODT_LISTVIEW) {
				return false;
			}
			if (!hwnd_is_object_list(dis->hwndItem)) {
				return false;
			}
			if (g_in_row_paint) {
				return false;
			}

			const RECT rc = dis->rcItem;
			if (rc.right <= rc.left || rc.bottom <= rc.top) {
				return false;
			}

			++g_in_row_paint;
			preload_list_skins();
			sample_gold_from_file();
			ensure_group_gold(dis->hwndItem);

			const int host_n = host_count();
			const int obj_slot = list_row_slot(dis, host_n);
			const bool host_member = obj_slot >= 0 && obj_slot < k_host_max &&
				g_gold_slot[obj_slot];
			const int shown = plugin_shown_at(obj_slot, host_n);
			const int item = static_cast<int>(dis->itemID);

			const int row_h0 = rc.bottom - rc.top;
			// Engine already BitBlt'd item*.bmp (DefSubclassProc first).
			// Sample with a local BitBlt-to-DIB — never a process-wide hook.
			const int sh = clamp_i(row_h0 > 16 ? 16 : row_h0, 8, kStripH);
			COLORREF strip[kStripW * kStripH]{};
			const bool got = grab_item_strip(dis->hDC, rc.left, rc.top, kStripW, sh, strip);
			const bool strip_ok = got && !strip_mostly_black(strip, kStripW, sh);
			const int cy = sh / 2;
			const COLORREF bg = strip_ok ? strip[cy * kStripW + 48] : CLR_INVALID;
			const COLORREF bg2 = strip_ok
				? strip[clamp_i(2, 0, sh - 1) * kStripW + 48] : CLR_INVALID;
			const COLORREF cb = strip_ok ? strip[cy * kStripW + 8] : CLR_INVALID;
			const int br = bg == CLR_INVALID ? 0 : GetRValue(bg);
			const int cr = cb == CLR_INVALID ? 0 : GetRValue(cb);

			const bool ods_sel = (dis->itemState & ODS_SELECTED) != 0;
			const bool ods_dis = (dis->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
			const skin_rec* matched = strip_ok ? match_strip_skin(strip, kStripW, sh) : nullptr;
			const bool pix_sel = color_is_red_skin(bg) || color_is_red_skin(bg2);
			const bool pix_grp = color_is_gold_skin(bg) || color_is_gold_skin(bg2) ||
				(matched && matched->row == skin_row::group) ||
				(strip_ok && strip_matches_gold_ref(strip, kStripW, sh));
			const bool pix_dis = br < 28 && bg != CLR_INVALID && GetGValue(bg) < 28 &&
				!pix_sel && !pix_grp;
			const bool pix_hid = !pix_sel && !pix_grp && br < 80 && br >= 28;
			const bool glyph_on = strip_ok && strip_has_check_glyph(strip, kStripW, sh);
			const bool cskin_on = matched && matched->checked;

			char text[512]{};
			const int n = static_cast<int>(SendMessageA(dis->hwndItem, LB_GETTEXT,
				item, reinterpret_cast<LPARAM>(text)));
			if (n <= 0) {
				text[0] = 0;
			}
			const bool header = text[0] == '-' || (n > 2 && text[0] == ' ' && text[1] == '-');
			const char* label = object_row_label(text, header);

			bool has_check = !header;
			bool checked = false;
			if (!header)
			{
				checked = row_checked(item, cskin_on, glyph_on);
				if (!checked && matched) {
					checked = matched->checked;
				}
				else if (!checked && !glyph_on && strip_ok &&
					cb != CLR_INVALID && bg != CLR_INVALID)
				{
					has_check = abs(cr - br) > 40 ||
						abs(GetGValue(cb) - GetGValue(bg)) > 40;
					if (has_check)
					{
						if (pix_sel || pix_grp || ods_sel) {
							checked = cr > 160;
						}
						else {
							checked = cr < 80;
						}
					}
				}
			}

			skin_row row = skin_row::unselected;
			if (ods_sel || pix_sel) {
				row = skin_row::selected;
			}
			else if (ods_dis || pix_dis) {
				row = skin_row::disabled;
			}
			else if (host_member || (pix_grp && !ods_sel)) {
				row = skin_row::group;
			}
			else if (pix_hid || (matched && matched->row == skin_row::hidden)) {
				row = skin_row::hidden;
			}
			else if (matched) {
				row = matched->row;
			}

			if (row == skin_row::group && g_grp_log < 12)
			{
				++g_grp_log;
				shared::common::log("Settings",
					std::format(
						"group-member row {} slot={} shown={} gold after-sample "
						"(host={} blit={} sel_slot={} group={})",
						item, obj_slot, shown, host_member ? 1 : 0, pix_grp ? 1 : 0,
						g_gold_sel_slot, g_gold_group_slot),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
			if (g_chk_log < 16)
			{
				++g_chk_log;
				shared::common::log("Settings",
					std::format(
						"check row {} on={} cskin={} glyph={} match={} got={} strip_ok={} "
						"shown={} slot={} bg={},{},{} cb={},{},{}",
						item, checked ? 1 : 0, cskin_on ? 1 : 0, glyph_on ? 1 : 0,
						matched ? matched->name : "-",
						got ? 1 : 0, strip_ok ? 1 : 0, shown, obj_slot,
						GetRValue(bg), GetGValue(bg), GetBValue(bg),
						GetRValue(cb), GetGValue(cb), GetBValue(cb)),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}

			const bool zebra = (item % 2) != 0;
			COLORREF top{}, bot{}, textc{};
			row_colors(row, zebra, top, bot, textc);

			fill_vgrad(dis->hDC, rc, top, bot);
			if (row == skin_row::selected || row == skin_row::group)
			{
				RECT accent{ rc.left, rc.top, rc.left + 3, rc.bottom };
				const COLORREF ac = row == skin_row::group
					? RGB(GetRValue(g_live.grp1) * 3 / 4,
						GetGValue(g_live.grp1) * 3 / 4,
						GetBValue(g_live.grp1) * 3 / 4)
					: RGB(255, 90, 90);
				HBRUSH abr = CreateSolidBrush(ac);
				FillRect(dis->hDC, &accent, abr);
				DeleteObject(abr);
			}

			int chk = display_check_size();
			const int row_h = rc.bottom - rc.top;
			if (chk > row_h - 2) {
				chk = clamp_i(row_h - 2, 8, chk);
			}
			int text_x = rc.left + 6;
			if (has_check)
			{
				RECT box{
					rc.left + 5,
					rc.top + (row_h - chk) / 2,
					rc.left + 5 + chk,
					rc.top + (row_h - chk) / 2 + chk
				};
				// ODS_SELECTED = crimson current object. Stock
				// itemSelected.bmp has no check (itemSelected1C is not
				// this skin). Gold / unselected rows still get GDI checks.
				if (!ods_sel) {
					draw_check_box(dis->hDC, box, checked, row);
				}
				text_x = box.right + 6;
			}

			HFONT font = g_ui_font;
			if (HFONT wf = reinterpret_cast<HFONT>(SendMessageA(dis->hwndItem, WM_GETFONT, 0, 0))) {
				font = wf;
			}
			HGDIOBJ oldf = font ? SelectObject(dis->hDC, font) : nullptr;
			SetBkMode(dis->hDC, TRANSPARENT);
			SetTextColor(dis->hDC, textc);
			RECT tr{ text_x, rc.top, rc.right - 4, rc.bottom };
			DrawTextA(dis->hDC, label, -1, &tr,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
			if (oldf) {
				SelectObject(dis->hDC, oldf);
			}
			--g_in_row_paint;
			return true;
		}

		static void paint_list_client(HWND hwnd, HDC hdc)
		{
			if (!hwnd || !hdc || !IsWindow(hwnd)) {
				return;
			}
			RECT crc{};
			GetClientRect(hwnd, &crc);
			HBRUSH br = CreateSolidBrush(g_live.uns0);
			FillRect(hdc, &crc, br);
			DeleteObject(br);

			ensure_group_gold(hwnd);
			const int count = static_cast<int>(SendMessageA(hwnd, LB_GETCOUNT, 0, 0));
			const int top = static_cast<int>(SendMessageA(hwnd, LB_GETTOPINDEX, 0, 0));
			const int cur = static_cast<int>(SendMessageA(hwnd, LB_GETCURSEL, 0, 0));
			for (int i = top; i < count; ++i)
			{
				RECT ir{};
				if (SendMessageA(hwnd, LB_GETITEMRECT, i, reinterpret_cast<LPARAM>(&ir)) == LB_ERR) {
					break;
				}
				if (ir.top >= crc.bottom) {
					break;
				}
				DRAWITEMSTRUCT dis{};
				dis.CtlType = ODT_LISTBOX;
				dis.hwndItem = hwnd;
				dis.hDC = hdc;
				dis.itemID = static_cast<UINT>(i);
				dis.itemData = static_cast<ULONG_PTR>(
					SendMessageA(hwnd, LB_GETITEMDATA, i, 0));
				dis.rcItem = ir;
				if (SendMessageA(hwnd, LB_GETSEL, i, 0) > 0 || i == cur) {
					dis.itemState |= ODS_SELECTED;
				}
				paint_object_row(&dis);
			}
		}


		static void apply_object_list(bool force)
		{
			HWND editor = editor_frame::editor_hwnd();
			HWND list = find_object_list(editor);
			if (list) {
				capture_list_stock(list);
				attach_list_subclass(list);
				ensure_group_gold(list);
			}

			const int want_w = g_live.list_width >= 80
				? clamp_i(g_live.list_width, 80, 640)
				: (editor_frame::stock_left_panel_width() >= 80
					? editor_frame::stock_left_panel_width()
					: 0);
			if (want_w >= 80 && (force || want_w != g_applied_list_w))
			{
				editor_frame::set_left_panel_width(want_w);
				g_applied_list_w = want_w;
			}

			const int row = g_live.row_height >= 12 ? clamp_i(g_live.row_height, 12, 48) : 0;
			const int chk = g_live.check_size >= 8 ? clamp_i(g_live.check_size, 8, 40) : 0;
			if (list && (force || row != g_applied_row || chk != g_applied_chk))
			{
				apply_list_metrics(list, row, chk);
				g_applied_row = row;
				g_applied_chk = chk;
			}
		}

		static void apply_fonts_if_changed()
		{
			HWND editor = editor_frame::editor_hwnd();
			if (!editor) {
				return;
			}
			if (g_applied_font_size == g_live.font_size &&
				_stricmp(g_applied_font_name, g_live.font_name) == 0)
			{
				return;
			}
			apply_fonts(editor);
		}

		static RECT slider_bar(const RECT& row)
		{
			RECT r = row;
			r.left += 8;
			r.right -= 72;
			const int cy = (r.top + r.bottom) / 2;
			r.top = cy - 3;
			r.bottom = cy + 4;
			return r;
		}

		static int slider_val(const RECT& bar, int x, int lo, int hi)
		{
			const int span = bar.right - bar.left;
			if (span < 1) {
				return lo;
			}
			int t = x - bar.left;
			if (t < 0) {
				t = 0;
			}
			if (t > span) {
				t = span;
			}
			return lo + MulDiv(t, hi - lo, span);
		}

		static void draw_slider(HDC hdc, const RECT& bar, int lo, int hi, int val)
		{
			HBRUSH fill = CreateSolidBrush(kFill);
			FillRect(hdc, &bar, fill);
			DeleteObject(fill);
			const int span = bar.right - bar.left;
			int x = bar.left;
			if (hi > lo && span > 0) {
				x = bar.left + MulDiv(val - lo, span, hi - lo);
			}
			RECT thumb{ x - 5, bar.top - 6, x + 6, bar.bottom + 6 };
			HBRUSH th = CreateSolidBrush(kAccent);
			FillRect(hdc, &thumb, th);
			DeleteObject(th);
		}

		static void set_edit_int(HWND hwnd, int id, int v)
		{
			char buf[16]{};
			sprintf_s(buf, "%d", v);
			SetDlgItemTextA(hwnd, id, buf);
		}

		static int get_edit_int(HWND hwnd, int id, int fallback)
		{
			char buf[32]{};
			GetDlgItemTextA(hwnd, id, buf, 32);
			if (!buf[0]) {
				return fallback;
			}
			return atoi(buf);
		}

		static SIZE pane_client_size()
		{
			const SIZE pane = editor_frame::viewport_client_size();
			SIZE out = pane;
			if (out.cx < 64 || out.cx > 3840) {
				out.cx = clamp_i(g_live.width, 640, 3840);
			}
			if (out.cy < 64 || out.cy > 2160) {
				out.cy = clamp_i(g_live.height, 360, 2160);
			}
			return out;
		}

		static bool hwnd_is_settings(HWND hwnd)
		{
			if (!hwnd || !IsWindow(hwnd)) {
				return false;
			}
			HWND walk = hwnd;
			for (int i = 0; i < 8 && walk; ++i)
			{
				if (walk == g_video_dlg || walk == g_ui_dlg || walk == g_picker_hwnd) {
					return true;
				}
				char cls[64]{};
				GetClassNameA(walk, cls, sizeof(cls));
				if (std::strcmp(cls, kDlgClass) == 0 ||
					std::strcmp(cls, kPickerClass) == 0) {
					return true;
				}
				HWND next = GetParent(walk);
				if (!next || next == walk) {
					next = GetWindow(walk, GW_OWNER);
				}
				if (!next || next == walk) {
					break;
				}
				walk = next;
			}
			return false;
		}

		static HWND any_open_dlg()
		{
			if (g_video_dlg && IsWindow(g_video_dlg)) {
				return g_video_dlg;
			}
			if (g_ui_dlg && IsWindow(g_ui_dlg)) {
				return g_ui_dlg;
			}
			return nullptr;
		}

		static void restore_editor_enabled()
		{
			HWND wrap = editor_frame::wrapper_hwnd();
			HWND ed = editor_frame::editor_hwnd();
			if (wrap && IsWindow(wrap) && !IsWindowEnabled(wrap)) {
				EnableWindow(wrap, TRUE);
			}
			if (ed && IsWindow(ed) && !IsWindowEnabled(ed)) {
				EnableWindow(ed, TRUE);
			}
		}

		static void drop_topmost(HWND hwnd)
		{
			if (hwnd && IsWindow(hwnd))
			{
				SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
					SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			}
		}

		static void raise_dlg(HWND hwnd)
		{
			if (!hwnd || !IsWindow(hwnd)) {
				return;
			}
			SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
			BringWindowToTop(hwnd);
			SetForegroundWindow(hwnd);
			SetActiveWindow(hwnd);
		}

		static void center_on_wrapper(int& x, int& y, int w, int h)
		{
			HWND wrap = editor_frame::wrapper_hwnd();
			RECT wr{};
			if (wrap && IsWindow(wrap)) {
				GetWindowRect(wrap, &wr);
			}
			else
			{
				wr.left = 0;
				wr.top = 0;
				wr.right = GetSystemMetrics(SM_CXSCREEN);
				wr.bottom = GetSystemMetrics(SM_CYSCREEN);
			}

			x = wr.left + ((wr.right - wr.left) - w) / 2;
			y = wr.top + ((wr.bottom - wr.top) - h) / 2;

			HMONITOR mon = MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST);
			MONITORINFO mi{};
			mi.cbSize = sizeof(mi);
			if (GetMonitorInfoA(mon, &mi))
			{
				if (x < mi.rcWork.left) {
					x = mi.rcWork.left + 16;
				}
				if (y < mi.rcWork.top) {
					y = mi.rcWork.top + 16;
				}
				if (x + w > mi.rcWork.right) {
					x = mi.rcWork.right - w - 16;
				}
				if (y + h > mi.rcWork.bottom) {
					y = mi.rcWork.bottom - h - 16;
				}
			}
		}

		static void layout_video(HWND hwnd, dlg_state& st)
		{
			RECT cr{};
			GetClientRect(hwnd, &cr);
			const int d = st.dpi;
			const int pad = px(16, d);
			const int row_h = px(26, d);
			const int gap = px(8, d);
			const int x0 = pad;
			const int x1 = cr.right - pad;
			// title + checkbox occupy the first two rows; sliders start below
			int y = pad + row_h + gap + row_h + gap + row_h;
			st.rc_w = RECT{ x0, y, x1, y + row_h };
			y += row_h + gap + row_h;
			st.rc_h = RECT{ x0, y, x1, y + row_h };
			y += row_h + gap + row_h;
			st.rc_pct = RECT{ x0, y, x1, y + row_h };
		}

		static void layout_ui(HWND hwnd, dlg_state& st)
		{
			RECT cr{};
			GetClientRect(hwnd, &cr);
			const int d = st.dpi;
			const int pad = px(16, d);
			const int row_h = px(26, d);
			const int gap = px(8, d);
			const int x0 = pad;
			const int x1 = cr.right - pad;
			int y = pad + row_h + gap + row_h;
			st.rc_ui = RECT{ x0, y, x1, y + row_h };
			y += row_h + gap + row_h;
			st.rc_hud = RECT{ x0, y, x1, y + row_h };
			y += row_h + gap + row_h;
			st.rc_fsz = RECT{ x0, y, x1, y + row_h };
			y += row_h + gap + row_h;
			st.rc_listw = RECT{ x0, y, x1, y + row_h };
			y += row_h + gap + row_h;
			st.rc_row = RECT{ x0, y, x1, y + row_h };
			y += row_h + gap + row_h;
			st.rc_chk = RECT{ x0, y, x1, y + row_h };
			y += row_h + gap;
			st.rc_ids = RECT{ x0, y, x1, y + px(22, d) };
			y += px(22, d) + px(18, d);
			const int cw = px(44, d);
			const int span = x1 - x0;
			const int gapc = (span - cw * 4) / 3;
			st.rc_c_sel = RECT{ x0, y, x0 + cw, y + cw };
			st.rc_c_uns = RECT{ x0 + cw + gapc, y, x0 + cw * 2 + gapc, y + cw };
			st.rc_c_grp = RECT{ x0 + (cw + gapc) * 2, y, x0 + cw * 3 + gapc * 2, y + cw };
			st.rc_c_hid = RECT{ x0 + (cw + gapc) * 3, y, x0 + cw * 4 + gapc * 3, y + cw };
		}

		static void paint_dlg(HWND hwnd, dlg_state& st)
		{
			PAINTSTRUCT ps{};
			HDC hdc = BeginPaint(hwnd, &ps);
			RECT cr{};
			GetClientRect(hwnd, &cr);
			FillRect(hdc, &cr, bg_brush());
			SetBkMode(hdc, TRANSPARENT);
			HGDIOBJ old = SelectObject(hdc, st.font ? st.font : GetStockObject(DEFAULT_GUI_FONT));
			SetTextColor(hdc, kText);
			const int d = st.dpi;
			const int pad = px(16, d);
			const int row_h = px(28, d);

			if (st.kind == dlg_kind::video)
			{
				layout_video(hwnd, st);
				RECT t{ pad, pad, cr.right - pad, pad + row_h };
				DrawTextA(hdc, "Render viewport (shown 3D / Remix MAIN)", -1, &t,
					DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				char line[96]{};
				sprintf_s(line, "Width  %d", clamp_i(st.draft.width, 640, 3840));
				RECT lw = st.rc_w;
				lw.bottom = lw.top;
				lw.top -= row_h;
				DrawTextA(hdc, line, -1, &lw, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				sprintf_s(line, "Height  %d", clamp_i(st.draft.height, 360, 2160));
				RECT lh = st.rc_h;
				lh.bottom = lh.top;
				lh.top -= row_h;
				DrawTextA(hdc, line, -1, &lh, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				const SIZE pane = pane_client_size();
				int pct = 100;
				if (pane.cx > 0) {
					pct = clamp_i(MulDiv(clamp_i(st.draft.width, 640, 3840), 100, pane.cx), 25, 200);
				}
				sprintf_s(line, "Scale %%  %d  (ChildClass client %dx%d)", pct, pane.cx, pane.cy);
				RECT lp = st.rc_pct;
				lp.bottom = lp.top;
				lp.top -= row_h;
				DrawTextA(hdc, line, -1, &lp, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				if (!st.draft.match_window)
				{
					draw_slider(hdc, slider_bar(st.rc_w), 640, 3840, clamp_i(st.draft.width, 640, 3840));
					draw_slider(hdc, slider_bar(st.rc_h), 360, 2160, clamp_i(st.draft.height, 360, 2160));
					draw_slider(hdc, slider_bar(st.rc_pct), 25, 200, pct);
				}
				SetTextColor(hdc, kMuted);
				RECT note{ pad, st.rc_pct.bottom + px(8, d), cr.right - pad, cr.bottom - px(48, d) };
				DrawTextA(hdc,
					"Remix still presents at the swapchain size. This injects SetViewport on "
					"the shown 3D pane (ChildClass) - letterboxed if the window is larger. "
					"No D3D Reset (Reset AVs Remix). Match window = engine/window size wins.\n"
					"HUD buttons (buttons.dds 512x512 / 4x4 atlas) keep hitboxes in engine "
					"mouse space (dll3impact viewport w/h). Those are rewritten after resize.",
					-1, &note, DT_LEFT | DT_WORDBREAK);
			}
			else
			{
				layout_ui(hwnd, st);
				RECT t{ pad, pad, cr.right - pad, pad + row_h };
				DrawTextA(hdc, "3D Rad system UI  (3DRad_res\\system\\ui)", -1, &t,
					DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				char line[96]{};
				sprintf_s(line, "UI scale  %d%%  (button01.x ... buttons.dds)", st.draft.ui_scale);
				RECT lu = st.rc_ui;
				lu.bottom = lu.top;
				lu.top -= row_h;
				DrawTextA(hdc, line, -1, &lu, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				sprintf_s(line, "HUD / 3D text  %d%%  (same ortho pass)", st.draft.hud_scale);
				RECT lh = st.rc_hud;
				lh.bottom = lh.top;
				lh.top -= row_h;
				DrawTextA(hdc, line, -1, &lh, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				sprintf_s(line, "Object-list font size  %d", st.draft.font_size);
				RECT lf = st.rc_fsz;
				lf.bottom = lf.top;
				lf.top -= row_h;
				DrawTextA(hdc, line, -1, &lf, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				sprintf_s(line, "List width  %d px", st.draft.list_width);
				RECT lw = st.rc_listw;
				lw.bottom = lw.top;
				lw.top -= row_h;
				DrawTextA(hdc, line, -1, &lw, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				sprintf_s(line, "Row height  %d px", st.draft.row_height);
				RECT lr = st.rc_row;
				lr.bottom = lr.top;
				lr.top -= row_h;
				DrawTextA(hdc, line, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				sprintf_s(line, "Toggle / checkbox  %d px", st.draft.check_size);
				RECT lc = st.rc_chk;
				lc.bottom = lc.top;
				lc.top -= row_h;
				DrawTextA(hdc, line, -1, &lc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				draw_slider(hdc, slider_bar(st.rc_ui), 50, 200, st.draft.ui_scale);
				draw_slider(hdc, slider_bar(st.rc_hud), 50, 200, st.draft.hud_scale);
				draw_slider(hdc, slider_bar(st.rc_fsz), 8, 32, st.draft.font_size);
				draw_slider(hdc, slider_bar(st.rc_listw), 80, 640, st.draft.list_width);
				draw_slider(hdc, slider_bar(st.rc_row), 12, 48, st.draft.row_height);
				draw_slider(hdc, slider_bar(st.rc_chk), 8, 40, st.draft.check_size);
				SetTextColor(hdc, kText);
				RECT clbl{ pad, st.rc_c_sel.top - px(18, d), cr.right - pad, st.rc_c_sel.top };
				DrawTextA(hdc, "Object-list colors (click a circle)", -1, &clbl,
					DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				COLORREF t0{}, b0{};
				role_colors(st.draft, 0, t0, b0);
				fill_circle_grad(hdc, st.rc_c_sel, t0, b0);
				role_colors(st.draft, 1, t0, b0);
				fill_circle_grad(hdc, st.rc_c_uns, t0, b0);
				role_colors(st.draft, 2, t0, b0);
				fill_circle_grad(hdc, st.rc_c_grp, t0, b0);
				role_colors(st.draft, 3, t0, b0);
				fill_circle_grad(hdc, st.rc_c_hid, t0, b0);
				SetTextColor(hdc, kMuted);
				auto lab = [&](const RECT& c, const char* name)
				{
					RECT lr{ c.left - 8, c.bottom + 2, c.right + 8, c.bottom + px(16, d) };
					DrawTextA(hdc, name, -1, &lr, DT_CENTER | DT_TOP | DT_SINGLELINE);
				};
				lab(st.rc_c_sel, "Selected");
				lab(st.rc_c_uns, "Unselected");
				lab(st.rc_c_grp, "Group");
				lab(st.rc_c_hid, "Hidden");
				RECT note{ pad, st.rc_c_hid.bottom + px(148, d), cr.right - pad, cr.bottom - px(48, d) };
				DrawTextA(hdc,
					"Font list is applied with posted WM_SETFONT to MFC list/tree (not ChildClass). "
					"fontDefault is mesh glyphs, not a Windows TTF. buttons.dds quads scale in "
					"WORLD by UI x HUD (128px cells at 100%, LINEAR filter); OrthoLH is not shrunk "
					"(that plus WORLD doubled S). Hits are child/S so picks follow the glyphs. "
					"The object list lets 3Impact blit item*.bmp first, then we sample the "
					"C-skin / tick and overpaint. Gold is the selected Group child list after "
					"that sample. Clicks remap to x=8 and go to the original ListBox proc. "
					"Color wheels live-apply. List width stays on maximize. Changes apply while dragging.",
					-1, &note, DT_LEFT | DT_WORDBREAK);
			}

			SelectObject(hdc, old);
			EndPaint(hwnd, &ps);
		}

		static int hit_slider(const dlg_state& st, POINT pt)
		{
			if (st.kind == dlg_kind::video && !st.draft.match_window)
			{
				RECT bw = slider_bar(st.rc_w);
				InflateRect(&bw, 0, 10);
				RECT bh = slider_bar(st.rc_h);
				InflateRect(&bh, 0, 10);
				RECT bp = slider_bar(st.rc_pct);
				InflateRect(&bp, 0, 10);
				if (PtInRect(&bw, pt)) {
					return 1;
				}
				if (PtInRect(&bh, pt)) {
					return 2;
				}
				if (PtInRect(&bp, pt)) {
					return 3;
				}
			}
			else if (st.kind == dlg_kind::ui)
			{
				RECT a = slider_bar(st.rc_ui);
				InflateRect(&a, 0, 10);
				RECT b = slider_bar(st.rc_hud);
				InflateRect(&b, 0, 10);
				RECT c = slider_bar(st.rc_fsz);
				InflateRect(&c, 0, 10);
				RECT d = slider_bar(st.rc_listw);
				InflateRect(&d, 0, 10);
				RECT e = slider_bar(st.rc_row);
				InflateRect(&e, 0, 10);
				RECT f = slider_bar(st.rc_chk);
				InflateRect(&f, 0, 10);
				if (PtInRect(&a, pt)) {
					return 4;
				}
				if (PtInRect(&b, pt)) {
					return 5;
				}
				if (PtInRect(&c, pt)) {
					return 6;
				}
				if (PtInRect(&d, pt)) {
					return 7;
				}
				if (PtInRect(&e, pt)) {
					return 8;
				}
				if (PtInRect(&f, pt)) {
					return 9;
				}
			}
			return 0;
		}

		static int hit_color_circle(const dlg_state& st, POINT pt)
		{
			if (st.kind != dlg_kind::ui) {
				return -1;
			}
			const RECT* rs[4] = { &st.rc_c_sel, &st.rc_c_uns, &st.rc_c_grp, &st.rc_c_hid };
			for (int i = 0; i < 4; ++i)
			{
				const RECT& r = *rs[i];
				const int cx = (r.left + r.right) / 2;
				const int cy = (r.top + r.bottom) / 2;
				const int rad = (r.right - r.left) / 2;
				const int dx = pt.x - cx;
				const int dy = pt.y - cy;
				if (dx * dx + dy * dy <= rad * rad) {
					return i;
				}
			}
			return -1;
		}

		static void close_color_picker()
		{
			if (g_picker_hwnd && IsWindow(g_picker_hwnd)) {
				DestroyWindow(g_picker_hwnd);
			}
			g_picker_hwnd = nullptr;
			g_picker.owner = nullptr;
			g_picker.drag = 0;
			g_picker.rgb_mode = false;
			g_picker.rgb_cb = nullptr;
			g_picker.rgb_ctx = nullptr;
			g_picker.notify = nullptr;
		}

		static void apply_picker_live()
		{
			const COLORREF c = hsv_to_rgb(g_picker.hue, g_picker.sat, g_picker.val);
			if (g_picker.rgb_mode)
			{
				g_picker.rgb[0] = GetRValue(c) / 255.f;
				g_picker.rgb[1] = GetGValue(c) / 255.f;
				g_picker.rgb[2] = GetBValue(c) / 255.f;
				if (g_picker.rgb_cb) {
					g_picker.rgb_cb(g_picker.rgb_ctx, g_picker.rgb[0],
						g_picker.rgb[1], g_picker.rgb[2]);
				}
				if (g_picker.notify && IsWindow(g_picker.notify)) {
					InvalidateRect(g_picker.notify, nullptr, FALSE);
				}
				if (g_picker_hwnd) {
					InvalidateRect(g_picker_hwnd, nullptr, FALSE);
				}
				return;
			}
			if (g_picker.owner) {
				set_role_stop(g_picker.owner->draft, g_picker.role, g_picker.stop, c);
			}
			set_role_stop(g_live, g_picker.role, g_picker.stop, c);
			if (g_picker.role == 2)
			{
				g_gold_top_c = g_live.grp0;
				g_gold_bot_c = g_live.grp1;
				g_grp_from_ini = true;
			}
			if (g_picker.owner && g_picker.owner->hwnd) {
				InvalidateRect(g_picker.owner->hwnd, nullptr, FALSE);
			}
			if (g_picker_hwnd) {
				InvalidateRect(g_picker_hwnd, nullptr, FALSE);
			}
			repaint_object_list();
		}

		static void load_picker_stop()
		{
			if (g_picker.rgb_mode)
			{
				const COLORREF c = RGB(
					clamp_i(static_cast<int>(g_picker.rgb[0] * 255.f + 0.5f), 0, 255),
					clamp_i(static_cast<int>(g_picker.rgb[1] * 255.f + 0.5f), 0, 255),
					clamp_i(static_cast<int>(g_picker.rgb[2] * 255.f + 0.5f), 0, 255));
				rgb_to_hsv(c, g_picker.hue, g_picker.sat, g_picker.val);
				return;
			}
			COLORREF top{}, bot{};
			role_colors(g_live, g_picker.role, top, bot);
			rgb_to_hsv(g_picker.stop ? bot : top, g_picker.hue, g_picker.sat, g_picker.val);
		}

		static void picker_from_wheel(int x, int y)
		{
			const RECT& w = g_picker.rc_wheel;
			const float cx = (w.left + w.right) * 0.5f;
			const float cy = (w.top + w.bottom) * 0.5f;
			const float rad = (w.right - w.left) * 0.5f - 2.f;
			const float dx = static_cast<float>(x) - cx;
			const float dy = static_cast<float>(y) - cy;
			const float dist = sqrtf(dx * dx + dy * dy);
			g_picker.sat = rad > 1.f ? (dist / rad) : 0.f;
			if (g_picker.sat > 1.f) {
				g_picker.sat = 1.f;
			}
			g_picker.hue = atan2f(dy, dx) * 180.f / 3.14159265f;
			if (g_picker.hue < 0.f) {
				g_picker.hue += 360.f;
			}
			apply_picker_live();
		}

		static void picker_from_val(int y)
		{
			const RECT& b = g_picker.rc_val;
			const int h = b.bottom - b.top;
			if (h < 1) {
				return;
			}
			int t = y - b.top;
			if (t < 0) {
				t = 0;
			}
			if (t > h) {
				t = h;
			}
			g_picker.val = 1.f - static_cast<float>(t) / static_cast<float>(h);
			apply_picker_live();
		}

		static void paint_hue_wheel(HDC hdc, const RECT& wr, float val)
		{
			const int w = wr.right - wr.left;
			const int h = wr.bottom - wr.top;
			if (w < 8 || h < 8) {
				return;
			}
			BITMAPINFO bi{};
			bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bi.bmiHeader.biWidth = w;
			bi.bmiHeader.biHeight = -h;
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			bi.bmiHeader.biCompression = BI_RGB;
			void* bits = nullptr;
			HDC mem = CreateCompatibleDC(hdc);
			HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
			if (!mem || !dib || !bits)
			{
				if (dib) {
					DeleteObject(dib);
				}
				if (mem) {
					DeleteDC(mem);
				}
				return;
			}
			HGDIOBJ old = SelectObject(mem, dib);
			auto* px = static_cast<unsigned char*>(bits);
			const float cx = (w - 1) * 0.5f;
			const float cy = (h - 1) * 0.5f;
			const float rad = (cx < cy ? cx : cy) - 1.f;
			for (int y = 0; y < h; ++y)
			{
				unsigned char* row = px + y * w * 4;
				for (int x = 0; x < w; ++x)
				{
					const float dx = static_cast<float>(x) - cx;
					const float dy = static_cast<float>(y) - cy;
					const float dist = sqrtf(dx * dx + dy * dy);
					unsigned char r = 18, g = 18, b = 20, a = 0;
					if (dist <= rad)
					{
						float hue = atan2f(dy, dx) * 180.f / 3.14159265f;
						if (hue < 0.f) {
							hue += 360.f;
						}
						const float sat = dist / rad;
						const COLORREF c = hsv_to_rgb(hue, sat, val);
						r = GetRValue(c);
						g = GetGValue(c);
						b = GetBValue(c);
						a = 255;
					}
					row[x * 4 + 0] = b;
					row[x * 4 + 1] = g;
					row[x * 4 + 2] = r;
					row[x * 4 + 3] = a;
				}
			}
			BitBlt(hdc, wr.left, wr.top, w, h, mem, 0, 0, SRCCOPY);
			SelectObject(mem, old);
			DeleteObject(dib);
			DeleteDC(mem);
		}

		static LRESULT CALLBACK picker_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			switch (msg)
			{
			case WM_PAINT:
			{
				PAINTSTRUCT ps{};
				HDC hdc = BeginPaint(hwnd, &ps);
				RECT cr{};
				GetClientRect(hwnd, &cr);
				FillRect(hdc, &cr, bg_brush());
				SetBkMode(hdc, TRANSPARENT);
				SetTextColor(hdc, kText);
				RECT title{ 10, 6, cr.right - 10, 24 };
				if (g_picker.rgb_mode) {
					DrawTextA(hdc, g_picker.title[0] ? g_picker.title : "Color", -1, &title,
						DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				}
				else
				{
					const char* names[] = { "Selected", "Unselected", "Group", "Hidden" };
					DrawTextA(hdc, names[clamp_i(g_picker.role, 0, 3)], -1, &title,
						DT_LEFT | DT_VCENTER | DT_SINGLELINE);
				}
				paint_hue_wheel(hdc, g_picker.rc_wheel, g_picker.val);
				for (int y = g_picker.rc_val.top; y < g_picker.rc_val.bottom; ++y)
				{
					const float t = 1.f - static_cast<float>(y - g_picker.rc_val.top) /
						static_cast<float>(g_picker.rc_val.bottom - g_picker.rc_val.top);
					const COLORREF c = hsv_to_rgb(g_picker.hue, g_picker.sat, t);
					HPEN pen = CreatePen(PS_SOLID, 1, c);
					HGDIOBJ oldp = SelectObject(hdc, pen);
					MoveToEx(hdc, g_picker.rc_val.left, y, nullptr);
					LineTo(hdc, g_picker.rc_val.right, y);
					SelectObject(hdc, oldp);
					DeleteObject(pen);
				}
				COLORREF top{}, bot{};
				if (g_picker.rgb_mode)
				{
					const COLORREF c = RGB(
						clamp_i(static_cast<int>(g_picker.rgb[0] * 255.f + 0.5f), 0, 255),
						clamp_i(static_cast<int>(g_picker.rgb[1] * 255.f + 0.5f), 0, 255),
						clamp_i(static_cast<int>(g_picker.rgb[2] * 255.f + 0.5f), 0, 255));
					fill_circle_grad(hdc, g_picker.rc_top, c, c);
				}
				else
				{
					role_colors(g_live, g_picker.role, top, bot);
					fill_circle_grad(hdc, g_picker.rc_top, top, top);
					fill_circle_grad(hdc, g_picker.rc_bot, bot, bot);
					SetTextColor(hdc, kMuted);
					RECT lt{ g_picker.rc_top.left - 4, g_picker.rc_top.bottom + 2,
						g_picker.rc_top.right + 4, g_picker.rc_top.bottom + 18 };
					RECT lb{ g_picker.rc_bot.left - 4, g_picker.rc_bot.bottom + 2,
						g_picker.rc_bot.right + 4, g_picker.rc_bot.bottom + 18 };
					DrawTextA(hdc, g_picker.stop == 0 ? "Top*" : "Top", -1, &lt, DT_CENTER | DT_TOP | DT_SINGLELINE);
					DrawTextA(hdc, g_picker.stop == 1 ? "Bot*" : "Bot", -1, &lb, DT_CENTER | DT_TOP | DT_SINGLELINE);
				}
				EndPaint(hwnd, &ps);
				return 0;
			}
			case WM_ERASEBKGND:
				return 1;
			case WM_LBUTTONDOWN:
			{
				POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
				if (!g_picker.rgb_mode && PtInRect(&g_picker.rc_top, pt))
				{
					g_picker.stop = 0;
					load_picker_stop();
					InvalidateRect(hwnd, nullptr, FALSE);
					return 0;
				}
				if (!g_picker.rgb_mode && PtInRect(&g_picker.rc_bot, pt))
				{
					g_picker.stop = 1;
					load_picker_stop();
					InvalidateRect(hwnd, nullptr, FALSE);
					return 0;
				}
				if (PtInRect(&g_picker.rc_wheel, pt))
				{
					g_picker.drag = 1;
					SetCapture(hwnd);
					picker_from_wheel(pt.x, pt.y);
					return 0;
				}
				if (PtInRect(&g_picker.rc_val, pt))
				{
					g_picker.drag = 2;
					SetCapture(hwnd);
					picker_from_val(pt.y);
					return 0;
				}
				return 0;
			}
			case WM_MOUSEMOVE:
				if (g_picker.drag == 1 && (wparam & MK_LBUTTON)) {
					picker_from_wheel(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
				}
				else if (g_picker.drag == 2 && (wparam & MK_LBUTTON)) {
					picker_from_val(GET_Y_LPARAM(lparam));
				}
				return 0;
			case WM_LBUTTONUP:
				if (g_picker.drag)
				{
					if (g_picker.drag == 1) {
						picker_from_wheel(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
					}
					else {
						picker_from_val(GET_Y_LPARAM(lparam));
					}
					g_picker.drag = 0;
					if (GetCapture() == hwnd) {
						ReleaseCapture();
					}
					if (!g_picker.rgb_mode) {
						save_ui();
					}
				}
				return 0;
			case WM_KEYDOWN:
				if (wparam == VK_ESCAPE) {
					close_color_picker();
					return 0;
				}
				break;
			case WM_CLOSE:
				close_color_picker();
				return 0;
			case WM_DESTROY:
				if (g_picker_hwnd == hwnd) {
					g_picker_hwnd = nullptr;
				}
				return 0;
			default:
				break;
			}
			return DefWindowProcA(hwnd, msg, wparam, lparam);
		}

		static bool register_picker_class()
		{
			static bool done = false;
			if (done) {
				return true;
			}
			WNDCLASSEXA wc{};
			wc.cbSize = sizeof(wc);
			wc.style = CS_HREDRAW | CS_VREDRAW;
			wc.lpfnWndProc = picker_proc;
			wc.hInstance = shared::globals::dll_hmodule;
			wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
			wc.hbrBackground = bg_brush();
			wc.lpszClassName = kPickerClass;
			if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
				return false;
			}
			done = true;
			return true;
		}

		static void open_color_picker(dlg_state& st, int role, POINT client)
		{
			if (!register_picker_class()) {
				return;
			}
			if (g_picker_hwnd && IsWindow(g_picker_hwnd) && g_picker.role == role)
			{
				close_color_picker();
				return;
			}
			close_color_picker();
			g_picker = {};
			g_picker.owner = &st;
			g_picker.role = clamp_i(role, 0, 3);
			g_picker.stop = 0;
			load_picker_stop();

			const int pw = 236;
			const int ph = 250;
			g_picker.rc_wheel = RECT{ 12, 28, 168, 184 };
			g_picker.rc_val = RECT{ 180, 28, 200, 184 };
			g_picker.rc_top = RECT{ 28, 198, 60, 230 };
			g_picker.rc_bot = RECT{ 88, 198, 120, 230 };

			POINT sp = client;
			ClientToScreen(st.hwnd, &sp);
			HWND hwnd = CreateWindowExA(
				WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
				kPickerClass,
				"List color",
				WS_POPUP | WS_CAPTION | WS_SYSMENU,
				sp.x + 12, sp.y + 12, pw, ph,
				nullptr, nullptr, shared::globals::dll_hmodule, nullptr);
			if (!hwnd) {
				return;
			}
			g_picker_hwnd = hwnd;
			ShowWindow(hwnd, SW_SHOW);
			UpdateWindow(hwnd);
			SetForegroundWindow(hwnd);
		}

		static void apply_slider(dlg_state& st, int which, int x)
		{
			if (which == 1) {
				st.draft.width = slider_val(slider_bar(st.rc_w), x, 640, 3840);
				st.draft.scale = scale_from_pane(st.draft.width, pane_client_size().cx);
			}
			else if (which == 2) {
				st.draft.height = slider_val(slider_bar(st.rc_h), x, 360, 2160);
			}
			else if (which == 3)
			{
				const int pct = slider_val(slider_bar(st.rc_pct), x, 25, 200);
				const SIZE pane = pane_client_size();
				st.draft.scale = pct;
				st.draft.width = clamp_i(pane.cx * pct / 100, 640, 3840);
				st.draft.height = clamp_i(pane.cy * pct / 100, 360, 2160);
			}
			else if (which == 4) {
				st.draft.ui_scale = slider_val(slider_bar(st.rc_ui), x, 50, 200);
			}
			else if (which == 5) {
				st.draft.hud_scale = slider_val(slider_bar(st.rc_hud), x, 50, 200);
			}
			else if (which == 6) {
				st.draft.font_size = slider_val(slider_bar(st.rc_fsz), x, 8, 32);
			}
			else if (which == 7) {
				st.draft.list_width = slider_val(slider_bar(st.rc_listw), x, 80, 640);
			}
			else if (which == 8) {
				st.draft.row_height = slider_val(slider_bar(st.rc_row), x, 12, 48);
			}
			else if (which == 9) {
				st.draft.check_size = slider_val(slider_bar(st.rc_chk), x, 8, 40);
			}
			if (st.hwnd)
			{
				if (which <= 3)
				{
					set_edit_int(st.hwnd, kIdWEdit, st.draft.width);
					set_edit_int(st.hwnd, kIdHEdit, st.draft.height);
				}
				else if (which == 4) {
					set_edit_int(st.hwnd, kIdUiScaleEdit, st.draft.ui_scale);
				}
				else if (which == 5) {
					set_edit_int(st.hwnd, kIdHudScaleEdit, st.draft.hud_scale);
				}
				else if (which == 6) {
					set_edit_int(st.hwnd, kIdFontSizeEdit, st.draft.font_size);
				}
				else if (which == 7) {
					set_edit_int(st.hwnd, kIdListWidthEdit, st.draft.list_width);
				}
				else if (which == 8) {
					set_edit_int(st.hwnd, kIdRowHeightEdit, st.draft.row_height);
				}
				else if (which == 9) {
					set_edit_int(st.hwnd, kIdCheckSizeEdit, st.draft.check_size);
				}
				InvalidateRect(st.hwnd, nullptr, FALSE);
			}
			preview_from_draft(st);
		}

		static void apply_video_now(bool persist)
		{
			if (persist) {
				save_video();
			}
			if (!g_live.match_window && g_live.width >= 64 && g_live.height >= 64)
			{
				editor_frame::request_viewport_client_size(g_live.width, g_live.height);
			}
			HWND vp = editor_frame::viewport_hwnd();
			sync_hud_hits(vp);
			if (persist)
			{
				shared::common::log("Settings",
					std::format("Video apply match={} {}x{} scale={} pane-grow={} (SetViewport inject, no D3D Reset)",
						g_live.match_window ? 1 : 0, g_live.width, g_live.height, g_live.scale,
						(!g_live.match_window && g_live.width >= 64) ? 1 : 0),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		}

		static void apply_ui_now(bool persist)
		{
			if (persist) {
				save_ui();
			}
			apply_fonts_if_changed();
			apply_object_list(false);
			repaint_object_list();
			HWND vp = editor_frame::viewport_hwnd();
			sync_hud_hits(vp);
			if (persist)
			{
				shared::common::log("Settings",
					std::format(
						"UI apply scale={} hud={} font='{}' size={} listw={} row={} chk={} ids={}",
						g_live.ui_scale, g_live.hud_scale, g_live.font_name, g_live.font_size,
						g_live.list_width, g_live.row_height, g_live.check_size,
						g_live.show_object_ids ? 1 : 0),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		}

		static void preview_from_draft(dlg_state& st)
		{
			if (st.kind == dlg_kind::video)
			{
				g_live.match_window = st.draft.match_window;
				g_live.width = st.draft.width;
				g_live.height = st.draft.height;
				g_live.scale = st.draft.scale;
				apply_video_now(false);
			}
			else
			{
				g_live.ui_scale = st.draft.ui_scale;
				g_live.hud_scale = st.draft.hud_scale;
				g_live.font_size = st.draft.font_size;
				if (st.draft.font_name[0]) {
					strncpy_s(g_live.font_name, st.draft.font_name, _TRUNCATE);
				}
				g_live.list_width = st.draft.list_width;
				g_live.row_height = st.draft.row_height;
				g_live.check_size = st.draft.check_size;
				g_live.show_object_ids = st.draft.show_object_ids;
				g_live.sel0 = st.draft.sel0;
				g_live.sel1 = st.draft.sel1;
				g_live.uns0 = st.draft.uns0;
				g_live.uns1 = st.draft.uns1;
				g_live.grp0 = st.draft.grp0;
				g_live.grp1 = st.draft.grp1;
				g_live.hid0 = st.draft.hid0;
				g_live.hid1 = st.draft.hid1;
				apply_ui_now(false);
				repaint_object_list();
			}
		}

		static void select_font_name(HWND hwnd, const char* name)
		{
			HWND list = GetDlgItem(hwnd, kIdFontList);
			if (!list || !name || !name[0]) {
				return;
			}
			const int found = static_cast<int>(SendMessageA(list, LB_FINDSTRINGEXACT,
				static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(name)));
			SendMessageA(list, LB_SETCURSEL, found >= 0 ? found : 0, 0);
			if (HWND prev = GetDlgItem(hwnd, kIdPreview))
			{
				HFONT f = make_font(query_dpi(hwnd), kDefaultFontSize, name);
				SendMessageA(prev, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
			}
		}

		static void reset_to_defaults(dlg_state& st)
		{
			if (!st.hwnd) {
				return;
			}
			restore_editor_enabled();
			if (st.kind == dlg_kind::video)
			{
				const SIZE pane = pane_client_size();
				st.draft.match_window = true;
				st.draft.scale = 100;
				st.draft.width = pane.cx >= 64 ? pane.cx : 0;
				st.draft.height = pane.cy >= 64 ? pane.cy : 0;
				CheckDlgButton(st.hwnd, kIdMatch, BST_CHECKED);
				EnableWindow(GetDlgItem(st.hwnd, kIdWEdit), FALSE);
				EnableWindow(GetDlgItem(st.hwnd, kIdHEdit), FALSE);
				set_edit_int(st.hwnd, kIdWEdit, st.draft.width);
				set_edit_int(st.hwnd, kIdHEdit, st.draft.height);
				g_live.match_window = true;
				g_live.scale = 100;
				g_live.width = st.draft.width;
				g_live.height = st.draft.height;
				apply_video_now(true);
			}
			else
			{
				st.draft.ui_scale = 100;
				st.draft.hud_scale = 100;
				st.draft.font_size = kDefaultFontSize;
				strncpy_s(st.draft.font_name, kDefaultFontName, _TRUNCATE);
				g_live.ui_scale = 100;
				g_live.hud_scale = 100;
				g_live.font_size = kDefaultFontSize;
				strncpy_s(g_live.font_name, kDefaultFontName, _TRUNCATE);
				g_live.list_width = 0;
				g_live.row_height = 0;
				g_live.check_size = 0;
				g_live.show_object_ids = true;
				st.draft.show_object_ids = true;
				CheckDlgButton(st.hwnd, kIdShowObjectIds, BST_CHECKED);
				g_grp_from_ini = false;
				stock_list_colors(g_live);
				if (g_gold_ref_ok)
				{
					g_live.grp0 = g_gold_top_c;
					g_live.grp1 = g_gold_bot_c;
				}
				st.draft.sel0 = g_live.sel0;
				st.draft.sel1 = g_live.sel1;
				st.draft.uns0 = g_live.uns0;
				st.draft.uns1 = g_live.uns1;
				st.draft.grp0 = g_live.grp0;
				st.draft.grp1 = g_live.grp1;
				st.draft.hid0 = g_live.hid0;
				st.draft.hid1 = g_live.hid1;
				st.draft.list_width = display_list_width();
				st.draft.row_height = display_row_height();
				st.draft.check_size = display_check_size();
				set_edit_int(st.hwnd, kIdUiScaleEdit, 100);
				set_edit_int(st.hwnd, kIdHudScaleEdit, 100);
				set_edit_int(st.hwnd, kIdFontSizeEdit, kDefaultFontSize);
				set_edit_int(st.hwnd, kIdListWidthEdit, st.draft.list_width);
				set_edit_int(st.hwnd, kIdRowHeightEdit, st.draft.row_height);
				set_edit_int(st.hwnd, kIdCheckSizeEdit, st.draft.check_size);
				select_font_name(st.hwnd, kDefaultFontName);
				apply_fonts(editor_frame::editor_hwnd());
				apply_object_list(true);
				apply_ui_now(true);
			}
			InvalidateRect(st.hwnd, nullptr, FALSE);
			raise_dlg(st.hwnd);
			shared::common::log("Settings",
				st.kind == dlg_kind::video
					? "Video Reset: MatchWindow=1 Scale=100 follow ChildClass"
					: "UI Reset: Scale=100 HudScale=100 Font=Segoe UI 13 list/row/chk=stock colors=stock ShowObjectIds=1",
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		static void commit_draft(dlg_state& st, bool close)
		{
			if (st.kind == dlg_kind::video)
			{
				st.draft.match_window = IsDlgButtonChecked(st.hwnd, kIdMatch) == BST_CHECKED;
				st.draft.width = clamp_i(get_edit_int(st.hwnd, kIdWEdit, st.draft.width), 640, 3840);
				st.draft.height = clamp_i(get_edit_int(st.hwnd, kIdHEdit, st.draft.height), 360, 2160);
				g_live.match_window = st.draft.match_window;
				g_live.width = st.draft.width;
				g_live.height = st.draft.height;
				if (g_live.match_window) {
					g_live.scale = 100;
				}
				else
				{
					const SIZE pane = pane_client_size();
					g_live.scale = st.draft.scale > 0
						? clamp_i(st.draft.scale, 25, 200)
						: scale_from_pane(g_live.width, pane.cx);
				}
				apply_video_now(true);
			}
			else
			{
				st.draft.ui_scale = clamp_i(get_edit_int(st.hwnd, kIdUiScaleEdit, st.draft.ui_scale), 50, 200);
				st.draft.hud_scale = clamp_i(get_edit_int(st.hwnd, kIdHudScaleEdit, st.draft.hud_scale), 50, 200);
				st.draft.font_size = clamp_i(get_edit_int(st.hwnd, kIdFontSizeEdit, st.draft.font_size), 8, 32);
				st.draft.list_width = clamp_i(get_edit_int(st.hwnd, kIdListWidthEdit, st.draft.list_width), 80, 640);
				st.draft.row_height = clamp_i(get_edit_int(st.hwnd, kIdRowHeightEdit, st.draft.row_height), 12, 48);
				st.draft.check_size = clamp_i(get_edit_int(st.hwnd, kIdCheckSizeEdit, st.draft.check_size), 8, 40);
				st.draft.show_object_ids =
					IsDlgButtonChecked(st.hwnd, kIdShowObjectIds) == BST_CHECKED;
				char face[64]{};
				if (HWND list = GetDlgItem(st.hwnd, kIdFontList))
				{
					const int sel = static_cast<int>(SendMessageA(list, LB_GETCURSEL, 0, 0));
					if (sel >= 0) {
						SendMessageA(list, LB_GETTEXT, sel, reinterpret_cast<LPARAM>(face));
					}
				}
				if (face[0]) {
					strncpy_s(st.draft.font_name, face, _TRUNCATE);
				}
				g_live.ui_scale = st.draft.ui_scale;
				g_live.hud_scale = st.draft.hud_scale;
				g_live.font_size = st.draft.font_size;
				if (st.draft.font_name[0]) {
					strncpy_s(g_live.font_name, st.draft.font_name, _TRUNCATE);
				}
				g_live.list_width = st.draft.list_width;
				g_live.row_height = st.draft.row_height;
				g_live.check_size = st.draft.check_size;
				g_live.show_object_ids = st.draft.show_object_ids;
				g_live.sel0 = st.draft.sel0;
				g_live.sel1 = st.draft.sel1;
				g_live.uns0 = st.draft.uns0;
				g_live.uns1 = st.draft.uns1;
				g_live.grp0 = st.draft.grp0;
				g_live.grp1 = st.draft.grp1;
				g_live.hid0 = st.draft.hid0;
				g_live.hid1 = st.draft.hid1;
				apply_ui_now(true);
			}
			if (close && st.hwnd) {
				DestroyWindow(st.hwnd);
			}
		}

		static void fill_fonts(HWND list)
		{
			if (!list) {
				return;
			}
			SendMessageA(list, LB_RESETCONTENT, 0, 0);
			const char* stock[] = {
				"Segoe UI", "Tahoma", "Arial", "Verdana", "Consolas",
				"Trebuchet MS", "MS Sans Serif", "Courier New", "Calibri"
			};
			for (const char* n : stock) {
				SendMessageA(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
			}

			LOGFONTA lf{};
			lf.lfCharSet = DEFAULT_CHARSET;
			HDC hdc = GetDC(list);
			EnumFontFamiliesExA(hdc, &lf, [](const LOGFONTA* lfp, const TEXTMETRICA*, DWORD, LPARAM lp) -> int
			{
				HWND lb = reinterpret_cast<HWND>(lp);
				if (lfp && lfp->lfFaceName[0] && lfp->lfFaceName[0] != '@')
				{
					if (SendMessageA(lb, LB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
						reinterpret_cast<LPARAM>(lfp->lfFaceName)) == LB_ERR)
					{
						SendMessageA(lb, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lfp->lfFaceName));
					}
				}
				return 1;
			}, reinterpret_cast<LPARAM>(list), 0);
			ReleaseDC(list, hdc);

			const int found = static_cast<int>(SendMessageA(list, LB_FINDSTRINGEXACT,
				static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(g_live.font_name)));
			SendMessageA(list, LB_SETCURSEL, found >= 0 ? found : 0, 0);
		}

		static void fill_assets(HWND list)
		{
			if (!list) {
				return;
			}
			SendMessageA(list, LB_RESETCONTENT, 0, 0);
			const std::string dir = ui_dir();
			char msg[280]{};
			sprintf_s(msg, "Folder: %s", dir.c_str());
			SendMessageA(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(msg));
			SendMessageA(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(
				"buttons.dds  512x512 A8R8G8B8  4x4 atlas (128px cells)  - viewport HUD"));
			SendMessageA(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(
				"button01.x ... button09.x  handle.x  - 1x1 quads, UV cells of buttons.dds"));
			SendMessageA(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(
				"fontDefault\\font.wid + font_XX.x  - mesh glyph font (not a TTF)"));

			WIN32_FIND_DATAA fd{};
			const std::string glob = dir + "\\*";
			HANDLE h = FindFirstFileA(glob.c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE) {
				return;
			}
			do
			{
				if (fd.cFileName[0] == '.') {
					continue;
				}
				SendMessageA(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(fd.cFileName));
			} while (FindNextFileA(h, &fd));
			FindClose(h);
		}

		static void create_children(HWND hwnd, dlg_state& st)
		{
			const int d = st.dpi;
			const int pad = px(16, d);
			RECT cr{};
			GetClientRect(hwnd, &cr);
			const int bw = px(72, d);
			const int bh = px(24, d);
			const int by = cr.bottom - pad - bh;

			auto mk = [&](const char* cls, const char* text, DWORD style, int id,
				int x, int y, int w, int h) -> HWND
			{
				HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style,
					x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
					shared::globals::dll_hmodule, nullptr);
				if (c)
				{
					SetWindowTheme(c, L"", L"");
					if (st.font) {
						SendMessageA(c, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
					}
				}
				return c;
			};

			mk("BUTTON", "OK", BS_PUSHBUTTON | WS_TABSTOP, kIdOk, pad, by, bw, bh);
			mk("BUTTON", "Apply", BS_PUSHBUTTON | WS_TABSTOP, kIdApply, pad + bw + px(8, d), by, bw, bh);
			mk("BUTTON", "Reset", BS_PUSHBUTTON | WS_TABSTOP, kIdReset,
				pad + (bw + px(8, d)) * 2, by, bw, bh);
			mk("BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, kIdCancel,
				cr.right - pad - bw, by, bw, bh);

			const int edit_w = px(56, d);
			const int edit_h = px(20, d);
			if (st.kind == dlg_kind::video)
			{
				layout_video(hwnd, st);
				HWND chk = mk("BUTTON", "Match window (follow ChildClass - current behavior)",
					BS_AUTOCHECKBOX | WS_TABSTOP, kIdMatch, pad, pad + px(28, d),
					cr.right - pad * 2, px(22, d));
				if (chk) {
					SendMessageA(chk, BM_SETCHECK, st.draft.match_window ? BST_CHECKED : BST_UNCHECKED, 0);
				}
				HWND we = mk("EDIT", "", ES_NUMBER | WS_BORDER | WS_TABSTOP, kIdWEdit,
					st.rc_w.right - edit_w, st.rc_w.top + 2, edit_w, edit_h);
				HWND he = mk("EDIT", "", ES_NUMBER | WS_BORDER | WS_TABSTOP, kIdHEdit,
					st.rc_h.right - edit_w, st.rc_h.top + 2, edit_w, edit_h);
				set_edit_int(hwnd, kIdWEdit, clamp_i(st.draft.width, 640, 3840));
				set_edit_int(hwnd, kIdHEdit, clamp_i(st.draft.height, 360, 2160));
				if (st.draft.match_window)
				{
					EnableWindow(we, FALSE);
					EnableWindow(he, FALSE);
				}
			}
			else
			{
				layout_ui(hwnd, st);
				mk("EDIT", "", ES_NUMBER | WS_BORDER | WS_TABSTOP, kIdUiScaleEdit,
					st.rc_ui.right - edit_w, st.rc_ui.top + 2, edit_w, edit_h);
				mk("EDIT", "", ES_NUMBER | WS_BORDER | WS_TABSTOP, kIdHudScaleEdit,
					st.rc_hud.right - edit_w, st.rc_hud.top + 2, edit_w, edit_h);
				mk("EDIT", "", ES_NUMBER | WS_BORDER | WS_TABSTOP, kIdFontSizeEdit,
					st.rc_fsz.right - edit_w, st.rc_fsz.top + 2, edit_w, edit_h);
				mk("EDIT", "", ES_NUMBER | WS_BORDER | WS_TABSTOP, kIdListWidthEdit,
					st.rc_listw.right - edit_w, st.rc_listw.top + 2, edit_w, edit_h);
				mk("EDIT", "", ES_NUMBER | WS_BORDER | WS_TABSTOP, kIdRowHeightEdit,
					st.rc_row.right - edit_w, st.rc_row.top + 2, edit_w, edit_h);
				mk("EDIT", "", ES_NUMBER | WS_BORDER | WS_TABSTOP, kIdCheckSizeEdit,
					st.rc_chk.right - edit_w, st.rc_chk.top + 2, edit_w, edit_h);
				set_edit_int(hwnd, kIdUiScaleEdit, st.draft.ui_scale);
				set_edit_int(hwnd, kIdHudScaleEdit, st.draft.hud_scale);
				set_edit_int(hwnd, kIdFontSizeEdit, st.draft.font_size);
				set_edit_int(hwnd, kIdListWidthEdit, st.draft.list_width);
				set_edit_int(hwnd, kIdRowHeightEdit, st.draft.row_height);
				set_edit_int(hwnd, kIdCheckSizeEdit, st.draft.check_size);
				HWND ids = mk("BUTTON", "Show object IDs", BS_AUTOCHECKBOX | WS_TABSTOP,
					kIdShowObjectIds, st.rc_ids.left, st.rc_ids.top,
					st.rc_ids.right - st.rc_ids.left, st.rc_ids.bottom - st.rc_ids.top);
				if (ids) {
					SendMessageA(ids, BM_SETCHECK,
						st.draft.show_object_ids ? BST_CHECKED : BST_UNCHECKED, 0);
				}

				const int list_y = st.rc_c_hid.bottom + px(20, d);
				const int list_h = px(64, d);
				HWND fonts = mk("LISTBOX", nullptr, WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP,
					kIdFontList, pad, list_y, (cr.right - pad * 2) / 2 - px(4, d), list_h);
				HWND assets = mk("LISTBOX", nullptr, WS_BORDER | WS_VSCROLL | WS_TABSTOP,
					kIdAssetList, pad + (cr.right - pad * 2) / 2 + px(4, d), list_y,
					(cr.right - pad * 2) / 2 - px(4, d), list_h);
				fill_fonts(fonts);
				fill_assets(assets);
				mk("STATIC", "Aa Bb Cc  0123  Object list preview", SS_LEFT, kIdPreview,
					pad, list_y + list_h + px(6, d), cr.right - pad * 2, px(22, d));
			}
		}

		static LRESULT CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			auto* st = reinterpret_cast<dlg_state*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
			if (msg == WM_NCCREATE)
			{
				auto* cs = reinterpret_cast<CREATESTRUCTA*>(lparam);
				st = static_cast<dlg_state*>(cs->lpCreateParams);
				SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
			}
			if (!st) {
				return DefWindowProcA(hwnd, msg, wparam, lparam);
			}

			switch (msg)
			{
			case WM_CREATE:
				st->hwnd = hwnd;
				st->dpi = query_dpi(hwnd);
				st->font = make_font(st->dpi, 9, "Segoe UI");
				apply_dark(hwnd);
				create_children(hwnd, *st);
				return 0;
			case WM_ACTIVATE:
				if (LOWORD(wparam) != WA_INACTIVE)
				{
					SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
						SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
				}
				break;
			case WM_PAINT:
				paint_dlg(hwnd, *st);
				return 0;
			case WM_ERASEBKGND:
				return 1;
			case WM_CTLCOLORDLG:
			case WM_CTLCOLORSTATIC:
			case WM_CTLCOLORBTN:
			case WM_CTLCOLORLISTBOX:
			case WM_CTLCOLOREDIT:
			{
				HDC hdc = reinterpret_cast<HDC>(wparam);
				SetBkMode(hdc, TRANSPARENT);
				SetBkColor(hdc, kBg);
				SetTextColor(hdc, kText);
				return reinterpret_cast<LRESULT>(bg_brush());
			}
			case WM_COMMAND:
			{
				const int id = LOWORD(wparam);
				const int code = HIWORD(wparam);
				if (id == kIdOk) {
					commit_draft(*st, true);
					return 0;
				}
				if (id == kIdApply) {
					commit_draft(*st, false);
					return 0;
				}
				if (id == kIdReset) {
					reset_to_defaults(*st);
					return 0;
				}
				if (id == kIdCancel) {
					DestroyWindow(hwnd);
					return 0;
				}
				if (id == kIdMatch && code == BN_CLICKED)
				{
					st->draft.match_window = IsDlgButtonChecked(hwnd, kIdMatch) == BST_CHECKED;
					EnableWindow(GetDlgItem(hwnd, kIdWEdit), st->draft.match_window ? FALSE : TRUE);
					EnableWindow(GetDlgItem(hwnd, kIdHEdit), st->draft.match_window ? FALSE : TRUE);
					preview_from_draft(*st);
					InvalidateRect(hwnd, nullptr, FALSE);
					return 0;
				}
				if (id == kIdShowObjectIds && code == BN_CLICKED)
				{
					st->draft.show_object_ids =
						IsDlgButtonChecked(hwnd, kIdShowObjectIds) == BST_CHECKED;
					g_live.show_object_ids = st->draft.show_object_ids;
					apply_ui_now(true);
					InvalidateRect(hwnd, nullptr, FALSE);
					return 0;
				}
				if (code == EN_KILLFOCUS)
				{
					if (id == kIdWEdit) {
						st->draft.width = clamp_i(get_edit_int(hwnd, id, st->draft.width), 640, 3840);
					}
					if (id == kIdHEdit) {
						st->draft.height = clamp_i(get_edit_int(hwnd, id, st->draft.height), 360, 2160);
					}
					if (id == kIdUiScaleEdit) {
						st->draft.ui_scale = clamp_i(get_edit_int(hwnd, id, st->draft.ui_scale), 50, 200);
					}
					if (id == kIdHudScaleEdit) {
						st->draft.hud_scale = clamp_i(get_edit_int(hwnd, id, st->draft.hud_scale), 50, 200);
					}
					if (id == kIdFontSizeEdit) {
						st->draft.font_size = clamp_i(get_edit_int(hwnd, id, st->draft.font_size), 8, 32);
					}
					if (id == kIdListWidthEdit) {
						st->draft.list_width = clamp_i(get_edit_int(hwnd, id, st->draft.list_width), 80, 640);
					}
					if (id == kIdRowHeightEdit) {
						st->draft.row_height = clamp_i(get_edit_int(hwnd, id, st->draft.row_height), 12, 48);
					}
					if (id == kIdCheckSizeEdit) {
						st->draft.check_size = clamp_i(get_edit_int(hwnd, id, st->draft.check_size), 8, 40);
					}
					preview_from_draft(*st);
					InvalidateRect(hwnd, nullptr, FALSE);
				}
				if (id == kIdFontList && code == LBN_SELCHANGE)
				{
					char face[64]{};
					const int sel = static_cast<int>(SendMessageA(GetDlgItem(hwnd, kIdFontList), LB_GETCURSEL, 0, 0));
					if (sel >= 0)
					{
						SendMessageA(GetDlgItem(hwnd, kIdFontList), LB_GETTEXT, sel, reinterpret_cast<LPARAM>(face));
						strncpy_s(st->draft.font_name, face, _TRUNCATE);
						if (HWND prev = GetDlgItem(hwnd, kIdPreview))
						{
							HFONT f = make_font(st->dpi, st->draft.font_size, face);
							SendMessageA(prev, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
						}
						preview_from_draft(*st);
					}
				}
				return 0;
			}
			case WM_KEYDOWN:
			case WM_SYSKEYDOWN:
				if (wparam == VK_ESCAPE)
				{
					drop_topmost(hwnd);
					restore_editor_enabled();
					DestroyWindow(hwnd);
					return 0;
				}
				break;
			case WM_LBUTTONDOWN:
			{
				POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
				if (st->kind == dlg_kind::video) {
					layout_video(hwnd, *st);
				}
				else {
					layout_ui(hwnd, *st);
				}
				st->drag = hit_slider(*st, pt);
				if (st->drag)
				{
					SetCapture(hwnd);
					apply_slider(*st, st->drag, pt.x);
					return 0;
				}
				if (st->kind == dlg_kind::ui)
				{
					const int role = hit_color_circle(*st, pt);
					if (role >= 0)
					{
						open_color_picker(*st, role, pt);
						return 0;
					}
				}
				return 0;
			}
			case WM_MOUSEMOVE:
				if (st->drag && (wparam & MK_LBUTTON))
				{
					apply_slider(*st, st->drag, GET_X_LPARAM(lparam));
					return 0;
				}
				break;
			case WM_LBUTTONUP:
				if (st->drag)
				{
					apply_slider(*st, st->drag, GET_X_LPARAM(lparam));
					st->drag = 0;
					if (GetCapture() == hwnd) {
						ReleaseCapture();
					}
					commit_draft(*st, false);
					return 0;
				}
				break;
			case WM_CLOSE:
				drop_topmost(hwnd);
				restore_editor_enabled();
				DestroyWindow(hwnd);
				return 0;
			case WM_DESTROY:
				close_color_picker();
				drop_topmost(hwnd);
				restore_editor_enabled();
				if (st->font)
				{
					DeleteObject(st->font);
					st->font = nullptr;
				}
				if (g_video_dlg == hwnd) {
					g_video_dlg = nullptr;
				}
				if (g_ui_dlg == hwnd) {
					g_ui_dlg = nullptr;
				}
				st->hwnd = nullptr;
				delete st;
				return 0;
			default:
				break;
			}
			return DefWindowProcA(hwnd, msg, wparam, lparam);
		}

		static bool register_class()
		{
			static bool done = false;
			if (done) {
				return true;
			}
			WNDCLASSEXA wc{};
			wc.cbSize = sizeof(wc);
			wc.style = CS_HREDRAW | CS_VREDRAW;
			wc.lpfnWndProc = dlg_proc;
			wc.hInstance = shared::globals::dll_hmodule;
			wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
			wc.hbrBackground = bg_brush();
			wc.lpszClassName = kDlgClass;
			if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
				return false;
			}
			done = true;
			return true;
		}

		static void open_dlg(dlg_kind kind, HWND /*owner*/)
		{
			// One settings dialog at a time. Do not stack. Do not disable the editor.
			if (HWND already = any_open_dlg())
			{
				raise_dlg(already);
				return;
			}
			if (!register_class()) {
				return;
			}

			auto* st = new dlg_state{};
			st->kind = kind;
			st->owner = nullptr;
			st->draft = g_live;
			st->draft.width = g_live.width > 0 ? clamp_i(g_live.width, 640, 3840) : 640;
			st->draft.height = g_live.height > 0 ? clamp_i(g_live.height, 360, 2160) : 360;
			st->draft.list_width = display_list_width();
			st->draft.row_height = display_row_height();
			st->draft.check_size = display_check_size();
			if (kind == dlg_kind::video)
			{
				const SIZE pane = pane_client_size();
				if (g_live.match_window && pane.cx > 0 && pane.cy > 0)
				{
					st->draft.width = pane.cx;
					st->draft.height = pane.cy;
					st->draft.scale = 100;
				}
			}

			HWND wrap = editor_frame::wrapper_hwnd();
			const int dpi = query_dpi(wrap);
			const int w = px(kind == dlg_kind::video ? 520 : 560, dpi);
			const int h = px(kind == dlg_kind::video ? 440 : 848, dpi);
			int x = 80;
			int y = 80;
			center_on_wrapper(x, y, w, h);

			// Unowned top-level + TOPMOST so we sit above 3DRADCLASS (also a
			// top-level owned popup of the wrapper). Owner=wrapper puts us
			// under 3DRADCLASS when maximized.
			HWND hwnd = CreateWindowExA(
				WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
				kDlgClass,
				kind == dlg_kind::video ? "Settings - Video" : "Settings - UI",
				WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
				x, y, w, h,
				nullptr, nullptr, shared::globals::dll_hmodule, st);
			if (!hwnd)
			{
				delete st;
				return;
			}
			if (kind == dlg_kind::video) {
				g_video_dlg = hwnd;
			}
			else {
				g_ui_dlg = hwnd;
			}
			restore_editor_enabled();
			ShowWindow(hwnd, SW_SHOW);
			UpdateWindow(hwnd);
			raise_dlg(hwnd);
			shared::common::log("Settings",
				kind == dlg_kind::video
					? "opened Video (unowned TOPMOST, editor left enabled)"
					: "opened UI (unowned TOPMOST, editor left enabled)",
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}
	}

	HMENU popup_menu()
	{
		if (!g_popup)
		{
			g_popup = CreatePopupMenu();
			AppendMenuW(g_popup, MF_STRING, kCmdVideo, L"Video...");
			AppendMenuW(g_popup, MF_STRING, kCmdUI, L"UI...");
		}
		return g_popup;
	}

	bool handle_menu_command(UINT id, HWND owner)
	{
		if (id != kCmdVideo && id != kCmdUI) {
			return false;
		}
		HWND wrap = owner;
		if (!wrap) {
			wrap = editor_frame::wrapper_hwnd();
		}
		open_dlg(id == kCmdVideo ? dlg_kind::video : dlg_kind::ui, wrap);
		return true;
	}

	bool is_settings_hwnd(HWND hwnd)
	{
		return hwnd_is_settings(hwnd);
	}

	bool filter_message(MSG* msg)
	{
		if (!msg || !msg->hwnd) {
			return false;
		}
		if (!hwnd_is_settings(msg->hwnd)) {
			return false;
		}
		// Mouse / NC must dispatch to dlg_proc. IsDialogMessage on those
		// swallows title-bar and slider clicks (then the hook NULLs them).
		if (msg->message >= WM_MOUSEFIRST && msg->message <= WM_MOUSELAST) {
			return false;
		}
		if (msg->message >= WM_NCMOUSEMOVE && msg->message <= WM_NCXBUTTONDBLCLK) {
			return false;
		}

		HWND dlg = nullptr;
		if (g_video_dlg && IsWindow(g_video_dlg) && hwnd_is_settings(msg->hwnd) &&
			(msg->hwnd == g_video_dlg || IsChild(g_video_dlg, msg->hwnd)))
		{
			dlg = g_video_dlg;
		}
		else if (g_ui_dlg && IsWindow(g_ui_dlg) &&
			(msg->hwnd == g_ui_dlg || IsChild(g_ui_dlg, msg->hwnd)))
		{
			dlg = g_ui_dlg;
		}
		if (!dlg) {
			return false;
		}
		return IsDialogMessageA(dlg, msg) != FALSE;
	}

	void reload()
	{
		auto& cfg = shared::common::config::get();
		g_live.match_window = cfg.video.match_window;
		g_live.scale = clamp_i(cfg.video.scale, 25, 200);
		if (g_live.match_window || cfg.video.width < 64 || cfg.video.height < 64)
		{
			g_live.width = cfg.video.width > 0 ? cfg.video.width : 0;
			g_live.height = cfg.video.height > 0 ? cfg.video.height : 0;
			if (g_live.match_window) {
				g_live.scale = 100;
			}
		}
		else
		{
			g_live.width = clamp_i(cfg.video.width, 640, 3840);
			g_live.height = clamp_i(cfg.video.height, 360, 2160);
		}
		g_live.ui_scale = clamp_i(cfg.ui.scale, 50, 200);
		g_live.hud_scale = clamp_i(cfg.ui.hud_scale, 50, 200);
		g_live.font_size = clamp_i(cfg.ui.font_size, 8, 32);
		g_live.list_width = cfg.ui.list_width > 0 ? clamp_i(cfg.ui.list_width, 80, 640) : 0;
		g_live.row_height = cfg.ui.row_height > 0 ? clamp_i(cfg.ui.row_height, 12, 48) : 0;
		g_live.check_size = cfg.ui.check_size > 0 ? clamp_i(cfg.ui.check_size, 8, 40) : 0;
		g_live.show_object_ids = cfg.ui.show_object_ids;
		stock_list_colors(g_live);
		g_grp_from_ini = read_ini_rgb("ColorGroup0", g_live.grp0);
		read_ini_rgb("ColorGroup1", g_live.grp1);
		read_ini_rgb("ColorSelected0", g_live.sel0);
		read_ini_rgb("ColorSelected1", g_live.sel1);
		read_ini_rgb("ColorUnselected0", g_live.uns0);
		read_ini_rgb("ColorUnselected1", g_live.uns1);
		read_ini_rgb("ColorHidden0", g_live.hid0);
		read_ini_rgb("ColorHidden1", g_live.hid1);
		if (!g_grp_from_ini && g_gold_ref_ok)
		{
			g_live.grp0 = g_gold_top_c;
			g_live.grp1 = g_gold_bot_c;
		}
		if (!cfg.ui.font_name.empty()) {
			strncpy_s(g_live.font_name, cfg.ui.font_name.c_str(), _TRUNCATE);
		}
		else {
			strncpy_s(g_live.font_name, kDefaultFontName, _TRUNCATE);
		}
		shared::common::log("Settings",
			std::format(
				"reload live Video match={} {}x{} scale={} | UI scale={} hud={} font='{}' size={} "
				"listw={} row={} chk={} ids={}",
				g_live.match_window ? 1 : 0, g_live.width, g_live.height, g_live.scale,
				g_live.ui_scale, g_live.hud_scale, g_live.font_name, g_live.font_size,
				g_live.list_width, g_live.row_height, g_live.check_size,
				g_live.show_object_ids ? 1 : 0),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}

	void on_editor_wrapped(HWND editor)
	{
		reload();
		install_hooks();
		g_boot_font_passes = 0;
		if (editor) {
			apply_fonts(editor);
		}
		apply_object_list(true);
		const char* boot_how = "no size";
		if (g_live.width >= 64 && g_live.height >= 64)
		{
			int client_w = g_live.width;
			int client_h = g_live.height;
			if (!g_live.match_window)
			{
				const int left_w = editor_frame::left_panel_width();
				if (left_w > 0) {
					client_w = left_w + g_live.width;
				}
			}
			editor_frame::apply_boot_editor_client_size(client_w, client_h);
			boot_how = g_live.match_window
				? "wrapper rescale (match window)"
				: "wrapper rescale + SetViewport inject";
		}
		sync_hud_hits(editor_frame::viewport_hwnd());
		shared::common::log("Settings",
			std::format(
				"boot apply after wrap match={} {}x{} ui={} hud={} font='{}' listw={} row={} chk={} ids={} {}",
				g_live.match_window ? 1 : 0, g_live.width, g_live.height,
				g_live.ui_scale, g_live.hud_scale, g_live.font_name,
				g_live.list_width, g_live.row_height, g_live.check_size,
				g_live.show_object_ids ? 1 : 0,
				boot_how),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}

	void install_hooks()
	{
		if (!shared::globals::is_editor_host) {
			return;
		}
		hook_d3dx_tex();
		hook_list_skins();
	}

	bool inject_editor_viewport(D3DVIEWPORT9& vp)
	{
		// Match window: no custom letterbox. MAIN follows ChildClass after
		// posted WM_SIZE (leftover pane sizes are expanded in camera.cpp).
		if (!shared::globals::is_editor_host || g_live.match_window) {
			return false;
		}
		if (g_live.width < 64 || g_live.height < 64) {
			return false;
		}
		if (vp.Width < 64 || vp.Height < 64) {
			return false;
		}
		if (vp.Width == vp.Height && vp.Width >= 32) {
			return false; // cubemap
		}

		const UINT req_w = vp.Width;
		const UINT req_h = vp.Height;
		const SIZE pane = editor_frame::viewport_client_size();
		if (pane.cx >= 64 && pane.cy >= 64)
		{
			const int dw = static_cast<int>(req_w) - pane.cx;
			const int dh = static_cast<int>(req_h) - pane.cy;
			if (dw > 4 || dw < -4 || dh > 4 || dh < -4) {
				return false;
			}
		}

		UINT want_w = static_cast<UINT>(clamp_i(g_live.width, 640, 3840));
		UINT want_h = static_cast<UINT>(clamp_i(g_live.height, 360, 2160));
		if (want_w > req_w) {
			want_w = req_w;
		}
		if (want_h > req_h) {
			want_h = req_h;
		}
		if (want_w == req_w && want_h == req_h) {
			return false;
		}

		vp.X = vp.X + static_cast<DWORD>((req_w - want_w) / 2);
		vp.Y = vp.Y + static_cast<DWORD>((req_h - want_h) / 2);
		vp.Width = want_w;
		vp.Height = want_h;
		return true;
	}

	void adjust_ortho(FLOAT& w, FLOAT& h)
	{
		// buttons.dds size is WORLD * S in begin_ui_draw (pixel-space quads).
		// OrthoLH shrink would apply the same S again.
		(void)w;
		(void)h;
	}

	void sync_hud_hits(HWND viewport)
	{
		if (!shared::globals::is_editor_host) {
			return;
		}
		if (!viewport || !IsWindow(viewport)) {
			viewport = editor_frame::viewport_hwnd();
		}
		if (!viewport || !IsWindow(viewport)) {
			return;
		}

		RECT cr{};
		GetClientRect(viewport, &cr);
		const int nw = cr.right - cr.left;
		const int nh = cr.bottom - cr.top;
		if (nw < 32 || nh < 32) {
			return;
		}

		if (g_boot_font_passes < 2)
		{
			++g_boot_font_passes;
			apply_fonts(editor_frame::editor_hwnd());
		}

		RECT wr{};
		GetWindowRect(viewport, &wr);

		int* mouse_w = reinterpret_cast<int*>(engine_at(kOffMouseW));
		int* mouse_h = reinterpret_cast<int*>(engine_at(kOffMouseH));
		int* rl = reinterpret_cast<int*>(engine_at(kOffRectL));
		int* rt = reinterpret_cast<int*>(engine_at(kOffRectT));
		int* rr = reinterpret_cast<int*>(engine_at(kOffRectR));
		int* rb = reinterpret_cast<int*>(engine_at(kOffRectB));

		// Same S as buttons.dds WORLD (ui * hud). WORLD is scale-about-origin
		// (viewport center, same as OrthoLH shrink); child/S maps picks onto
		// the drawn glyphs. In-place cell scale would miss the expanded ring.
		float s = (static_cast<float>(g_live.ui_scale) / 100.0f) *
			(static_cast<float>(g_live.hud_scale) / 100.0f);
		if (s < 0.2f || fabsf(s - 1.0f) < 0.01f) {
			s = 1.0f;
		}
		int ew = static_cast<int>(static_cast<float>(nw) / s + 0.5f);
		int eh = static_cast<int>(static_cast<float>(nh) / s + 0.5f);
		if (ew < 32) {
			ew = 32;
		}
		if (eh < 32) {
			eh = 32;
		}

		const int old_w = (writable(mouse_w, 4) ? *mouse_w : 0);
		const int old_h = (writable(mouse_h, 4) ? *mouse_h : 0);
		auto away = [](int a, int b) { return a > b ? a - b : b - a; };
		const bool size_changed = away(nw, g_last_child_w) > 8 || away(nh, g_last_child_h) > 8;

		if (!size_changed && away(old_w, ew) <= 8 && away(old_h, eh) <= 8 &&
			away(g_last_hit_w, ew) <= 8 && away(g_last_hit_h, eh) <= 8)
		{
			return;
		}

		if (size_changed)
		{
			PostMessageA(viewport, WM_SIZE, SIZE_RESTORED, MAKELPARAM(nw, nh));
			editor_frame::request_delayed_hit_write();
		}

		bool wrote = false;
		if (writable(mouse_w, 4) && writable(mouse_h, 4))
		{
			*mouse_w = ew;
			*mouse_h = eh;
			wrote = true;
		}
		if (writable(rl, 16))
		{
			*rl = wr.left;
			*rt = wr.top;
			*rr = wr.right;
			*rb = wr.bottom;
			wrote = true;
		}

		g_last_hit_w = ew;
		g_last_hit_h = eh;
		g_last_child_w = nw;
		g_last_child_h = nh;

		shared::common::log("EditorFrame",
			std::format(
				"HUD hit sync scale={:.2f} engine_wh={}x{} child={}x{} "
				"ChildClass 0x{:X} engine_was {}x{} wrote={} posted_WM_SIZE={} "
				"(no SendMessage) buttons.dds atlas 512x512 4x4",
				s, ew, eh, nw, nh,
				reinterpret_cast<std::uintptr_t>(viewport),
				old_w, old_h, wrote ? 1 : 0, size_changed ? 1 : 0),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}

	bool measure_object_list(MEASUREITEMSTRUCT* mis)
	{
		if (!mis) {
			return false;
		}
		if (mis->CtlType != ODT_LISTVIEW && mis->CtlType != ODT_LISTBOX) {
			return false;
		}
		const int h = g_live.row_height >= 12 ? clamp_i(g_live.row_height, 12, 48) : 0;
		if (h < 8) {
			return false;
		}
		mis->itemHeight = static_cast<UINT>(h);
		return true;
	}

	bool restyle_object_list_item(const DRAWITEMSTRUCT* dis)
	{
		return paint_object_row(dis);
	}

	void on_object_list_command(HWND ctl, UINT code)
	{
		if (code != LBN_SELCHANGE || !ctl || !IsWindow(ctl)) {
			return;
		}
		if (!hwnd_is_object_list(ctl)) {
			return;
		}
		const int cur = static_cast<int>(SendMessageA(ctl, LB_GETCURSEL, 0, 0));
		const int prev_n = g_gold_n;
		const int prev_group = g_gold_group_slot;
		rebuild_group_gold(ctl, true);
		const bool members_changed = prev_n != g_gold_n || prev_group != g_gold_group_slot;
		if (cur == g_list_sel && !members_changed) {
			return;
		}
		g_list_sel = cur;
		InvalidateRect(ctl, nullptr, FALSE);
	}

	bool rewrite_object_list_click(MSG* msg)
	{
		if (!msg || !msg->hwnd) {
			return false;
		}
		if (msg->message != WM_LBUTTONDOWN && msg->message != WM_LBUTTONDBLCLK &&
			msg->message != WM_LBUTTONUP)
		{
			return false;
		}
		if (!hwnd_is_object_list(msg->hwnd)) {
			return false;
		}
		return remap_checkbox_click(msg->hwnd, msg->message, msg->lParam);
	}

	void note_texture(DWORD stage, IDirect3DBaseTexture9* tex)
	{
		if (stage == 0) {
			g_buttons_bound = (g_buttons_tex != nullptr && tex == g_buttons_tex);
		}
	}

	bool begin_ui_draw(IDirect3DDevice9* dev)
	{
		if (shared::globals::is_editor_host) {
			poll_shown_bits();
		}
		if (!dev || !g_buttons_bound || !shared::globals::is_editor_host) {
			return false;
		}

		bool changed = false;
		g_samp_saved = false;
		g_world_scaled = false;

		if (SUCCEEDED(dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &g_samp_mag)))
		{
			dev->GetSamplerState(0, D3DSAMP_MINFILTER, &g_samp_min);
			dev->GetSamplerState(0, D3DSAMP_MIPFILTER, &g_samp_mip);
			dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
			dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
			g_samp_saved = true;
			changed = true;
		}

		const float s = (static_cast<float>(g_live.ui_scale) / 100.0f) *
			(static_cast<float>(g_live.hud_scale) / 100.0f);
		if (s >= 0.2f && fabsf(s - 1.0f) >= 0.01f &&
			SUCCEEDED(dev->GetTransform(D3DTS_WORLD, &g_saved_world)))
		{
			// Scale about origin (ortho center) so on-screen diameter is
			// S * 128px and widgets move with the same origin as child/S hits.
			D3DXMATRIX world = g_saved_world;
			D3DXMATRIX scale, out;
			D3DXMatrixScaling(&scale, s, s, 1.0f);
			out = scale * world;
			dev->SetTransform(D3DTS_WORLD, &out);
			g_world_scaled = true;
			changed = true;
		}
		return changed;
	}

	void end_ui_draw(IDirect3DDevice9* dev, bool scaled)
	{
		if (!scaled || !dev) {
			return;
		}
		if (g_world_scaled)
		{
			dev->SetTransform(D3DTS_WORLD, &g_saved_world);
			g_world_scaled = false;
		}
		if (g_samp_saved)
		{
			dev->SetSamplerState(0, D3DSAMP_MAGFILTER, g_samp_mag);
			dev->SetSamplerState(0, D3DSAMP_MINFILTER, g_samp_min);
			dev->SetSamplerState(0, D3DSAMP_MIPFILTER, g_samp_mip);
			g_samp_saved = false;
		}
	}

	bool match_window()
	{
		return g_live.match_window;
	}

	int video_width()
	{
		return g_live.width;
	}

	int video_height()
	{
		return g_live.height;
	}

	float ui_scale()
	{
		return static_cast<float>(g_live.ui_scale) / 100.0f;
	}

	float hud_scale()
	{
		return static_cast<float>(g_live.hud_scale) / 100.0f;
	}

	HWND object_list_hwnd()
	{
		return find_object_list(editor_frame::editor_hwnd());
	}

	int object_list_cursel()
	{
		HWND list = object_list_hwnd();
		if (!list) {
			return -1;
		}
		return static_cast<int>(SendMessageA(list, LB_GETCURSEL, 0, 0));
	}

	bool object_list_item_text(int index, char* buf, int cap)
	{
		if (!buf || cap <= 0) {
			return false;
		}
		buf[0] = 0;
		HWND list = object_list_hwnd();
		if (!list || index < 0) {
			return false;
		}
		const int n = static_cast<int>(SendMessageA(list, LB_GETTEXT, index,
			reinterpret_cast<LPARAM>(buf)));
		return n > 0;
	}

	void open_rgb_picker(HWND owner, const char* title, float r, float g, float b,
		void* ctx, rgb_pick_fn cb)
	{
		if (!register_picker_class()) {
			return;
		}
		close_color_picker();
		g_picker = {};
		g_picker.rgb_mode = true;
		g_picker.notify = owner;
		g_picker.rgb_ctx = ctx;
		g_picker.rgb_cb = cb;
		g_picker.rgb[0] = r < 0.f ? 0.f : (r > 1.f ? 1.f : r);
		g_picker.rgb[1] = g < 0.f ? 0.f : (g > 1.f ? 1.f : g);
		g_picker.rgb[2] = b < 0.f ? 0.f : (b > 1.f ? 1.f : b);
		if (title && title[0]) {
			std::strncpy(g_picker.title, title, 63);
		}
		else {
			std::strncpy(g_picker.title, "Color", 63);
		}
		g_picker.title[63] = 0;
		load_picker_stop();
		g_picker.rc_wheel = RECT{ 12, 28, 168, 184 };
		g_picker.rc_val = RECT{ 180, 28, 200, 184 };
		g_picker.rc_top = RECT{ 28, 198, 60, 230 };

		POINT sp{ 12, 12 };
		if (owner) {
			ClientToScreen(owner, &sp);
		}
		HWND hwnd = CreateWindowExA(
			WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
			kPickerClass,
			g_picker.title,
			WS_POPUP | WS_CAPTION | WS_SYSMENU,
			sp.x, sp.y, 236, 250,
			nullptr, nullptr, shared::globals::dll_hmodule, nullptr);
		if (!hwnd) {
			return;
		}
		g_picker_hwnd = hwnd;
		ShowWindow(hwnd, SW_SHOW);
		UpdateWindow(hwnd);
		SetForegroundWindow(hwnd);
	}
}
