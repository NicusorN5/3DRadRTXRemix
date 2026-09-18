#pragma once

// Written once at init, read from the single D3D9 thread — no locks needed.
namespace shared::globals
{
	extern D3DXMATRIX IDENTITY;
	
	// Project-local tree: editor/compiler = DLL/install dir (C:\3DRadRTX).
	// Compiled player = this exe's directory even if d3d9.dll came from install.
	extern std::string root_path;
	extern HWND main_window;

#define EXE_BASE shared::globals::exe_module_addr

	extern HMODULE exe_hmodule;
	extern DWORD exe_module_addr; // x64: use uintptr_t
	extern DWORD exe_size;
	extern void setup_exe_module();

	// 3D Rad Compiler (3DRad_compiler.exe) must not load Remix / DXVK.
	// True after setup_exe_module() when the host is that utility.
	extern bool skip_remix;

	// 3DRad.exe only — compiled players (3drad_player.exe / scary.exe / …) stay false.
	extern bool is_editor_host;

	// Compiled player: not editor, not compiler. scary.exe / 3drad_player.exe / client.exe.
	extern bool is_compiled_host;

	enum class host_kind
	{
		compiled_player,
		editor,
		compiler,
	};
	extern host_kind current_host;
	const char* host_kind_name();

	enum class launch_backend
	{
		remix, // d3d9_remix.dll bridge + FFP/camera/lights
		dx9,   // stock system d3d9, no FFP / MinHook game hooks / NvRemixBridge
		dxvk,  // vanilla DXVK d3d9_dxvk.dll, no Remix / FFP
	};
	extern launch_backend editor_backend;

	const char* launch_backend_name(launch_backend b);
	launch_backend parse_launch_backend(const char* s);

	// Compiler, or editor Base DX9 / DXVK: return the real D3D9 interface.
	bool d3d_passthrough();

	extern HMODULE dll_hmodule;
	extern DWORD dll_module_addr; // x64: use uintptr_t
	extern void setup_dll_module(const HMODULE mod);

	extern void setup_homepath();

	// Editor / compiler: C:\3DRadRTX (install). Compiled player: directory of
	// this exe (the compiledProject build), never the editor install tree.
	std::string host_data_root();

	extern IDirect3DDevice9* d3d_device;
	extern IDirect3D9* d3d9_interface;

	// Module handle of the real d3d9 chain (Remix bridge or system d3d9.dll).
	// Set by d3d9_proxy::init(), consumed by remix_api for API lookups.
	extern HMODULE d3d9_chain_module;

	extern bool imgui_is_rendering;
	extern bool imgui_menu_open;
	extern bool imgui_allow_input_bypass;
	extern bool imgui_wants_text_input;
	extern uint32_t imgui_allow_input_bypass_timeout;
}
