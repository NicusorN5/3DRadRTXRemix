#include "std_include.hpp"
#include <psapi.h>

#include "comp.hpp"
#include "compiler_inject.hpp"
#include "d3d9_proxy.hpp"
#include "launch_dialog.hpp"
#include "display_options.hpp"
#include "editor_settings.hpp"
#include "shader_cache.hpp"
#include "shared/common/flags.hpp"
#include "shared/common/config.hpp"

namespace comp
{
	std::unordered_set<HWND> wnd_class_list;

	// Editor frame is 3DRADCLASS. The compiled player never creates that class:
	// it uses a dialog (#32770) as CreateDevice hwnd, then "Fullscreen Window".
	#define WINDOW_CLASS_NAME "3DRADCLASS"
	#define PLAYER_WINDOW_CLASS "Fullscreen Window"

	bool class_is_game_frame(const std::string_view class_name)
	{
		if (class_name.contains(WINDOW_CLASS_NAME)) {
			return true;
		}
		return class_name == PLAYER_WINDOW_CLASS;
	}

	BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lParam)
	{
		DWORD window_pid, target_pid = static_cast<DWORD>(lParam);
		GetWindowThreadProcessId(hwnd, &window_pid);

		if (window_pid == target_pid && IsWindowVisible(hwnd))
		{
			char class_name[256];
			GetClassNameA(hwnd, class_name, sizeof(class_name));

			if (!wnd_class_list.contains(hwnd))
			{
				char debug_msg[256];
				wsprintfA(debug_msg, "> HWND: %p, PID: %u, Class: %s, Visible: %d \n", hwnd, window_pid, class_name, IsWindowVisible(hwnd));
				shared::common::log("Main", debug_msg, shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
				wnd_class_list.insert(hwnd);
			}

			if (class_is_game_frame(class_name))
			{
				shared::globals::main_window = hwnd;
				return FALSE;
			}
		}

		return TRUE;
	}

	DWORD WINAPI find_game_window([[maybe_unused]] LPVOID lpParam)
	{
		std::uint32_t T = 0;

		shared::common::log("Main", "Waiting for window '" WINDOW_CLASS_NAME "' or '" PLAYER_WINDOW_CLASS "' ...", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		{
			while (!shared::globals::main_window)
			{
				EnumWindows(enum_windows_proc, static_cast<LPARAM>(GetCurrentProcessId()));
				if (!shared::globals::main_window) {
					Sleep(1u); T += 1u;
				}

				if (T >= 30000)
				{
					if (shared::globals::d3d_device)
					{
						shared::common::log("Main",
							"No '" WINDOW_CLASS_NAME "' / '" PLAYER_WINDOW_CLASS "' yet — continuing from CreateDevice hwnd (compiled player).",
							shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
						break;
					}

					Beep(300, 100); Sleep(100); Beep(200, 100);
					shared::common::log("Main", "Could not find '" WINDOW_CLASS_NAME "' Window. Not loading RTX Compatibility Mod.", shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
					return TRUE;
				}
			}
		}

		if (!shared::common::flags::has_flag("nobeep")) {
			Beep(523, 100);
		}

		// Post-load DLLs (after window is found, game is running)
		d3d9_proxy::load_postload_dlls();

		comp::main();
		return 0;
	}

	bool start_game_hooks()
	{
		static bool started = false;
		if (started) {
			return true;
		}
		started = true;

		if (const auto MH_INIT_STATUS = MH_Initialize();
			MH_INIT_STATUS != MH_STATUS::MH_OK &&
			MH_INIT_STATUS != MH_STATUS::MH_ERROR_ALREADY_INITIALIZED)
		{
			shared::common::log("Main", std::format("MinHook failed to initialize with code: {:d}", static_cast<int>(MH_INIT_STATUS)), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return false;
		}

		comp::game::init_game_addresses();

		if (const auto t = CreateThread(nullptr, 0, comp::find_game_window, nullptr, 0, nullptr); t) {
			CloseHandle(t);
		}
		return true;
	}
}

BOOL APIENTRY DllMain(HMODULE hmodule, const DWORD ul_reason_for_call, LPVOID)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
	{
		shared::globals::setup_dll_module(hmodule);
		DisableThreadLibraryCalls(hmodule);
		if (comp::shader_cache::host_is_wipe_helper()) {
			return TRUE;
		}
		shared::globals::setup_exe_module();
		shared::globals::setup_homepath();

		if (shared::globals::skip_remix)
		{
			// Compiler must not start NvRemixBridge / DXVK / FFP.
			// Load stock system d3d9 and return it from Direct3DCreate9.
			// MinHook is used only in compiler_inject (file/dialog APIs).
			// Console first so log() (inject copies, dest, errors) is visible.
			shared::common::console();
			SetConsoleTitleA("RTX-Comp Debug Console - compiler");

			char exe_path[MAX_PATH]{};
			GetModuleFileNameA(shared::globals::exe_hmodule, exe_path, MAX_PATH);
			const char* exe_slash = std::strrchr(exe_path, '\\');
			const char* exe_name = exe_slash ? exe_slash + 1 : exe_path;

			shared::common::set_console_color_blue(true);
			std::cout << "Launching RTX Remix Comp [" << COMP_MOD_VERSION_MAJOR << "." << COMP_MOD_VERSION_MINOR << "." << COMP_MOD_VERSION_PATCH << "] — compiler passthrough\n";
			std::cout << "> Compiled On : " + std::string(__DATE__) + " " + std::string(__TIME__) + "\n";
			std::cout << "> Host        : " << exe_name << "\n";
			std::cout << "> Remix/DXVK/FFP skipped — using system d3d9\n";
			std::cout << "> Inject watcher copies Remix runtime into compiled games\n\n";
			shared::common::set_console_color_default();

			shared::common::log("Main",
				std::format("compiler detected ({}) — skip Remix/DXVK, system d3d9 only.", exe_name),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			shared::common::log("Main",
				std::format("dll folder: {}", shared::globals::root_path));

			if (!d3d9_proxy::init()) {
				shared::common::log("Main",
					"system d3d9 failed to load — inject watcher not started.",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				return TRUE;
			}
			comp::compiler_inject::start();
			return TRUE;
		}

		shared::common::console();

		shared::common::set_console_color_blue(true);
		std::cout << "Launching RTX Remix Comp [" << COMP_MOD_VERSION_MAJOR << "." << COMP_MOD_VERSION_MINOR << "." << COMP_MOD_VERSION_PATCH << "]\n";
		std::cout << "> Compiled On : " + std::string(__DATE__) + " " + std::string(__TIME__) + "\n";
		std::cout << "> Based on xoxor4d/remix-comp-base\n";
		std::cout << "> Adapted by kim2091 for Vibe Reverse Engineering\n";
		std::cout << "> Running as d3d9.dll proxy\n";
		if (shared::globals::is_editor_host) {
			std::cout << "> Host        : 3DRad.exe (editor) — launch dialog before WinMain\n";
		}
		else {
			std::cout << "> Host        : compiled player — Display Options, then Remix\n";
		}
		std::cout << "\n";
		shared::common::set_console_color_default();

		if (shared::globals::is_editor_host) {
			SetConsoleTitleA("RTX-Comp Debug Console - editor");
		}
		else {
			SetConsoleTitleA("RTX-Comp Debug Console - compiled player");
			shared::common::demote_debug_console();
		}

		// Load config from INI file next to the DLL
		shared::common::config::get().load(shared::globals::root_path + "\\remix-comp-proxy.ini");
		if (shared::globals::is_editor_host) {
			comp::editor_settings::reload();
		}

		// Pre-load DLLs (before the d3d9 chain is established)
		d3d9_proxy::load_preload_dlls();

		if (shared::globals::is_editor_host)
		{
			// Dialog + Remix LoadLibrary must not run under the loader lock, but
			// Remix still has to load before WinMain creates windows (late
			// LoadLibrary from Direct3DCreate9 AVs NvRemixBridge on the editor's
			// second CreateDevice: no WndProc hook, WndProc::set without unset).
			shared::common::log("Main",
				std::format("editor detected (host={} exe) — launch dialog before WinMain.",
					shared::globals::host_kind_name()),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			comp::launch::start_from_dllmain();
			return TRUE;
		}

		// Compiled player: hook Display Options, defer Remix LoadLibrary until OK.
		shared::common::log("Main",
			std::format("compiled player detected (host={}) data_root={} — Display Options catcher, defer Remix until OK.",
				shared::globals::host_kind_name(), shared::globals::root_path),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		comp::display_options::start();
		if (!d3d9_proxy::init())
			return TRUE;

		comp::start_game_hooks();
	}

	return TRUE;
}
