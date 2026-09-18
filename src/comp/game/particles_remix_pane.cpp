#include "std_include.hpp"
#include "particles.hpp"
#include "../editor_frame.hpp"
#include "../editor_settings.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#pragma comment(lib, "comdlg32.lib")

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

namespace comp::game::particles
{
	namespace
	{
		constexpr char kClass[] = "3DRadRTXParticleRemix";
		constexpr COLORREF kBg = RGB(0, 0, 0);
		constexpr COLORREF kText = RGB(230, 230, 230);

		enum : int
		{
			kIdOk = 1, kIdCancel = 2, kIdApply = 3, kIdReset = 4,
			kIdSpawnMinR = 0x8100, kIdSpawnMaxR = 0x8104,
			kIdRotMin = 0x8110, kIdRotMax, kIdSizeMin, kIdSizeMax,
			kIdTtlMin, kIdTtlMax, kIdHide, kIdCone, kIdVelMotion, kIdVelNormal,
			kIdMaxP, kIdRate, kIdUseUv,
			kIdTgtMinR = 0x8120, kIdTgtMaxR = 0x8124,
			kIdTgtRotMin = 0x8130, kIdTgtRotMax, kIdTgtSizeMin, kIdTgtSizeMax,
			kIdAlign, kIdBillboard, kIdTrail, kIdTrailMult,
			kIdBounce, kIdThick, kIdCollide,
			kIdGrav, kIdMaxSpd, kIdTurbF, kIdTurbHz, kIdTurbOn,
			kIdSpawnMinSw = 0x8180, kIdSpawnMinA, kIdSpawnMaxSw, kIdSpawnMaxA,
			kIdTgtMinSw, kIdTgtMinA, kIdTgtMaxSw, kIdTgtMaxA,
			kIdSheetRows = 0x8190, kIdSheetCols, kIdSheetFps, kIdSheetMode,
			kIdCollideMode, kIdPickAnim, kIdAnimLabel
		};

		HWND g_hwnd = nullptr;
		HFONT g_font = nullptr;
		HBRUSH g_bg = nullptr;
		std::uint64_t g_ident = 0;
		remix_ext_params g_draft{};
		int g_pick_field = 0;
		bool g_filling = false;

		struct pick_ctx
		{
			std::uint64_t ident = 0;
			int field = 0;
		};
		pick_ctx g_rgb_ctx{};

		int px(int v, int dpi)
		{
			return MulDiv(v, dpi ? dpi : 96, 96);
		}

		int dpi_of(HWND hwnd)
		{
			using fn = UINT(WINAPI*)(HWND);
			static fn p = reinterpret_cast<fn>(
				GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow"));
			if (p && hwnd) {
				const UINT d = p(hwnd);
				if (d) {
					return static_cast<int>(d);
				}
			}
			return 96;
		}

		HBRUSH bg_brush()
		{
			if (!g_bg) {
				g_bg = CreateSolidBrush(kBg);
			}
			return g_bg;
		}

		void apply_dark(HWND hwnd)
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

		HWND mk(HWND parent, const char* cls, const char* text, DWORD style, int id,
			int x, int y, int w, int h)
		{
			HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style,
				x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
				shared::globals::dll_hmodule, nullptr);
			if (c)
			{
				SetWindowTheme(c, L"", L"");
				if (g_font) {
					SendMessageA(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
				}
			}
			return c;
		}

		void set_f(HWND dlg, int id, float v)
		{
			char t[32]{};
			sprintf_s(t, "%.4g", static_cast<double>(v));
			SetDlgItemTextA(dlg, id, t);
		}

		float get_f(HWND dlg, int id, float def)
		{
			char t[32]{};
			if (!GetDlgItemTextA(dlg, id, t, 32) || !t[0]) {
				return def;
			}
			return static_cast<float>(std::atof(t));
		}

		void set_rgba(HWND dlg, int id0, const float c[4])
		{
			for (int i = 0; i < 4; i++) {
				set_f(dlg, id0 + i, c[i]);
			}
		}

		void get_rgba(HWND dlg, int id0, float c[4])
		{
			for (int i = 0; i < 4; i++) {
				c[i] = get_f(dlg, id0 + i, c[i]);
			}
		}

		float* rgb_of(remix_ext_params& p, int field)
		{
			switch (field)
			{
			case 0: return p.min_spawn_color;
			case 1: return p.max_spawn_color;
			case 2: return p.min_target_color;
			default: return p.max_target_color;
			}
		}

		float* draft_rgb(int field)
		{
			return rgb_of(g_draft, field);
		}

		int swatch_field(int id)
		{
			if (id == kIdSpawnMinSw) {
				return 0;
			}
			if (id == kIdSpawnMaxSw) {
				return 1;
			}
			if (id == kIdTgtMinSw) {
				return 2;
			}
			if (id == kIdTgtMaxSw) {
				return 3;
			}
			return -1;
		}

		void on_rgb_picked(void* ctx, float r, float g, float b)
		{
			const auto* pc = static_cast<pick_ctx*>(ctx);
			const std::uint64_t ident = pc && pc->ident ? pc->ident : g_ident;
			const int field = pc ? pc->field : g_pick_field;
			if (!ident) {
				return;
			}
			remix_ext_params p{};
			if (ident == g_ident) {
				p = g_draft;
			}
			else if (!load_remix_for_identity(ident, p)) {
				remix_defaults_for(ident, p);
			}
			float* c = rgb_of(p, field);
			c[0] = r;
			c[1] = g;
			c[2] = b;
			p.has_override = true;
			set_remix_ext(ident, p);
			save_project_ini();
			if (ident == g_ident)
			{
				g_draft = p;
				if (g_hwnd && IsWindow(g_hwnd)) {
					InvalidateRect(g_hwnd, nullptr, FALSE);
				}
			}
		}

		void row_color(HWND hwnd, int y, int x, int lw, int eh, int sw, int aw,
			const char* label, int sw_id, int a_id)
		{
			mk(hwnd, "STATIC", label, SS_LEFT, -1, x, y + 2, lw, eh);
			mk(hwnd, "BUTTON", "", BS_OWNERDRAW | WS_TABSTOP, sw_id,
				x + lw, y, sw, eh);
			mk(hwnd, "STATIC", "A", SS_LEFT, -1, x + lw + sw + 6, y + 2, 14, eh);
			mk(hwnd, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, a_id,
				x + lw + sw + 22, y, aw, eh);
		}

		void fill_edits(HWND hwnd, const remix_ext_params& p)
		{
			g_filling = true;
			g_draft = p;
			set_f(hwnd, kIdSpawnMinA, p.min_spawn_color[3]);
			set_f(hwnd, kIdSpawnMaxA, p.max_spawn_color[3]);
			set_f(hwnd, kIdTgtMinA, p.min_target_color[3]);
			set_f(hwnd, kIdTgtMaxA, p.max_target_color[3]);
			set_f(hwnd, kIdRotMin, p.min_rot_speed);
			set_f(hwnd, kIdRotMax, p.max_rot_speed);
			set_f(hwnd, kIdSizeMin, p.min_spawn_size);
			set_f(hwnd, kIdSizeMax, p.max_spawn_size);
			set_f(hwnd, kIdTtlMin, p.min_ttl);
			set_f(hwnd, kIdTtlMax, p.max_ttl);
			CheckDlgButton(hwnd, kIdHide, p.hide_emitter ? BST_CHECKED : BST_UNCHECKED);
			set_f(hwnd, kIdCone, p.cone_deg);
			set_f(hwnd, kIdVelMotion, p.vel_from_motion);
			set_f(hwnd, kIdVelNormal, p.vel_from_normal);
			set_f(hwnd, kIdMaxP, static_cast<float>(p.max_particles));
			set_f(hwnd, kIdRate, p.spawn_rate);
			CheckDlgButton(hwnd, kIdUseUv, p.use_spawn_uv ? BST_CHECKED : BST_UNCHECKED);
			set_f(hwnd, kIdTgtRotMin, p.min_target_rot);
			set_f(hwnd, kIdTgtRotMax, p.max_target_rot);
			set_f(hwnd, kIdTgtSizeMin, p.min_target_size);
			set_f(hwnd, kIdTgtSizeMax, p.max_target_size);
			CheckDlgButton(hwnd, kIdAlign, p.align_motion ? BST_CHECKED : BST_UNCHECKED);
			HWND combo = GetDlgItem(hwnd, kIdBillboard);
			if (combo) {
				SendMessageA(combo, CB_SETCURSEL, p.billboard <= 3 ? p.billboard : 0, 0);
			}
			CheckDlgButton(hwnd, kIdTrail, p.motion_trail ? BST_CHECKED : BST_UNCHECKED);
			set_f(hwnd, kIdTrailMult, p.trail_mult);
			set_f(hwnd, kIdBounce, p.restitution);
			set_f(hwnd, kIdThick, p.thickness);
			CheckDlgButton(hwnd, kIdCollide, p.collide ? BST_CHECKED : BST_UNCHECKED);
			set_f(hwnd, kIdGrav, p.gravity_force);
			set_f(hwnd, kIdMaxSpd, p.max_speed);
			set_f(hwnd, kIdTurbF, p.turb_force);
			set_f(hwnd, kIdTurbHz, p.turb_freq);
			CheckDlgButton(hwnd, kIdTurbOn, p.use_turbulence ? BST_CHECKED : BST_UNCHECKED);
			set_f(hwnd, kIdSheetRows, static_cast<float>(p.sheet_rows));
			set_f(hwnd, kIdSheetCols, static_cast<float>(p.sheet_cols));
			set_f(hwnd, kIdSheetFps, static_cast<float>(p.sheet_fps));
			HWND sheet_mode = GetDlgItem(hwnd, kIdSheetMode);
			if (sheet_mode) {
				SendMessageA(sheet_mode, CB_SETCURSEL, p.sheet_mode <= 2 ? p.sheet_mode : 0, 0);
			}
			HWND cmode = GetDlgItem(hwnd, kIdCollideMode);
			if (cmode) {
				SendMessageA(cmode, CB_SETCURSEL, p.collision_mode <= 2 ? p.collision_mode : 0, 0);
			}
			{
				char label[128]{};
				if (p.animation[0]) {
					sprintf_s(label, "Animation: %s  (%u x %u @ %u fps)",
						p.animation, p.sheet_cols ? p.sheet_cols : 1,
						p.sheet_rows ? p.sheet_rows : 1, p.sheet_fps);
				}
				else {
					std::strncpy(label, "Animation: (plugin default / data\\animation)", 127);
				}
				SetDlgItemTextA(hwnd, kIdAnimLabel, label);
			}
			InvalidateRect(hwnd, nullptr, FALSE);
			g_filling = false;
		}

		void read_edits(HWND hwnd, remix_ext_params& p)
		{
			p = g_draft;
			p.min_spawn_color[3] = get_f(hwnd, kIdSpawnMinA, p.min_spawn_color[3]);
			p.max_spawn_color[3] = get_f(hwnd, kIdSpawnMaxA, p.max_spawn_color[3]);
			p.min_target_color[3] = get_f(hwnd, kIdTgtMinA, p.min_target_color[3]);
			p.max_target_color[3] = get_f(hwnd, kIdTgtMaxA, p.max_target_color[3]);
			p.min_rot_speed = get_f(hwnd, kIdRotMin, p.min_rot_speed);
			p.max_rot_speed = get_f(hwnd, kIdRotMax, p.max_rot_speed);
			p.min_spawn_size = get_f(hwnd, kIdSizeMin, p.min_spawn_size);
			p.max_spawn_size = get_f(hwnd, kIdSizeMax, p.max_spawn_size);
			p.min_ttl = get_f(hwnd, kIdTtlMin, p.min_ttl);
			p.max_ttl = get_f(hwnd, kIdTtlMax, p.max_ttl);
			p.hide_emitter = IsDlgButtonChecked(hwnd, kIdHide) == BST_CHECKED;
			p.cone_deg = get_f(hwnd, kIdCone, p.cone_deg);
			p.vel_from_motion = get_f(hwnd, kIdVelMotion, p.vel_from_motion);
			p.vel_from_normal = get_f(hwnd, kIdVelNormal, p.vel_from_normal);
			const float mp = get_f(hwnd, kIdMaxP, static_cast<float>(p.max_particles));
			p.max_particles = mp > 1.0f ? static_cast<std::uint32_t>(mp) : 1u;
			p.spawn_rate = get_f(hwnd, kIdRate, p.spawn_rate);
			p.use_spawn_uv = IsDlgButtonChecked(hwnd, kIdUseUv) == BST_CHECKED;
			p.min_target_rot = get_f(hwnd, kIdTgtRotMin, p.min_target_rot);
			p.max_target_rot = get_f(hwnd, kIdTgtRotMax, p.max_target_rot);
			p.min_target_size = get_f(hwnd, kIdTgtSizeMin, p.min_target_size);
			p.max_target_size = get_f(hwnd, kIdTgtSizeMax, p.max_target_size);
			p.align_motion = IsDlgButtonChecked(hwnd, kIdAlign) == BST_CHECKED;
			const int bb = static_cast<int>(SendDlgItemMessageA(hwnd, kIdBillboard, CB_GETCURSEL, 0, 0));
			p.billboard = bb >= 0 && bb <= 3 ? static_cast<std::uint8_t>(bb) : 0;
			p.motion_trail = IsDlgButtonChecked(hwnd, kIdTrail) == BST_CHECKED;
			p.trail_mult = get_f(hwnd, kIdTrailMult, p.trail_mult);
			p.restitution = get_f(hwnd, kIdBounce, p.restitution);
			p.thickness = get_f(hwnd, kIdThick, p.thickness);
			p.collide = IsDlgButtonChecked(hwnd, kIdCollide) == BST_CHECKED;
			const float old_g = g_draft.gravity_force;
			p.gravity_force = get_f(hwnd, kIdGrav, p.gravity_force);
			p.grav_override = g_draft.grav_override ||
				std::fabs(p.gravity_force - old_g) > 0.01f;
			p.max_speed = get_f(hwnd, kIdMaxSpd, p.max_speed);
			p.turb_force = get_f(hwnd, kIdTurbF, p.turb_force);
			p.turb_freq = get_f(hwnd, kIdTurbHz, p.turb_freq);
			p.use_turbulence = IsDlgButtonChecked(hwnd, kIdTurbOn) == BST_CHECKED;
			p.sheet_rows = static_cast<std::uint8_t>(
				std::clamp(get_f(hwnd, kIdSheetRows, p.sheet_rows), 0.0f, 255.0f));
			p.sheet_cols = static_cast<std::uint8_t>(
				std::clamp(get_f(hwnd, kIdSheetCols, p.sheet_cols), 0.0f, 255.0f));
			p.sheet_fps = static_cast<std::uint8_t>(
				std::clamp(get_f(hwnd, kIdSheetFps, p.sheet_fps), 0.0f, 255.0f));
			const int sm = static_cast<int>(SendDlgItemMessageA(hwnd, kIdSheetMode, CB_GETCURSEL, 0, 0));
			p.sheet_mode = sm >= 0 && sm <= 2 ? static_cast<std::uint8_t>(sm) : 0;
			const int cm = static_cast<int>(SendDlgItemMessageA(hwnd, kIdCollideMode, CB_GETCURSEL, 0, 0));
			p.collision_mode = cm >= 0 && cm <= 2 ? static_cast<std::uint8_t>(cm) : 0;
			p.has_override = true;
		}

		void row4(HWND hwnd, int y, int x, int lw, int ew, int eh, int gap,
			const char* label, int id0)
		{
			mk(hwnd, "STATIC", label, SS_LEFT, -1, x, y + 2, lw, eh);
			for (int i = 0; i < 4; i++) {
				mk(hwnd, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, id0 + i,
					x + lw + i * (ew + gap), y, ew, eh);
			}
		}

		void row2(HWND hwnd, int y, int x, int lw, int ew, int eh, int gap,
			const char* l0, int id0, const char* l1, int id1)
		{
			mk(hwnd, "STATIC", l0, SS_LEFT, -1, x, y + 2, lw, eh);
			mk(hwnd, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, id0,
				x + lw, y, ew, eh);
			const int x1 = x + lw + ew + gap * 2;
			mk(hwnd, "STATIC", l1, SS_LEFT, -1, x1, y + 2, lw, eh);
			mk(hwnd, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, id1,
				x1 + lw, y, ew, eh);
		}

		void create_children(HWND hwnd)
		{
			const int d = dpi_of(hwnd);
			RECT cr{};
			GetClientRect(hwnd, &cr);
			const int pad = px(14, d);
			const int eh = px(20, d);
			const int rh = px(24, d);
			const int lw = px(120, d);
			const int ew = px(52, d);
			const int gap = px(4, d);
			const int bw = px(72, d);
			const int bh = px(24, d);
			const int col_gap = px(18, d);
			const int col_w = (cr.right - pad * 2 - col_gap) / 2;
			const int xL = pad;
			const int xR = pad + col_w + col_gap;
			int yL = pad;
			int yR = pad;
			const int sw = px(48, d);
			const int aw = px(44, d);

			auto head_at = [&](int x, int& y, const char* t)
			{
				mk(hwnd, "STATIC", t, SS_LEFT, -1, x, y, col_w, eh);
				y += rh;
			};

			head_at(xL, yL, "Spawn");
			row_color(hwnd, yL, xL, lw, eh, sw, aw,
				"Min spawn color", kIdSpawnMinSw, kIdSpawnMinA); yL += rh;
			row_color(hwnd, yL, xL, lw, eh, sw, aw,
				"Max spawn color", kIdSpawnMaxSw, kIdSpawnMaxA); yL += rh;
			row2(hwnd, yL, xL, lw, ew, eh, gap, "Min rotation speed", kIdRotMin,
				"Max rotation speed", kIdRotMax); yL += rh;
			row2(hwnd, yL, xL, lw, ew, eh, gap, "Min particle size", kIdSizeMin,
				"Max particle size", kIdSizeMax); yL += rh;
			row2(hwnd, yL, xL, lw, ew, eh, gap, "Min time to live", kIdTtlMin,
				"Max time to live", kIdTtlMax); yL += rh;
			mk(hwnd, "BUTTON", "Hide emitter", BS_AUTOCHECKBOX | WS_TABSTOP, kIdHide,
				xL, yL, px(140, d), eh);
			yL += rh;
			row2(hwnd, yL, xL, lw, ew, eh, gap, "Velocity cone angle", kIdCone,
				"Velocity from motion", kIdVelMotion); yL += rh;
			row2(hwnd, yL, xL, lw, ew, eh, gap, "Velocity from normal", kIdVelNormal,
				"Max number of particles", kIdMaxP); yL += rh;
			mk(hwnd, "STATIC", "Particle spawn rate", SS_LEFT, -1, xL, yL + 2, lw, eh);
			mk(hwnd, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, kIdRate,
				xL + lw, yL, ew, eh);
			mk(hwnd, "BUTTON", "Use spawn mesh texture coords",
				BS_AUTOCHECKBOX | WS_TABSTOP, kIdUseUv,
				xL + lw + ew + gap * 2, yL, col_w - lw - ew - gap * 2, eh);
			yL += rh;

			head_at(xL, yL, "Target");
			row_color(hwnd, yL, xL, lw, eh, sw, aw,
				"Min target color", kIdTgtMinSw, kIdTgtMinA); yL += rh;
			row_color(hwnd, yL, xL, lw, eh, sw, aw,
				"Max target color", kIdTgtMaxSw, kIdTgtMaxA); yL += rh;
			row2(hwnd, yL, xL, lw, ew, eh, gap, "Min rotation speed", kIdTgtRotMin,
				"Max rotation speed", kIdTgtRotMax); yL += rh;
			row2(hwnd, yL, xL, lw, ew, eh, gap, "Min particle size", kIdTgtSizeMin,
				"Max particle size", kIdTgtSizeMax); yL += rh;

			head_at(xR, yR, "Visual");
			mk(hwnd, "BUTTON", "Align particles to motion", BS_AUTOCHECKBOX | WS_TABSTOP,
				kIdAlign, xR, yR, col_w, eh);
			yR += rh;
			mk(hwnd, "STATIC", "Billboard type", SS_LEFT, -1, xR, yR + 2, lw, eh);
			HWND combo = mk(hwnd, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
				kIdBillboard, xR + lw, yR, col_w - lw, px(120, d));
			if (combo)
			{
				SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("FaceCamera_Spherical"));
				SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("FaceCamera_UpAxisLocked"));
				SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("FaceCamera_Position"));
				SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("FaceWorldUp"));
			}
			yR += rh;
			mk(hwnd, "BUTTON", "Enable motion trail", BS_AUTOCHECKBOX | WS_TABSTOP,
				kIdTrail, xR, yR, px(160, d), eh);
			mk(hwnd, "STATIC", "Motion trail multiplier", SS_LEFT, -1,
				xR + px(164, d), yR + 2, px(140, d), eh);
			mk(hwnd, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, kIdTrailMult,
				xR + px(308, d), yR, ew, eh);
			yR += rh;

			head_at(xR, yR, "Sprite sheet");
			const int pick_w = px(120, d);
			mk(hwnd, "STATIC", "Animation: (plugin default / data\\animation)", SS_LEFT,
				kIdAnimLabel, xR, yR + 2, col_w - pick_w - gap * 2, eh);
			mk(hwnd, "BUTTON", "Pick frames...", BS_PUSHBUTTON | WS_TABSTOP, kIdPickAnim,
				xR + col_w - pick_w, yR, pick_w, eh);
			yR += rh;
			row2(hwnd, yR, xR, lw, ew, eh, gap, "Sheet rows", kIdSheetRows,
				"Sheet columns", kIdSheetCols); yR += rh;
			mk(hwnd, "STATIC", "Sheet FPS", SS_LEFT, -1, xR, yR + 2, lw, eh);
			mk(hwnd, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, kIdSheetFps,
				xR + lw, yR, ew, eh);
			mk(hwnd, "STATIC", "Playback", SS_LEFT, -1, xR + lw + ew + gap * 2, yR + 2,
				px(64, d), eh);
			HWND smode = mk(hwnd, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
				kIdSheetMode, xR + lw + ew + gap * 2 + px(64, d), yR,
				col_w - lw - ew - gap * 2 - px(64, d), px(120, d));
			if (smode)
			{
				SendMessageA(smode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Use material sheet"));
				SendMessageA(smode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Override - lifetime"));
				SendMessageA(smode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Override - random"));
			}
			yR += rh;

			head_at(xR, yR, "Collision");
			row2(hwnd, yR, xR, lw, ew, eh, gap, "Bounciness", kIdBounce,
				"Thickness", kIdThick); yR += rh;
			mk(hwnd, "BUTTON", "Enable collision detection", BS_AUTOCHECKBOX | WS_TABSTOP,
				kIdCollide, xR, yR, px(200, d), eh);
			mk(hwnd, "STATIC", "On hit", SS_LEFT, -1, xR + px(204, d), yR + 2, px(48, d), eh);
			HWND cmode = mk(hwnd, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
				kIdCollideMode, xR + px(252, d), yR, col_w - px(252, d), px(120, d));
			if (cmode)
			{
				SendMessageA(cmode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Bounce"));
				SendMessageA(cmode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Stop"));
				SendMessageA(cmode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Kill"));
			}
			yR += rh;

			head_at(xR, yR, "Simulation");
			row2(hwnd, yR, xR, lw, ew, eh, gap, "Gravity force", kIdGrav,
				"Max speed limit", kIdMaxSpd); yR += rh;
			row2(hwnd, yR, xR, lw, ew, eh, gap, "Turbulence force", kIdTurbF,
				"Turbulence frequency", kIdTurbHz); yR += rh;
			mk(hwnd, "BUTTON", "Apply velocity turbulence", BS_AUTOCHECKBOX | WS_TABSTOP,
				kIdTurbOn, xR, yR, px(240, d), eh);

			const int by = cr.bottom - pad - bh;
			mk(hwnd, "BUTTON", "OK", BS_PUSHBUTTON | WS_TABSTOP, kIdOk, pad, by, bw, bh);
			mk(hwnd, "BUTTON", "Apply", BS_PUSHBUTTON | WS_TABSTOP, kIdApply,
				pad + bw + px(8, d), by, bw, bh);
			mk(hwnd, "BUTTON", "Reset", BS_PUSHBUTTON | WS_TABSTOP, kIdReset,
				pad + (bw + px(8, d)) * 2, by, bw, bh);
			mk(hwnd, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, kIdCancel,
				cr.right - pad - bw, by, bw, bh);
		}

		void expand_numbered_sequence(std::vector<std::string>& files)
		{
			if (files.size() != 1) {
				return;
			}
			char dir[MAX_PATH]{};
			char name[MAX_PATH]{};
			std::strncpy(dir, files[0].c_str(), MAX_PATH - 1);
			char* slash = std::strrchr(dir, '\\');
			if (!slash) {
				slash = std::strrchr(dir, '/');
			}
			if (!slash) {
				return;
			}
			std::strncpy(name, slash + 1, MAX_PATH - 1);
			slash[1] = 0;
			char stem[MAX_PATH]{};
			std::strncpy(stem, name, MAX_PATH - 1);
			char* dot = std::strrchr(stem, '.');
			char ext[16]{};
			if (dot) {
				std::strncpy(ext, dot, 15);
				*dot = 0;
			}
			const std::size_t len = std::strlen(stem);
			std::size_t digits = 0;
			while (digits < len && stem[len - 1 - digits] >= '0' && stem[len - 1 - digits] <= '9') {
				digits++;
			}
			if (digits < 1) {
				return;
			}
			char prefix[MAX_PATH]{};
			std::strncpy(prefix, stem, len - digits);
			prefix[len - digits] = 0;
			char find[MAX_PATH]{};
			std::snprintf(find, MAX_PATH, "%s%s*%s", dir, prefix, ext[0] ? ext : ".*");
			WIN32_FIND_DATAA fd{};
			HANDLE h = FindFirstFileA(find, &fd);
			if (h == INVALID_HANDLE_VALUE) {
				return;
			}
			std::vector<std::string> found;
			do
			{
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					continue;
				}
				char full[MAX_PATH]{};
				std::snprintf(full, MAX_PATH, "%s%s", dir, fd.cFileName);
				found.emplace_back(full);
			} while (FindNextFileA(h, &fd));
			FindClose(h);
			if (found.size() > 1)
			{
				std::sort(found.begin(), found.end(),
					[](const std::string& a, const std::string& b) {
						return _stricmp(a.c_str(), b.c_str()) < 0;
					});
				files.swap(found);
			}
		}

		void pick_animation_frames(HWND hwnd)
		{
			if (!g_ident) {
				return;
			}
			char buf[32768]{};
			OPENFILENAMEA ofn{};
			ofn.lStructSize = sizeof(ofn);
			ofn.hwndOwner = hwnd;
			ofn.lpstrFilter =
				"Particle frames (*.dds;*.tga;*.png;*.jpg)\0*.dds;*.tga;*.png;*.jpg\0"
				"All files\0*.*\0";
			ofn.lpstrFile = buf;
			ofn.nMaxFile = sizeof(buf);
			ofn.lpstrInitialDir = particle_animation_dir();
			ofn.lpstrTitle = "Select particle animation frames (multi-select)";
			ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
				OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
			if (!GetOpenFileNameA(&ofn)) {
				return;
			}
			std::vector<std::string> files;
			const char* p = buf;
			if (p[std::strlen(p) + 1] == 0)
			{
				files.emplace_back(p);
			}
			else
			{
				char dir[MAX_PATH]{};
				std::strncpy(dir, p, MAX_PATH - 1);
				p += std::strlen(p) + 1;
				while (*p)
				{
					char full[MAX_PATH]{};
					std::snprintf(full, MAX_PATH, "%s\\%s", dir, p);
					files.emplace_back(full);
					p += std::strlen(p) + 1;
				}
			}
			expand_numbered_sequence(files);
			if (files.empty()) {
				return;
			}
			std::vector<const char*> ptrs;
			ptrs.reserve(files.size());
			for (auto& f : files) {
				ptrs.push_back(f.c_str());
			}
			if (apply_animation_frames(g_ident, ptrs.data(), static_cast<int>(ptrs.size())))
			{
				remix_ext_params loaded{};
				load_remix_for_identity(g_ident, loaded);
				g_draft = loaded;
				fill_edits(hwnd, loaded);
			}
		}

		void persist_from_hwnd(HWND hwnd)
		{
			if (!hwnd || !g_ident || g_filling) {
				return;
			}
			remix_ext_params p{};
			read_edits(hwnd, p);
			p.has_override = true;
			g_draft = p;
			set_remix_ext(g_ident, p);
			set_collide(g_ident, p.collide);
			save_project_ini();
		}

		void bind_pane_values(HWND hwnd, std::uint64_t identity)
		{
			remix_ext_params p{};
			load_remix_for_identity(identity, p);
			fill_edits(hwnd, p);
		}

		void commit(HWND hwnd, bool close)
		{
			persist_from_hwnd(hwnd);
			if (close) {
				DestroyWindow(hwnd);
			}
		}

		LRESULT CALLBACK pane_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			switch (msg)
			{
			case WM_CREATE:
			{
				const int d = dpi_of(hwnd);
				LOGFONTA lf{};
				lf.lfHeight = -MulDiv(9, d, 72);
				lf.lfWeight = FW_NORMAL;
				lf.lfQuality = CLEARTYPE_QUALITY;
				lf.lfCharSet = DEFAULT_CHARSET;
				strncpy_s(lf.lfFaceName, "Segoe UI", _TRUNCATE);
				g_font = CreateFontIndirectA(&lf);
				apply_dark(hwnd);
				create_children(hwnd);
				bind_pane_values(hwnd, g_ident);
				return 0;
			}
			case WM_ACTIVATE:
				if (LOWORD(wparam) != WA_INACTIVE) {
					SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
						SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
				}
				break;
			case WM_ERASEBKGND:
			{
				RECT r{};
				GetClientRect(hwnd, &r);
				FillRect(reinterpret_cast<HDC>(wparam), &r, bg_brush());
				return 1;
			}
			case WM_CTLCOLORDLG:
			case WM_CTLCOLORSTATIC:
			case WM_CTLCOLORBTN:
			case WM_CTLCOLOREDIT:
			case WM_CTLCOLORLISTBOX:
			{
				HDC hdc = reinterpret_cast<HDC>(wparam);
				SetBkMode(hdc, TRANSPARENT);
				SetBkColor(hdc, kBg);
				SetTextColor(hdc, kText);
				return reinterpret_cast<LRESULT>(bg_brush());
			}
			case WM_DRAWITEM:
			{
				const auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
				if (!dis || !dis->hDC) {
					break;
				}
				const int field = swatch_field(static_cast<int>(dis->CtlID));
				if (field < 0) {
					break;
				}
				const float* c = draft_rgb(field);
				auto to8 = [](float v)
				{
					if (v < 0.0f) {
						v = 0.0f;
					}
					if (v > 1.0f) {
						v = 1.0f;
					}
					return static_cast<int>(v * 255.0f + 0.5f);
				};
				const COLORREF rgb = RGB(to8(c[0]), to8(c[1]), to8(c[2]));
				HBRUSH br = CreateSolidBrush(rgb);
				FillRect(dis->hDC, &dis->rcItem, br);
				DeleteObject(br);
				FrameRect(dis->hDC, &dis->rcItem, GetSysColorBrush(COLOR_WINDOWFRAME));
				return TRUE;
			}
			case WM_COMMAND:
			{
				const int id = LOWORD(wparam);
				const int field = swatch_field(id);
				if (field >= 0 && HIWORD(wparam) == BN_CLICKED)
				{
					g_pick_field = field;
					g_rgb_ctx.ident = g_ident;
					g_rgb_ctx.field = field;
					const float* c = draft_rgb(field);
					static const char* titles[] = {
						"Min spawn color", "Max spawn color",
						"Min target color", "Max target color" };
					editor_settings::open_rgb_picker(hwnd, titles[field],
						c[0], c[1], c[2], &g_rgb_ctx, on_rgb_picked);
					return 0;
				}
				if (id == kIdPickAnim && HIWORD(wparam) == BN_CLICKED)
				{
					pick_animation_frames(hwnd);
					return 0;
				}
				if (id == kIdOk) {
					commit(hwnd, true);
					return 0;
				}
				if (id == kIdApply) {
					commit(hwnd, false);
					return 0;
				}
				if (id == kIdReset)
				{
					remix_ext_params p{};
					remix_defaults_for(g_ident, p);
					p.has_override = false;
					p.grav_override = false;
					fill_edits(hwnd, p);
					set_remix_ext(g_ident, p);
					save_project_ini();
					return 0;
				}
				if (id == kIdCancel) {
					DestroyWindow(hwnd);
					return 0;
				}
				if (!g_filling && g_ident)
				{
					const int note = HIWORD(wparam);
					if (note == EN_CHANGE || note == CBN_SELCHANGE ||
						(note == BN_CLICKED && field < 0))
					{
						persist_from_hwnd(hwnd);
					}
				}
				break;
			}
			case WM_CLOSE:
				DestroyWindow(hwnd);
				return 0;
			case WM_DESTROY:
				g_hwnd = nullptr;
				if (g_font)
				{
					DeleteObject(g_font);
					g_font = nullptr;
				}
				return 0;
			default:
				break;
			}
			return DefWindowProcA(hwnd, msg, wparam, lparam);
		}

		bool register_class()
		{
			static bool done = false;
			if (done) {
				return true;
			}
			WNDCLASSEXA wc{};
			wc.cbSize = sizeof(wc);
			wc.style = CS_HREDRAW | CS_VREDRAW;
			wc.lpfnWndProc = pane_proc;
			wc.hInstance = shared::globals::dll_hmodule;
			wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
			wc.hbrBackground = bg_brush();
			wc.lpszClassName = kClass;
			if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
				return false;
			}
			done = true;
			return true;
		}
	}

	void open_remix_pane(HWND /*owner*/, std::uint64_t identity)
	{
		if (!identity) {
			return;
		}
		if (g_hwnd && IsWindow(g_hwnd) && g_ident && g_ident != identity) {
			persist_from_hwnd(g_hwnd);
		}
		if (!(g_hwnd && IsWindow(g_hwnd) && g_ident == identity)) {
			prepare_properties_ini(identity);
		}
		g_ident = identity;
		if (g_hwnd && IsWindow(g_hwnd))
		{
			bind_pane_values(g_hwnd, g_ident);
			SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE);
			ShowWindow(g_hwnd, SW_SHOW);
			SetForegroundWindow(g_hwnd);
			return;
		}
		if (!register_class()) {
			return;
		}
		HWND wrap = editor_frame::wrapper_hwnd();
		const int d = dpi_of(wrap);
		const int w = px(840, d);
		const int h = px(500, d);
		RECT wr{};
		if (wrap) {
			GetWindowRect(wrap, &wr);
		}
		const int x = wr.left > 0 ? wr.left + 40 : 80;
		const int y = wr.top > 0 ? wr.top + 40 : 80;
		g_hwnd = CreateWindowExA(
			WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
			kClass, "Advanced RTX Remix - Particle Properties",
			WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
			x, y, w, h,
			nullptr, nullptr, shared::globals::dll_hmodule, nullptr);
		if (!g_hwnd) {
			return;
		}
		ShowWindow(g_hwnd, SW_SHOW);
		UpdateWindow(g_hwnd);
		SetForegroundWindow(g_hwnd);
		shared::common::log("Particles",
			std::format("opened Advanced RTX Remix pane ident={}", identity));
	}

	bool is_remix_pane_hwnd(HWND hwnd)
	{
		if (!hwnd || !g_hwnd) {
			return false;
		}
		return hwnd == g_hwnd || IsChild(g_hwnd, hwnd);
	}

	void reload_remix_pane_if_open(std::uint64_t identity)
	{
		if (!identity || !g_hwnd || !IsWindow(g_hwnd) || g_ident != identity) {
			return;
		}
		bind_pane_values(g_hwnd, g_ident);
	}

	bool filter_remix_pane_message(MSG* msg)
	{
		if (!msg || !msg->hwnd || !g_hwnd || !IsWindow(g_hwnd)) {
			return false;
		}
		if (msg->hwnd != g_hwnd && !IsChild(g_hwnd, msg->hwnd)) {
			return false;
		}
		if (msg->message >= WM_MOUSEFIRST && msg->message <= WM_MOUSELAST) {
			return false;
		}
		if (msg->message >= WM_NCMOUSEMOVE && msg->message <= WM_NCXBUTTONDBLCLK) {
			return false;
		}
		return IsDialogMessageA(g_hwnd, msg) != FALSE;
	}
}
