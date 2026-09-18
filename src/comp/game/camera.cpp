#include "std_include.hpp"
#include "camera.hpp"
#include "lights.hpp"
#include "particles.hpp"
#include "fog.hpp"

#include "shared/common/config.hpp"
#include "shared/common/ffp_state.hpp"
#include "shared/common/remix_api.hpp"
#include "shared/globals.hpp"
#include "comp/d3d9_proxy.hpp"
#include "comp/display_options.hpp"
#include "comp/editor_settings.hpp"
#include "comp/editor_frame.hpp"
#include "comp/project_file.hpp"
#include "comp/remix_graphics.hpp"

namespace comp::game::camera
{
	namespace
	{
		using multiply_transpose_t = D3DXMATRIX* (WINAPI*)(D3DXMATRIX*, const D3DXMATRIX*, const D3DXMATRIX*);
		using look_at_t = D3DXMATRIX* (WINAPI*)(D3DXMATRIX*, const D3DXVECTOR3*, const D3DXVECTOR3*, const D3DXVECTOR3*);
		using perspective_t = D3DXMATRIX* (WINAPI*)(D3DXMATRIX*, FLOAT, FLOAT, FLOAT, FLOAT);
		using ortho_t = D3DXMATRIX* (WINAPI*)(D3DXMATRIX*, FLOAT, FLOAT, FLOAT, FLOAT);
		using object_run_loop_t = void (__cdecl*)();

		multiply_transpose_t engine_multiply_transpose = nullptr;
		look_at_t engine_look_at = nullptr;
		perspective_t engine_perspective = nullptr;
		ortho_t engine_ortho = nullptr;
		object_run_loop_t engine_object_run_loop = nullptr;

		// Last recovered ViewProjection. The engine concatenates the same scene VP
		// for every rigid object, so recover_view (multiply + rigid test) is a
		// one-time cost per distinct matrix, not per MultiplyTranspose.
		struct recovered_vp
		{
			D3DXMATRIX m2{};
			D3DXMATRIX view{};
			int slot = -1;
			bool valid = false;
			bool scene = false;
		};

		recovered_vp last_vp{};

		HMODULE engine_module = nullptr;
		const int* camera_count_at = nullptr;
		void** const* camera_list_at = nullptr;
		void* last_list_ptr = nullptr;
		int last_list_count = 0;
		bool list_region_ok = false;

		// Remix derives the camera by inverting these matrices, so a single non-finite
		// element becomes a NaN camera and takes the Remix runtime down with it.
		bool all_finite(const D3DMATRIX& m)
		{
			for (int row = 0; row < 4; row++)
			{
				for (int col = 0; col < 4; col++)
				{
					if (!std::isfinite(m.m[row][col])) {
						return false;
					}
				}
			}
			return true;
		}

		/*
		 * Projections captured straight from the engine's D3DXMatrixPerspectiveFovLH
		 * calls, which state the camera outright: a field of view, an aspect, a near
		 * and a far plane. This is the one matrix that does not have to be inferred.
		 *
		 * Several are live at once, and more than one shares the back buffer's aspect:
		 * besides the six cubemap faces at 90 degrees and aspect 1.0, the engine also
		 * builds the scene's aspect with a horizontal field of view, 75.19 degrees
		 * being the horizontal equivalent of the camera's 60 vertical. Only distinct
		 * matrices are kept, so the camera's own projection cannot be crowded out by
		 * repeats of a near-miss, and the right one is then picked by residual.
		 */
		constexpr int candidate_slots = 16;

		struct projection_candidate
		{
			D3DXMATRIX matrix{};
			D3DXMATRIX inverse{};
			float fov = 0.0f;
			float aspect = 0.0f;
			float z_near = 0.0f;
			float z_far = 0.0f;
			bool valid = false;
		};

		projection_candidate proj_candidates[candidate_slots]{};
		int proj_next = 0;

		// The candidate that last produced a valid view, tried first so the common
		// case costs one multiply rather than a scan.
		int matched_proj = -1;

		// CreateDevice / Reset back-buffer size. Compiled-player MAIN is this
		// swapchain. Editor 3D is a separate RT (editor_rt_*), not 827×620.
		UINT viewport_w = 0;
		UINT viewport_h = 0;
		float viewport_aspect = 0.0f;
		UINT editor_rt_w = 0;
		UINT editor_rt_h = 0;

		constexpr int scene_size_slots = 8;
		UINT scene_ws[scene_size_slots]{};
		UINT scene_hs[scene_size_slots]{};
		int scene_size_n = 0;

		/*
		 * Compiled player: #32770 resolution dialog, then a small Windowed
		 * CreateDevice client (HUD authored here), then Reset / style to the
		 * chosen fullscreen, then loading, then the first scene VP.
		 *
		 * That windowed-boot client was missing — CreateDevice 1080×810
		 * Windowed was labelled picker, then Reset 1920×1080 jumped straight
		 * to loading. Remix treated the 1080 HUD as the 3D viewport.
		 *
		 * Editor (3DRADCLASS) is live from CreateDevice. Never SetTransform
		 * identity to "clear" View/P — Remix MAIN must not become identity.
		 */
		enum class init_phase
		{
			editor,
			picker,
			windowed,
			loading,
			live
		};

		init_phase phase = init_phase::editor;
		bool compiled_player = false;
		UINT sim_tick_frame = 0xFFFFFFFFu;
		bool sim_was_running = false;
		bool seen_fullscreen = false;
		bool device_windowed = true;
		UINT boot_w = 0;
		UINT boot_h = 0;
		UINT chosen_w = 0;
		UINT chosen_h = 0;
		UINT desktop_w = 0;
		UINT desktop_h = 0;
		bool confirmed_editor = false;
		bool editor_search_done = false;

		const char* phase_cstr(const init_phase p)
		{
			switch (p)
			{
			case init_phase::editor: return "editor";
			case init_phase::picker: return "picker";
			case init_phase::windowed: return "windowed";
			case init_phase::loading: return "loading";
			case init_phase::live: return "live";
			}
			return "?";
		}

		void note_sim_tick()
		{
			sim_tick_frame = shared::common::ffp_state::get().frame_count();
		}

		void __cdecl object_run_loop_stub()
		{
			note_sim_tick();
			if (engine_object_run_loop) {
				engine_object_run_loop();
			}
		}

		bool editor_sim_running()
		{
			if (compiled_player) {
				return true;
			}
			if (sim_tick_frame == 0xFFFFFFFFu) {
				return false;
			}
			const UINT frame = shared::common::ffp_state::get().frame_count();
			return (frame - sim_tick_frame) <= 1u;
		}

		void log_sim_edge()
		{
			const bool now = editor_sim_running();
			if (now == sim_was_running) {
				return;
			}
			sim_was_running = now;
			shared::common::log("Camera", now ? "sim start" : "sim stop",
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		bool is_pregame()
		{
			return compiled_player &&
				(phase == init_phase::picker ||
					phase == init_phase::windowed ||
					phase == init_phase::loading);
		}

		void log_phase(const char* why)
		{
			shared::common::log("Camera", std::format(
				"Init phase={} ({}) viewport={}x{} windowed={} compiled={}",
				phase_cstr(phase), why, viewport_w, viewport_h,
				device_windowed ? 1 : 0, compiled_player ? 1 : 0));
		}

		bool hwnd_class_is(HWND hwnd, const char* want)
		{
			if (!hwnd || !want) {
				return false;
			}

			char cls[256]{};
			if (!GetClassNameA(hwnd, cls, sizeof(cls))) {
				return false;
			}

			return std::strcmp(cls, want) == 0;
		}

		bool hwnd_is_editor(HWND hwnd)
		{
			if (!hwnd) {
				return false;
			}

			char cls[256]{};
			if (!GetClassNameA(hwnd, cls, sizeof(cls))) {
				return false;
			}

			return std::strstr(cls, "3DRADCLASS") != nullptr;
		}

		bool hwnd_is_player_frame(HWND hwnd)
		{
			return hwnd_class_is(hwnd, "Fullscreen Window") ||
				hwnd_class_is(hwnd, "Rendering Window");
		}

		bool hwnd_or_ancestor_is_editor(HWND hwnd);

		HWND find_compiled_frame_hwnd()
		{
			struct enum_ctx
			{
				DWORD pid = 0;
				HWND fullscreen = nullptr;
				HWND rendering = nullptr;
			};

			enum_ctx ctx{};
			ctx.pid = GetCurrentProcessId();
			EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL
			{
				auto* ctx = reinterpret_cast<enum_ctx*>(lp);
				DWORD pid = 0;
				GetWindowThreadProcessId(hwnd, &pid);
				if (pid != ctx->pid) {
					return TRUE;
				}

				char cls[256]{};
				if (!GetClassNameA(hwnd, cls, sizeof(cls))) {
					return TRUE;
				}

				if (std::strcmp(cls, "Fullscreen Window") == 0) {
					ctx->fullscreen = hwnd;
				}
				else if (std::strcmp(cls, "Rendering Window") == 0 && !ctx->rendering) {
					ctx->rendering = hwnd;
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&ctx));

			return ctx.fullscreen ? ctx.fullscreen : ctx.rendering;
		}

		void adopt_compiled_hwnd(HWND hint)
		{
			if (!compiled_player) {
				return;
			}
			if (hwnd_is_player_frame(shared::globals::main_window)) {
				return;
			}

			HWND want = find_compiled_frame_hwnd();
			if (!want) {
				want = hint;
			}
			if (!want || hwnd_is_editor(want) || hwnd_or_ancestor_is_editor(want)) {
				return;
			}
			if (hwnd_class_is(want, "#32770") && find_compiled_frame_hwnd()) {
				want = find_compiled_frame_hwnd();
			}
			if (!want || want == shared::globals::main_window) {
				return;
			}

			char cls[64]{};
			char was[64]{};
			GetClassNameA(want, cls, sizeof(cls));
			if (shared::globals::main_window) {
				GetClassNameA(shared::globals::main_window, was, sizeof(was));
			}
			shared::common::log("Camera", std::format(
				"player hwnd=0x{:X} class={} (was 0x{:X} {})",
				reinterpret_cast<std::uintptr_t>(want),
				cls[0] ? cls : "?",
				reinterpret_cast<std::uintptr_t>(shared::globals::main_window),
				was[0] ? was : "-"));
			shared::globals::main_window = want;
		}

		HWND compiled_player_frame()
		{
			HWND hwnd = find_compiled_frame_hwnd();
			if (hwnd_is_player_frame(hwnd)) {
				return hwnd;
			}
			if (hwnd_is_player_frame(shared::globals::main_window)) {
				return shared::globals::main_window;
			}
			return hwnd_is_player_frame(hwnd) ? hwnd : nullptr;
		}

		void release_compiled_player_mouse(HWND frame)
		{
			ClipCursor(nullptr);
			if (frame && GetCapture() == frame) {
				ReleaseCapture();
			}
		}

		void hwnd_sizes(HWND hwnd, int& cw, int& ch, int& ww, int& wh)
		{
			cw = ch = ww = wh = 0;
			if (!hwnd) {
				return;
			}
			RECT cr{};
			if (GetClientRect(hwnd, &cr)) {
				cw = cr.right - cr.left;
				ch = cr.bottom - cr.top;
			}
			RECT wr{};
			if (GetWindowRect(hwnd, &wr)) {
				ww = wr.right - wr.left;
				wh = wr.bottom - wr.top;
			}
		}

		bool hwnd_is_tiny(HWND hwnd)
		{
			int cw = 0, ch = 0, ww = 0, wh = 0;
			hwnd_sizes(hwnd, cw, ch, ww, wh);
			const bool client_ok = cw >= 64 && ch >= 64;
			const bool window_ok = ww >= 64 && wh >= 64;
			return !client_ok && !window_ok;
		}

		bool hwnd_is_taskbar_entry(HWND hwnd)
		{
			if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
				return false;
			}
			const LONG style = GetWindowLongA(hwnd, GWL_STYLE);
			const LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
			if (style & WS_MINIMIZE) {
				return false;
			}
			if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) {
				return false;
			}
			HWND owner = GetWindow(hwnd, GW_OWNER);
			if (ex & WS_EX_APPWINDOW) {
				return true;
			}
			return owner == nullptr && (style & WS_CAPTION);
		}

		bool compiled_play_class_ok(HWND hwnd)
		{
			if (!hwnd || !IsWindow(hwnd)) {
				return false;
			}
			if (comp::display_options::is_display_options(hwnd)) {
				return false;
			}
			if (hwnd_class_is(hwnd, "ConsoleWindowClass") ||
				hwnd_class_is(hwnd, "#32770") ||
				hwnd == GetConsoleWindow())
			{
				return false;
			}
			return hwnd_is_player_frame(hwnd) || hwnd_is_editor(hwnd);
		}

		HWND find_compiled_d3d_hwnd()
		{
			HWND hwnd = compiled_player_frame();
			if (compiled_play_class_ok(hwnd)) {
				return hwnd;
			}
			hwnd = shared::globals::main_window;
			if (compiled_play_class_ok(hwnd)) {
				return hwnd;
			}

			struct enum_ctx
			{
				DWORD pid = 0;
				HWND fullscreen = nullptr;
				HWND rendering = nullptr;
				HWND rad = nullptr;
			};
			enum_ctx ctx{};
			ctx.pid = GetCurrentProcessId();
			EnumWindows([](HWND w, LPARAM lp) -> BOOL
			{
				auto* ctx = reinterpret_cast<enum_ctx*>(lp);
				DWORD pid = 0;
				GetWindowThreadProcessId(w, &pid);
				if (pid != ctx->pid) {
					return TRUE;
				}
				if (!compiled_play_class_ok(w)) {
					return TRUE;
				}
				if (hwnd_class_is(w, "Fullscreen Window") && !ctx->fullscreen) {
					ctx->fullscreen = w;
				}
				else if (hwnd_class_is(w, "Rendering Window") && !ctx->rendering) {
					ctx->rendering = w;
				}
				else if (hwnd_is_editor(w) && !ctx->rad) {
					ctx->rad = w;
				}
				return TRUE;
			}, reinterpret_cast<LPARAM>(&ctx));
			if (ctx.fullscreen) {
				return ctx.fullscreen;
			}
			if (ctx.rendering) {
				return ctx.rendering;
			}
			return ctx.rad;
		}

		HWND compiled_owner_candidate(HWND hwnd)
		{
			if (!hwnd) {
				return nullptr;
			}
			HWND cands[4] = {
				GetWindow(hwnd, GW_OWNER),
				reinterpret_cast<HWND>(GetWindowLongPtrA(hwnd, GWLP_HWNDPARENT)),
				GetAncestor(hwnd, GA_ROOTOWNER),
				GetParent(hwnd),
			};
			for (HWND cand : cands)
			{
				if (!cand || cand == hwnd || !IsWindow(cand)) {
					continue;
				}
				if (hwnd_class_is(cand, "ConsoleWindowClass") ||
					hwnd_class_is(cand, "#32770") ||
					cand == GetConsoleWindow() ||
					comp::display_options::is_display_options(cand))
				{
					continue;
				}
				if (compiled_play_class_ok(cand) ||
					(GetWindowLongA(cand, GWL_STYLE) & WS_CAPTION))
				{
					return cand;
				}
			}
			return nullptr;
		}

		void show_restore_play_hwnd(HWND hwnd)
		{
			if (!hwnd || !IsWindow(hwnd)) {
				return;
			}
			EnableWindow(hwnd, TRUE);
			const LONG style = GetWindowLongA(hwnd, GWL_STYLE);
			const bool need_restore = IsIconic(hwnd) || hwnd_is_tiny(hwnd) ||
				(style & WS_MINIMIZE) != 0;
			ShowWindow(hwnd, need_restore ? SW_RESTORE : SW_SHOW);
			SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
		}

		void mark_play_taskbar(HWND hwnd)
		{
			if (!hwnd || !IsWindow(hwnd) ||
				hwnd_class_is(hwnd, "ConsoleWindowClass") ||
				hwnd == GetConsoleWindow())
			{
				return;
			}
			LONG_PTR ex = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
			ex |= WS_EX_APPWINDOW;
			ex &= ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW);
			SetWindowLongPtrA(hwnd, GWL_EXSTYLE, ex);
			SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
		}

		void hwnd_class_name(HWND hwnd, char* out, int cap)
		{
			if (!out || cap <= 0) {
				return;
			}
			out[0] = 0;
			if (hwnd && IsWindow(hwnd)) {
				GetClassNameA(hwnd, out, cap);
			}
			if (!out[0]) {
				std::strncpy(out, "-", static_cast<std::size_t>(cap) - 1);
			}
		}

		bool hwnd_is_console(HWND hwnd)
		{
			if (!hwnd) {
				return false;
			}
			char cls[64]{};
			hwnd_class_name(hwnd, cls, 64);
			if (std::strcmp(cls, "ConsoleWindowClass") == 0) {
				return true;
			}
			HWND con = GetConsoleWindow();
			return con && hwnd == con;
		}

		DWORD attach_input_to(HWND hwnd, DWORD our_tid)
		{
			if (!hwnd || !our_tid) {
				return 0;
			}
			DWORD pid = 0;
			const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
			if (!tid || tid == our_tid) {
				return 0;
			}
			return AttachThreadInput(tid, our_tid, TRUE) ? tid : 0;
		}

		void maybe_focus_compiled_play_window()
		{
			if (!compiled_player ||
				!shared::globals::is_compiled_host ||
				shared::globals::is_editor_host ||
				shared::globals::skip_remix)
			{
				return;
			}

			static bool done = false;
			static int tries = 0;
			if (done || tries >= 3) {
				return;
			}
			if (phase != init_phase::live) {
				return;
			}
			if (comp::display_options::picker_visible()) {
				return;
			}

			HWND d3d = find_compiled_d3d_hwnd();
			if (!d3d) {
				return;
			}

			HWND show = d3d;
			const bool d3d_hidden = !IsWindowVisible(d3d) || IsIconic(d3d) ||
				hwnd_is_tiny(d3d) || !hwnd_is_taskbar_entry(d3d);
			if (d3d_hidden)
			{
				if (HWND owner = compiled_owner_candidate(d3d)) {
					show = owner;
				}
			}

			char d3d_cls[64]{};
			char show_cls[64]{};
			hwnd_class_name(d3d, d3d_cls, 64);
			hwnd_class_name(show, show_cls, 64);

			int cw0 = 0, ch0 = 0, ww0 = 0, wh0 = 0;
			hwnd_sizes(d3d, cw0, ch0, ww0, wh0);
			const int vis0 = IsWindowVisible(d3d) ? 1 : 0;
			const int ic0 = IsIconic(d3d) ? 1 : 0;

			HWND fg_was = GetForegroundWindow();
			HWND focus_was = GetFocus();
			HWND cap_was = GetCapture();
			HWND con = GetConsoleWindow();
			char fg_was_cls[64]{};
			char focus_was_cls[64]{};
			char cap_was_cls[64]{};
			hwnd_class_name(fg_was, fg_was_cls, 64);
			hwnd_class_name(focus_was, focus_was_cls, 64);
			hwnd_class_name(cap_was, cap_was_cls, 64);

			ReleaseCapture();
			ClipCursor(nullptr);
			shared::common::demote_debug_console();

			SystemParametersInfoA(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, nullptr, 0);
			LockSetForegroundWindow(LSFW_UNLOCK);
			AllowSetForegroundWindow(ASFW_ANY);

			const DWORD our_tid = GetCurrentThreadId();
			const DWORD t_show = attach_input_to(show, our_tid);
			const DWORD t_d3d = attach_input_to(d3d, our_tid);
			const DWORD t_fg = attach_input_to(fg_was, our_tid);
			const DWORD t_focus = attach_input_to(focus_was, our_tid);
			const DWORD t_con = attach_input_to(con, our_tid);

			if (show != d3d) {
				show_restore_play_hwnd(show);
			}
			show_restore_play_hwnd(d3d);
			mark_play_taskbar(show);

			BringWindowToTop(show);
			SetForegroundWindow(show);
			SetActiveWindow(show);
			SetFocus(show);
			if (d3d != show) {
				BringWindowToTop(d3d);
				SetForegroundWindow(d3d);
				SetActiveWindow(d3d);
			}
			SetFocus(d3d);
			SendMessageA(show, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0),
				reinterpret_cast<LPARAM>(show));
			PostMessageA(d3d, WM_SETFOCUS, 0, 0);

			if (t_show) {
				AttachThreadInput(t_show, our_tid, FALSE);
			}
			if (t_d3d && t_d3d != t_show) {
				AttachThreadInput(t_d3d, our_tid, FALSE);
			}
			if (t_fg && t_fg != t_show && t_fg != t_d3d) {
				AttachThreadInput(t_fg, our_tid, FALSE);
			}
			if (t_focus && t_focus != t_show && t_focus != t_d3d && t_focus != t_fg) {
				AttachThreadInput(t_focus, our_tid, FALSE);
			}
			if (t_con && t_con != t_show && t_con != t_d3d && t_con != t_fg && t_con != t_focus) {
				AttachThreadInput(t_con, our_tid, FALSE);
			}

			HWND fg_now = GetForegroundWindow();
			HWND focus_now = GetFocus();
			HWND cap_now = GetCapture();
			char fg_now_cls[64]{};
			char focus_now_cls[64]{};
			char cap_now_cls[64]{};
			hwnd_class_name(fg_now, fg_now_cls, 64);
			hwnd_class_name(focus_now, focus_now_cls, 64);
			hwnd_class_name(cap_now, cap_now_cls, 64);

			int cw1 = 0, ch1 = 0, ww1 = 0, wh1 = 0;
			hwnd_sizes(d3d, cw1, ch1, ww1, wh1);
			const bool shown = (IsWindowVisible(show) && !IsIconic(show)) ||
				(IsWindowVisible(d3d) && !IsIconic(d3d));
			const bool focus_ok = focus_now == d3d || focus_now == show ||
				(focus_now && (IsChild(show, focus_now) || IsChild(d3d, focus_now) ||
					compiled_play_class_ok(focus_now)));
			const bool still_console = hwnd_is_console(focus_now) ||
				hwnd_is_console(fg_now);
			++tries;
			if ((shown && focus_ok) || tries >= 3) {
				done = true;
			}
			else if (shown && !still_console && tries >= 2) {
				done = true;
			}

			const char* stem = project_file::stem();
			shared::common::log("Camera",
				std::format(
					"focused play hwnd=0x{:X} class={} show=0x{:X} {} "
					"vis={}->{} iconic={}->{} client={}x{}->{}x{} window={}x{}->{}x{} "
					"fg_was={} fg_now={} focus_was={} focus_now={} "
					"cap_was={} cap_now={} try={} {} stem={}",
					reinterpret_cast<std::uintptr_t>(d3d),
					d3d_cls[0] ? d3d_cls : "?",
					reinterpret_cast<std::uintptr_t>(show),
					show_cls[0] ? show_cls : "?",
					vis0, IsWindowVisible(d3d) ? 1 : 0,
					ic0, IsIconic(d3d) ? 1 : 0,
					cw0, ch0, cw1, ch1, ww0, wh0, ww1, wh1,
					fg_was_cls, fg_now_cls,
					focus_was_cls, focus_now_cls,
					cap_was_cls, cap_now_cls,
					tries,
					!shown ? "hidden" : (focus_ok ? "ok" : (still_console ? "retry" : "forced")),
					(stem && stem[0]) ? stem : "-"),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		void wrap_cursor_in_client(HWND hwnd)
		{
			RECT client{};
			if (!hwnd || !GetClientRect(hwnd, &client)) {
				return;
			}
			if (client.right - client.left < 16 || client.bottom - client.top < 16) {
				return;
			}
			POINT tl{ client.left, client.top };
			POINT br{ client.right, client.bottom };
			ClientToScreen(hwnd, &tl);
			ClientToScreen(hwnd, &br);
			POINT pt{};
			if (!GetCursorPos(&pt)) {
				return;
			}
			const int m = 2;
			bool wrap = false;
			if (pt.x <= tl.x + m) {
				pt.x = br.x - m - 1;
				wrap = true;
			}
			else if (pt.x >= br.x - m) {
				pt.x = tl.x + m + 1;
				wrap = true;
			}
			if (pt.y <= tl.y + m) {
				pt.y = br.y - m - 1;
				wrap = true;
			}
			else if (pt.y >= br.y - m) {
				pt.y = tl.y + m + 1;
				wrap = true;
			}
			if (wrap) {
				SetCursorPos(pt.x, pt.y);
			}
		}

		void apply_compiled_player_mouse()
		{
			if (!compiled_player) {
				return;
			}

			HWND hwnd = compiled_player_frame();
			if (!hwnd) {
				hwnd = find_compiled_frame_hwnd();
			}

			static int logged_mouse = 0;
			static bool captured = false;
			if (logged_mouse < 3)
			{
				char cls[64]{};
				if (hwnd) {
					GetClassNameA(hwnd, cls, sizeof(cls));
				}
				POINT pt{};
				GetCursorPos(&pt);
				shared::common::log("Camera", std::format(
					"mouse hwnd=0x{:X} class={} fg=0x{:X} menu={} live={} captured={} cursor=({},{})",
					reinterpret_cast<std::uintptr_t>(hwnd),
					cls[0] ? cls : "-",
					reinterpret_cast<std::uintptr_t>(GetForegroundWindow()),
					shared::globals::imgui_menu_open ? 1 : 0,
					phase == init_phase::live ? 1 : 0,
					captured ? 1 : 0,
					pt.x, pt.y));
				logged_mouse++;
			}

			if (phase != init_phase::live || shared::globals::imgui_menu_open)
			{
				captured = false;
				release_compiled_player_mouse(hwnd);
				return;
			}

			HWND fg = GetForegroundWindow();
			DWORD pid = 0;
			if (fg) {
				GetWindowThreadProcessId(fg, &pid);
			}
			char fg_cls[64]{};
			if (fg) {
				GetClassNameA(fg, fg_cls, sizeof(fg_cls));
			}
			if (pid != GetCurrentProcessId() || !hwnd || !hwnd_is_player_frame(hwnd) ||
				std::strcmp(fg_cls, "ConsoleWindowClass") == 0 ||
				std::strcmp(fg_cls, "#32770") == 0)
			{
				captured = false;
				release_compiled_player_mouse(hwnd);
				return;
			}

			RECT client{};
			if (!GetClientRect(hwnd, &client) ||
				(client.right - client.left) < 64 ||
				(client.bottom - client.top) < 64)
			{
				captured = false;
				release_compiled_player_mouse(hwnd);
				return;
			}

			if (fg != hwnd && !hwnd_is_player_frame(fg) &&
				!IsChild(hwnd, fg) && !IsChild(fg, hwnd))
			{
				captured = false;
				release_compiled_player_mouse(hwnd);
				return;
			}

			if (fg == hwnd || hwnd_is_player_frame(fg) ||
				IsChild(hwnd, fg) || IsChild(fg, hwnd) ||
				(GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0)
			{
				captured = true;
			}
			if (!captured)
			{
				release_compiled_player_mouse(hwnd);
				return;
			}

			if (GetCapture() != hwnd) {
				SetCapture(hwnd);
			}
			wrap_cursor_in_client(hwnd);

			uint32_t counter = 0;
			while (::ShowCursor(FALSE) >= 0 && ++counter < 8) {}
		}

		void compiled_boot_sentinel_path(char* out, int cap)
		{
			if (!out || cap <= 0) {
				return;
			}
			out[0] = 0;
			char exe[MAX_PATH]{};
			GetModuleFileNameA(nullptr, exe, MAX_PATH);
			char* slash = std::strrchr(exe, '\\');
			if (slash) {
				*slash = 0;
				sprintf_s(out, static_cast<std::size_t>(cap), "%s\\rtx_comp_boot.ok", exe);
			}
			else {
				std::strncpy(out, "rtx_comp_boot.ok", static_cast<std::size_t>(cap - 1));
			}
		}

		bool editor_env_warmed()
		{
			wchar_t buf[8]{};
			return GetEnvironmentVariableW(L"RTX_COMP_EDITOR_WARMED", buf, 8) > 0 &&
				buf[0] == L'1';
		}

		void strip_cmd_flag(wchar_t* cmd, const wchar_t* flag)
		{
			if (!cmd || !flag) {
				return;
			}
			for (;;)
			{
				wchar_t* p = wcsstr(cmd, flag);
				if (!p) {
					return;
				}
				wchar_t* end = p + wcslen(flag);
				while (*end == L' ') {
					end++;
				}
				if (p > cmd && p[-1] == L' ') {
					p--;
				}
				wmemmove(p, end, wcslen(end) + 1);
			}
		}

		bool cmdline_has_warmed_flag()
		{
			if (shared::globals::is_editor_host) {
				return editor_env_warmed();
			}
			const wchar_t* cmd = GetCommandLineW();
			return cmd && wcsstr(cmd, L"--rtx-comp-warmed");
		}

		bool compiled_boot_warmed()
		{
			if (cmdline_has_warmed_flag()) {
				return true;
			}
			char path[MAX_PATH]{};
			compiled_boot_sentinel_path(path, MAX_PATH);
			const DWORD a = GetFileAttributesA(path);
			return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
		}

		void write_compiled_boot_sentinel()
		{
			char path[MAX_PATH]{};
			compiled_boot_sentinel_path(path, MAX_PATH);
			HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				shared::common::log("Camera",
					std::format("first-boot sentinel write failed {} err={}",
						path, GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return;
			}
			const char msg[] = "rtx-comp compiled player first boot\r\n";
			DWORD nw = 0;
			WriteFile(h, msg, static_cast<DWORD>(sizeof(msg) - 1), &nw, nullptr);
			CloseHandle(h);
			shared::common::log("Camera",
				std::format("first-boot sentinel {}", path));
		}

		bool relaunch_host_warmed()
		{
			wchar_t exe[MAX_PATH]{};
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			wchar_t cwd[MAX_PATH]{};
			GetCurrentDirectoryW(MAX_PATH, cwd);
			wchar_t cmd[4096]{};
			const wchar_t* live = GetCommandLineW();
			if (live) {
				wcsncpy_s(cmd, live, _TRUNCATE);
			}
			if (shared::globals::is_editor_host)
			{
				strip_cmd_flag(cmd, L"--rtx-comp-warmed");
				strip_cmd_flag(cmd, L"--rtx-comp-gfx-ok");
				SetEnvironmentVariableW(L"RTX_COMP_EDITOR_WARMED", L"1");
			}
			else if (!wcsstr(cmd, L"--rtx-comp-warmed")) {
				wcsncat_s(cmd, L" --rtx-comp-warmed", _TRUNCATE);
			}

			STARTUPINFOW si{};
			si.cb = sizeof(si);
			PROCESS_INFORMATION pi{};
			if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr,
				cwd[0] ? cwd : nullptr, &si, &pi))
			{
				shared::common::log("Camera",
					std::format("first-boot relaunch failed err={}", GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				return false;
			}
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			return true;
		}

		void maybe_relaunch_compiled_first_boot(const char* why)
		{
			(void)why;
		}

		void maybe_relaunch_editor_first_boot(const char* why)
		{
			// Editor must not ExitProcess here. The first published camera often
			// happens when a project loads, which looked like a compiler restart.
			// Compiled-player first-boot bounce is separate (scary.exe only).
			(void)why;
		}

		bool hwnd_or_ancestor_is_editor(HWND hwnd)
		{
			if (!hwnd) {
				return false;
			}

			for (HWND walk = hwnd; walk; walk = GetParent(walk))
			{
				if (hwnd_is_editor(walk)) {
					return true;
				}
			}

			const HWND owner = GetWindow(hwnd, GW_OWNER);
			if (hwnd_is_editor(owner)) {
				return true;
			}

			const HWND root = GetAncestor(hwnd, GA_ROOT);
			return hwnd_is_editor(root);
		}

		bool process_has_editor_window()
		{
		if (confirmed_editor) {
			return true;
		}
		if (hwnd_is_editor(shared::globals::main_window)) {
			confirmed_editor = true;
			return true;
		}
			if (editor_search_done) {
				return false;
			}
			editor_search_done = true;

			struct enum_ctx
			{
				DWORD pid = 0;
				bool found = false;
			};

			enum_ctx ctx{};
			ctx.pid = GetCurrentProcessId();
			EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL
			{
				auto* ctx = reinterpret_cast<enum_ctx*>(lp);
				DWORD pid = 0;
				GetWindowThreadProcessId(hwnd, &pid);
				if (pid != ctx->pid) {
					return TRUE;
				}

				char cls[256]{};
				if (GetClassNameA(hwnd, cls, sizeof(cls)) &&
					std::strstr(cls, "3DRADCLASS") != nullptr)
				{
					ctx->found = true;
					return FALSE;
				}

				return TRUE;
			}, reinterpret_cast<LPARAM>(&ctx));

			if (ctx.found) {
				confirmed_editor = true;
			}
			return ctx.found;
		}

		void refresh_desktop_size(HWND hwnd)
		{
			const HWND probe = hwnd ? hwnd : GetDesktopWindow();
			const HMONITOR mon = MonitorFromWindow(probe, MONITOR_DEFAULTTONEAREST);
			MONITORINFO info{};
			info.cbSize = sizeof(info);
			if (mon && GetMonitorInfoA(mon, &info))
			{
				desktop_w = static_cast<UINT>(info.rcMonitor.right - info.rcMonitor.left);
				desktop_h = static_cast<UINT>(info.rcMonitor.bottom - info.rcMonitor.top);
				return;
			}

			desktop_w = static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
			desktop_h = static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));
		}

		bool client_size(HWND hwnd, UINT& width, UINT& height)
		{
			RECT client{};
			if (!hwnd || !GetClientRect(hwnd, &client)) {
				return false;
			}

			width = static_cast<UINT>(client.right - client.left);
			height = static_cast<UINT>(client.bottom - client.top);
			return width != 0 && height != 0;
		}

		bool size_below_target(const UINT width, const UINT height,
			const UINT target_w, const UINT target_h)
		{
			if (width == 0 || height == 0 || target_w == 0 || target_h == 0) {
				return false;
			}

			return width + 16 < target_w || height + 16 < target_h;
		}

		bool is_small_boot_size(const UINT width, const UINT height)
		{
			if (chosen_w != 0 && chosen_h != 0 &&
				size_below_target(width, height, chosen_w, chosen_h))
			{
				return true;
			}

			if (desktop_w != 0 && desktop_h != 0 &&
				size_below_target(width, height, desktop_w, desktop_h))
			{
				return true;
			}

			if (boot_w != 0 && boot_h != 0 && chosen_w != 0 && chosen_h != 0 &&
				width <= boot_w + 2 && height <= boot_h + 2 &&
				(boot_w + 16 < chosen_w || boot_h + 16 < chosen_h))
			{
				return true;
			}

			return false;
		}

		bool matches_tracked_backbuffer(const UINT width, const UINT height)
		{
			if (viewport_w == 0 || viewport_h == 0 || width == 0 || height == 0) {
				return false;
			}

			const int dw = static_cast<int>(width) - static_cast<int>(viewport_w);
			const int dh = static_cast<int>(height) - static_cast<int>(viewport_h);
			return dw <= 2 && dw >= -2 && dh <= 2 && dh >= -2;
		}

		bool matches_editor_rt(const UINT width, const UINT height)
		{
			if (editor_rt_w == 0 || editor_rt_h == 0 || width == 0 || height == 0) {
				return false;
			}

			const int dw = static_cast<int>(width) - static_cast<int>(editor_rt_w);
			const int dh = static_cast<int>(height) - static_cast<int>(editor_rt_h);
			return dw <= 2 && dw >= -2 && dh <= 2 && dh >= -2;
		}

		// Editor MAIN is the 3D pane RT (1332×614), not the 827×620 swapchain.
		// Compiled player MAIN stays the tracked backbuffer.
		bool matches_main_viewport(const UINT width, const UINT height)
		{
			if (phase == init_phase::editor && editor_rt_w && editor_rt_h) {
				return matches_editor_rt(width, height);
			}
			return matches_tracked_backbuffer(width, height);
		}

		bool is_cubemap_viewport(const UINT width, const UINT height)
		{
			return width == height && width >= 32;
		}

		// Windowed/client 3D view or parent rect still issued after the
		// swapchain is 1920×1080 (log: 1519×824 @0,0 next to tracked_bb).
		// Cubemap squares and tiny preview RTs are not this.
		bool is_leftover_client_size(const UINT width, const UINT height)
		{
			if (width == 0 || height == 0 || viewport_w == 0 || viewport_h == 0) {
				return false;
			}
			if (matches_tracked_backbuffer(width, height)) {
				return false;
			}
			if (is_cubemap_viewport(width, height)) {
				return false;
			}

			const bool boot_leftover = is_small_boot_size(width, height) &&
				!is_small_boot_size(viewport_w, viewport_h);
			const bool smaller = (width + 16 < viewport_w) || (height + 16 < viewport_h);
			const unsigned area = width * height;
			const unsigned bb_area = viewport_w * viewport_h;
			const bool substantial = bb_area > 0 && area > bb_area / 4;

			float aspect = 0.0f;
			if (height != 0) {
				aspect = static_cast<float>(width) / static_cast<float>(height);
			}
			const float bb_aspect = viewport_aspect > 0.05f ? viewport_aspect :
				(viewport_h != 0 ? static_cast<float>(viewport_w) /
					static_cast<float>(viewport_h) : 0.0f);
			const bool aspect_mis = bb_aspect > 0.05f &&
				std::fabs(aspect - bb_aspect) > 0.04f;

			return boot_leftover || (smaller && substantial) ||
				(smaller && aspect_mis && substantial);
		}

		// Remix MAIN / scene matching:
		//   editor  — 3D RT once known (1332×614); 827 swapchain is not MAIN
		//   player  — tracked backbuffer only
		// Editor also SetViewports the window client. Tagging both MAIN
		// flipped Remix cameras every call. Do not pin 1332 to 827.
		bool accept_as_scene_size(const UINT width, const UINT height)
		{
			if (width == 0 || height == 0) {
				return false;
			}
			if (is_cubemap_viewport(width, height)) {
				return false;
			}
			if (viewport_w == 0 || viewport_h == 0) {
				return true;
			}
			return matches_main_viewport(width, height);
		}

		bool looks_fullscreen(HWND hwnd, const UINT width, const UINT height)
		{
			if (!device_windowed) {
				return true;
			}

			const bool player_frame = hwnd_is_player_frame(hwnd);
			const bool small_bb = is_small_boot_size(width, height);
			UINT cw = 0;
			UINT ch = 0;
			const bool have_client = client_size(hwnd, cw, ch);
			const bool small_client = have_client && is_small_boot_size(cw, ch);

			if (small_bb || small_client) {
				return false;
			}

			if (player_frame) {
				return true;
			}

			if (desktop_w != 0 && desktop_h != 0 &&
				width + 8 >= desktop_w && height + 8 >= desktop_h)
			{
				return true;
			}

			if (chosen_w != 0 && chosen_h != 0 &&
				width + 8 >= chosen_w && height + 8 >= chosen_h)
			{
				return true;
			}

			return false;
		}

		void note_boot_size(const UINT width, const UINT height)
		{
			if (width == 0 || height == 0 || boot_w != 0) {
				return;
			}

			boot_w = width;
			boot_h = height;
		}

		void note_chosen_size(const UINT width, const UINT height)
		{
			if (width == 0 || height == 0) {
				return;
			}

			if (width >= chosen_w) {
				chosen_w = width;
			}
			if (height >= chosen_h) {
				chosen_h = height;
			}
		}

		void enter_windowed(const char* why)
		{
			if (shared::globals::is_editor_host) {
				compiled_player = false;
				return;
			}
			compiled_player = true;
			if (phase == init_phase::live || phase == init_phase::loading) {
				return;
			}

			if (phase != init_phase::windowed)
			{
				phase = init_phase::windowed;
				log_phase(why);
			}
		}

		void enter_loading(const char* why)
		{
			if (shared::globals::is_editor_host) {
				compiled_player = false;
				return;
			}
			compiled_player = true;
			seen_fullscreen = true;
			note_chosen_size(viewport_w, viewport_h);
			if (phase != init_phase::loading && phase != init_phase::live)
			{
				phase = init_phase::loading;
				log_phase(why);
			}
		}

		void classify_hwnd(HWND hwnd)
		{
			refresh_desktop_size(hwnd);

			// Exe-name host kind wins. #32770 is the editor launch dialog,
			// Particles, Properties, AND compiled Display Options — never
			// proof of compiled player while 3DRad.exe is the host.
			if (shared::globals::is_editor_host)
			{
				compiled_player = false;
				confirmed_editor = true;
				if (phase != init_phase::live && phase != init_phase::editor)
				{
					phase = init_phase::editor;
					log_phase("host=editor");
				}
				return;
			}
			if (shared::globals::skip_remix) {
				return;
			}

			if (hwnd_or_ancestor_is_editor(hwnd) || process_has_editor_window())
			{
				compiled_player = false;
				confirmed_editor = true;
				if (phase != init_phase::live && phase != init_phase::editor)
				{
					phase = init_phase::editor;
					log_phase("3DRADCLASS");
				}
				return;
			}

			if (hwnd_class_is(hwnd, "#32770"))
			{
				compiled_player = true;
				if (phase == init_phase::editor)
				{
					phase = init_phase::picker;
					log_phase("resolution dialog");
				}
				return;
			}

			if (!hwnd_is_editor(hwnd)) {
				compiled_player = true;
			}

			if (looks_fullscreen(hwnd, viewport_w, viewport_h))
			{
				if (phase == init_phase::editor ||
					phase == init_phase::picker ||
					phase == init_phase::windowed)
				{
					enter_loading(hwnd_is_player_frame(hwnd) ?
						"Fullscreen Window" : "exclusive / fullscreen");
				}
				return;
			}

			if (phase == init_phase::editor || phase == init_phase::picker)
			{
				char cls[256]{};
				if (hwnd) {
					GetClassNameA(hwnd, cls, sizeof(cls));
				}
				enter_windowed(cls[0] ? cls : "windowed client");
			}
		}

		void promote_windowed_boot(const char* why)
		{
			if (!compiled_player || phase == init_phase::live ||
				phase == init_phase::loading || phase == init_phase::editor)
			{
				return;
			}

			if (looks_fullscreen(nullptr, viewport_w, viewport_h)) {
				return;
			}

			if (device_windowed || is_small_boot_size(viewport_w, viewport_h))
			{
				enter_windowed(why);
			}
		}

		bool conversion_allowed()
		{
			return phase == init_phase::editor || phase == init_phase::live;
		}

		void log_device_surfaces(IDirect3DDevice9* dev, const char* why)
		{
			if (!dev || !why) {
				return;
			}

			D3DVIEWPORT9 viewport{};
			dev->GetViewport(&viewport);

			UINT bb_w = 0;
			UINT bb_h = 0;
			IDirect3DSurface9* back = nullptr;
			if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back)
			{
				D3DSURFACE_DESC desc{};
				if (SUCCEEDED(back->GetDesc(&desc)))
				{
					bb_w = desc.Width;
					bb_h = desc.Height;
				}
				back->Release();
			}

			UINT rt_w = 0;
			UINT rt_h = 0;
			IDirect3DSurface9* rt = nullptr;
			if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt)
			{
				D3DSURFACE_DESC desc{};
				if (SUCCEEDED(rt->GetDesc(&desc)))
				{
					rt_w = desc.Width;
					rt_h = desc.Height;
				}
				rt->Release();
			}

			HWND hwnd = shared::globals::main_window;
			if (!hwnd)
			{
				D3DDEVICE_CREATION_PARAMETERS cp{};
				if (SUCCEEDED(dev->GetCreationParameters(&cp))) {
					hwnd = cp.hFocusWindow;
				}
			}

			UINT cw = 0;
			UINT ch = 0;
			char cls[64]{};
			RECT wr{};
			if (hwnd)
			{
				GetClassNameA(hwnd, cls, sizeof(cls));
				client_size(hwnd, cw, ch);
				GetWindowRect(hwnd, &wr);
			}

			D3DMATRIX proj{};
			D3DMATRIX view{};
			dev->GetTransform(D3DTS_PROJECTION, &proj);
			dev->GetTransform(D3DTS_VIEW, &view);

			static char last[192]{};
			char sig[192]{};
			std::snprintf(sig, sizeof(sig), "%s|%u|%u|%u|%u|%u|%u|%.2f|%s",
				why, viewport.Width, viewport.Height, bb_w, bb_h, rt_w, rt_h, proj._44,
				phase_cstr(phase));
			if (std::strcmp(last, sig) == 0) {
				return;
			}
			std::snprintf(last, sizeof(last), "%s", sig);

			shared::common::log("Camera", std::format(
				"Surfaces {}: class={} client={}x{} win={}x{} vp={}x{}@{},{} "
				"bb={}x{} rt={}x{} tracked={}x{} proj44={:.4f} view44={:.4f} windowed={}",
				why, cls[0] ? cls : "-", cw, ch,
				wr.right - wr.left, wr.bottom - wr.top,
				viewport.Width, viewport.Height, viewport.X, viewport.Y,
				bb_w, bb_h, rt_w, rt_h, viewport_w, viewport_h,
				proj._44, view._44, device_windowed ? 1 : 0));
		}

		void clear_scene_sizes()
		{
			scene_size_n = 0;
			for (int i = 0; i < scene_size_slots; i++)
			{
				scene_ws[i] = 0;
				scene_hs[i] = 0;
			}
			viewport_w = 0;
			viewport_h = 0;
			viewport_aspect = 0.0f;
			editor_rt_w = 0;
			editor_rt_h = 0;
		}

		void apply_backbuffer_size(const UINT width, const UINT height)
		{
			if (width == 0 || height == 0) {
				return;
			}

			viewport_w = width;
			viewport_h = height;
			viewport_aspect = static_cast<float>(width) / static_cast<float>(height);
		}

		void remember_scene_size(const UINT width, const UINT height)
		{
			if (width == 0 || height == 0) {
				return;
			}

			// Picker / windowed-boot / loading HUD is drawn on the swapchain.
			// Registering that size as scene makes Remix (and our RT match)
			// treat UI as the 3D viewport — 1080×810 stretched into 1920×1080.
			if (!conversion_allowed()) {
				return;
			}

			// Live leftover client rects (1519×824) and anything smaller /
			// aspect-mismatched vs the tracked backbuffer are not MAIN.
			if (!accept_as_scene_size(width, height)) {
				return;
			}

			for (int i = 0; i < scene_size_n; i++)
			{
				if (scene_ws[i] == width && scene_hs[i] == height) {
					return;
				}
			}

			if (scene_size_n >= scene_size_slots) {
				return;
			}

			scene_ws[scene_size_n] = width;
			scene_hs[scene_size_n] = height;
			scene_size_n++;

			shared::common::log("Camera", std::format(
				"Scene size {}: {}x{} aspect={:.4f}",
				scene_size_n, width, height,
				static_cast<float>(width) / static_cast<float>(height)));
		}

		bool aspect_matches_scene(const float aspect)
		{
			if (!(aspect > 0.05f) || !std::isfinite(aspect)) {
				return false;
			}

			if (viewport_aspect > 0.05f &&
				std::fabs(aspect - viewport_aspect) <= 0.08f)
			{
				return true;
			}

			for (int i = 0; i < scene_size_n; i++)
			{
				if (scene_hs[i] == 0) {
					continue;
				}
				const float a = static_cast<float>(scene_ws[i]) /
					static_cast<float>(scene_hs[i]);
				if (std::fabs(aspect - a) <= 0.08f) {
					return true;
				}
			}

			return false;
		}

		/*
		 * Forward axes of the cameras the engine aimed this frame, taken as
		 * normalize(at - eye) from D3DXMatrixLookAtLH.
		 *
		 * These are an independent witness. A recovered view's third column is the
		 * same forward axis, arrived at along a completely different route, so the
		 * two agreeing confirms the recovered matrix really is a camera the engine
		 * built rather than a coincidentally rigid matrix.
		 */
		constexpr int forward_slots = 8;
		D3DXVECTOR3 camera_forwards[forward_slots]{};
		int forward_next = 0;

		/*
		 * The ViewProjection most of the previous frame's draws used.
		 *
		 * Startup frames aim several cameras, and whichever happened to be set last
		 * would otherwise decide what Remix sees. The scene camera is the one the
		 * bulk of the draws share, so that is the only one applied.
		 */
		D3DXMATRIX dominant_view_proj{};
		D3DXMATRIX dominant_view{};
		D3DXMATRIX dominant_proj{};
		bool have_dominant = false;

		/*
		 * Remix's MAIN camera is first-write-wins per frame (RtCamera::update
		 * returns if m_frameLastTouched == frameIdx). Publish the first good
		 * scene camera this Present immediately — do not wait extra frames,
		 * and do not wait until Present. Later cubemap 90 / preview RTs must
		 * not overwrite that identity.
		 *
		 * Across frames Remix follows the engine Rendering flag (+0x124==0)
		 * on every CamChase / Cam1StPerson / Camera plugin instance. Identity
		 * is the plugin pointer (ObjectId like Particles). Not sticky-handle,
		 * not sim-start/stop, not draw count. A list-walk miss may reuse last
		 * C/View for that Present only.
		 */
		struct frame_camera_lock
		{
			bool identity_locked = false;
			bool matrices_locked = false;
			const void* handle = nullptr;
			float fov = 0.0f;
			D3DXVECTOR3 eye{};
			bool have_eye = false;
			bool reused_last_eye = false;
			bool switched = false;
			int proj_slot = -1;
			D3DXMATRIX view{};
			D3DXMATRIX proj{};
			int skipped_non_scene = 0;
			int view_updates = 0;
			int list_walks = 0;
			int published = 0;
			bool api_inject = false;
			const char* skip_reason = nullptr;
			const char* identity_source = nullptr;
			const char* publish_source = nullptr;
		};

		frame_camera_lock frame_lock{};

		struct last_good_camera
		{
			const void* handle = nullptr;
			float fov = 0.0f;
			D3DXVECTOR3 eye{};
			bool have_eye = false;
			D3DXMATRIX view{};
			D3DXMATRIX proj{};
			bool have_matrices = false;
		};

		last_good_camera last_good{};

		// Shown camera object cached after a successful list match. Later frames
		// read C / FOV / shown directly from this pointer instead of walking
		// 0x100B7A28 on every draw or concatenation.
		struct cached_shown_camera
		{
			const void* handle = nullptr;
			float fov = 0.0f;
			D3DXVECTOR3 eye{};
			bool have_eye = false;
			bool probed = false;
		};

		cached_shown_camera cached_cam{};

		// camera_proxy 0.3.0: FNV-1a + memcmp skip unchanged constant uploads.
		// We hash composed View/P + C and SetTransform only when that changes
		// (or the first time a scene camera is seen).
		uint32_t published_hash = 0;
		bool have_published = false;

		uint32_t fnv1a_words(const void* data, const size_t words)
		{
			uint32_t hash = 2166136261u;
			const auto* p = static_cast<const std::uint32_t*>(data);
			for (size_t i = 0; i < words; i++)
			{
				hash ^= p[i];
				hash *= 16777619u;
			}
			return hash;
		}

		bool same_matrix(const D3DMATRIX& a, const D3DMATRIX& b)
		{
			return std::memcmp(&a, &b, sizeof(D3DMATRIX)) == 0;
		}

		uint32_t hash_published_camera(const D3DMATRIX& view, const D3DMATRIX& proj,
			const D3DXVECTOR3* eye)
		{
			uint32_t hash = fnv1a_words(&view, 16);
			hash ^= fnv1a_words(&proj, 16);
			hash *= 16777619u;
			if (eye) {
				hash ^= fnv1a_words(eye, 3);
			}
			return hash;
		}

		bool compose_locked_camera(D3DMATRIX& view, D3DMATRIX& proj, bool prepare);
		void publish_if_changed(const char* source);

		// Scene ViewProjections seen this frame, for Present if no scene draw locked.
		struct stashed_scene_vp
		{
			D3DXMATRIX view{};
			D3DXMATRIX proj{};
			D3DXMATRIX view_proj{};
			int slot = -1;
			bool valid = false;
		};

		constexpr int stash_slots = 4;
		stashed_scene_vp scene_stash[stash_slots]{};
		int scene_stash_n = 0;

		void invalidate_camera_after_reset()
		{
			last_vp = {};
			published_hash = 0;
			have_published = false;
			have_dominant = false;
			lights::reset();
			particles::reset();
			fog::reset();
			last_good.have_matrices = false;
			matched_proj = -1;
			frame_lock = {};
			scene_stash_n = 0;
			for (auto& entry : scene_stash) {
				entry = {};
			}
			editor_rt_w = 0;
			editor_rt_h = 0;
			// Do not SetTransform identity. Remix MAIN must not become identity.
		}

		/*
		 * What the engine concatenated, and out of which parts.
		 *
		 * The hook is the only place where the world, the ViewProjection and the
		 * exact value the shader will receive are all visible at once, so recording
		 * the three together turns the camera question from a guess into a lookup: a
		 * draw finds its own entry by the constant it was given.
		 *
		 * This replaced applying the camera from inside the hook. That could not be
		 * made correct — the engine aims several cameras per frame, so whichever
		 * fired last decided what every following draw was transformed by,
		 * regardless of which camera the draw belonged to.
		 */
		struct draw_record
		{
			D3DXMATRIX constant{}; // World * View * Projection, as the shader sees it
			D3DXMATRIX view{};
			D3DXMATRIX proj{};
			D3DXMATRIX world{};
			int proj_index = -1;
			bool valid = false;
		};

		constexpr int record_slots = 64;
		draw_record records[record_slots]{};
		int record_next = 0;

		/*
		 * True when two matrices agree to within a relative tolerance.
		 *
		 * Scaled per element because these matrices mix magnitudes: projection depth
		 * terms sit near 1 while a view translation can be in the thousands, so one
		 * absolute epsilon cannot serve both.
		 */
		bool nearly_equal(const D3DXMATRIX& a, const D3DXMATRIX& b)
		{
			constexpr float relative_tolerance = 1e-4f;

			for (int row = 0; row < 4; row++)
			{
				for (int col = 0; col < 4; col++)
				{
					const float lhs = a.m[row][col];
					const float rhs = b.m[row][col];
					const float scale = std::max(1.0f, std::max(std::fabs(lhs), std::fabs(rhs)));

					if (std::fabs(lhs - rhs) > relative_tolerance * scale) {
						return false;
					}
				}
			}
			return true;
		}

		/*
		 * True when a matrix is a rigid body transform, which every view matrix is:
		 * a rotation and a translation, nothing else.
		 *
		 * This is the test that identifies the camera. Pairing the wrong projection
		 * with a ViewProjection leaves the mismatch behind as scale or shear, and a
		 * World * ViewProjection leaves the object's own scale behind, so neither
		 * survives here. The earlier approach only checked the shape of the product
		 * and could not tell those cases apart at all.
		 */
		bool is_rigid_transform(const D3DXMATRIX& m)
		{
			// Loose enough to absorb a float32 multiply chain, tight enough that a
			// scaled or sheared matrix has no chance: a correct view lands within
			// 0.0002 of unit rows, two orders of magnitude inside this.
			constexpr float rigid_tolerance = 0.01f;

			if (std::fabs(m._14) > rigid_tolerance ||
				std::fabs(m._24) > rigid_tolerance ||
				std::fabs(m._34) > rigid_tolerance ||
				std::fabs(m._44 - 1.0f) > rigid_tolerance)
			{
				return false;
			}

			// Rows of the rotation block must be unit length and mutually perpendicular.
			for (int row = 0; row < 3; row++)
			{
				float length_sq = 0.0f;
				for (int col = 0; col < 3; col++) {
					length_sq += m.m[row][col] * m.m[row][col];
				}

				if (std::fabs(length_sq - 1.0f) > rigid_tolerance) {
					return false;
				}

				for (int other = row + 1; other < 3; other++)
				{
					float dot = 0.0f;
					for (int col = 0; col < 3; col++) {
						dot += m.m[row][col] * m.m[other][col];
					}

					if (std::fabs(dot) > rigid_tolerance) {
						return false;
					}
				}
			}

			return true;
		}

		/*
		 * Dumps everything needed to see why a ViewProjection was not accepted:
		 * the matrix itself, the projections on offer, and the shape of the view each
		 * one implies. Row lengths of the rotation block are the telling number, since
		 * a uniform scale means the wrong projection and a non-uniform one means the
		 * matrix was not a plain ViewProjection to begin with.
		 */
		/*
		 * Confirms a recovered view against the directions the engine actually aimed.
		 *
		 * The third column of a view matrix is the camera's forward axis, and
		 * D3DXMatrixLookAtLH was handed that same axis as at - eye. Deriving it twice
		 * by unrelated means and getting the same answer is the strongest
		 * identification available here, and it costs nothing.
		 */
		bool forward_axis_corroborated(const D3DXMATRIX& view, float& best_error)
		{
			constexpr float axis_tolerance = 0.01f;

			const D3DXVECTOR3 recovered{ view._13, view._23, view._33 };
			best_error = 2.0f;

			for (const auto& forward : camera_forwards)
			{
				if (forward.x == 0.0f && forward.y == 0.0f && forward.z == 0.0f) {
					continue;
				}

				const float error = std::sqrt(
					(recovered.x - forward.x) * (recovered.x - forward.x) +
					(recovered.y - forward.y) * (recovered.y - forward.y) +
					(recovered.z - forward.z) * (recovered.z - forward.z));

				best_error = std::min(best_error, error);
			}

			return best_error <= axis_tolerance;
		}

		void dump_matrix(const char* label, const D3DXMATRIX& m, const shared::common::LOG_TYPE type)
		{
			for (int row = 0; row < 4; row++)
			{
				shared::common::log("Camera", std::format("{} row{}: {: 10.5f} {: 10.5f} {: 10.5f} {: 10.5f}",
					label, row, m.m[row][0], m.m[row][1], m.m[row][2], m.m[row][3]), type, true);
			}
		}

		void report_rejection(const D3DXMATRIX& combined)
		{
			dump_matrix("m2", combined, shared::common::LOG_TYPE::LOG_TYPE_WARN);

			auto column_length = [&combined](const int col)
				{
					float sum = 0.0f;
					for (int row = 0; row < 3; row++) {
						sum += combined.m[row][col] * combined.m[row][col];
					}
					return std::sqrt(sum);
				};

			shared::common::log("Camera", std::format(
				"m2 column lengths: w={:.5f} h={:.5f} (the projection scales to look for)",
				column_length(0), column_length(1)),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);

			for (int p = 0; p < candidate_slots; p++)
			{
				const auto& candidate = proj_candidates[p];
				if (!candidate.valid) {
					continue;
				}

				D3DXMATRIX view{};
				D3DXMatrixMultiply(&view, &combined, &candidate.inverse);

				float len[3]{};
				for (int row = 0; row < 3; row++)
				{
					len[row] = std::sqrt(view.m[row][0] * view.m[row][0] +
						view.m[row][1] * view.m[row][1] + view.m[row][2] * view.m[row][2]);
				}

				shared::common::log("Camera", std::format(
					"proj[{}] fov={:.2f}deg aspect={:.4f} zn={:.3f} zf={:.1f} w={:.5f} h={:.5f} "
					"-> view rows=({:.4f} {:.4f} {:.4f})",
					p, candidate.fov * 57.2957795f, candidate.aspect, candidate.z_near,
					candidate.z_far, candidate.matrix._11, candidate.matrix._22,
					len[0], len[1], len[2]),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
		}

		/*
		 * Recovers the view matrix as ViewProjection * inverse(Projection).
		 *
		 * Both inputs are exact — the engine hands the ViewProjection to its shaders
		 * and built the projection from stated parameters — so this is a single
		 * well-conditioned multiply. The previous approach had to treat both factors
		 * as unknown and recover the projection's scales from column lengths, which
		 * loses the far plane entirely (see kb.h).
		 */
		bool recover_view(const D3DXMATRIX& combined, D3DXMATRIX& view, int& proj_index)
		{
			/*
			 * Which projection built this ViewProjection is not a guess.
			 *
			 * ViewProjection = R * P with R a rotation, and a rotation preserves
			 * length, so the first two columns of the product are exactly as long as
			 * the projection's own w and h. Reading those two lengths names the
			 * projection directly, and no cubemap face or secondary camera shares them.
			 */
			auto column_length = [&combined](const int col)
				{
					float sum = 0.0f;
					for (int row = 0; row < 3; row++) {
						sum += combined.m[row][col] * combined.m[row][col];
					}
					return std::sqrt(sum);
				};

			const float width_scale = column_length(0);
			const float height_scale = column_length(1);

			if (!std::isfinite(width_scale) || !std::isfinite(height_scale) ||
				width_scale <= 0.0f || height_scale <= 0.0f)
			{
				return false;
			}

			auto attempt = [&](const int p)
				{
					const auto& candidate = proj_candidates[p];
					if (!candidate.valid) {
						return false;
					}

					// A relative match, since w and h differ in magnitude. Tight,
					// because two of the engine's projections share the scene aspect
					// and only these scales tell them apart: 1.29844 against 0.97697.
					constexpr float scale_tolerance = 0.001f;
					if (std::fabs(candidate.matrix._11 - width_scale) > scale_tolerance * width_scale ||
						std::fabs(candidate.matrix._22 - height_scale) > scale_tolerance * height_scale)
					{
						return false;
					}

					D3DXMatrixMultiply(&view, &combined, &candidate.inverse);

					// The scales matching is strong but not sufficient: a World with
					// unit scale would also pass it, so the view still has to prove
					// it is a rigid transform.
					return all_finite(view) && is_rigid_transform(view);
				};

			if (matched_proj >= 0 && attempt(matched_proj))
			{
				proj_index = matched_proj;
				return true;
			}

			for (int p = 0; p < candidate_slots; p++)
			{
				if (p != matched_proj && attempt(p))
				{
					proj_index = p;
					return true;
				}
			}

			return false;
		}

		/*
		 * The engine's camera objects live in a pointer table at a pair of
		 * preferred-base globals (rebase by dll3impact.dll's load address).
		 *
		 * Live-confirmed offsets, 2026-09-13:
		 *   +0x50  iCameraLocation xyz (world space)
		 *   +0x78  vertical FOV in radians (pi/3 = 60deg editor camera)
		 *   +0x124 0 = shown (iCameraShown returns this xor 1), 1 = hidden
		 *
		 * Hidden 75deg cameras are Cam 1StPerson / CamChase. When shown (play)
		 * they are the scene camera; when hidden they must not drive Remix.
		 */
		constexpr std::uintptr_t preferred_base = 0x10000000;
		constexpr std::uintptr_t camera_count_va = 0x100B7A88;
		constexpr std::uintptr_t camera_list_va = 0x100B7A28;
		constexpr int off_location = 0x50;
		constexpr int off_fov = 0x78;
		constexpr int off_shown = 0x124;
		constexpr float fov_lock_tolerance = 0.08f;
		constexpr float fov_band_60 = 1.04719755f; // pi/3, editor
		constexpr float fov_band_75 = 1.309f;      // ~75deg, play / CamChase
		constexpr float fov_band_width = 0.15f;    // ~8.6deg; 60 and 75 stay apart

		bool memory_readable(const void* p, const SIZE_T bytes)
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

		bool fov_close(const float a, const float b)
		{
			return std::fabs(a - b) <= fov_lock_tolerance;
		}

		int fov_band(const float fov)
		{
			if (!(fov > 0.0f) || !std::isfinite(fov)) {
				return 0;
			}

			const float d60 = std::fabs(fov - fov_band_60);
			const float d75 = std::fabs(fov - fov_band_75);
			if (d60 <= fov_band_width && d60 <= d75) {
				return 60;
			}
			if (d75 <= fov_band_width) {
				return 75;
			}
			return 0;
		}

		bool same_fov_band(const float a, const float b)
		{
			const int ba = fov_band(a);
			const int bb = fov_band(b);
			if (ba != 0 && bb != 0) {
				return ba == bb;
			}
			return fov_close(a, b);
		}

		float vertical_fov_of(const projection_candidate& candidate)
		{
			if (candidate.matrix._22 > 0.01f && std::isfinite(candidate.matrix._22)) {
				return 2.0f * std::atan(1.0f / candidate.matrix._22);
			}
			return candidate.fov;
		}

		bool is_scene_projection(const projection_candidate& candidate)
		{
			if (!candidate.valid) {
				return false;
			}

			// Cubemap faces: 90deg, aspect 1. Preview RTs are usually square.
			constexpr float half_pi = 1.57079632679f;
			if (std::fabs(candidate.fov - half_pi) < 0.05f &&
				std::fabs(candidate.aspect - 1.0f) < 0.05f)
			{
				return false;
			}

			if (aspect_matches_scene(candidate.aspect)) {
				return true;
			}

			// Before any back-buffer size is known, accept a non-square 3D FOV.
			// After CreateDevice/Reset the current viewport aspect is the only
			// scene match — leftover 1080×810 / 4:3 must not stay scene at 16:9.
			if (viewport_aspect <= 0.05f &&
				std::fabs(candidate.aspect - 1.0f) > 0.08f &&
				candidate.fov > 0.35f && candidate.fov < 2.20f)
			{
				return true;
			}

			return false;
		}

		bool try_enter_live_from_scene_vp(const int slot)
		{
			if (phase == init_phase::editor || phase == init_phase::live) {
				return true;
			}

			if (slot < 0 || slot >= candidate_slots || !proj_candidates[slot].valid) {
				return false;
			}

			if (!is_scene_projection(proj_candidates[slot])) {
				return false;
			}

			// Live only after a real fullscreen Reset / exclusive / Fullscreen
			// Window at the chosen size. Windowed-boot (1080×810 Windowed)
			// and the picker must not publish a 3D camera — HUD is authored
			// in that small pixel space.
			if (phase != init_phase::loading || !seen_fullscreen) {
				return false;
			}

			phase = init_phase::live;
			remember_scene_size(viewport_w, viewport_h);
			log_phase("first scene MultiplyTranspose / perspective VP");
			maybe_relaunch_compiled_first_boot("first live scene camera");
			// Do not InitializeLibrary here. This fires on the first loading-screen
			// scene VP, before a camera is published. Remix abort()/terminate then
			// shows the VC80 "Runtime to terminate in an unusual way" dialog.
			// BeginScene / CreateDevice init when remix_api_init_allowed().
			return true;
		}

		bool matches_locked_fov(const projection_candidate& candidate, const float locked_fov)
		{
			return same_fov_band(vertical_fov_of(candidate), locked_fov) ||
				same_fov_band(candidate.fov, locked_fov);
		}

		void apply_world_space_view(D3DMATRIX& view, const D3DXVECTOR3& eye);
		void apply_world_space_world(D3DMATRIX& world, const D3DXVECTOR3& eye);

		struct shown_camera
		{
			const void* handle = nullptr;
			float fov = 0.0f;
			D3DXVECTOR3 eye{};
			bool have_eye = false;
		};

		bool pointer_looks_valid(const void* p);
		bool read_camera_fields(const void* handle, shown_camera& out, int& hidden,
			const bool probe);

		bool bind_camera_table()
		{
			if (camera_count_at && camera_list_at) {
				return true;
			}

			if (!engine_module)
			{
				engine_module = GetModuleHandleA("dll3impact.dll");
				if (!engine_module) {
					return false;
				}
			}

			const auto rebase = [](const std::uintptr_t preferred)
				{
					return reinterpret_cast<std::uintptr_t>(engine_module) + (preferred - preferred_base);
				};

			const auto base = reinterpret_cast<std::uintptr_t>(engine_module);
			const auto* count_at = reinterpret_cast<const int*>(rebase(camera_count_va));
			const auto* list_at = reinterpret_cast<void** const*>(rebase(camera_list_va));
			if (!memory_readable(count_at, sizeof(int)) || !memory_readable(list_at, sizeof(void*))) {
				return false;
			}

			static bool logged_rebase = false;
			if (!logged_rebase)
			{
				logged_rebase = true;
				shared::common::log("Camera", std::format(
					"dll3impact base=0x{:X} (preferred 0x{:X}{}) camera_count=0x{:X} camera_list=0x{:X}",
					base, preferred_base,
					base == preferred_base ? ", at preferred" : ", rebased",
					reinterpret_cast<std::uintptr_t>(count_at),
					reinterpret_cast<std::uintptr_t>(list_at)));
			}

			camera_count_at = count_at;
			camera_list_at = list_at;
			return true;
		}

		int collect_shown_cameras(shown_camera* out, const int max_n)
		{
			if (!out || max_n <= 0) {
				return 0;
			}

			frame_lock.list_walks++;

			if (!bind_camera_table()) {
				return 0;
			}

			const int count = *camera_count_at;
			void* const* list = *camera_list_at;
			if (count <= 0 || count > 256 || !list) {
				return 0;
			}

			if (list != last_list_ptr || count != last_list_count)
			{
				list_region_ok = memory_readable(list, static_cast<SIZE_T>(count) * sizeof(void*));
				last_list_ptr = const_cast<void*>(static_cast<const void*>(list));
				last_list_count = count;
			}

			if (!list_region_ok) {
				return 0;
			}

			int found = 0;
			for (int i = 0; i < count && found < max_n; i++)
			{
				const auto* cam = static_cast<const std::uint8_t*>(list[i]);
				if (!pointer_looks_valid(cam)) {
					continue;
				}

				shown_camera entry{};
				int hidden = 1;
				if (!read_camera_fields(cam, entry, hidden, false) || hidden != 0) {
					continue;
				}

				out[found++] = entry;
			}

			return found;
		}

		void adopt_eye(const D3DXVECTOR3& eye, const bool reused)
		{
			frame_lock.eye = eye;
			frame_lock.have_eye = true;
			frame_lock.reused_last_eye = reused;
			if (!reused)
			{
				last_good.eye = eye;
				last_good.have_eye = true;
			}
		}

		bool pointer_looks_valid(const void* p)
		{
			const auto addr = reinterpret_cast<std::uintptr_t>(p);
			return p && (addr & 3) == 0 && addr >= 0x10000 && addr < 0x7FFE0000;
		}

		bool read_camera_fields(const void* handle, shown_camera& out, int& hidden,
			const bool probe)
		{
			if (!pointer_looks_valid(handle)) {
				return false;
			}

			const auto* cam = static_cast<const std::uint8_t*>(handle);
			if (probe)
			{
				if (!memory_readable(cam + off_shown, sizeof(int)) ||
					!memory_readable(cam + off_fov, sizeof(float)) ||
					!memory_readable(cam + off_location, sizeof(float) * 3))
				{
					return false;
				}
			}

			hidden = *reinterpret_cast<const int*>(cam + off_shown);
			if (hidden != 0 && hidden != 1) {
				return false;
			}

			out = {};
			out.handle = handle;
			out.fov = *reinterpret_cast<const float*>(cam + off_fov);
			if (!std::isfinite(out.fov) || out.fov <= 0.1f || out.fov > 3.2f) {
				return false;
			}

			const auto* xyz = reinterpret_cast<const float*>(cam + off_location);
			if (std::isfinite(xyz[0]) && std::isfinite(xyz[1]) && std::isfinite(xyz[2]))
			{
				out.eye = { xyz[0], xyz[1], xyz[2] };
				out.have_eye = true;
			}

			return true;
		}

		constexpr std::uintptr_t host_preferred_base = 0x00400000;
		constexpr int host_max_objects = 4096;
		constexpr int cam_plugin_cap = 64;
		constexpr int plugin_off_shown = 0x04;
		constexpr int plugin_off_render_start = 0x08;
		constexpr int plugin_off_active = 0x0C;
		constexpr int plugin_off_name = 0x424;
		constexpr int plugin_scan_bytes = 0x600;
		constexpr std::uint64_t k_cam_ident_tag = 0x43414D0000000000ull;

		struct host_layout
		{
			std::uintptr_t count_va;
			std::uintptr_t list_va;
			std::uintptr_t hmod_va;
			const char* tag;
		};

		constexpr host_layout editor_host_layout{
			0x00450460, 0x00454468, 0x0044AE58, "3DRad.exe" };
		constexpr host_layout player_host_layout{
			0x0044445C, 0x00448460, 0x0043EE58, "3drad_player" };

		HMODULE plugin_host_module = nullptr;
		const int* plugin_count_at = nullptr;
		void* const* plugin_list_at = nullptr;
		HMODULE const* plugin_hmod_at = nullptr;
		bool plugin_host_ok = false;
		int cached_plugin_host_count = -1;

		enum class cam_klass : int
		{
			none = 0,
			chase,
			first_person,
			generic
		};

		struct detected_camera
		{
			std::uint64_t identity = 0;
			int object_id = -1;
			cam_klass klass = cam_klass::none;
			const void* plugin = nullptr;
			const void* engine = nullptr;
			int shown = 0;
			int render_start = 0;
			int active = 0;
			int hidden = 1;
			int rendering = 0;
			float fov = 0.0f;
			D3DXVECTOR3 eye{};
			bool have_eye = false;
			char title[64]{};
		};

		detected_camera detected[cam_plugin_cap]{};
		int detected_n = 0;
		int detected_rendering_n = 0;
		std::uint64_t logged_pick_id = 0;
		int logged_pick_n = -1;
		int logged_pick_rend = -1;

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
				if ((prev == '\\' || prev == '/') && (next == '\\' || next == '/' || next == 0)) {
					return true;
				}
			}
			return false;
		}

		cam_klass klass_from_path(const char* path)
		{
			if (path_has_folder(path, "CamChase")) {
				return cam_klass::chase;
			}
			if (path_has_folder(path, "Cam1StPerson")) {
				return cam_klass::first_person;
			}
			if (path_has_folder(path, "Camera")) {
				return cam_klass::generic;
			}
			return cam_klass::none;
		}

		const char* klass_cstr(const cam_klass k)
		{
			switch (k)
			{
			case cam_klass::chase: return "CamChase";
			case cam_klass::first_person: return "Cam1StPerson";
			case cam_klass::generic: return "Camera";
			default: return "list";
			}
		}

		std::uint64_t camera_identity(const void* plugin)
		{
			const auto p = static_cast<std::uint32_t>(
				reinterpret_cast<std::uintptr_t>(plugin));
			return k_cam_ident_tag | (p ? p : 1u);
		}

		bool plugin_host_sane(const int* count_at, void* const* list_at,
			HMODULE const* hmod_at)
		{
			if (!memory_readable(count_at, sizeof(int))) {
				return false;
			}
			const int count = *count_at;
			if (count <= 0 || count > host_max_objects) {
				return false;
			}
			const auto bytes = static_cast<SIZE_T>(count) * sizeof(void*);
			return memory_readable(list_at, bytes) && memory_readable(hmod_at, bytes);
		}

		bool try_bind_plugin_host(HMODULE mod, const host_layout& layout)
		{
			if (!mod) {
				return false;
			}
			const auto rebase = [mod](const std::uintptr_t preferred)
				{
					return reinterpret_cast<std::uintptr_t>(mod) +
						(preferred - host_preferred_base);
				};
			const auto* count_at = reinterpret_cast<const int*>(rebase(layout.count_va));
			const auto* list_at = reinterpret_cast<void* const*>(rebase(layout.list_va));
			const auto* hmod_at = reinterpret_cast<HMODULE const*>(rebase(layout.hmod_va));
			if (!plugin_host_sane(count_at, list_at, hmod_at)) {
				return false;
			}
			plugin_host_module = mod;
			plugin_count_at = count_at;
			plugin_list_at = list_at;
			plugin_hmod_at = hmod_at;
			plugin_host_ok = true;
			return true;
		}

		bool bind_plugin_host()
		{
			if (plugin_host_ok)
			{
				if (plugin_host_sane(plugin_count_at, plugin_list_at, plugin_hmod_at)) {
					return true;
				}
				plugin_host_ok = false;
				plugin_count_at = nullptr;
				plugin_list_at = nullptr;
				plugin_hmod_at = nullptr;
			}

			HMODULE editor = GetModuleHandleA("3DRad.exe");
			if (!editor) {
				editor = GetModuleHandleA("3DRadRT.exe");
			}
			if (editor && try_bind_plugin_host(editor, editor_host_layout)) {
				return true;
			}
			const HMODULE self = GetModuleHandleA(nullptr);
			if (self && self != editor &&
				try_bind_plugin_host(self, player_host_layout))
			{
				return true;
			}
			return false;
		}

		const void* engine_cam_in_list(const void* p)
		{
			if (!p || !bind_camera_table() || !camera_list_at || !camera_count_at) {
				return nullptr;
			}
			const int n = *camera_count_at;
			void* const* list = *camera_list_at;
			if (!list || n <= 0 || n > 256 ||
				!memory_readable(list, static_cast<SIZE_T>(n) * sizeof(void*)))
			{
				return nullptr;
			}
			for (int i = 0; i < n; i++)
			{
				if (list[i] == p) {
					return p;
				}
			}
			return nullptr;
		}

		const void* engine_for_plugin(const std::uint8_t* plugin)
		{
			if (!plugin) {
				return nullptr;
			}
			if (const void* hit = engine_cam_in_list(plugin)) {
				return hit;
			}
			if (!memory_readable(plugin, static_cast<SIZE_T>(plugin_scan_bytes))) {
				return nullptr;
			}
			for (int off = 0; off + 4 <= plugin_scan_bytes; off += 4)
			{
				const void* p = *reinterpret_cast<const void* const*>(plugin + off);
				if (const void* hit = engine_cam_in_list(p)) {
					return hit;
				}
			}
			return nullptr;
		}

		void take_cam_title(char* dest, const char* src, const cam_klass k)
		{
			dest[0] = 0;
			if (src && src[0] && src[0] >= 32 && src[0] < 127)
			{
				std::strncpy(dest, src, 63);
				dest[63] = 0;
				return;
			}
			std::strncpy(dest, klass_cstr(k), 63);
			dest[63] = 0;
		}

		void reread_detected_flags()
		{
			detected_rendering_n = 0;
			for (int i = 0; i < detected_n; i++)
			{
				auto& rec = detected[i];
				const auto* plugin = static_cast<const std::uint8_t*>(rec.plugin);
				if (plugin && memory_readable(plugin + plugin_off_shown, 12))
				{
					rec.shown = *reinterpret_cast<const int*>(plugin + plugin_off_shown);
					rec.render_start = *reinterpret_cast<const int*>(plugin + plugin_off_render_start);
					rec.active = *reinterpret_cast<const int*>(plugin + plugin_off_active);
					if (rec.shown != 0 && rec.shown != 1) {
						rec.shown = 0;
					}
					if (rec.render_start != 0 && rec.render_start != 1) {
						rec.render_start = 0;
					}
					if (rec.active != 0 && rec.active != 1) {
						rec.active = 0;
					}
				}
				shown_camera eng{};
				int hidden = 1;
				if (rec.engine && read_camera_fields(rec.engine, eng, hidden, true))
				{
					rec.hidden = hidden;
					rec.fov = eng.fov;
					rec.eye = eng.eye;
					rec.have_eye = eng.have_eye;
				}
				else
				{
					rec.engine = engine_for_plugin(plugin);
					if (rec.engine && read_camera_fields(rec.engine, eng, hidden, true))
					{
						rec.hidden = hidden;
						rec.fov = eng.fov;
						rec.eye = eng.eye;
						rec.have_eye = eng.have_eye;
					}
					else {
						rec.hidden = 1;
					}
				}
				rec.rendering = rec.hidden == 0 ? 1 : 0;
				if (rec.rendering) {
					detected_rendering_n++;
				}
			}
		}

		void rescan_camera_plugins()
		{
			detected_n = 0;
			detected_rendering_n = 0;
			cached_plugin_host_count = -1;
			if (shared::globals::skip_remix || !bind_plugin_host() || !plugin_count_at) {
				return;
			}
			const int count = *plugin_count_at;
			if (count <= 0 || count > host_max_objects) {
				return;
			}
			cached_plugin_host_count = count;

			for (int i = 0; i < count && detected_n < cam_plugin_cap; i++)
			{
				const auto* host_obj = static_cast<const std::uint8_t*>(plugin_list_at[i]);
				const HMODULE plugin_mod = plugin_hmod_at[i];
				if (!host_obj || !plugin_mod || !memory_readable(host_obj, sizeof(void*))) {
					continue;
				}
				char path[MAX_PATH]{};
				if (!GetModuleFileNameA(plugin_mod, path, MAX_PATH) || !path[0]) {
					continue;
				}
				const cam_klass klass = klass_from_path(path);
				if (klass == cam_klass::none) {
					continue;
				}
				const auto* plugin = *reinterpret_cast<const std::uint8_t* const*>(host_obj);
				if (!plugin || !memory_readable(plugin, 0x10)) {
					continue;
				}

				auto& rec = detected[detected_n];
				rec = {};
				rec.identity = camera_identity(plugin);
				rec.object_id = i;
				rec.klass = klass;
				rec.plugin = plugin;
				rec.engine = engine_for_plugin(plugin);
				if (memory_readable(plugin + plugin_off_name, 8)) {
					take_cam_title(rec.title,
						reinterpret_cast<const char*>(plugin + plugin_off_name), klass);
				}
				else {
					take_cam_title(rec.title, nullptr, klass);
				}
				detected_n++;
			}
			reread_detected_flags();
		}

		void refresh_detected_cameras()
		{
			if (shared::globals::skip_remix) {
				detected_n = 0;
				detected_rendering_n = 0;
				return;
			}
			if (!bind_plugin_host() || !plugin_count_at) {
				detected_n = 0;
				detected_rendering_n = 0;
				return;
			}
			const int count = *plugin_count_at;
			if (detected_n > 0 && cached_plugin_host_count == count)
			{
				bool stale = false;
				for (int i = 0; i < detected_n; i++)
				{
					if (!detected[i].plugin ||
						!memory_readable(detected[i].plugin, 0x10))
					{
						stale = true;
						break;
					}
				}
				if (!stale)
				{
					reread_detected_flags();
					return;
				}
			}
			rescan_camera_plugins();
		}

		void drop_detected_cameras()
		{
			detected_n = 0;
			detected_rendering_n = 0;
			cached_plugin_host_count = -1;
			plugin_host_ok = false;
			plugin_host_module = nullptr;
			plugin_count_at = nullptr;
			plugin_list_at = nullptr;
			plugin_hmod_at = nullptr;
			logged_pick_id = 0;
			logged_pick_n = -1;
			logged_pick_rend = -1;
			cached_cam = {};
			last_good.handle = nullptr;
			last_good.fov = 0.0f;
			last_good.have_eye = false;
			frame_lock = {};
		}

		void remember_cache(const shown_camera& cam)
		{
			if (!cam.handle) {
				return;
			}

			cached_cam.handle = cam.handle;
			cached_cam.fov = cam.fov;
			cached_cam.eye = cam.eye;
			cached_cam.have_eye = cam.have_eye;
			cached_cam.probed = true;
		}

		void clear_cache_if(const void* handle)
		{
			if (!handle || cached_cam.handle == handle) {
				cached_cam = {};
			}
		}

		bool same_identity(const void* ha, const float fa, const void* hb, const float fb)
		{
			if (ha && hb) {
				return ha == hb;
			}
			const int ba = fov_band(fa);
			const int bb = fov_band(fb);
			return ba != 0 && ba == bb;
		}

		bool have_last_identity()
		{
			return last_good.handle || last_good.fov > 0.0f || last_good.have_eye;
		}

		const shown_camera* pick_shown(const shown_camera* shown, const int n,
			const void* prefer_handle, const float prefer_fov)
		{
			const shown_camera* handle_match = nullptr;
			const shown_camera* fov_match = nullptr;
			const shown_camera* first_scene = nullptr;
			const shown_camera* play_75 = nullptr;

			for (int i = 0; i < n; i++)
			{
				const int band = fov_band(shown[i].fov);
				if (band != 60 && band != 75) {
					continue;
				}

				if (!first_scene) {
					first_scene = &shown[i];
				}
				if (band == 75 && !play_75) {
					play_75 = &shown[i];
				}

				if (prefer_handle && shown[i].handle == prefer_handle && !handle_match)
				{
					handle_match = &shown[i];
				}

				if (!fov_match && prefer_fov > 0.0f && same_fov_band(shown[i].fov, prefer_fov)) {
					fov_match = &shown[i];
				}
			}

			// Compiled player: 60° is leftover editor fly-cam. 75° is
			// Cam 1StPerson / CamChase — prefer 75 even when 60 is still shown.
			// Editor Play is chosen in lock_identity via shown_play_75()
			// (+0x124==0). This list walk must not prefer 75° in the editor
			// or HUD overlay Present rebuilds Remix MAIN against orbit 60°.
			if (compiled_player && play_75)
			{
				if (handle_match && fov_band(handle_match->fov) == 75) {
					return handle_match;
				}
				return play_75;
			}

			if (handle_match) {
				return handle_match;
			}
			if (fov_match) {
				return fov_match;
			}
			return first_scene;
		}

		bool any_scene_vp_matches(const float fov)
		{
			if (!(fov > 0.0f)) {
				return false;
			}

			for (int i = 0; i < scene_stash_n; i++)
			{
				const auto& entry = scene_stash[i];
				if (entry.valid && entry.slot >= 0 && entry.slot < candidate_slots &&
					matches_locked_fov(proj_candidates[entry.slot], fov))
				{
					return true;
				}
			}

			return false;
		}

		float other_scene_vp_fov(const float locked_fov)
		{
			for (int i = 0; i < scene_stash_n; i++)
			{
				const auto& entry = scene_stash[i];
				if (!entry.valid || entry.slot < 0 || entry.slot >= candidate_slots) {
					continue;
				}

				const float vfov = vertical_fov_of(proj_candidates[entry.slot]);
				if (fov_band(vfov) != 0 && (locked_fov <= 0.0f || !same_fov_band(vfov, locked_fov))) {
					return vfov;
				}
			}

			return 0.0f;
		}

		void accept_identity(const shown_camera& cam, const char* source)
		{
			frame_lock.handle = cam.handle;
			frame_lock.fov = cam.fov;
			if (cam.have_eye) {
				adopt_eye(cam.eye, false);
			}
			else if (last_good.have_eye) {
				adopt_eye(last_good.eye, true);
			}

			frame_lock.identity_locked = true;
			frame_lock.identity_source = source;
			last_good.handle = cam.handle;
			if (cam.fov > 0.0f) {
				last_good.fov = cam.fov;
			}
			remember_cache(cam);
		}

		void reuse_last_identity(const char* source)
		{
			frame_lock.handle = last_good.handle;
			frame_lock.fov = last_good.fov;
			if (last_good.have_eye) {
				adopt_eye(last_good.eye, true);
			}

			frame_lock.identity_locked = true;
			frame_lock.identity_source = source;
		}

		bool last_handle_still_shown()
		{
			if (!last_good.handle) {
				return false;
			}

			shown_camera cam{};
			int hidden = 1;
			const bool probe = !cached_cam.probed || cached_cam.handle != last_good.handle;
			return read_camera_fields(last_good.handle, cam, hidden, probe) && hidden == 0;
		}

		bool observe_shown_candidate(shown_camera& out, const float scene_vp_fov)
		{
			shown_camera cached{};
			int hidden = 1;
			if (cached_cam.handle)
			{
				const bool probe = !cached_cam.probed;
				if (read_camera_fields(cached_cam.handle, cached, hidden, probe))
				{
					cached_cam.probed = true;
					cached_cam.fov = cached.fov;
					cached_cam.eye = cached.eye;
					cached_cam.have_eye = cached.have_eye;

					const bool shown = hidden == 0;
					const bool vp_mismatch = scene_vp_fov > 0.0f && fov_band(scene_vp_fov) != 0 &&
						!same_fov_band(cached.fov, scene_vp_fov);

					// Compiled: cache is enough. Editor lock_identity does not
					// use this path; a still-shown 60° orbit cam must not hide
					// a newly shown 75° Play camera if observe is called.
					if (shown && !vp_mismatch && compiled_player)
					{
						out = cached;
						return true;
					}

					if (!shown) {
						cached_cam = {};
					}
				}
				else {
					cached_cam = {};
				}
			}

			shown_camera shown[16]{};
			const int n = collect_shown_cameras(shown, 16);
			const shown_camera* pick = pick_shown(shown, n, last_good.handle, last_good.fov);

			if (compiled_player && scene_vp_fov > 0.0f && fov_band(scene_vp_fov) != 0)
			{
				const shown_camera* vp_match = pick_shown(shown, n, nullptr, scene_vp_fov);
				if (vp_match)
				{
					if (!(pick && pick->handle == last_good.handle &&
						same_fov_band(pick->fov, scene_vp_fov)))
					{
						pick = vp_match;
					}
				}
			}

			if (!pick && n > 0) {
				pick = &shown[0];
			}

			if (!pick) {
				return false;
			}

			out = *pick;
			remember_cache(out);
			return true;
		}

		void bind_proj_slot()
		{
			if (frame_lock.proj_slot >= 0) {
				return;
			}

			for (int p = 0; p < candidate_slots; p++)
			{
				if (is_scene_projection(proj_candidates[p]) &&
					matches_locked_fov(proj_candidates[p], frame_lock.fov))
				{
					frame_lock.proj_slot = p;
					break;
				}
			}
		}

		void refresh_locked_eye(const bool allow_rescan)
		{
			if (!shared::common::config::get().camera.world_space_position) {
				return;
			}

			const void* handle = frame_lock.handle ? frame_lock.handle : cached_cam.handle;
			if (handle)
			{
				shown_camera cam{};
				int hidden = 1;
				const bool probe = allow_rescan || !cached_cam.probed || cached_cam.handle != handle;
				if (read_camera_fields(handle, cam, hidden, probe))
				{
					if (handle == cached_cam.handle) {
						cached_cam.probed = true;
					}

					if (hidden == 0)
					{
						if (frame_lock.fov > 0.0f && !same_fov_band(cam.fov, frame_lock.fov))
						{
							clear_cache_if(handle);
						}
						else
						{
							if (!frame_lock.handle) {
								frame_lock.handle = handle;
							}
							adopt_eye(cam.eye, false);
							remember_cache(cam);
							return;
						}
					}
					else {
						clear_cache_if(handle);
					}
				}
				else {
					clear_cache_if(handle);
				}
			}

			if (!allow_rescan)
			{
				if (!frame_lock.have_eye && last_good.have_eye) {
					adopt_eye(last_good.eye, true);
				}
				return;
			}

			shown_camera shown[16]{};
			const int n = collect_shown_cameras(shown, 16);
			for (int i = 0; i < n; i++)
			{
				if (frame_lock.handle && shown[i].handle != frame_lock.handle) {
					continue;
				}
				if (frame_lock.fov > 0.0f && !same_fov_band(shown[i].fov, frame_lock.fov)) {
					continue;
				}
				if (shown[i].have_eye)
				{
					if (!frame_lock.handle) {
						frame_lock.handle = shown[i].handle;
					}
					adopt_eye(shown[i].eye, false);
					remember_cache(shown[i]);
					return;
				}
			}

			if (!frame_lock.have_eye && last_good.have_eye) {
				adopt_eye(last_good.eye, true);
			}
		}

		bool present_looks_like_hud_overlay()
		{
			// HUD/gizmo Present ~27–30 draws. Orbit/Play MAIN ~220.
			// draws==0 means identity locked before the first DIP — not overlay.
			// Overlay never blocks Play: lock_identity only uses this when
			// Cam1StPerson / CamChase is still hidden (+0x124!=0).
			const UINT draws = shared::common::ffp_state::get().draw_call_count();
			return draws >= 8u && draws < 48u;
		}

		const shown_camera* shown_play_75(const shown_camera* shown, const int n)
		{
			for (int i = 0; i < n; i++)
			{
				if (fov_band(shown[i].fov) == 75) {
					return &shown[i];
				}
			}
			return nullptr;
		}

		const shown_camera* shown_editor_60(const shown_camera* shown, const int n,
			const void* prefer_handle)
		{
			const shown_camera* first = nullptr;
			for (int i = 0; i < n; i++)
			{
				if (fov_band(shown[i].fov) != 60) {
					continue;
				}
				if (prefer_handle && shown[i].handle == prefer_handle) {
					return &shown[i];
				}
				if (!first) {
					first = &shown[i];
				}
			}
			return first;
		}

		const detected_camera* pick_rendering_camera()
		{
			if (detected_rendering_n <= 0) {
				return nullptr;
			}
			const float scene_fov = other_scene_vp_fov(0.0f);
			const float want = scene_fov > 0.0f ? scene_fov : last_good.fov;
			const detected_camera* vp_match = nullptr;
			const detected_camera* chase = nullptr;
			const detected_camera* first = nullptr;
			const detected_camera* any = nullptr;
			for (int i = 0; i < detected_n; i++)
			{
				if (!detected[i].rendering) {
					continue;
				}
				if (!any) {
					any = &detected[i];
				}
				if (detected[i].klass == cam_klass::chase && !chase) {
					chase = &detected[i];
				}
				if (detected[i].klass == cam_klass::first_person && !first) {
					first = &detected[i];
				}
				if (want > 0.0f && detected[i].fov > 0.0f &&
					same_fov_band(detected[i].fov, want) && !vp_match)
				{
					vp_match = &detected[i];
				}
			}
			if (detected_rendering_n > 1 && vp_match) {
				return vp_match;
			}
			if (chase) {
				return chase;
			}
			if (first) {
				return first;
			}
			return any;
		}

		void log_cam_pick(const detected_camera* rec, const char* fallback_name,
			const float fallback_fov, const int fallback_shown)
		{
			const std::uint64_t id = rec ? rec->identity : 0;
			if (id == logged_pick_id && detected_n == logged_pick_n &&
				detected_rendering_n == logged_pick_rend)
			{
				return;
			}
			logged_pick_id = id;
			logged_pick_n = detected_n;
			logged_pick_rend = detected_rendering_n;
			if (rec)
			{
				shared::common::log("Camera",
					std::format(
						"cam pick n={} rendering={} id={} name={} fov={:.2f} shown={}",
						detected_n, detected_rendering_n, rec->object_id,
						rec->title[0] ? rec->title : klass_cstr(rec->klass),
						rec->fov * 57.2957795f, rec->shown),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
			else
			{
				shared::common::log("Camera",
					std::format(
						"cam pick n={} rendering=0 id=- name={} fov={:.2f} shown={}",
						detected_n, fallback_name ? fallback_name : "list",
						fallback_fov * 57.2957795f, fallback_shown),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		}

		void lock_identity()
		{
			log_sim_edge();
			if (frame_lock.identity_locked)
			{
				refresh_locked_eye(false);
				return;
			}

			refresh_detected_cameras();

			shown_camera observed{};
			bool have_obs = false;
			const bool have_last = have_last_identity();
			const detected_camera* rend = pick_rendering_camera();

			if (rend && rend->engine)
			{
				int hidden = 1;
				if (read_camera_fields(rend->engine, observed, hidden, true) && hidden == 0)
				{
					have_obs = true;
					log_cam_pick(rend, nullptr, 0.0f, 0);
				}
			}

			if (!have_obs)
			{
				shown_camera shown[16]{};
				const int n = collect_shown_cameras(shown, 16);
				const shown_camera* editor60 = shown_editor_60(shown, n, last_good.handle);
				const shown_camera* pick = pick_shown(shown, n, last_good.handle, last_good.fov);
				if (editor60)
				{
					observed = *editor60;
					have_obs = true;
					log_cam_pick(nullptr, "list", observed.fov, 1);
				}
				else if (pick)
				{
					observed = *pick;
					have_obs = true;
					log_cam_pick(nullptr, "list", observed.fov, 1);
				}
			}

			if (have_obs)
			{
				const bool same = have_last &&
					same_identity(observed.handle, observed.fov, last_good.handle, last_good.fov);

				if (!have_last || same)
				{
					accept_identity(observed, have_last ? "shown" : "first-shown");
				}
				else
				{
					accept_identity(observed, rend ? "cam-plugin" : "editor-cam");
					frame_lock.switched = true;
				}
			}
			else if (have_last)
			{
				reuse_last_identity("list-miss");
			}
			else {
				return;
			}

			if (!frame_lock.have_eye && last_good.have_eye) {
				adopt_eye(last_good.eye, true);
			}

			refresh_locked_eye(true);
			bind_proj_slot();
		}

		void stash_scene_vp(const D3DXMATRIX& view, const D3DXMATRIX& proj,
			const D3DXMATRIX& view_proj, const int slot)
		{
			if (slot < 0 || slot >= candidate_slots || !is_scene_projection(proj_candidates[slot])) {
				return;
			}

			for (int i = 0; i < scene_stash_n; i++)
			{
				if (scene_stash[i].slot == slot)
				{
					scene_stash[i].view = view;
					scene_stash[i].proj = proj;
					scene_stash[i].view_proj = view_proj;
					scene_stash[i].valid = true;
					return;
				}
			}

			if (scene_stash_n >= stash_slots) {
				return;
			}

			auto& entry = scene_stash[scene_stash_n++];
			entry.view = view;
			entry.proj = proj;
			entry.view_proj = view_proj;
			entry.slot = slot;
			entry.valid = true;
		}

		void adopt_identity_for_slot(const int slot)
		{
			// First-session only. Within-Present lock owns later frames so a
			// leftover 60/75 VP cannot steal identity mid-frame.
			if (frame_lock.identity_locked || have_last_identity()) {
				return;
			}

			if (slot < 0 || slot >= candidate_slots || !proj_candidates[slot].valid) {
				return;
			}

			const float vfov = vertical_fov_of(proj_candidates[slot]);
			shown_camera shown[16]{};
			const int n = collect_shown_cameras(shown, 16);
			const shown_camera* pick = nullptr;
			for (int i = 0; i < n; i++)
			{
				if (same_fov_band(shown[i].fov, vfov))
				{
					pick = &shown[i];
					break;
				}
			}

			if (!pick) {
				return;
			}

			frame_lock.handle = pick->handle;
			frame_lock.fov = pick->fov;
			if (pick->have_eye) {
				adopt_eye(pick->eye, false);
			}
			else if (last_good.have_eye) {
				adopt_eye(last_good.eye, true);
			}
			frame_lock.identity_locked = true;
			frame_lock.identity_source = "first-vp";
			remember_cache(*pick);
		}

		bool view_is_unrotated_identity(const D3DXMATRIX& view)
		{
			auto close1 = [](const float v)
				{
					return std::fabs(v - 1.0f) < 0.02f;
				};
			auto close0 = [](const float v)
				{
					return std::fabs(v) < 0.02f;
				};
			return close1(view._11) && close0(view._12) && close0(view._13) &&
				close0(view._21) && close1(view._22) && close0(view._23) &&
				close0(view._31) && close0(view._32) && close1(view._33);
		}

		bool try_lock_matrices(const D3DXMATRIX& view, const D3DXMATRIX& proj, const int slot)
		{
			if (frame_lock.matrices_locked && frame_lock.proj_slot == slot &&
				same_matrix(frame_lock.view, view) && same_matrix(frame_lock.proj, proj))
			{
				return true;
			}

			if (compiled_player && view_is_unrotated_identity(view) &&
				!frame_lock.matrices_locked && !last_good.have_matrices)
			{
				// Loading / default PerspectiveFov with identity View. LookAt
				// disagrees (error 2). Must not become MAIN or lock the 60°
				// fly-cam — wait for the real Cam 1StPerson concat.
				return false;
			}

			if (!conversion_allowed() && !try_enter_live_from_scene_vp(slot)) {
				return false;
			}

			if (!frame_lock.identity_locked) {
				lock_identity();
			}

			if (slot < 0 || slot >= candidate_slots || !proj_candidates[slot].valid) {
				return frame_lock.matrices_locked;
			}

			const auto& candidate = proj_candidates[slot];
			if (!is_scene_projection(candidate)) {
				return frame_lock.matrices_locked;
			}

			const float want_fov = frame_lock.identity_locked ? frame_lock.fov :
				(last_good.fov > 0.0f ? last_good.fov : 0.0f);
			if (want_fov > 0.0f && !matches_locked_fov(candidate, want_fov)) {
				return frame_lock.matrices_locked;
			}

			if (!frame_lock.identity_locked)
			{
				frame_lock.fov = vertical_fov_of(candidate);
				frame_lock.identity_locked = true;
				if (last_good.have_eye) {
					adopt_eye(last_good.eye, true);
				}
			}

			// Identity stays. View/P refresh so a moving camera still reaches Remix.
			frame_lock.view = view;
			frame_lock.proj = proj;
			frame_lock.proj_slot = slot;
			frame_lock.matrices_locked = true;
			frame_lock.view_updates++;
			matched_proj = slot;

			dominant_view = view;
			dominant_proj = proj;
			have_dominant = true;

			last_good.view = view;
			last_good.proj = proj;
			last_good.have_matrices = true;
			last_good.handle = frame_lock.handle;
			last_good.fov = frame_lock.fov;
			refresh_locked_eye(false);
			publish_if_changed("early-vp");
			return true;
		}

		void try_lock_from_stash()
		{
			if (!frame_lock.identity_locked) {
				lock_identity();
			}

			if (frame_lock.identity_locked)
			{
				for (int i = 0; i < scene_stash_n; i++)
				{
					const auto& entry = scene_stash[i];
					if (!entry.valid ||
						!matches_locked_fov(proj_candidates[entry.slot], frame_lock.fov))
					{
						continue;
					}

					if (try_lock_matrices(entry.view, entry.proj, entry.slot))
					{
						dominant_view_proj = entry.view_proj;
					}
				}

				if (frame_lock.matrices_locked) {
					return;
				}
			}

			if (have_last_identity()) {
				return;
			}

			for (int i = 0; i < scene_stash_n; i++)
			{
				const auto& entry = scene_stash[i];
				if (!entry.valid) {
					continue;
				}

				adopt_identity_for_slot(entry.slot);
				if (try_lock_matrices(entry.view, entry.proj, entry.slot))
				{
					dominant_view_proj = entry.view_proj;
					if (frame_lock.identity_locked) {
						return;
					}
				}
			}
		}

		bool current_eye(D3DXVECTOR3& eye)
		{
			if (!shared::common::config::get().camera.world_space_position) {
				return false;
			}

			if (frame_lock.have_eye)
			{
				eye = frame_lock.eye;
				return true;
			}

			if (last_good.have_eye)
			{
				adopt_eye(last_good.eye, true);
				eye = last_good.eye;
				return true;
			}

			return false;
		}

		bool compose_locked_camera(D3DMATRIX& view, D3DMATRIX& proj, const bool prepare)
		{
			if (prepare)
			{
				lock_identity();
				refresh_locked_eye(true);
			}

			if (frame_lock.matrices_locked)
			{
				view = frame_lock.view;
				proj = frame_lock.proj;
			}
			else if (last_good.have_matrices)
			{
				// List read or FOV match missed this frame (common while the
				// camera is moving). Still publish last-good View/P so Remix
				// MAIN does not go empty.
				view = last_good.view;
				proj = last_good.proj;
				if (!frame_lock.skip_reason) {
					frame_lock.skip_reason = "reused last-good View/P";
				}
			}
			else {
				frame_lock.skip_reason = "no scene View/P recovered yet";
				return false;
			}

			D3DXVECTOR3 eye{};
			if (current_eye(eye)) {
				apply_world_space_view(view, eye);
			}

			return true;
		}

		bool proj_params_for_inject(float& fov_deg, float& aspect, float& z_near, float& z_far)
		{
			const int slot = frame_lock.proj_slot;
			if (slot >= 0 && slot < candidate_slots && proj_candidates[slot].valid)
			{
				const auto& candidate = proj_candidates[slot];
				fov_deg = vertical_fov_of(candidate) * 57.2957795f;
				aspect = candidate.aspect;
				z_near = candidate.z_near;
				z_far = candidate.z_far;
				return std::isfinite(fov_deg) && fov_deg > 1.0f &&
					std::isfinite(aspect) && aspect > 0.05f &&
					std::isfinite(z_near) && z_near > 0.0f &&
					std::isfinite(z_far) && z_far > z_near;
			}

			if (frame_lock.fov > 0.0f && viewport_aspect > 0.05f)
			{
				fov_deg = frame_lock.fov * 57.2957795f;
				aspect = viewport_aspect;
				z_near = 0.1f;
				z_far = 10000.0f;
				return true;
			}

			return false;
		}

		void publish_if_changed(const char* source)
		{
			if (!conversion_allowed()) {
				return;
			}

			D3DMATRIX view{};
			D3DMATRIX proj{};
			if (!compose_locked_camera(view, proj, false)) {
				return;
			}

			static const D3DMATRIX identity{
				1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
			if (same_matrix(view, identity) && same_matrix(proj, identity)) {
				return;
			}

			const D3DXVECTOR3* eye_ptr = frame_lock.have_eye ? &frame_lock.eye :
				(last_good.have_eye ? &last_good.eye : nullptr);
			const uint32_t hash = hash_published_camera(view, proj, eye_ptr);
			if (have_published && hash == published_hash) {
				return;
			}

			auto& ffp = shared::common::ffp_state::get();
			ffp.set_camera(view, proj);
			if (IDirect3DDevice9* dev = shared::globals::d3d_device) {
				ffp.apply_pending_camera(dev);
			}

			float fov_deg = 0.0f, aspect = 0.0f, z_near = 0.0f, z_far = 0.0f;
			const bool have_params = proj_params_for_inject(fov_deg, aspect, z_near, z_far);
			const float* eye_xyz = eye_ptr ? &eye_ptr->x : nullptr;
			if (shared::common::remix_api::setup_world_camera(view, proj, eye_xyz,
				have_params, fov_deg, aspect, z_near, z_far))
			{
				frame_lock.api_inject = true;
			}

			published_hash = hash;
			have_published = true;
			frame_lock.published++;
			if (!frame_lock.publish_source) {
				frame_lock.publish_source = source;
			}
		}

		void apply_world_space_view(D3DMATRIX& view, const D3DXVECTOR3& eye)
		{
			// D3DXMatrixLookAtLH stores -dot(axis, eye) in the translation row.
			view._41 = -(view._11 * eye.x + view._21 * eye.y + view._31 * eye.z);
			view._42 = -(view._12 * eye.x + view._22 * eye.y + view._32 * eye.z);
			view._43 = -(view._13 * eye.x + view._23 * eye.y + view._33 * eye.z);
		}

		void apply_world_space_world(D3DMATRIX& world, const D3DXVECTOR3& eye)
		{
			// world * Translate(eye) for a D3DX row-major affine matrix is just
			// translation += w44 * eye. Avoids two copies + D3DXMatrixTranslation.
			world._41 += world._44 * eye.x;
			world._42 += world._44 * eye.y;
			world._43 += world._44 * eye.z;
		}

		// Vertex shader constants hold the transpose of a matrix, because the HLSL
		// transforms a row vector: mul(position, matrix).
		void matrix_from_constants(const float* src, D3DXMATRIX& dst)
		{
			for (int row = 0; row < 4; row++)
			{
				for (int col = 0; col < 4; col++) {
					dst.m[row][col] = src[col * 4 + row];
				}
			}
		}

		/*
		 * Reads the register index of a named float4 constant out of a compiled
		 * shader's embedded constant table (the "CTAB" comment block).
		 *
		 * 3Impact compiles one effect per material/light-count combination, and the
		 * register mxViewProj lands on moves between variants — c0 for rigid meshes,
		 * c78 and up for skinned ones, where a 26-bone palette occupies c0-c77. Asking
		 * the shader itself is the only way to stay correct across all of them.
		 */
		int parse_constant_register(const std::vector<BYTE>& bytecode, const char* name,
			int* out_count = nullptr)
		{
			constexpr DWORD ctab_fourcc = 0x42415443; // 'CTAB'
			constexpr size_t table_size = 28;
			constexpr size_t record_size = 20;
			constexpr WORD register_set_float4 = 2;

			if (bytecode.size() < sizeof(DWORD) + table_size) {
				return -1;
			}

			const auto* bytes = bytecode.data();
			const size_t limit = bytecode.size();

			for (size_t at = 0; at + sizeof(DWORD) + table_size <= limit; at += sizeof(DWORD))
			{
				if (*reinterpret_cast<const DWORD*>(bytes + at) != ctab_fourcc) {
					continue;
				}

				const size_t table = at + sizeof(DWORD);
				const auto* fields = reinterpret_cast<const DWORD*>(bytes + table);

				if (fields[0] != table_size) {
					continue;
				}

				const DWORD count = fields[3];
				const DWORD info_offset = fields[4];

				if (count == 0 || count > 4096) {
					continue;
				}

				for (DWORD i = 0; i < count; i++)
				{
					const size_t record = table + info_offset + i * record_size;
					if (record + record_size > limit) {
						break;
					}

					const DWORD name_offset = *reinterpret_cast<const DWORD*>(bytes + record);
					const WORD register_set = *reinterpret_cast<const WORD*>(bytes + record + 4);
					const WORD register_index = *reinterpret_cast<const WORD*>(bytes + record + 6);
					const WORD register_count = *reinterpret_cast<const WORD*>(bytes + record + 8);

					const size_t name_at = table + name_offset;
					if (name_at >= limit || register_set != register_set_float4) {
						continue;
					}

					const auto* candidate = reinterpret_cast<const char*>(bytes + name_at);
					const size_t available = limit - name_at;
					if (strnlen(candidate, available) < available &&
						std::strcmp(candidate, name) == 0)
					{
						if (out_count) {
							*out_count = static_cast<int>(register_count);
						}
						return static_cast<int>(register_index);
					}
				}
			}

			return -1;
		}

		/*
		 * Register lookups are cached per shader: parsing a constant table means
		 * copying and scanning the whole bytecode, which is far too slow per draw.
		 *
		 * Keying on the interface pointer can go stale if the engine frees an effect
		 * and a new shader lands on the same address. That resolves itself safely —
		 * a wrong register yields a non-affine world matrix, which resolve_world()
		 * rejects, and the draw keeps its original shaders.
		 */
		struct shader_registers
		{
			int view_proj = -1;
			int world = -1;
			int world_count = 0;
		};

		const shader_registers& registers_for(IDirect3DVertexShader9* shader)
		{
			static std::unordered_map<IDirect3DVertexShader9*, shader_registers> cache;

			if (const auto known = cache.find(shader); known != cache.end()) {
				return known->second;
			}

			shader_registers found{};

			UINT size = 0;
			if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size > 0)
			{
				std::vector<BYTE> bytecode(size);
				if (SUCCEEDED(shader->GetFunction(bytecode.data(), &size)))
				{
					found.view_proj = parse_constant_register(bytecode, "mxViewProj");
					found.world = parse_constant_register(bytecode, "mxWorld", &found.world_count);
				}
			}

			return cache.emplace(shader, found).first->second;
		}

		/*
		 * Reads a matrix out of a constant block that may be allocated short.
		 *
		 * mxWorld is declared float4x4 but the compiler gives it three registers in
		 * some variants, because a world matrix's fourth column is always (0,0,0,1)
		 * and no shader reads it. Each register holds one column, so the missing one
		 * is exactly that constant column.
		 */
		void read_matrix(const float* src, const int registers, D3DXMATRIX& dst)
		{
			D3DXMatrixIdentity(&dst);

			const int columns = std::min(registers, 4);
			for (int col = 0; col < columns; col++)
			{
				for (int row = 0; row < 4; row++) {
					dst.m[row][col] = src[col * 4 + row];
				}
			}
		}

		/*
		 * The engine's two camera builders. Their arguments state the camera outright —
		 * an eye point, a field of view, a near and a far plane — so capturing here
		 * yields the exact matrices the engine uses.
		 *
		 * This replaced an attempt to recover View and Projection algebraically from
		 * their product. That approach cannot work: the far plane is unrecoverable,
		 * because q = zf / (zf - zn) sits a rounding error away from 1.0 for any
		 * distant far plane, and the shape test it relied on accepts a World *
		 * ViewProjection just as readily as a ViewProjection, so an object's transform
		 * could be mistaken for the camera. See kb.h.
		 */
		D3DXMATRIX* WINAPI look_at_stub(D3DXMATRIX* out, const D3DXVECTOR3* eye,
			const D3DXVECTOR3* at, const D3DXVECTOR3* up)
		{
			auto* result = engine_look_at(out, eye, at, up);

			/*
			 * Not a camera source, only evidence. Every call so far arrives with the
			 * eye at the origin, which is how 3Impact renders: the camera stays put
			 * and its offset is folded into each object's world matrix instead. The
			 * target and up vector are logged too, since they are where any remaining
			 * clue to the true camera position would have to be.
			 */
			if (eye && at)
			{
				const D3DXVECTOR3 direction{ at->x - eye->x, at->y - eye->y, at->z - eye->z };
				const float length = std::sqrt(direction.x * direction.x +
					direction.y * direction.y + direction.z * direction.z);

				if (length > 1e-4f && std::isfinite(length))
				{
					camera_forwards[forward_next] = { direction.x / length,
						direction.y / length, direction.z / length };
					forward_next = (forward_next + 1) % forward_slots;
				}
			}

			static int logged = 0;
			if (logged < 4 && eye && at && up)
			{
				logged++;
				shared::common::log("Camera", std::format(
					"LookAtLH {}: eye=({:.3f}, {:.3f}, {:.3f}) at=({:.3f}, {:.3f}, {:.3f}) "
					"up=({:.2f}, {:.2f}, {:.2f})", logged,
					eye->x, eye->y, eye->z, at->x, at->y, at->z, up->x, up->y, up->z),
					shared::common::LOG_TYPE::LOG_TYPE_GREEN);
				maybe_focus_compiled_play_window();
			}
			return result;
		}

		D3DXMATRIX* WINAPI perspective_stub(D3DXMATRIX* out, FLOAT fov, FLOAT aspect, FLOAT zn, FLOAT zf)
		{
			auto* result = engine_perspective(out, fov, aspect, zn, zf);

			if (out && all_finite(*out))
			{
				/*
				 * Only distinct matrices take a slot.
				 *
				 * Without this the array is useless: the engine builds one projection
				 * far more often than the camera's, so a plain ring buffer ends up
				 * holding sixteen copies of the same near-miss and the camera's own
				 * projection is long gone by the time a draw asks for it. That is
				 * exactly what defeated the first attempt at this.
				 */
				bool known = false;
				for (const auto& candidate : proj_candidates)
				{
					if (candidate.valid && nearly_equal(candidate.matrix, *out))
					{
						known = true;
						break;
					}
				}

				if (!known)
				{
					auto& slot = proj_candidates[proj_next];
					proj_next = (proj_next + 1) % candidate_slots;

					slot.matrix = *out;
					slot.fov = fov;
					slot.aspect = aspect;
					slot.z_near = zn;
					slot.z_far = zf;

					// Inverting the projection is safe precisely because it is exact
					// and its structure is fixed, unlike the product it appears in.
					slot.valid = D3DXMatrixInverse(&slot.inverse, nullptr, out) != nullptr &&
						all_finite(slot.inverse);

					shared::common::log("Camera", std::format(
						"PerspectiveFovLH -> slot {}: fov={:.2f}deg aspect={:.4f} zn={:.3f} zf={:.1f} "
						"w={:.5f} h={:.5f}",
						proj_next - 1, fov * 57.2957795f, aspect, zn, zf,
						slot.matrix._11, slot.matrix._22),
						shared::common::LOG_TYPE::LOG_TYPE_GREEN);
				}
			}
			return result;
		}

		D3DXMATRIX* WINAPI ortho_stub(D3DXMATRIX* out, FLOAT w, FLOAT h, FLOAT zn, FLOAT zf)
		{
			editor_settings::adjust_ortho(w, h);
			if (engine_ortho) {
				return engine_ortho(out, w, h, zn, zf);
			}
			return D3DXMatrixOrthoLH(out, w, h, zn, zf);
		}

		/*
		 * Settles whether the engine really hands the same ViewProjection to every
		 * object in a frame, or a different one each time. The distinction decided
		 * whether capturing the camera here was viable at all, so the evidence is
		 * reported rather than assumed.
		 */
		void log_frame_stats(const D3DXMATRIX& combined, const bool identified)
		{
			// Startup evidence only. The Present line is the session heartbeat;
			// nearly_equal on every concatenation was the cost of the old one.
			static UINT last_frame = 0xFFFFFFFF;
			static D3DXMATRIX first_of_frame{};
			static int calls = 0;
			static int distinct = 0;
			static int identified_count = 0;
			static int matches_first = 0;
			static int frames_logged = 0;

			if (frames_logged >= 8) {
				return;
			}

			const UINT frame = shared::common::ffp_state::get().frame_count();

			if (frame != last_frame)
			{
				if (calls > 0)
				{
					frames_logged++;
					shared::common::log("Camera", std::format(
						"Frame {}: {} calls of {} accepted, across {} distinct ViewProjection(s); "
						"{} shared the frame's first.",
						last_frame, identified_count, calls, distinct, matches_first));
				}

				if (calls > 0 && matches_first * 2 > calls)
				{
					dominant_view_proj = first_of_frame;
					have_dominant = true;
				}

				last_frame = frame;
				calls = 0;
				distinct = 0;
				identified_count = 0;
				matches_first = 0;
				first_of_frame = combined;
			}

			calls++;
			if (identified) identified_count++;

			if (same_matrix(combined, first_of_frame)) {
				matches_first++;
			}
			else {
				distinct++;
			}

			if (calls == 1) distinct++;
		}

		D3DXMATRIX* WINAPI multiply_transpose_stub(D3DXMATRIX* out, const D3DXMATRIX* m1, const D3DXMATRIX* m2)
		{
			auto* result = engine_multiply_transpose(out, m1, m2);

			// m2 is the ViewProjection the shaders will receive. It is not used as the
			// camera itself, only to say which captured View and Projection built it —
			// a positive identification that a shadow or reflection matrix cannot pass.
			if (m2)
			{
				D3DXMATRIX view{};
				int p = -1;
				bool recovered = false;
				bool scene = false;

				if (last_vp.valid && same_matrix(last_vp.m2, *m2))
				{
					recovered = true;
					view = last_vp.view;
					p = last_vp.slot;
					scene = last_vp.scene;
				}
				else
				{
					recovered = recover_view(*m2, view, p);
					last_vp.m2 = *m2;
					last_vp.view = view;
					last_vp.slot = p;
					last_vp.valid = recovered;
					last_vp.scene = recovered && p >= 0 && p < candidate_slots &&
						is_scene_projection(proj_candidates[p]);
					scene = last_vp.scene;
					log_frame_stats(*m2, recovered);
				}

				if (recovered && p >= 0 && p < candidate_slots)
				{
					// recover_view already proved this VP is a rigid view * known P.
					// Do not invert the product: algebraic W=WVP*inv(VP) stays off,
					// and P's inverse is cached when the projection is first seen.
					matched_proj = p;

					const auto& proj = proj_candidates[p];

					static bool reported = false;
					if (!reported)
					{
						reported = true;
						shared::common::log("Camera", std::format(
							"Camera recovered: fov={:.2f}deg aspect={:.4f} zn={:.3f} zf={:.1f}, "
							"view translation=({:.3f}, {:.3f}, {:.3f})",
							proj.fov * 57.2957795f, proj.aspect, proj.z_near, proj.z_far,
							view._41, view._42, view._43),
							shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);

						constexpr auto green = shared::common::LOG_TYPE::LOG_TYPE_GREEN;
						dump_matrix("  ViewProj", *m2, green);
						dump_matrix("  Proj    ", proj.matrix, green);
						dump_matrix("  invProj ", proj.inverse, green);
						dump_matrix("  View    ", view, green);

						float axis_error = 0.0f;
						const bool corroborated = forward_axis_corroborated(view, axis_error);
						shared::common::log("Camera", std::format(
							"Forward axis ({:.4f}, {:.4f}, {:.4f}) vs the engine's LookAtLH "
							"aim: {} (error {:.5f}).", view._13, view._23, view._33,
							corroborated ? "agrees" : "NO MATCH", axis_error),
							corroborated ? green : shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
					}

					auto& record = records[record_next];
					record_next = (record_next + 1) % record_slots;

					D3DXMatrixTranspose(&record.constant, out);
					record.view = view;
					record.proj = proj.matrix;
					record.proj_index = p;
					static const D3DXMATRIX identity{
						1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
					record.world = m1 ? *m1 : identity;
					record.valid = all_finite(record.constant) && all_finite(record.world);

					if (!scene)
					{
						frame_lock.skipped_non_scene++;
					}
					else
					{
						stash_scene_vp(view, proj.matrix, *m2, p);
						try_lock_matrices(view, proj.matrix, p);
					}

					static UINT last_reported = 0xFFFFFFFF;
					static int reports = 0;
					if (reports < 6)
					{
						const UINT frame = shared::common::ffp_state::get().frame_count();
						if (frame != last_reported)
						{
							last_reported = frame;
							reports++;
							float axis_error = 0.0f;
							const bool corroborated = forward_axis_corroborated(view, axis_error);
							shared::common::log("Camera", std::format(
								"Frame {} camera: slot {} fov={:.2f}deg zf={:.1f} forward=({:.3f}, "
								"{:.3f}, {:.3f}) LookAt {} ({:.5f}).",
								frame, p, proj.fov * 57.2957795f, proj.z_far,
								view._13, view._23, view._33,
								corroborated ? "agrees" : "DISAGREES", axis_error));
						}
					}
				}
				else
				{
					static bool warned = false;
					if (!warned && viewport_aspect > 0.0f)
					{
						warned = true;
						shared::common::log("Camera",
							"No projection reproduces the engine's ViewProjection as a rigid view - "
							"camera not applied. FFP draws will keep the game's shaders.",
							shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
						report_rejection(*m2);
					}
				}
			}

			return result;
		}
	}

	/*
	 * Finds the camera and world transform belonging to one specific draw.
	 *
	 * The draw is identified by the value in its own mxViewProj constant, which the
	 * engine built in the hook and which is recorded there alongside the parts it
	 * was made from. So this is an exact lookup rather than an inference, and a
	 * frame containing several cameras is an ordinary case: each draw finds its own.
	 */
	bool resolve_draw(IDirect3DVertexShader9* shader, const float* vs_constants,
		D3DMATRIX& view, D3DMATRIX& proj, D3DMATRIX& world)
	{
		if (!conversion_allowed()) {
			return false;
		}

		if (!shader || !vs_constants) {
			return false;
		}

		const auto& regs = registers_for(shader);
		if (regs.view_proj < 0 || regs.view_proj + 4 > 256 || !have_dominant) {
			return false;
		}

		// What this draw will actually be transformed by: World * View * Projection,
		// despite the uniform's name (see kb.h).
		D3DXMATRIX wvp{};
		matrix_from_constants(&vs_constants[regs.view_proj * 4], wvp);

		if (!all_finite(wvp)) {
			return false;
		}

		// The draws whose concatenation the hook witnessed. This is the only path
		// observed stable, and it is what ships.
		for (const auto& record : records)
		{
			if (record.valid && nearly_equal(record.constant, wvp))
			{
				world = record.world;

				if (!frame_lock.matrices_locked)
				{
					if (!frame_lock.identity_locked) {
						lock_identity();
					}

					if (record.proj_index >= 0 && record.proj_index < candidate_slots &&
						is_scene_projection(proj_candidates[record.proj_index]) &&
						(!frame_lock.identity_locked ||
							matches_locked_fov(proj_candidates[record.proj_index], frame_lock.fov)))
					{
						try_lock_matrices(record.view, record.proj, record.proj_index);
					}
				}

				if (frame_lock.matrices_locked)
				{
					view = frame_lock.view;
					proj = frame_lock.proj;
				}
				else if (last_good.have_matrices)
				{
					view = last_good.view;
					proj = last_good.proj;
				}
				else
				{
					view = record.view;
					proj = record.proj;
				}

				D3DXVECTOR3 eye{};
				if (current_eye(eye))
				{
					apply_world_space_view(view, eye);
					apply_world_space_world(world, eye);
				}

				return true;
			}
		}

		/*
		 * Off by default. Reading mxWorld is better founded than dividing the
		 * ViewProjection out — no inversion, and validated by a forward multiply —
		 * but both take the Remix runtime down once coverage rises from a handful of
		 * draws to most of them. Since the two obtain the world by entirely
		 * different means, the fault is very likely in the FFP draw path rather than
		 * in the transforms, and that has to be bisected before either is enabled.
		 */
		static volatile bool enable_constant_world = false;
		if (!enable_constant_world) {
			return false;
		}

		/*
		 * The world comes from the engine's own mxWorld uniform, read straight out of
		 * the constants. Nothing is inverted and nothing is divided, so there is no
		 * ill-conditioned step and no way to manufacture a plausible-looking but
		 * wrong transform.
		 *
		 * A shader without mxWorld is a skinned one, where the bone palette has
		 * already moved vertices into world space. Identity is then the right answer,
		 * not a failure.
		 */
		D3DXMATRIX candidate{};
		if (regs.world >= 0 && regs.world + regs.world_count <= 256 && regs.world_count >= 3) {
			read_matrix(&vs_constants[regs.world * 4], regs.world_count, candidate);
		}
		else {
			D3DXMatrixIdentity(&candidate);
		}

		if (!all_finite(candidate)) {
			return false;
		}

		/*
		 * Proof that this draw belongs to the frame's camera, and that the constants
		 * read are this draw's rather than a previous one's: multiplying the world by
		 * the camera's ViewProjection has to reproduce what the shader was given.
		 *
		 * This is the discriminating per-draw test that was missing. Recovering the
		 * world by dividing the ViewProjection out and checking the result was affine
		 * accepted matrices belonging to other cameras, and crashed the Remix runtime
		 * twice. A forward multiply cannot: there is nothing to solve for, only an
		 * identity to confirm.
		 */
		D3DXMATRIX expected{};
		D3DXMatrixMultiply(&expected, &candidate, &dominant_view_proj);

		if (!nearly_equal(expected, wvp)) {
			return false;
		}

		// Checked once against a draw whose concatenation was recorded in the hook,
		// so the constant-read world is confirmed against the world the engine
		// itself passed in before the path is trusted for the draws with no record.
		static bool reported = false;
		if (!reported)
		{
			for (const auto& record : records)
			{
				if (!record.valid || !nearly_equal(record.constant, wvp)) {
					continue;
				}

				reported = true;
				const bool agrees = nearly_equal(record.world, candidate);
				shared::common::log("Camera", std::format(
					"mxWorld read vs the engine's own world argument: {}. Translation "
					"({:.3f}, {:.3f}, {:.3f}) against ({:.3f}, {:.3f}, {:.3f}).",
					agrees ? "agrees" : "DISAGREES",
					candidate._41, candidate._42, candidate._43,
					record.world._41, record.world._42, record.world._43),
					agrees ? shared::common::LOG_TYPE::LOG_TYPE_GREEN
						: shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				break;
			}
		}

		view = dominant_view;
		proj = dominant_proj;
		world = candidate;

		if (frame_lock.matrices_locked)
		{
			view = frame_lock.view;
			proj = frame_lock.proj;
		}

		D3DXVECTOR3 eye{};
		if (current_eye(eye))
		{
			apply_world_space_view(view, eye);
			apply_world_space_world(world, eye);
		}

		return true;
	}

	void on_begin_scene()
	{
		// Remix first-write-wins MAIN. The editor issues swapchain SetViewport
		// 827×620 *first* each frame, then the 3D RT 1332×614. Publishing
		// last-good here lands our 3D camera before 827. Stale by 1 frame is
		// better than flipping. Do not InitializeLibrary during picker /
		// windowed-boot: exposing the API and then destroying that device
		// (compiled player CreateDevice #2) access-violated NvRemixBridge.
		if (conversion_allowed()) {
			maybe_relaunch_compiled_first_boot("BeginScene live");
			if (remix_api_init_allowed()) {
				shared::common::remix_api::try_initialize("BeginScene");
			}
			if (last_good.have_matrices) {
				publish_if_changed("BeginScene last-good");
			}
			lights::on_frame();
			particles::on_frame();
			fog::on_frame();
			maybe_relaunch_editor_first_boot("BeginScene editor");
		}
	}

	void note_non_scene_skip()
	{
		frame_lock.skipped_non_scene++;
	}

	bool remix_camera(D3DMATRIX& view, D3DMATRIX& proj)
	{
		return compose_locked_camera(view, proj, true);
	}

	void log_present_stats(const char* line)
	{
		static UINT last_frame = 0;
		static init_phase last_phase = init_phase::editor;
		static const char* last_skip = nullptr;
		static const char* last_src = nullptr;
		static int last_ptcl = -2;
		static int last_lights = -2;
		static int last_fog = -2;
		const UINT frame = shared::common::ffp_state::get().frame_count();
		const int ptcl = particles::captured_count();
		const int nlights = lights::captured_count();
		const int nfog = fog::captured_count();
		const bool interesting = last_frame == 0 ||
			phase != last_phase ||
			frame_lock.skip_reason != last_skip ||
			frame_lock.identity_source != last_src ||
			ptcl != last_ptcl ||
			nlights != last_lights ||
			nfog != last_fog ||
			frame_lock.switched;
		if (!interesting && (frame - last_frame) < 60u) {
			return;
		}
		last_frame = frame;
		last_phase = phase;
		last_skip = frame_lock.skip_reason;
		last_src = frame_lock.identity_source;
		last_ptcl = ptcl;
		last_lights = nlights;
		last_fog = nfog;
		shared::common::log("Camera", line);
		if (phase == init_phase::live) {
			maybe_focus_compiled_play_window();
		}
	}

	void on_present()
	{
		adopt_compiled_hwnd(shared::globals::main_window);
		apply_compiled_player_mouse();

		if (is_pregame())
		{
			HWND hwnd = shared::globals::main_window;
			if (!hwnd && shared::globals::d3d_device)
			{
				D3DDEVICE_CREATION_PARAMETERS cp{};
				if (SUCCEEDED(shared::globals::d3d_device->GetCreationParameters(&cp))) {
					hwnd = cp.hFocusWindow;
				}
			}
			classify_hwnd(hwnd);
			promote_windowed_boot("pregame windowed client");
		}

		if (!conversion_allowed())
		{
			lights::on_frame();
			particles::on_frame();
			fog::on_frame();
			auto& ffp = shared::common::ffp_state::get();
			if (!frame_lock.skip_reason)
			{
				if (phase == init_phase::picker) {
					frame_lock.skip_reason = "pre-game picker";
				}
				else if (phase == init_phase::windowed) {
					frame_lock.skip_reason = "pre-game windowed";
				}
				else {
					frame_lock.skip_reason = "pre-game loading";
				}
			}

			const float fov_deg = frame_lock.fov * 57.2957795f;
			const D3DXVECTOR3 eye = frame_lock.have_eye ? frame_lock.eye :
				(last_good.have_eye ? last_good.eye : D3DXVECTOR3{ 0, 0, 0 });

			using route = shared::common::ffp_state::draw_route;
			char line[512];
			std::snprintf(line, sizeof(line),
				"Frame %u: phase=%s locked fov=%.2fdeg eye=(%.3f, %.3f, %.3f) reused_last_C=%d "
				"view_updates=%d list_walks=%d src=%s pub=%s switched=%d "
				"skipped_non_scene=%d applied=%d published=%d api=%d skip=%s "
				"ffp=%u unresolved=%u skinned=%u noNormal=%u secondary=%u hud=%d pregame=%u draws=%u "
				"lights=%d sun=%d pt=%d ptcl=%d fog=%d",
				ffp.frame_count(),
				phase_cstr(phase),
				fov_deg, eye.x, eye.y, eye.z,
				frame_lock.reused_last_eye ? 1 : 0,
				frame_lock.view_updates,
				frame_lock.list_walks,
				frame_lock.identity_source ? frame_lock.identity_source : "-",
				frame_lock.publish_source ? frame_lock.publish_source : "-",
				frame_lock.switched ? 1 : 0,
				frame_lock.skipped_non_scene,
				0,
				frame_lock.published,
				frame_lock.api_inject ? 1 : 0,
				frame_lock.skip_reason ? frame_lock.skip_reason : "-",
				ffp.route_count(route::ffp),
				ffp.route_count(route::world_unresolved),
				ffp.route_count(route::skinned),
				ffp.route_count(route::no_normal),
				ffp.route_count(route::secondary_target),
				static_cast<int>(ffp.route_count(route::hud)),
				ffp.route_count(route::pre_game),
				ffp.draw_call_count(),
				lights::captured_count(),
				lights::captured_suns(),
				lights::captured_points(),
				particles::captured_count(),
				fog::captured_count());
			log_present_stats(line);

			frame_lock = {};
			scene_stash_n = 0;
			for (auto& entry : scene_stash) {
				entry = {};
			}
			return;
		}

		if (!frame_lock.identity_locked) {
			lock_identity();
		}
		else {
			refresh_locked_eye(true);
		}
		try_lock_from_stash();

		// List-miss / no new VP this frame: still publish last-good immediately
		// (same Present, no extra frame). Hash skip if early-vp already sent it.
		publish_if_changed("present");
		lights::on_frame();
		particles::on_frame();
		fog::on_frame();

		D3DMATRIX view{};
		D3DMATRIX proj{};
		const bool have = compose_locked_camera(view, proj, false);
		auto& ffp = shared::common::ffp_state::get();
		if (!have)
		{
			if (!frame_lock.skip_reason) {
				frame_lock.skip_reason = "compose failed";
			}
			if (!have_published)
			{
				char skip[160];
				std::snprintf(skip, sizeof(skip), "Frame %u: set_camera skipped (%s)",
					ffp.frame_count(), frame_lock.skip_reason);
				shared::common::log("Camera", skip, shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
		}

		const float fov_deg = frame_lock.fov * 57.2957795f;
		const D3DXVECTOR3 eye = frame_lock.have_eye ? frame_lock.eye :
			(last_good.have_eye ? last_good.eye : D3DXVECTOR3{ 0, 0, 0 });

		using route = shared::common::ffp_state::draw_route;
		char line[512];
		std::snprintf(line, sizeof(line),
			"Frame %u: phase=%s locked fov=%.2fdeg eye=(%.3f, %.3f, %.3f) reused_last_C=%d "
			"view_updates=%d list_walks=%d src=%s pub=%s switched=%d "
			"skipped_non_scene=%d applied=%d published=%d api=%d skip=%s "
			"ffp=%u unresolved=%u skinned=%u noNormal=%u secondary=%u hud=%d pregame=%u draws=%u "
			"lights=%d sun=%d pt=%d ptcl=%d fog=%d",
			ffp.frame_count(),
			phase_cstr(phase),
			fov_deg, eye.x, eye.y, eye.z,
			frame_lock.reused_last_eye ? 1 : 0,
			frame_lock.view_updates,
			frame_lock.list_walks,
			frame_lock.identity_source ? frame_lock.identity_source : "-",
			frame_lock.publish_source ? frame_lock.publish_source : "-",
			frame_lock.switched ? 1 : 0,
			frame_lock.skipped_non_scene,
			have ? 1 : 0,
			frame_lock.published,
			frame_lock.api_inject ? 1 : 0,
			frame_lock.skip_reason ? frame_lock.skip_reason : "-",
			ffp.route_count(route::ffp),
			ffp.route_count(route::world_unresolved),
			ffp.route_count(route::skinned),
			ffp.route_count(route::no_normal),
			ffp.route_count(route::secondary_target),
			static_cast<int>(ffp.route_count(route::hud)),
			ffp.route_count(route::pre_game),
			ffp.draw_call_count(),
			lights::captured_count(),
			lights::captured_suns(),
			lights::captured_points(),
			particles::captured_count(),
			fog::captured_count());
		log_present_stats(line);

		if (frame_lock.identity_locked)
		{
			last_good.handle = frame_lock.handle;
			last_good.fov = frame_lock.fov;
		}
		if (frame_lock.matrices_locked)
		{
			last_good.view = frame_lock.view;
			last_good.proj = frame_lock.proj;
			last_good.have_matrices = true;
		}

		frame_lock = {};
		scene_stash_n = 0;
		for (auto& entry : scene_stash) {
			entry = {};
		}
	}

	void set_viewport(const UINT width, const UINT height)
	{
		if (width == 0 || height == 0) {
			return;
		}

		apply_backbuffer_size(width, height);
		if (conversion_allowed()) {
			remember_scene_size(width, height);
		}
		else
		{
			static UINT logged_w = 0;
			static UINT logged_h = 0;
			static init_phase logged_phase = init_phase::editor;
			if (width != logged_w || height != logged_h || phase != logged_phase)
			{
				logged_w = width;
				logged_h = height;
				logged_phase = phase;
				const char* kind = (phase == init_phase::windowed)
					? "Pregame windowed" : "Pregame backbuffer";
				shared::common::log("Camera", std::format(
					"{} {}x{} aspect={:.4f} (not scene until live)",
					kind, width, height,
					static_cast<float>(width) / static_cast<float>(height)));
				if (shared::globals::d3d_device) {
					log_device_surfaces(shared::globals::d3d_device, kind);
				}
			}
		}
	}

	void note_surface_size(const UINT width, const UINT height)
	{
		if (width == 0 || height == 0 || !conversion_allowed()) {
			return;
		}

		// Parent/dialog client rects (e.g. 1519×824 next to a 1920×1080
		// back buffer) are not scene sizes. MAIN is the tracked backbuffer.
		if (!accept_as_scene_size(width, height)) {
			return;
		}
		if (viewport_w != 0 && viewport_h != 0 &&
			!matches_main_viewport(width, height))
		{
			return;
		}

		remember_scene_size(width, height);
	}

	void on_device_created(HWND hwnd, const UINT width, const UINT height,
		const BOOL windowed)
	{
		device_windowed = windowed != FALSE;
		editor_search_done = false;
		refresh_desktop_size(hwnd);
		if (width != 0 && height != 0)
		{
			apply_backbuffer_size(width, height);
			note_boot_size(width, height);
		}

		classify_hwnd(hwnd);
		adopt_compiled_hwnd(hwnd);

		if (shared::globals::is_editor_host ||
			hwnd_or_ancestor_is_editor(hwnd) || process_has_editor_window())
		{
			compiled_player = false;
			confirmed_editor = true;
			phase = init_phase::editor;
			log_phase("CreateDevice editor");
		}
		else if (!shared::globals::is_editor_host &&
			(compiled_player || !hwnd_is_editor(hwnd)))
		{
			compiled_player = true;
			if (phase == init_phase::editor)
			{
				if (hwnd_class_is(hwnd, "#32770"))
				{
					phase = init_phase::picker;
					log_phase("CreateDevice compiled player");
				}
				else {
					enter_windowed("CreateDevice windowed");
				}
			}
			promote_windowed_boot("CreateDevice windowed client");
		}

		// Classify + promote first so an unknown compiled hwnd cannot
		// register 1080×810 as a scene size while still defaulting to editor.
		set_viewport(width, height);
		init();
	}

	void on_device_reset(IDirect3DDevice9* dev, UINT width, UINT height,
		HWND hwnd, const BOOL windowed)
	{
		const UINT old_w = viewport_w;
		const UINT old_h = viewport_h;
		device_windowed = windowed != FALSE;
		editor_search_done = false;

		invalidate_camera_after_reset();
		clear_scene_sizes();
		refresh_desktop_size(hwnd);

		if ((width == 0 || height == 0) && dev)
		{
			IDirect3DSurface9* back = nullptr;
			if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back)
			{
				D3DSURFACE_DESC desc{};
				if (SUCCEEDED(back->GetDesc(&desc)))
				{
					width = desc.Width;
					height = desc.Height;
				}
				back->Release();
			}
		}

		apply_backbuffer_size(width, height);

		const bool size_changed = old_w != 0 && old_h != 0 && width != 0 && height != 0 &&
			(width != old_w || height != old_h);
		if (size_changed && !device_windowed) {
			note_chosen_size(width, height);
		}
		if (size_changed && (width > old_w || height > old_h)) {
			note_chosen_size(width, height);
		}

		classify_hwnd(hwnd);
		adopt_compiled_hwnd(hwnd);

		const bool player_frame = hwnd_is_player_frame(hwnd);
		const bool fullscreen_now = looks_fullscreen(hwnd, width, height);

		if (compiled_player)
		{
			if (fullscreen_now || (!device_windowed && (size_changed || player_frame)) ||
				(size_changed && !is_small_boot_size(width, height)))
			{
				enter_loading(size_changed ? "Reset to chosen size" :
					(!device_windowed ? "Reset exclusive" :
						(player_frame ? "Reset Fullscreen Window" : "Reset fullscreen")));
			}
			else if (phase == init_phase::picker && hwnd_class_is(hwnd, "#32770") &&
				!size_changed)
			{
				log_phase("Reset same size (still picker)");
				promote_windowed_boot("Reset windowed after picker");
			}
			else if (phase == init_phase::live)
			{
				enter_loading("Reset (was live)");
			}
			else
			{
				enter_windowed(size_changed ?
					"Reset windowed (still below fullscreen)" :
					"Reset windowed");
			}
		}
		else
		{
			phase = init_phase::editor;
			log_phase("Reset editor");
		}

		if (conversion_allowed()) {
			remember_scene_size(viewport_w, viewport_h);
		}
		else {
			set_viewport(viewport_w, viewport_h);
		}

		if (dev) {
			log_device_surfaces(dev, "Reset");
		}

		// IAT hooks are idempotent. Restart is a no-op if CreateDevice already
		// installed them; covers a Reset before dll3impact was ready.
		init();
	}

	bool scene_conversion_allowed()
	{
		return conversion_allowed();
	}

	bool remix_api_init_allowed()
	{
		if (shared::globals::is_compiled_host &&
			!shared::globals::is_editor_host)
		{
			if (d3d9_proxy::remix_deferred() ||
				display_options::picker_visible())
			{
				return false;
			}
		}
		// conversion_allowed is true for editor from the first CreateDevice,
		// but 3D Rad still Reset / recreates that device. Wait until a scene
		// View/P has been recovered so InitializeLibrary is not sitting on a
		// throwaway swapchain.
		return conversion_allowed() && (have_published || last_good.have_matrices);
	}

	const char* init_phase_name()
	{
		return phase_cstr(phase);
	}

	bool size_is_scene_viewport(const UINT width, const UINT height)
	{
		if (!conversion_allowed() || width == 0 || height == 0) {
			return false;
		}

		if (!accept_as_scene_size(width, height)) {
			return false;
		}

		return matches_main_viewport(width, height);
	}

	// Pregame XYZRHW dummy + rtx.enableRaytracing toggle crashed NvRemixBridge
	// (0xc0000005) on the compiled player's second CreateDevice. Left as a
	// no-op: pregame still skips scene size / FFP / camera, never identity View/P.
	void mark_pregame_swapchain_ui(IDirect3DDevice9*)
	{
	}

	void note_pregame_draw(IDirect3DDevice9* dev)
	{
		if (!dev || conversion_allowed()) {
			return;
		}

		static UINT last_w = 0;
		static UINT last_h = 0;
		static init_phase last_phase = init_phase::editor;
		static int samples = 0;
		if (last_w != viewport_w || last_h != viewport_h || last_phase != phase)
		{
			last_w = viewport_w;
			last_h = viewport_h;
			last_phase = phase;
			samples = 0;
		}
		if (samples >= 8) {
			return;
		}
		samples++;

		log_device_surfaces(dev, "pregame-draw");

		auto& ffp = shared::common::ffp_state::get();
		IDirect3DVertexShader9* vs = nullptr;
		dev->GetVertexShader(&vs);
		const int have_vs = vs ? 1 : 0;
		if (vs) {
			vs->Release();
		}

		float minx = 0.0f, maxx = 0.0f, miny = 0.0f, maxy = 0.0f, minz = 0.0f, maxz = 0.0f;
		int n = 0;
		const UINT stride = ffp.stream_stride(0);
		auto* vb = ffp.stream_vb(0);
		if (vb && stride >= 12)
		{
			void* raw = nullptr;
			const UINT bytes = stride * 8;
			if (SUCCEEDED(vb->Lock(ffp.stream_offset(0), bytes, &raw, D3DLOCK_READONLY)) && raw)
			{
				minx = miny = minz = 1.0e9f;
				maxx = maxy = maxz = -1.0e9f;
				auto* data = static_cast<unsigned char*>(raw);
				for (int i = 0; i < 8; i++)
				{
					auto* p = reinterpret_cast<float*>(data + static_cast<size_t>(i) * stride);
					if (p[0] < minx) minx = p[0];
					if (p[0] > maxx) maxx = p[0];
					if (p[1] < miny) miny = p[1];
					if (p[1] > maxy) maxy = p[1];
					if (p[2] < minz) minz = p[2];
					if (p[2] > maxz) maxz = p[2];
					n++;
				}
				vb->Unlock();
			}
		}

		shared::common::log("Camera", std::format(
			"HUD draw phase={} vs={} posT={} stride={} n={} "
			"xyz=({:.1f}..{:.1f}, {:.1f}..{:.1f}, {:.2f}..{:.2f}) tracked={}x{}",
			phase_cstr(phase), have_vs, ffp.cur_decl_has_pos_t() ? 1 : 0,
			stride, n, minx, maxx, miny, maxy, minz, maxz,
			viewport_w, viewport_h));
	}

	bool adjust_pregame_viewport(IDirect3DDevice9* dev, D3DVIEWPORT9& viewport)
	{
		const UINT req_w = viewport.Width;
		const UINT req_h = viewport.Height;
		// Editor never SetRenderTarget'd the 3D pane through our hook
		// (editor_rt stayed 0x0 for 900 frames). The 1332×614 SetViewport
		// *does* fire every frame — treat that size as MAIN.
		note_editor_scene_rt(req_w, req_h);
		const bool not_main = viewport_w != 0 && viewport_h != 0 &&
			!matches_main_viewport(req_w, req_h) &&
			!is_cubemap_viewport(req_w, req_h);
		// Compiled leftover client (1519×824 on 1920×1080) only. Never pin
		// editor 1332 to 827 or 827 to 1332 — that squashes the 3D pane / UI.
		const bool pin_to_bb = compiled_player && is_leftover_client_size(req_w, req_h);

		{
			static UINT seen_w[8]{};
			static UINT seen_h[8]{};
			static int seen_n = 0;

			bool already = false;
			for (int i = 0; i < seen_n; i++)
			{
				if (seen_w[i] == req_w && seen_h[i] == req_h)
				{
					already = true;
					break;
				}
			}

			if (!already)
			{
				if (seen_n < 8)
				{
					seen_w[seen_n] = req_w;
					seen_h[seen_n] = req_h;
					seen_n++;
				}

				const char* tag = not_main
					? "not MAIN (window/UI viewport)"
					: "MAIN";
				shared::common::log("Camera", std::format(
					"SetViewport {}x{} @{},{} phase={} tracked_bb={}x{} editor_rt={}x{} {}",
					req_w, req_h, viewport.X, viewport.Y,
					phase_cstr(phase), viewport_w, viewport_h,
					editor_rt_w, editor_rt_h, tag));
			}
		}

		if (!pin_to_bb || !dev) {
			// Match window: leftover 3D-pane SetViewport (old ChildClass) must
			// not stay letterboxed smaller than the hwnd. Never rewrite the
			// 827 swapchain blit — that pins FOV to the leftover buffer.
			if (phase == init_phase::editor &&
				editor_settings::match_window() &&
				!is_cubemap_viewport(req_w, req_h) &&
				!matches_tracked_backbuffer(req_w, req_h) &&
				matches_editor_rt(req_w, req_h))
			{
				const SIZE pane = editor_frame::viewport_client_size();
				if (pane.cx >= 64 && pane.cy >= 64)
				{
					const int dw = pane.cx - static_cast<int>(req_w);
					const int dh = pane.cy - static_cast<int>(req_h);
					if (dw > 4 || dh > 4)
					{
						viewport.X = 0;
						viewport.Y = 0;
						viewport.Width = static_cast<DWORD>(pane.cx);
						viewport.Height = static_cast<DWORD>(pane.cy);
						note_editor_scene_rt(viewport.Width, viewport.Height);
						return true;
					}
				}
			}
			return editor_settings::inject_editor_viewport(viewport);
		}

		UINT bb_w = viewport_w;
		UINT bb_h = viewport_h;
		IDirect3DSurface9* back = nullptr;
		if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back)
		{
			D3DSURFACE_DESC desc{};
			if (SUCCEEDED(back->GetDesc(&desc)))
			{
				if (desc.Width >= bb_w && desc.Height >= bb_h)
				{
					bb_w = desc.Width;
					bb_h = desc.Height;
				}
			}
			back->Release();
		}

		if (bb_w == 0 || bb_h == 0) {
			return false;
		}
		if (req_w == bb_w && req_h == bb_h) {
			return false;
		}

		static UINT warned_w = 0;
		static UINT warned_h = 0;
		static init_phase warned_phase = init_phase::editor;
		if (req_w != warned_w || req_h != warned_h || phase != warned_phase)
		{
			warned_w = req_w;
			warned_h = req_h;
			warned_phase = phase;
			shared::common::log("Camera", std::format(
				"Leftover viewport {}x{} on backbuffer {}x{} — pinning Remix MAIN "
				"(boot HUD {}x{} / windowed client must not rescale the scene)",
				req_w, req_h, bb_w, bb_h, boot_w, boot_h),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}

		viewport.X = 0;
		viewport.Y = 0;
		viewport.Width = bb_w;
		viewport.Height = bb_h;
		viewport.MinZ = 0.0f;
		viewport.MaxZ = 1.0f;
		return true;
	}

	UINT viewport_width()
	{
		return viewport_w;
	}

	UINT viewport_height()
	{
		return viewport_h;
	}

	bool is_compiled_player()
	{
		return compiled_player;
	}

	void note_editor_scene_rt(const UINT width, const UINT height)
	{
		if (phase != init_phase::editor) {
			return;
		}
		if (is_cubemap_viewport(width, height)) {
			return;
		}
		if (width < 64 || height < 64) {
			return;
		}
		// Swapchain blit is the tracked backbuffer — not the 3D pane.
		if (matches_tracked_backbuffer(width, height)) {
			return;
		}
		if (editor_rt_w == width && editor_rt_h == height) {
			return;
		}

		editor_rt_w = width;
		editor_rt_h = height;
		shared::common::log("Camera", std::format(
			"Editor 3D RT MAIN {}x{} (swapchain stays {}x{})",
			width, height, viewport_w, viewport_h));
		remember_scene_size(width, height);
	}

	void on_viewport(const UINT width, const UINT height)
	{
		if (!conversion_allowed() || !last_good.have_matrices) {
			return;
		}
		if (matches_main_viewport(width, height) || is_cubemap_viewport(width, height)) {
			return;
		}

		// 827 swapchain (editor) or leftover client (player) landed. Re-assert
		// MAIN so Remix does not keep the UI viewport's camera.
		have_published = false;
		published_hash = 0;
		publish_if_changed("after non-MAIN SetViewport");
	}

	/*
	 * Checks the recovery against a frame captured from the running game and worked
	 * out by hand independently, so a regression here shows up as a startup warning
	 * rather than as a wrong camera nobody notices. Values are recorded in kb.h.
	 */
	bool self_test()
	{
		const D3DXMATRIX view_proj{
			 1.16152f, -0.24261f, -0.42445f, -0.42445f,
			-0.00000f,  1.64480f, -0.31339f, -0.31339f,
			 0.58035f,  0.48557f,  0.84949f,  0.84949f,
			 0.00000f,  0.00000f, -0.20000f,  0.00000f };

		const D3DXMATRIX projection{
			1.29844f, 0.00000f, 0.00000f, 0.00000f,
			0.00000f, 1.73205f, 0.00000f, 0.00000f,
			0.00000f, 0.00000f, 1.00002f, 1.00000f,
			0.00000f, 0.00000f, -0.20000f, 0.00000f };

		const D3DXMATRIX expected{
			 0.89455f, -0.14007f, -0.42445f, 0.0f,
			 0.00000f,  0.94962f, -0.31339f, 0.0f,
			 0.44696f,  0.28034f,  0.84949f, 0.0f,
			 0.00000f,  0.00000f,  0.00000f, 1.0f };

		D3DXMATRIX inverse{}, view{};
		if (!D3DXMatrixInverse(&inverse, nullptr, &projection))
		{
			shared::common::log("Camera", "Self-test: the reference projection would not invert.",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return false;
		}

		D3DXMatrixMultiply(&view, &view_proj, &inverse);

		float worst = 0.0f;
		for (int row = 0; row < 4; row++)
		{
			for (int col = 0; col < 4; col++) {
				worst = std::max(worst, std::fabs(view.m[row][col] - expected.m[row][col]));
			}
		}

		const bool rigid = is_rigid_transform(view);
		const bool ok = rigid && worst < 1e-3f;

		shared::common::log("Camera", std::format(
			"Self-test: reference frame recovers its view to {:.6f}, rigid={} -> {}",
			worst, rigid, ok ? "PASS" : "FAIL"),
			ok ? shared::common::LOG_TYPE::LOG_TYPE_GREEN : shared::common::LOG_TYPE::LOG_TYPE_ERROR,
			true);

		return ok;
	}

	bool init()
	{
		/*
		 * Installing twice would be fatal, not merely wasteful: redirect() takes the
		 * slot's current value as the original to call through to, so a second pass
		 * would record our own stub as the original and recurse until the stack ran
		 * out. The device is created twice in this game, so this is reachable.
		 *
		 * Do not latch `installed` until dll3impact is actually loaded — the
		 * compiled player can call Direct3DCreate9 before a retry is possible.
		 */
		static bool installed = false;
		if (installed) {
			return true;
		}

		const auto engine = GetModuleHandleA("dll3impact.dll");
		if (!engine)
		{
			shared::common::log("Camera", "dll3impact.dll is not loaded - camera capture disabled.",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return false;
		}

		installed = true;
		self_test();

		// Redirects one d3dx9_27.dll import of the engine, handing back the original
		// so the stub can still do the work the engine asked for.
		auto redirect = [engine](const char* name, void* stub, void** original) -> bool
			{
				const auto slot = shared::utils::mem::find_import_addr(engine, "d3dx9_27.dll", name);
				if (!slot)
				{
					shared::common::log("Camera", std::format("No {} import in dll3impact.dll.", name),
						shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
					return false;
				}

				auto** entry = reinterpret_cast<void**>(slot);

				DWORD previous_protection = 0;
				if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &previous_protection))
				{
					shared::common::log("Camera", "Could not unprotect the engine import table.",
						shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
					return false;
				}

				*original = *entry;
				*entry = stub;
				VirtualProtect(entry, sizeof(void*), previous_protection, &previous_protection);

				shared::common::log("Camera", std::format("Hooked {} (IAT slot 0x{:X}).", name,
					reinterpret_cast<std::uintptr_t>(entry)), shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
				return true;
			};

		const bool ok = redirect("D3DXMatrixMultiplyTranspose", &multiply_transpose_stub,
			reinterpret_cast<void**>(&engine_multiply_transpose));

		redirect("D3DXMatrixLookAtLH", &look_at_stub, reinterpret_cast<void**>(&engine_look_at));
		redirect("D3DXMatrixPerspectiveFovLH", &perspective_stub, reinterpret_cast<void**>(&engine_perspective));
		redirect("D3DXMatrixOrthoLH", &ortho_stub, reinterpret_cast<void**>(&engine_ortho));
		editor_settings::install_hooks();

		// Editor only: leaf must be 3DRad.exe and the ObjectRun loop prologue
		// must still be `mov edx,[0x450460]`. GetModuleHandle("3DRad.exe") is
		// not enough — a compiled player must never detour 0x414F20.
		if (shared::globals::is_editor_host && !shared::globals::skip_remix)
		{
			const HMODULE editor = GetModuleHandleA(nullptr);
			if (editor)
			{
				const auto loop = static_cast<DWORD>(
					reinterpret_cast<std::uintptr_t>(editor) + (0x00414F20u - 0x00400000u));
				const auto* bytes = reinterpret_cast<const unsigned char*>(
					static_cast<std::uintptr_t>(loop));
				static const unsigned char k_prologue[] = { 0x8B, 0x15, 0x60, 0x04, 0x45, 0x00 };
				if (memory_readable(bytes, sizeof(k_prologue)) &&
					std::memcmp(bytes, k_prologue, sizeof(k_prologue)) == 0)
				{
					if (shared::utils::hook::detour(loop, &object_run_loop_stub,
						reinterpret_cast<void**>(&engine_object_run_loop)))
					{
						shared::common::log("Camera",
							std::format("Hooked editor ObjectRun loop (sim start/stop) @ 0x{:X}.", loop),
							shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
					}
					else {
						shared::common::log("Camera",
							"ObjectRun loop hook failed — editor Play/Stop gate inactive.",
							shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
					}
				}
				else {
					shared::common::log("Camera",
						"ObjectRun loop prologue mismatch — editor Play/Stop gate inactive.",
						shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				}
			}
		}

		return ok;
	}

	void on_project_before()
	{
		drop_detected_cameras();
	}

	void on_project_after()
	{
		plugin_host_ok = false;
		cached_plugin_host_count = -1;
	}
}
