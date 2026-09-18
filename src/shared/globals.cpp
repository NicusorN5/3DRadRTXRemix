#include "std_include.hpp"
#include <Psapi.h>
#include <cctype>
#include <cstring>

namespace shared::globals
{
	D3DXMATRIX IDENTITY =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	std::string root_path;
	HWND main_window = nullptr;

	HMODULE exe_hmodule;
	DWORD exe_module_addr;
	DWORD exe_size = 0u;
	bool skip_remix = false;
	bool is_editor_host = false;
	bool is_compiled_host = false;
	host_kind current_host = host_kind::compiled_player;

	const char* host_kind_name()
	{
		switch (current_host)
		{
		case host_kind::editor: return "editor";
		case host_kind::compiler: return "compiler";
		default: return "compiled_player";
		}
	}
	launch_backend editor_backend = launch_backend::remix;

	const char* launch_backend_name(launch_backend b)
	{
		switch (b)
		{
		case launch_backend::dx9:  return "dx9";
		case launch_backend::dxvk: return "dxvk";
		default:                   return "remix";
		}
	}

	launch_backend parse_launch_backend(const char* s)
	{
		if (!s || !s[0]) {
			return launch_backend::remix;
		}
		char lower[32]{};
		size_t n = 0;
		for (; s[n] && n + 1 < sizeof(lower); n++)
		{
			lower[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[n])));
		}
		lower[n] = 0;
		if (std::strcmp(lower, "dx9") == 0 || std::strcmp(lower, "d3d9") == 0 ||
			std::strcmp(lower, "base") == 0 || std::strcmp(lower, "base_dx9") == 0 ||
			std::strcmp(lower, "stock") == 0)
		{
			return launch_backend::dx9;
		}
		if (std::strcmp(lower, "dxvk") == 0) {
			return launch_backend::dxvk;
		}
		return launch_backend::remix;
	}

	bool d3d_passthrough()
	{
		if (skip_remix) {
			return true;
		}
		if (is_editor_host && editor_backend != launch_backend::remix) {
			return true;
		}
		return false;
	}

	static void ascii_lower_copy(const char* name, char* lower, size_t cap)
	{
		size_t n = 0;
		if (name)
		{
			for (; name[n] && n + 1 < cap; n++)
			{
				lower[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[n])));
			}
		}
		lower[n] = 0;
	}

	static bool exe_name_is_compiler(const char* lower)
	{
		if (!lower || !lower[0]) {
			return false;
		}
		if (std::strcmp(lower, "3drad_compiler.exe") == 0 ||
			std::strcmp(lower, "3dradcompiler.exe") == 0 ||
			std::strcmp(lower, "3d rad compiler.exe") == 0)
		{
			return true;
		}
		// Stock 7.22 host is 3DRad_compiler.exe. Also "3D Rad Compiler.exe".
		// Never 3DRad.exe (no "compiler") and never 3drad_player.exe.
		return std::strstr(lower, "compiler") != nullptr &&
			(std::strstr(lower, "3drad") != nullptr ||
			 std::strstr(lower, "3d rad") != nullptr ||
			 std::strstr(lower, "3d_rad") != nullptr);
	}

	static bool exe_name_is_editor(const char* lower)
	{
		// Exact leaf only. 3drad_player.exe / 3drad_compiler.exe must not match.
		return lower && (
			std::strcmp(lower, "3drad.exe") == 0 ||
			std::strcmp(lower, "3dradrt.exe") == 0);
	}

	static bool module_is_self(const char* name)
	{
		if (!name || !name[0] || !exe_hmodule) {
			return false;
		}
		HMODULE m = GetModuleHandleA(name);
		return m && m == exe_hmodule;
	}

	static host_kind classify_host(const char* leaf_lower)
	{
		if (exe_name_is_compiler(leaf_lower) ||
			module_is_self("3DRad_compiler.exe") ||
			module_is_self("3DRadCompiler.exe"))
		{
			return host_kind::compiler;
		}
		if (exe_name_is_editor(leaf_lower) ||
			module_is_self("3DRad.exe") ||
			module_is_self("3DRadRT.exe"))
		{
			return host_kind::editor;
		}
		return host_kind::compiled_player;
	}

	void setup_exe_module()
	{
		exe_hmodule = GetModuleHandleA(nullptr);
		exe_module_addr = (DWORD)exe_hmodule; // x64: use uintptr_t

		MODULEINFO moduleInfo;
		if (!GetModuleInformation(GetCurrentProcess(), exe_hmodule, &moduleInfo, sizeof(moduleInfo))) {
			shared::common::log("Globals", std::format("Failed to get exe module information. Error: (0x{:X})", GetLastError()), shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
		} else {
			exe_size = moduleInfo.SizeOfImage;
		}

		char path[MAX_PATH]{};
		char lower[MAX_PATH]{};
		if (GetModuleFileNameA(exe_hmodule, path, MAX_PATH) && path[0])
		{
			const char* slash = std::strrchr(path, '\\');
			const char* name = slash ? slash + 1 : path;
			ascii_lower_copy(name, lower, MAX_PATH);
		}

		current_host = classify_host(lower);
		skip_remix = (current_host == host_kind::compiler);
		is_editor_host = (current_host == host_kind::editor);
		is_compiled_host = (current_host == host_kind::compiled_player);
	}

	HMODULE dll_hmodule;
	DWORD dll_module_addr;
	void setup_dll_module(const HMODULE mod)
	{
		dll_hmodule = mod;
		dll_module_addr = (DWORD)dll_hmodule; // x64: use uintptr_t
	}

	void setup_homepath()
	{
		char path[MAX_PATH]; GetModuleFileNameA(dll_hmodule, path, MAX_PATH);
		const std::string dll_root = std::filesystem::path(path).parent_path().string();
		root_path = dll_root;
		// Compiled player: every project-local file (conf/ini/.trex/remix/logs)
		// lives next to THIS exe, even if the proxy d3d9.dll was loaded from
		// C:\3DRadRTX. Editor/compiler keep the DLL/install directory.
		if (is_compiled_host) {
			root_path = host_data_root();
		}
		shared::common::log("Globals",
			std::format("host={} data_root={} dll_root={}",
				host_kind_name(), root_path, dll_root),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}

	std::string host_data_root()
	{
		if (is_compiled_host)
		{
			char path[MAX_PATH]{};
			HMODULE exe = exe_hmodule ? exe_hmodule : GetModuleHandleA(nullptr);
			if (GetModuleFileNameA(exe, path, MAX_PATH) && path[0])
			{
				char* slash = std::strrchr(path, '\\');
				if (slash) {
					*slash = 0;
				}
				if (path[0]) {
					return path;
				}
			}
			return root_path;
		}
		const DWORD a = GetFileAttributesA("C:\\3DRadRTX");
		if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
			return "C:\\3DRadRTX";
		}
		return root_path;
	}

	IDirect3DDevice9* d3d_device = nullptr;
	IDirect3D9* d3d9_interface = nullptr;
	HMODULE d3d9_chain_module = nullptr;

	bool imgui_is_rendering = false;
	bool imgui_menu_open = false;
	bool imgui_allow_input_bypass = false;
	bool imgui_wants_text_input = false;
	uint32_t imgui_allow_input_bypass_timeout = 0u;
}
