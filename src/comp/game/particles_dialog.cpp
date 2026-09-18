#include "std_include.hpp"
#include "particles.hpp"
#include "particles_dialog.hpp"

#include <commctrl.h>
#include <format>
#include <uxtheme.h>

namespace comp::game::particles
{
	namespace
	{
		constexpr int k_id_collide = 0x7E80;
		constexpr int k_id_remix = 0x7E81;
		constexpr int k_id_grav_x = 0x40A;
		constexpr int k_id_grav_y = 0x40B;
		constexpr int k_id_grav_z = 0x40C;
		constexpr int k_id_air = 0x40D;
		constexpr char k_prop_injected[] = "vrePCollide";
		constexpr char k_prop_identity[] = "vrePIdent";
		constexpr UINT k_subclass_id = 0x5043;

		bool path_has_folder(const char* path, const char* folder)
		{
			if (!path || !folder) {
				return false;
			}
			const std::size_t n = std::strlen(folder);
			for (const char* p = path; *p; p++)
			{
				if (_strnicmp(p, folder, n) != 0) {
					continue;
				}
				const char prev = (p == path) ? '\\' : p[-1];
				const char next = p[n];
				if ((prev == '\\' || prev == '/' || prev == ':') &&
					(next == '\\' || next == '/' || next == 0 || next == '.'))
				{
					return true;
				}
			}
			return false;
		}

		bool child_text_has(HWND dlg, const char* needle)
		{
			if (!dlg || !needle) {
				return false;
			}
			struct ctx { const char* needle; bool found; };
			ctx c{ needle, false };
			EnumChildWindows(dlg, [](HWND child, LPARAM lp) -> BOOL
			{
				auto* c = reinterpret_cast<ctx*>(lp);
				char text[96]{};
				GetWindowTextA(child, text, 96);
				if (text[0] && std::strstr(text, c->needle)) {
					c->found = true;
					return FALSE;
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&c));
			return c.found;
		}

		int dlu_y(HWND dlg, int dlu)
		{
			RECT r{ 0, 0, 0, dlu };
			MapDialogRect(dlg, &r);
			return r.bottom;
		}

		void offset_ctrl(HWND dlg, int id, int dy)
		{
			HWND c = GetDlgItem(dlg, id);
			if (!c) {
				return;
			}
			RECT r{};
			GetWindowRect(c, &r);
			MapWindowPoints(HWND_DESKTOP, dlg, reinterpret_cast<POINT*>(&r), 2);
			SetWindowPos(c, nullptr, r.left, r.top + dy, 0, 0,
				SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}

		void grow_parameters_group(HWND dlg, int dy)
		{
			struct ctx { HWND dlg; int dy; };
			ctx c{ dlg, dy };
			EnumChildWindows(dlg, [](HWND child, LPARAM lp) -> BOOL
			{
				auto* c = reinterpret_cast<ctx*>(lp);
				const LONG style = GetWindowLongA(child, GWL_STYLE);
				if ((style & BS_TYPEMASK) != BS_GROUPBOX) {
					return TRUE;
				}
				char text[64]{};
				GetWindowTextA(child, text, 64);
				if (!std::strstr(text, "Parameters")) {
					return TRUE;
				}
				RECT r{};
				GetWindowRect(child, &r);
				MapWindowPoints(HWND_DESKTOP, c->dlg, reinterpret_cast<POINT*>(&r), 2);
				SetWindowPos(child, nullptr, 0, 0, r.right - r.left,
					(r.bottom - r.top) + c->dy,
					SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
				return FALSE;
			}, reinterpret_cast<LPARAM>(&c));
		}

		LRESULT CALLBACK props_subclass(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
			UINT_PTR id, DWORD_PTR)
		{
			const int cid = LOWORD(wparam);
			const int note = HIWORD(wparam);
			if (msg == WM_COMMAND && cid == k_id_collide && note == BN_CLICKED)
			{
				HWND chk = GetDlgItem(hwnd, k_id_collide);
				const bool on = chk && SendMessageA(chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
				const std::uint64_t id64 = identity_for_properties_dialog(hwnd);
				if (id64) {
					SetPropA(hwnd, k_prop_identity,
						reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(
							id64 & 0xFFFFFFFFu)));
					set_collide(id64, on);
					save_project_ini();
				}
			}
			if (msg == WM_COMMAND && cid == k_id_remix && note == BN_CLICKED)
			{
				const std::uint64_t id64 = identity_for_properties_dialog(hwnd);
				if (id64) {
					SetPropA(hwnd, k_prop_identity,
						reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(id64 & 0xFFFFFFFFu)));
					open_remix_pane(hwnd, id64);
				}
			}
			if (msg == WM_COMMAND && cid != k_id_remix)
			{
				const bool mapped_edit = (cid >= 0x40A && cid <= 0x417) && note == EN_CHANGE;
				const bool ok = (cid == IDOK && note == BN_CLICKED);
				bool apply = false;
				if (note == BN_CLICKED && cid != k_id_collide)
				{
					HWND b = GetDlgItem(hwnd, cid);
					char t[48]{};
					if (b && GetWindowTextA(b, t, 48) &&
						(std::strstr(t, "Apply") || std::strcmp(t, "OK") == 0))
					{
						apply = true;
					}
				}
				const bool color_btn = note == BN_CLICKED && cid != k_id_collide &&
					cid != k_id_remix && cid > 7;
				if (mapped_edit || ok || apply || color_btn) {
					sync_mapped_from_properties_dialog(hwnd, ok || apply);
				}
			}
			if (msg == WM_NCDESTROY)
			{
				forget_properties_hwnd(hwnd);
				RemovePropA(hwnd, k_prop_injected);
				RemovePropA(hwnd, k_prop_identity);
				RemovePropA(hwnd, "vrePHost");
				RemovePropA(hwnd, "vrePPlug");
				RemovePropA(hwnd, "vrePTick");
				RemovePropA(hwnd, "vreOrigDlg");
				RemovePropA(hwnd, "vrePDlgArg");
				RemovePropA(hwnd, "vrePBind");
				RemoveWindowSubclass(hwnd, props_subclass, id);
			}
			return DefSubclassProc(hwnd, msg, wparam, lparam);
		}
	}

	bool is_particles_properties_dialog(HWND dlg)
	{
		if (!dlg || !IsWindow(dlg)) {
			return false;
		}
		char cls[32]{};
		GetClassNameA(dlg, cls, 32);
		if (std::strcmp(cls, "#32770") != 0) {
			return false;
		}
		char title[64]{};
		GetWindowTextA(dlg, title, 64);
		(void)title;
		if (!GetDlgItem(dlg, k_id_grav_x) || !GetDlgItem(dlg, k_id_grav_y) ||
			!GetDlgItem(dlg, k_id_grav_z) || !GetDlgItem(dlg, k_id_air))
		{
			return false;
		}
		if (!child_text_has(dlg, "Gravity (abs. vector)")) {
			return false;
		}
		const auto inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrA(dlg, GWLP_HINSTANCE));
		if (inst)
		{
			char path[MAX_PATH]{};
			if (GetModuleFileNameA(inst, path, MAX_PATH) && path[0] &&
				!path_has_folder(path, "Particles") &&
				std::strstr(path, "3DRad") == nullptr)
			{
				return false;
			}
		}
		return true;
	}

	void inject_collide_checkbox(HWND dlg)
	{
		if (!is_particles_properties_dialog(dlg)) {
			return;
		}
		if (GetWindowThreadProcessId(dlg, nullptr) != GetCurrentThreadId() &&
			!GetDlgItem(dlg, k_id_collide))
		{
			return;
		}

		HWND chk = GetDlgItem(dlg, k_id_collide);
		HWND btn = GetDlgItem(dlg, k_id_remix);
		const bool created = chk && btn;
		if (!created)
		{
			const int dy = dlu_y(dlg, 16);
			RECT wr{};
			GetWindowRect(dlg, &wr);
			SetWindowPos(dlg, nullptr, 0, 0, wr.right - wr.left,
				(wr.bottom - wr.top) + dy,
				SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			grow_parameters_group(dlg, dy);
			for (int id = 1; id <= 7; id++) {
				offset_ctrl(dlg, id, dy);
			}

			RECT box{ 17, 268, 210, 282 };
			MapDialogRect(dlg, &box);
			chk = CreateWindowExA(0, "BUTTON", "Collide with geometry",
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
				box.left, box.top, box.right - box.left, box.bottom - box.top,
				dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(k_id_collide)),
				shared::globals::dll_hmodule,
				nullptr);
			if (!chk) {
				shared::common::log("Particles",
					std::format("Collide checkbox CreateWindow failed hwnd=0x{:X} err={}",
						reinterpret_cast<std::uintptr_t>(dlg), GetLastError()));
				return;
			}

			RECT rtx{ 218, 268, 309, 282 };
			MapDialogRect(dlg, &rtx);
			btn = CreateWindowExA(0, "BUTTON", "Advanced RTX Remix",
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
				rtx.left, rtx.top, rtx.right - rtx.left, rtx.bottom - rtx.top,
				dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(k_id_remix)),
				shared::globals::dll_hmodule,
				nullptr);

			HWND font_src = GetDlgItem(dlg, k_id_grav_x);
			if (font_src)
			{
				const auto font = reinterpret_cast<HFONT>(SendMessageA(font_src, WM_GETFONT, 0, 0));
				if (font) {
					SendMessageA(chk, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
					if (btn) {
						SendMessageA(btn, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
					}
				}
			}
		}

		SetWindowSubclass(dlg, props_subclass, k_subclass_id, 0);
		const bool first_hook = !GetPropA(dlg, k_prop_injected);
		SetPropA(dlg, k_prop_injected, reinterpret_cast<HANDLE>(1));

		const std::uint64_t ident = identity_for_properties_dialog(dlg);
		if (ident) {
			SetPropA(dlg, k_prop_identity,
				reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(ident & 0xFFFFFFFFu)));
			if (first_hook) {
				prepare_properties_ini(ident);
			}
		}
		if (chk) {
			SendMessageA(chk, BM_SETCHECK,
				collide_enabled(ident) ? BST_CHECKED : BST_UNCHECKED, 0);
		}
		if (btn) {
			EnableWindow(btn, TRUE);
		}

		char title[64]{};
		GetWindowTextA(dlg, title, 64);
		if (first_hook || !created) {
			shared::common::log("Particles",
				std::format("{} Particles v1.16 hwnd=0x{:X} title='{}' Collide id=0x{:X} "
					"RTX button id=0x{:X} ident=0x{:X}",
					created ? "refresh" : "hook",
					reinterpret_cast<std::uintptr_t>(dlg), title[0] ? title : "?",
					k_id_collide, k_id_remix, ident));
		}
	}
}
