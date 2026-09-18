#include "std_include.hpp"
#include "d3d9_proxy.hpp"
#include "remix_graphics.hpp"
#include "display_options.hpp"
#include "shared/common/config.hpp"
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace d3d9_proxy
{
	static HMODULE chain_module_ = nullptr;
	static HMODULE remix_module_ = nullptr;
	static IDirect3D9* remix_d3d_ = nullptr;
	static std::vector<HMODULE> loaded_dlls_;
	static bool remix_deferred_ = false;

	static PFN_Direct3DCreate9      pDirect3DCreate9 = nullptr;
	static PFN_Direct3DCreate9Ex    pDirect3DCreate9Ex = nullptr;
	static PFN_D3DPERF_BeginEvent   pD3DPERF_BeginEvent = nullptr;
	static PFN_D3DPERF_EndEvent     pD3DPERF_EndEvent = nullptr;
	static PFN_D3DPERF_SetMarker    pD3DPERF_SetMarker = nullptr;
	static PFN_D3DPERF_SetRegion    pD3DPERF_SetRegion = nullptr;
	static PFN_D3DPERF_QueryRepeatFrame pD3DPERF_QueryRepeatFrame = nullptr;
	static PFN_D3DPERF_SetOptions   pD3DPERF_SetOptions = nullptr;
	static PFN_D3DPERF_GetStatus    pD3DPERF_GetStatus = nullptr;
	static FARPROC pDirect3DShaderValidatorCreate9 = nullptr;
	static FARPROC pDebugSetLevel = nullptr;
	static FARPROC pDebugSetMute = nullptr;

	static void resolve_procs(HMODULE mod)
	{
		pDirect3DCreate9                = (PFN_Direct3DCreate9)GetProcAddress(mod, "Direct3DCreate9");
		pDirect3DCreate9Ex              = (PFN_Direct3DCreate9Ex)GetProcAddress(mod, "Direct3DCreate9Ex");
		pD3DPERF_BeginEvent             = (PFN_D3DPERF_BeginEvent)GetProcAddress(mod, "D3DPERF_BeginEvent");
		pD3DPERF_EndEvent               = (PFN_D3DPERF_EndEvent)GetProcAddress(mod, "D3DPERF_EndEvent");
		pD3DPERF_SetMarker              = (PFN_D3DPERF_SetMarker)GetProcAddress(mod, "D3DPERF_SetMarker");
		pD3DPERF_SetRegion              = (PFN_D3DPERF_SetRegion)GetProcAddress(mod, "D3DPERF_SetRegion");
		pD3DPERF_QueryRepeatFrame       = (PFN_D3DPERF_QueryRepeatFrame)GetProcAddress(mod, "D3DPERF_QueryRepeatFrame");
		pD3DPERF_SetOptions             = (PFN_D3DPERF_SetOptions)GetProcAddress(mod, "D3DPERF_SetOptions");
		pD3DPERF_GetStatus              = (PFN_D3DPERF_GetStatus)GetProcAddress(mod, "D3DPERF_GetStatus");
		pDirect3DShaderValidatorCreate9 = GetProcAddress(mod, "Direct3DShaderValidatorCreate9");
		pDebugSetLevel                  = GetProcAddress(mod, "DebugSetLevel");
		pDebugSetMute                   = GetProcAddress(mod, "DebugSetMute");
	}

	static void load_dll_list(const std::string& list, const char* tag)
	{
		if (list.empty()) return;

		std::stringstream ss(list);
		std::string entry;
		while (std::getline(ss, entry, ';'))
		{
			auto start = entry.find_first_not_of(" \t");
			if (start == std::string::npos) continue;
			auto end = entry.find_last_not_of(" \t");
			entry = entry.substr(start, end - start + 1);
			if (entry.empty()) continue;

			HMODULE mod = LoadLibraryA(entry.c_str());
			if (mod)
			{
				loaded_dlls_.push_back(mod);
				shared::common::log("Chain", std::format("[{}] Loaded: {}", tag, entry));
			}
			else
			{
				shared::common::log("Chain",
					std::format("[{}] Failed to load: {} (error 0x{:X})", tag, entry, GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
			}
		}
	}

	static bool line_sets_expose_remix_api(const std::string& line)
	{
		size_t i = 0;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
			i++;
		}
		if (i < line.size() && (line[i] == '#' || line[i] == ';')) {
			return false;
		}
		return line.compare(i, 14, "exposeRemixApi") == 0;
	}

	static bool line_expose_remix_api_is_true(const std::string& line)
	{
		const auto eq = line.find('=');
		if (eq == std::string::npos) {
			return false;
		}
		size_t i = eq + 1;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
			i++;
		}
		return line.compare(i, 4, "True") == 0 ||
			line.compare(i, 4, "true") == 0 ||
			line.compare(i, 1, "1") == 0;
	}

	// Remix reads .trex/bridge.conf at LoadLibrary. exposeRemixApi defaults
	// False → InitializeLibrary is always Code 11. Write/patch the flag before
	// loading d3d9_remix.dll. Do not SetConfigVariable (enableRaytracing etc.)
	// during picker — that AVed the Remix server on device recreate.
	static void ensure_expose_remix_api()
	{
		const std::string trex = shared::globals::root_path + "\\.trex";
		CreateDirectoryA(trex.c_str(), nullptr);
		const std::string path = trex + "\\bridge.conf";

		std::string text;
		{
			std::ifstream in(path);
			if (in) {
				text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			}
		}

		std::string out;
		out.reserve(text.size() + 64);
		bool saw_key = false;
		bool already_true = false;
		size_t start = 0;
		while (start <= text.size())
		{
			size_t end = text.find_first_of("\r\n", start);
			if (end == std::string::npos) {
				end = text.size();
			}
			const std::string line = text.substr(start, end - start);
			if (line_sets_expose_remix_api(line))
			{
				saw_key = true;
				if (line_expose_remix_api_is_true(line))
				{
					already_true = true;
					out += line;
				}
				else
				{
					out += "exposeRemixApi = True";
				}
			}
			else {
				out += line;
			}
			if (end < text.size())
			{
				out += text[end];
				if (text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n')
				{
					out += '\n';
					end++;
				}
			}
			start = end + 1;
			if (end == text.size()) {
				break;
			}
		}

		if (already_true)
		{
			shared::common::log("Proxy",
				std::format("Remix API enabled ({} has exposeRemixApi=True)", path));
			return;
		}

		if (!saw_key)
		{
			if (!out.empty() && out.back() != '\n') {
				out += "\n";
			}
			out += "\n# Written by remix-comp-proxy — InitializeLibrary is Code 11 without this.\n";
			out += "exposeRemixApi = True\n";
		}

		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			shared::common::log("Proxy",
				std::format("Could not write {} (Code 11 will persist)", path),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}

		file.write(out.data(), static_cast<std::streamsize>(out.size()));
		shared::common::log("Proxy",
			std::format("{} exposeRemixApi=True in {}",
				saw_key ? "Patched" : "Wrote", path),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}

	static bool line_sets_conf_key(const std::string& line, const char* key)
	{
		size_t i = 0;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
			i++;
		}
		if (i < line.size() && (line[i] == '#' || line[i] == ';')) {
			return false;
		}
		const size_t n = std::strlen(key);
		if (line.compare(i, n, key) != 0) {
			return false;
		}
		const char next = (i + n < line.size()) ? line[i + n] : 0;
		return next == 0 || next == ' ' || next == '\t' || next == '=';
	}

	// Remix 1.5.2 GPU particles. Write user.conf (and rtx.conf if present).
	// Do not SetConfigVariable on the picker device — that AVed NvRemixBridge.
	static bool line_conf_value_equals(const std::string& line, const char* value)
	{
		const auto eq = line.find('=');
		if (eq == std::string::npos) {
			return false;
		}
		size_t i = eq + 1;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
			i++;
		}
		size_t e = line.size();
		while (e > i && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r')) {
			e--;
		}
		const std::string have = line.substr(i, e - i);
		return have == value;
	}

	static void upsert_conf_kv(const std::string& path, const char* key, const char* value, bool create_file)
	{
		std::string text;
		{
			std::ifstream in(path);
			if (in) {
				text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			}
			else if (!create_file) {
				return;
			}
		}

		std::string out;
		out.reserve(text.size() + 64);
		bool saw_key = false;
		bool already = false;
		size_t start = 0;
		while (start <= text.size())
		{
			size_t end = text.find_first_of("\r\n", start);
			if (end == std::string::npos) {
				end = text.size();
			}
			const std::string line = text.substr(start, end - start);
			if (line_sets_conf_key(line, key))
			{
				saw_key = true;
				if (line_conf_value_equals(line, value))
				{
					already = true;
					out += line;
				}
				else
				{
					out += key;
					out += " = ";
					out += value;
				}
			}
			else {
				out += line;
			}
			if (end < text.size())
			{
				out += text[end];
				if (text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n')
				{
					out += '\n';
					end++;
				}
			}
			start = end + 1;
			if (end == text.size()) {
				break;
			}
		}

		if (already) {
			return;
		}

		if (!saw_key)
		{
			if (!out.empty() && out.back() != '\n') {
				out += "\n";
			}
			out += key;
			out += " = ";
			out += value;
			out += "\n";
		}

		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			shared::common::log("Proxy",
				std::format("Could not write {} ({})", path, key),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return;
		}
		file.write(out.data(), static_cast<std::streamsize>(out.size()));
		shared::common::log("Proxy",
			std::format("{} {}={} in {}",
				saw_key ? "Patched" : "Wrote", key, value, path));
	}

	static void ensure_remix_particles_enabled()
	{
		const std::string root = shared::globals::root_path;
		const auto write_both = [&](const char* key, const char* value)
		{
			upsert_conf_kv(root + "\\user.conf", key, value, true);
			upsert_conf_kv(root + "\\rtx.conf", key, value, false);
		};
		write_both("rtx.particles.enable", "True");
		write_both("rtx.particles.enableSpawning", "True");
		// Engine-wide Remix floor only. Per-object ParticleSystemEXT on DrawInstance
		// overrides size/color/collide; never treat this as a particle identity.
		write_both("rtx.particles.globalPreset.enableMotionTrail", "False");
		write_both("rtx.particles.globalPreset.minSpawnSize", "14.0");
		write_both("rtx.particles.globalPreset.maxSpawnSize", "14.0");
		write_both("rtx.particles.globalPreset.minTargetSize", "20.0");
		write_both("rtx.particles.globalPreset.maxTargetSize", "20.0");
		// Remix default target color alpha is 0 — particles faded out of existence.
		write_both("rtx.particles.globalPreset.minSpawnColor", "1.0, 1.0, 1.0, 1.0");
		write_both("rtx.particles.globalPreset.maxSpawnColor", "1.0, 1.0, 1.0, 1.0");
		write_both("rtx.particles.globalPreset.minTargetColor", "1.0, 1.0, 1.0, 1.0");
		write_both("rtx.particles.globalPreset.maxTargetColor", "1.0, 1.0, 1.0, 1.0");
		write_both("rtx.particles.globalPreset.numberOfParticlesPerMaterial", "2048");
		write_both("rtx.particleSoftnessFactor", "0.45");
		write_both("rtx.particles.globalPreset.enableCollisionDetection", "False");
	}

	static bool hwnd_is_dummy_10x10(HWND hwnd)
	{
		if (!hwnd || !IsWindow(hwnd)) {
			return false;
		}
		RECT cr{};
		if (!GetClientRect(hwnd, &cr)) {
			return false;
		}
		const int w = cr.right - cr.left;
		const int h = cr.bottom - cr.top;
		return w > 0 && h > 0 && w < 64 && h < 64;
	}

	static void ensure_compiled_bridge_dll(std::string& remix_path)
	{
		const DWORD a = GetFileAttributesA(remix_path.c_str());
		if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
			return;
		}
		const char* k_src = "C:\\3DRadRTX\\d3d9_remix.dll";
		if (GetFileAttributesA(k_src) == INVALID_FILE_ATTRIBUTES) {
			return;
		}
		if (CopyFileA(k_src, remix_path.c_str(), TRUE))
		{
			shared::common::log("Proxy",
				std::format("copied d3d9_remix.dll from C:\\3DRadRTX -> {}", remix_path),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}
		else
		{
			shared::common::log("Proxy",
				std::format("could not copy d3d9_remix.dll to {} err={}",
					remix_path, GetLastError()),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}
	}

	static bool load_remix_module(UINT sdk)
	{
		if (remix_d3d_) {
			return true;
		}

		auto& cfg = shared::common::config::get();
		ensure_expose_remix_api();
		ensure_remix_particles_enabled();

		std::string remix_path = cfg.remix.dll_name.empty() ? "d3d9_remix.dll" : cfg.remix.dll_name;
		if (remix_path.find('\\') == std::string::npos && remix_path.find('/') == std::string::npos)
		{
			remix_path = shared::globals::root_path + "\\" + remix_path;
		}
		ensure_compiled_bridge_dll(remix_path);

		remix_module_ = LoadLibraryA(remix_path.c_str());
		if (!remix_module_)
		{
			shared::common::log("Proxy",
				std::format("late Remix load failed '{}' (error 0x{:X})",
					remix_path, GetLastError()),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return false;
		}

		auto create = reinterpret_cast<PFN_Direct3DCreate9>(
			GetProcAddress(remix_module_, "Direct3DCreate9"));
		if (!create)
		{
			shared::common::log("Proxy",
				"late Remix Direct3DCreate9 missing",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return false;
		}

		remix_d3d_ = create(sdk ? sdk : D3D_SDK_VERSION);
		if (!remix_d3d_)
		{
			shared::common::log("Proxy",
				"late Remix Direct3DCreate9 returned null",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return false;
		}

		remix_deferred_ = false;
		shared::globals::d3d9_chain_module = remix_module_;
		shared::common::log("Proxy",
			std::format("Loaded Remix bridge: {}", remix_path),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		return true;
	}

	IDirect3D9* remix_d3d_for_hwnd(UINT sdk, HWND focus, HWND device, UINT bb_w, UINT bb_h)
	{
		if (!remix_deferred_ && !remix_d3d_) {
			return nullptr;
		}
		if (remix_d3d_) {
			return remix_d3d_;
		}

		const HWND hwnd = device ? device : focus;
		char reason[160]{};

		if (!comp::display_options::remix_load_allowed())
		{
			sprintf_s(reason, "waiting for Display Options OK");
		}
		else if (!hwnd)
		{
			sprintf_s(reason, "hwnd=0");
		}
		else if (bb_w < 64 || bb_h < 64)
		{
			sprintf_s(reason, "backbuffer %ux%u < 64", bb_w, bb_h);
		}
		else if (hwnd_is_dummy_10x10(hwnd) && hwnd_is_dummy_10x10(focus))
		{
			sprintf_s(reason, "10x10 dummy hwnd");
		}

		if (reason[0])
		{
			static char last[160]{};
			if (strcmp(last, reason) != 0)
			{
				strncpy_s(last, reason, _TRUNCATE);
				shared::common::log("Proxy",
					std::format("compiled Remix deferred — {} (hwnd=0x{:X} bb={}x{})",
						reason, reinterpret_cast<std::uintptr_t>(hwnd), bb_w, bb_h),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
			return nullptr;
		}

		if (!load_remix_module(sdk)) {
			return nullptr;
		}
		return remix_d3d_;
	}

	bool init()
	{
		auto& cfg = shared::common::config::get();

		enum class chain_kind { remix, dxvk, system };
		chain_kind want = chain_kind::system;

		if (shared::globals::skip_remix)
		{
			want = chain_kind::system;
			shared::common::log("Proxy",
				"3D Rad Compiler host — skipping Remix/DXVK, using system d3d9.");
		}
		else if (shared::globals::is_editor_host)
		{
			switch (shared::globals::editor_backend)
			{
			case shared::globals::launch_backend::dxvk:
				want = chain_kind::dxvk;
				break;
			case shared::globals::launch_backend::dx9:
				want = chain_kind::system;
				shared::common::log("Proxy",
					"Editor Base DX9 — system d3d9, no Remix / DXVK / FFP.");
				break;
			default:
				want = chain_kind::remix;
				break;
			}
		}
		else if (cfg.remix.enabled && !cfg.remix.dll_name.empty())
		{
			want = chain_kind::remix;
		}

		if (want == chain_kind::remix &&
			comp::remix_graphics::compiled_should_defer_remix())
		{
			remix_deferred_ = true;
			want = chain_kind::system;
			shared::common::log("Proxy",
				"compiled player — system d3d9 until Display Options OK, then Remix on that CreateDevice",
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		if (want == chain_kind::remix)
		{
			ensure_expose_remix_api();
			ensure_remix_particles_enabled();

			std::string remix_path = cfg.remix.dll_name;
			if (remix_path.find('\\') == std::string::npos && remix_path.find('/') == std::string::npos)
			{
				remix_path = shared::globals::root_path + "\\" + remix_path;
			}
			ensure_compiled_bridge_dll(remix_path);

			chain_module_ = LoadLibraryA(remix_path.c_str());
			if (chain_module_)
			{
				resolve_procs(chain_module_);
				shared::common::log("Proxy", std::format("Loaded Remix bridge: {}", remix_path));
			}
			else
			{
				shared::common::log("Proxy",
					std::format("Remix enabled but could not load '{}' (error 0x{:X}). Falling back to system d3d9.",
						remix_path, GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				want = chain_kind::system;
			}
		}
		else if (want == chain_kind::dxvk)
		{
			const std::string dxvk_path = shared::globals::root_path + "\\d3d9_dxvk.dll";
			chain_module_ = LoadLibraryA(dxvk_path.c_str());
			if (chain_module_)
			{
				resolve_procs(chain_module_);
				shared::common::log("Proxy", std::format("Loaded vanilla DXVK: {}", dxvk_path));
			}
			else
			{
				shared::common::log("Proxy",
					std::format("DXVK d3d9_dxvk.dll missing or failed (error 0x{:X}). Not loading Remix. Falling back to system d3d9.",
						GetLastError()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				MessageBoxA(nullptr,
					"d3d9_dxvk.dll could not be loaded.\n"
					"Remix will not be used. Falling back to stock Direct3D9.\n"
					"Hit Update on the next editor start, or place a 32-bit DXVK d3d9.dll as d3d9_dxvk.dll.",
					"3D Rad RTX - DXVK missing", MB_OK | MB_ICONWARNING);
				want = chain_kind::system;
			}
		}

		// System d3d9.dll (compiler, Base DX9, or fallback)
		if (!pDirect3DCreate9)
		{
			char sys_path[MAX_PATH];
			GetSystemDirectoryA(sys_path, MAX_PATH);
			strcat_s(sys_path, "\\d3d9.dll");

			chain_module_ = LoadLibraryA(sys_path);
			if (chain_module_)
			{
				resolve_procs(chain_module_);
				shared::common::log("Proxy", std::format("Loaded system d3d9: {}", sys_path));
			}
		}

		if (!pDirect3DCreate9)
		{
			shared::common::log("Proxy", "FATAL: Could not load any d3d9 implementation",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return false;
		}

		// Publish to shared globals so remix_api can find the chain module
		shared::globals::d3d9_chain_module = chain_module_;

		return true;
	}

	void load_preload_dlls()
	{
		load_dll_list(shared::common::config::get().chain.preload, "Pre");
	}

	void load_postload_dlls()
	{
		load_dll_list(shared::common::config::get().chain.postload, "Post");
	}

	bool remix_deferred() { return remix_deferred_; }
	HMODULE get_chain_module()                          { return chain_module_; }
	PFN_Direct3DCreate9      get_Direct3DCreate9()      { return pDirect3DCreate9; }
	PFN_Direct3DCreate9Ex    get_Direct3DCreate9Ex()    { return pDirect3DCreate9Ex; }
	PFN_D3DPERF_BeginEvent   get_D3DPERF_BeginEvent()   { return pD3DPERF_BeginEvent; }
	PFN_D3DPERF_EndEvent     get_D3DPERF_EndEvent()     { return pD3DPERF_EndEvent; }
	PFN_D3DPERF_SetMarker    get_D3DPERF_SetMarker()    { return pD3DPERF_SetMarker; }
	PFN_D3DPERF_SetRegion    get_D3DPERF_SetRegion()    { return pD3DPERF_SetRegion; }
	PFN_D3DPERF_QueryRepeatFrame get_D3DPERF_QueryRepeatFrame() { return pD3DPERF_QueryRepeatFrame; }
	PFN_D3DPERF_SetOptions   get_D3DPERF_SetOptions()   { return pD3DPERF_SetOptions; }
	PFN_D3DPERF_GetStatus    get_D3DPERF_GetStatus()    { return pD3DPERF_GetStatus; }
	FARPROC get_Direct3DShaderValidatorCreate9()         { return pDirect3DShaderValidatorCreate9; }
	FARPROC get_DebugSetLevel()                          { return pDebugSetLevel; }
	FARPROC get_DebugSetMute()                           { return pDebugSetMute; }
}

// ============================================================
// Exported d3d9.dll functions
//
// Direct3DCreate9 and Direct3DCreate9Ex are intercepted (see d3d9ex.cpp).
// Everything else forwards to the real d3d9 chain.
// ============================================================

extern "C"
{
	int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName)
	{
		auto fn = d3d9_proxy::get_D3DPERF_BeginEvent();
		return fn ? fn(col, wszName) : 0;
	}

	int WINAPI D3DPERF_EndEvent()
	{
		auto fn = d3d9_proxy::get_D3DPERF_EndEvent();
		return fn ? fn() : 0;
	}

	void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName)
	{
		auto fn = d3d9_proxy::get_D3DPERF_SetMarker();
		if (fn) fn(col, wszName);
	}

	void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName)
	{
		auto fn = d3d9_proxy::get_D3DPERF_SetRegion();
		if (fn) fn(col, wszName);
	}

	BOOL WINAPI D3DPERF_QueryRepeatFrame()
	{
		auto fn = d3d9_proxy::get_D3DPERF_QueryRepeatFrame();
		return fn ? fn() : FALSE;
	}

	void WINAPI D3DPERF_SetOptions(DWORD dwOptions)
	{
		auto fn = d3d9_proxy::get_D3DPERF_SetOptions();
		if (fn) fn(dwOptions);
	}

	DWORD WINAPI D3DPERF_GetStatus()
	{
		auto fn = d3d9_proxy::get_D3DPERF_GetStatus();
		return fn ? fn() : 0;
	}

	// Undocumented but some games import it
	void* WINAPI Direct3DShaderValidatorCreate9()
	{
		auto fn = (void* (WINAPI*)())d3d9_proxy::get_Direct3DShaderValidatorCreate9();
		return fn ? fn() : nullptr;
	}

	void WINAPI DebugSetLevel(DWORD level)
	{
		auto fn = (void (WINAPI*)(DWORD))d3d9_proxy::get_DebugSetLevel();
		if (fn) fn(level);
	}

	void WINAPI DebugSetMute()
	{
		auto fn = (void (WINAPI*)())d3d9_proxy::get_DebugSetMute();
		if (fn) fn();
	}
}
