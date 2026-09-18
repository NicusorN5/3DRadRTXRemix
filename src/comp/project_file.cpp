#include "std_include.hpp"
#include "project_file.hpp"
#include "editor_frame.hpp"
#include "game/fog.hpp"
#include "game/camera.hpp"

#include "shared/common/remix_api.hpp"
#include "shared/globals.hpp"
#include "shared/utils/hooking.hpp"

#include <cstring>
#include <format>
#include <mutex>

namespace comp::project_file
{
	namespace
	{
		std::mutex g_mu;
		char g_stem[64]{};
		char g_folder[MAX_PATH]{};
		char g_ini[MAX_PATH]{};
		char g_seen_stem[64]{};
		char g_seen_folder[MAX_PATH]{};
		bool g_bound = false;
		bool g_explicit = false;
		bool g_hooks = false;
		void (*g_before)() = nullptr;
		void (*g_after)() = nullptr;

		void fire_before()
		{
			if (g_before) {
				g_before();
			}
			comp::game::fog::on_project_before();
			comp::game::camera::on_project_before();
		}

		void fire_after()
		{
			if (g_after) {
				g_after();
			}
			comp::game::fog::on_project_after();
			comp::game::camera::on_project_after();
		}

		using CreateFileW_fn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
			DWORD, DWORD, HANDLE);
		CreateFileW_fn CreateFileW_og = nullptr;
		using CreateFileA_fn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
			DWORD, DWORD, HANDLE);
		CreateFileA_fn CreateFileA_og = nullptr;

		bool file_exists_a(const char* path)
		{
			if (!path || !path[0]) {
				return false;
			}
			const DWORD a = GetFileAttributesA(path);
			return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
		}

		bool looks_like_3dr(const wchar_t* path)
		{
			if (!path || !path[0]) {
				return false;
			}
			const std::size_t n = std::wcslen(path);
			return n > 4 && _wcsicmp(path + n - 4, L".3dr") == 0;
		}

		bool looks_like_3dr_a(const char* path)
		{
			if (!path || !path[0]) {
				return false;
			}
			const std::size_t n = std::strlen(path);
			return n > 4 && _stricmp(path + n - 4, ".3dr") == 0;
		}

		bool generic_caption(const char* stem)
		{
			if (!stem || !stem[0]) {
				return true;
			}
			return _strnicmp(stem, "PROCESSING", 10) == 0 ||
				_strnicmp(stem, "3D Rad", 6) == 0 ||
				_stricmp(stem, "3DRad") == 0 ||
				std::strstr(stem, "www.3DRad") != nullptr ||
				std::strchr(stem, '*') != nullptr ||
				std::strchr(stem, '?') != nullptr;
		}

		void stem_from_leaf(const char* leaf, char* out, int cap)
		{
			if (!out || cap <= 0) {
				return;
			}
			out[0] = 0;
			if (!leaf || !leaf[0]) {
				return;
			}
			const char* slash = std::strrchr(leaf, '\\');
			const char* slash2 = std::strrchr(leaf, '/');
			if (slash2 && (!slash || slash2 > slash)) {
				slash = slash2;
			}
			const char* name = slash ? slash + 1 : leaf;
			char tmp[MAX_PATH]{};
			std::strncpy(tmp, name, MAX_PATH - 1);
			char* dot = std::strrchr(tmp, '.');
			if (dot) {
				*dot = 0;
			}
			if (!tmp[0] || generic_caption(tmp)) {
				return;
			}
			std::strncpy(out, tmp, static_cast<std::size_t>(cap - 1));
		}

		void folder_from_path(const char* path, char* out, int cap)
		{
			if (!out || cap <= 0) {
				return;
			}
			out[0] = 0;
			if (!path || !path[0]) {
				return;
			}
			char dir[MAX_PATH]{};
			std::strncpy(dir, path, MAX_PATH - 1);
			char* slash = std::strrchr(dir, '\\');
			char* slash2 = std::strrchr(dir, '/');
			if (slash2 && (!slash || slash2 > slash)) {
				slash = slash2;
			}
			if (slash) {
				*slash = 0;
			}
			std::strncpy(out, dir, static_cast<std::size_t>(cap - 1));
		}

		void rebuild_ini_unlocked()
		{
			g_ini[0] = 0;
			if (g_folder[0] && g_stem[0]) {
				sprintf_s(g_ini, "%s\\%s.ini", g_folder, g_stem);
			}
		}

		void caption_project_stem(char* stem, int cap)
		{
			if (!stem || cap <= 0) {
				return;
			}
			stem[0] = 0;
			HWND ed = editor_frame::editor_hwnd();
			if (!ed) {
				ed = FindWindowA("3DRADCLASS", nullptr);
			}
			char title[256]{};
			if (!ed || !GetWindowTextA(ed, title, 256) || !title[0]) {
				return;
			}
			if (_strnicmp(title, "PROCESSING", 10) == 0) {
				return;
			}
			char* cut = std::strstr(title, " - 3D Rad");
			if (!cut) {
				cut = std::strstr(title, " - 3DRad");
			}
			if (cut) {
				*cut = 0;
			}
			if (generic_caption(title)) {
				return;
			}
			std::strncpy(stem, title, static_cast<std::size_t>(cap - 1));
		}

		bool try_3dr(const char* dir, const char* stem, char* folder_out, int cap)
		{
			if (!dir || !dir[0] || !stem || !stem[0]) {
				return false;
			}
			char file[MAX_PATH]{};
			sprintf_s(file, "%s\\%s.3dr", dir, stem);
			if (!file_exists_a(file)) {
				sprintf_s(file, "%s\\%s\\%s.3dr", dir, stem, stem);
				if (!file_exists_a(file)) {
					return false;
				}
				sprintf_s(folder_out, cap, "%s\\%s", dir, stem);
				return true;
			}
			std::strncpy(folder_out, dir, static_cast<std::size_t>(cap - 1));
			return true;
		}

		bool lone_3dr_in_dir(const char* dir, char* folder_out, char* stem_out, int folder_cap, int stem_cap)
		{
			if (!dir || !dir[0]) {
				return false;
			}
			char glob[MAX_PATH]{};
			sprintf_s(glob, "%s\\*.3dr", dir);
			WIN32_FIND_DATAA fd{};
			HANDLE h = FindFirstFileA(glob, &fd);
			if (h == INVALID_HANDLE_VALUE) {
				return false;
			}
			char first[MAX_PATH]{};
			int n = 0;
			do
			{
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					continue;
				}
				if (++n > 1) {
					FindClose(h);
					return false;
				}
				std::strncpy(first, fd.cFileName, MAX_PATH - 1);
			} while (FindNextFileA(h, &fd));
			FindClose(h);
			if (n != 1 || !first[0]) {
				return false;
			}
			std::strncpy(folder_out, dir, static_cast<std::size_t>(folder_cap - 1));
			stem_from_leaf(first, stem_out, stem_cap);
			return stem_out[0] != 0;
		}

		bool lone_ini_in_dir(const char* dir, char* stem_out, int stem_cap = 64)
		{
			if (!dir || !dir[0] || !stem_out || stem_cap <= 0) {
				return false;
			}
			stem_out[0] = 0;
			char glob[MAX_PATH]{};
			sprintf_s(glob, "%s\\*.ini", dir);
			WIN32_FIND_DATAA fd{};
			HANDLE h = FindFirstFileA(glob, &fd);
			if (h == INVALID_HANDLE_VALUE) {
				return false;
			}
			char first[MAX_PATH]{};
			int n = 0;
			do
			{
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					continue;
				}
				if (_stricmp(fd.cFileName, "windowed.ini") == 0 ||
					_stricmp(fd.cFileName, "imgui.ini") == 0 ||
					_stricmp(fd.cFileName, "remix-comp-proxy.ini") == 0)
				{
					continue;
				}
				if (++n > 1) {
					FindClose(h);
					return false;
				}
				std::strncpy(first, fd.cFileName, MAX_PATH - 1);
			} while (FindNextFileA(h, &fd));
			FindClose(h);
			if (n != 1 || !first[0]) {
				return false;
			}
			stem_from_leaf(first, stem_out, stem_cap);
			return stem_out[0] != 0;
		}

		void compiled_exe_dir(char* out, int cap)
		{
			if (!out || cap <= 0) {
				return;
			}
			out[0] = 0;
			if (!GetModuleFileNameA(nullptr, out, cap) || !out[0]) {
				return;
			}
			char* slash = std::strrchr(out, '\\');
			if (slash) {
				*slash = 0;
			}
		}

		void compiled_projects_dir(char* out, int cap = MAX_PATH)
		{
			if (!out || cap <= 0) {
				return;
			}
			out[0] = 0;
			char exe[MAX_PATH]{};
			compiled_exe_dir(exe, MAX_PATH);
			if (!exe[0]) {
				return;
			}
			sprintf_s(out, static_cast<size_t>(cap), "%s\\3DRad_res\\projects", exe);
		}

		void apply_folder_stem(const char* folder, const char* stem, bool explicit_open = false)
		{
			if (!folder || !folder[0] || !stem || !stem[0] || generic_caption(stem)) {
				return;
			}
			char bind_folder[MAX_PATH]{};
			std::strncpy(bind_folder, folder, MAX_PATH - 1);
			if (shared::globals::is_compiled_host)
			{
				char local[MAX_PATH]{};
				compiled_projects_dir(local);
				if (!local[0]) {
					return;
				}
				std::strncpy(bind_folder, local, MAX_PATH - 1);
				char ini[MAX_PATH]{};
				sprintf_s(ini, "%s\\%s.ini", local, stem);
				char found[MAX_PATH]{};
				if (!file_exists_a(ini) && !try_3dr(local, stem, found, MAX_PATH)) {
					return;
				}
			}
			folder = bind_folder;
			bool changed = false;
			bool skip_autoload = false;
			{
				std::lock_guard lock(g_mu);
				changed = !g_bound ||
					_stricmp(g_folder, folder) != 0 ||
					_stricmp(g_stem, stem) != 0;
				skip_autoload = g_explicit && !explicit_open && changed;
			}
			if (!changed)
			{
				if (explicit_open)
				{
					shared::common::log("Project",
						std::format("reload same stem='{}' from disk (no empty persist)",
							stem),
						shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
					fire_before();
					{
						std::lock_guard lock(g_mu);
						g_explicit = true;
					}
					fire_after();
				}
				return;
			}
			if (skip_autoload)
			{
				shared::common::log("Project",
					std::format("skip autoload stem='{}' — File-Open stays", stem),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				return;
			}

			fire_before();

			char ini_copy[MAX_PATH]{};
			{
				std::lock_guard lock(g_mu);
				std::strncpy(g_folder, folder, MAX_PATH - 1);
				std::strncpy(g_stem, stem, 63);
				rebuild_ini_unlocked();
				g_bound = true;
				if (explicit_open) {
					g_explicit = true;
				}
				std::strncpy(ini_copy, g_ini, MAX_PATH - 1);
			}

			shared::common::log("Project",
				std::format("loaded project stem='{}' folder='{}' ini='{}' explicit={}",
					stem, folder, ini_copy[0] ? ini_copy : "-", explicit_open ? 1 : 0));

			fire_after();
		}

		void remember_3dr_path_w(const wchar_t* path)
		{
			if (!looks_like_3dr(path)) {
				return;
			}
			char utf[MAX_PATH]{};
			WideCharToMultiByte(CP_ACP, 0, path, -1, utf, MAX_PATH, nullptr, nullptr);
			char folder[MAX_PATH]{};
			char stem[64]{};
			folder_from_path(utf, folder, MAX_PATH);
			stem_from_leaf(utf, stem, 64);
			if (!folder[0] || !stem[0] || generic_caption(stem)) {
				return;
			}
			std::lock_guard lock(g_mu);
			std::strncpy(g_seen_folder, folder, MAX_PATH - 1);
			std::strncpy(g_seen_stem, stem, 63);
		}

		void apply_3dr_path_w(const wchar_t* path, bool explicit_open = false)
		{
			if (!looks_like_3dr(path)) {
				return;
			}
			char utf[MAX_PATH]{};
			WideCharToMultiByte(CP_ACP, 0, path, -1, utf, MAX_PATH, nullptr, nullptr);
			char folder[MAX_PATH]{};
			char stem[64]{};
			folder_from_path(utf, folder, MAX_PATH);
			stem_from_leaf(utf, stem, 64);
			if (folder[0] && stem[0]) {
				apply_folder_stem(folder, stem, explicit_open);
			}
		}

		HANDLE WINAPI CreateFileW_hk(LPCWSTR name, DWORD access, DWORD share,
			LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
		{
			const HANDLE h = CreateFileW_og
				? CreateFileW_og(name, access, share, sa, disp, flags, tmpl)
				: INVALID_HANDLE_VALUE;
			if (h != INVALID_HANDLE_VALUE && looks_like_3dr(name)) {
				remember_3dr_path_w(name);
			}
			(void)access;
			return h;
		}

		HANDLE WINAPI CreateFileA_hk(LPCSTR name, DWORD access, DWORD share,
			LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
		{
			const HANDLE h = CreateFileA_og
				? CreateFileA_og(name, access, share, sa, disp, flags, tmpl)
				: INVALID_HANDLE_VALUE;
			if (h != INVALID_HANDLE_VALUE && looks_like_3dr_a(name))
			{
				wchar_t w[MAX_PATH]{};
				MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH);
				remember_3dr_path_w(w);
			}
			(void)access;
			return h;
		}

		bool read_text_file_first_3dr(const char* path, char* out, int cap)
		{
			if (!path || !out || cap <= 0) {
				return false;
			}
			out[0] = 0;
			HANDLE h = CreateFileA(path, GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				return false;
			}
			char buf[4096]{};
			DWORD n = 0;
			const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
			CloseHandle(h);
			if (!ok || n == 0) {
				return false;
			}
			buf[n] = 0;
			char* p = buf;
			while (*p)
			{
				while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
					++p;
				}
				char line[MAX_PATH]{};
				int i = 0;
				while (*p && *p != '\r' && *p != '\n' && i + 1 < MAX_PATH) {
					line[i++] = *p++;
				}
				line[i] = 0;
				while (i > 0 && (line[i - 1] == ' ' || line[i - 1] == '\t' || line[i - 1] == '"')) {
					line[--i] = 0;
				}
				char* found = line;
				if (*found == '"') {
					++found;
				}
				if (looks_like_3dr_a(found) && file_exists_a(found))
				{
					std::strncpy(out, found, static_cast<std::size_t>(cap - 1));
					return true;
				}
			}
			return false;
		}

		void editor_search_roots(char roots[][MAX_PATH], int& n)
		{
			n = 0;
			auto add = [&](const char* p)
			{
				if (!p || !p[0] || n >= 6) {
					return;
				}
				for (int i = 0; i < n; ++i)
				{
					if (_stricmp(roots[i], p) == 0) {
						return;
					}
				}
				std::strncpy(roots[n++], p, MAX_PATH - 1);
			};
			char cwd[MAX_PATH]{};
			GetCurrentDirectoryA(MAX_PATH, cwd);
			add(cwd);
			char exe[MAX_PATH]{};
			GetModuleFileNameA(nullptr, exe, MAX_PATH);
			char* slash = std::strrchr(exe, '\\');
			if (slash) {
				*slash = 0;
			}
			add(exe);
			if (!shared::globals::root_path.empty()) {
				add(shared::globals::root_path.c_str());
			}
			if (!shared::globals::is_compiled_host) {
				add("C:\\3DRadRTX");
			}
		}

		bool try_last_project_file(char* path_out, int cap)
		{
			char roots[6][MAX_PATH]{};
			int n = 0;
			editor_search_roots(roots, n);
			for (int i = 0; i < n; ++i)
			{
				char file[MAX_PATH]{};
				sprintf_s(file, "%s\\3DRad_res\\system\\lastProject.txt", roots[i]);
				if (read_text_file_first_3dr(file, path_out, cap)) {
					return true;
				}
				sprintf_s(file, "%s\\3DRad_res\\system\\windowed.ini", roots[i]);
				if (read_text_file_first_3dr(file, path_out, cap)) {
					return true;
				}
			}
			return false;
		}

		bool try_command_line_3dr(char* path_out, int cap)
		{
			if (!path_out || cap <= 0) {
				return false;
			}
			path_out[0] = 0;
			const wchar_t* cmd = GetCommandLineW();
			if (!cmd) {
				return false;
			}
			const wchar_t* p = cmd;
			while (*p)
			{
				while (*p == L' ' || *p == L'\t') {
					++p;
				}
				if (!*p) {
					break;
				}
				wchar_t tok[MAX_PATH]{};
				int i = 0;
				if (*p == L'"')
				{
					++p;
					while (*p && *p != L'"' && i + 1 < MAX_PATH) {
						tok[i++] = *p++;
					}
					if (*p == L'"') {
						++p;
					}
				}
				else
				{
					while (*p && *p != L' ' && *p != L'\t' && i + 1 < MAX_PATH) {
						tok[i++] = *p++;
					}
				}
				tok[i] = 0;
				if (looks_like_3dr(tok))
				{
					char utf[MAX_PATH]{};
					WideCharToMultiByte(CP_ACP, 0, tok, -1, utf, MAX_PATH, nullptr, nullptr);
					if (file_exists_a(utf))
					{
						std::strncpy(path_out, utf, static_cast<std::size_t>(cap - 1));
						return true;
					}
				}
			}
			return false;
		}

		bool apply_if_3dr_file(const char* path, const char* why)
		{
			if (!looks_like_3dr_a(path) || !file_exists_a(path)) {
				return false;
			}
			wchar_t w[MAX_PATH]{};
			MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH);
			char before[64]{};
			{
				std::lock_guard lock(g_mu);
				std::strncpy(before, g_stem, 63);
			}
			apply_3dr_path_w(w, false);
			char after[64]{};
			{
				std::lock_guard lock(g_mu);
				std::strncpy(after, g_stem, 63);
			}
			if (after[0] && _stricmp(before, after) != 0)
			{
				shared::common::log("Project",
					std::format("autoload {} path='{}'", why ? why : "3dr", path),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
			return after[0] != 0;
		}

		void resolve_from_caption_or_cwd()
		{
			char stem[64]{};
			caption_project_stem(stem, 64);

			char cwd[MAX_PATH]{};
			GetCurrentDirectoryA(MAX_PATH, cwd);
			char exe[MAX_PATH]{};
			GetModuleFileNameA(nullptr, exe, MAX_PATH);
			char* slash = std::strrchr(exe, '\\');
			if (slash) {
				*slash = 0;
			}

			char folder[MAX_PATH]{};
			char seen_folder[MAX_PATH]{};
			char seen_stem[64]{};
			{
				std::lock_guard lock(g_mu);
				if (g_seen_folder[0]) {
					std::strncpy(seen_folder, g_seen_folder, MAX_PATH - 1);
				}
				if (g_seen_stem[0]) {
					std::strncpy(seen_stem, g_seen_stem, 63);
				}
			}

			char cmdline[MAX_PATH]{};
			char lastp[MAX_PATH]{};
			const bool have_cmd = try_command_line_3dr(cmdline, MAX_PATH);
			const bool have_last = try_last_project_file(lastp, MAX_PATH);

			bool bound = false;
			bool explicit_open = false;
			char cur_stem[64]{};
			{
				std::lock_guard lock(g_mu);
				bound = g_bound;
				explicit_open = g_explicit;
				if (g_stem[0]) {
					std::strncpy(cur_stem, g_stem, 63);
				}
			}

			if (stem[0])
			{
				if (bound && cur_stem[0] && _stricmp(cur_stem, stem) == 0) {
					return;
				}
				if (seen_stem[0] && _stricmp(seen_stem, stem) == 0 && seen_folder[0]) {
					apply_folder_stem(seen_folder, stem, true);
					return;
				}
				if (explicit_open && bound && cur_stem[0] && _stricmp(cur_stem, stem) != 0)
				{
					return;
				}
				char last_stem[64]{};
				stem_from_leaf(lastp, last_stem, 64);
				if (have_last && last_stem[0] && _stricmp(last_stem, stem) == 0)
				{
					apply_if_3dr_file(lastp, "lastProject (caption match)");
					return;
				}
				char cmd_stem[64]{};
				stem_from_leaf(cmdline, cmd_stem, 64);
				if (have_cmd && cmd_stem[0] && _stricmp(cmd_stem, stem) == 0)
				{
					apply_if_3dr_file(cmdline, "command line (caption match)");
					return;
				}

				char roots[6][MAX_PATH]{};
				int nr = 0;
				editor_search_roots(roots, nr);
				for (int i = 0; i < nr; ++i)
				{
					char projects[MAX_PATH]{};
					sprintf_s(projects, "%s\\3DRad_res\\projects", roots[i]);
					if (try_3dr(projects, stem, folder, MAX_PATH) ||
						try_3dr(roots[i], stem, folder, MAX_PATH))
					{
						apply_folder_stem(folder, stem);
						return;
					}
				}
				if (try_3dr(cwd, stem, folder, MAX_PATH) ||
					try_3dr(exe, stem, folder, MAX_PATH) ||
					try_3dr(seen_folder, stem, folder, MAX_PATH))
				{
					apply_folder_stem(folder, stem);
					return;
				}
			}

			if (bound) {
				return;
			}

			// Autoload before caption is ready. CreateFile of the .3dr already
			// ran in WinMain, before MinHook, so lastProject.txt is the source.
			if (have_cmd && apply_if_3dr_file(cmdline, "command line")) {
				return;
			}
			if (have_last && apply_if_3dr_file(lastp, "lastProject.txt / windowed.ini")) {
				return;
			}
			if (seen_folder[0] && seen_stem[0]) {
				apply_folder_stem(seen_folder, seen_stem);
				return;
			}

			char lone_stem[64]{};
			if (lone_3dr_in_dir(cwd, folder, lone_stem, MAX_PATH, 64) ||
				lone_3dr_in_dir(exe, folder, lone_stem, MAX_PATH, 64))
			{
				apply_folder_stem(folder, lone_stem);
			}
		}

		bool ini_exists_in(const char* dir, const char* stem)
		{
			if (!dir || !dir[0] || !stem || !stem[0]) {
				return false;
			}
			char file[MAX_PATH]{};
			sprintf_s(file, "%s\\%s.ini", dir, stem);
			return file_exists_a(file);
		}

		void resolve_compiled_game()
		{
			char projects[MAX_PATH]{};
			compiled_projects_dir(projects);
			if (!projects[0]) {
				return;
			}

			char cur_folder[MAX_PATH]{};
			char cur_stem[64]{};
			bool bound = false;
			{
				std::lock_guard lock(g_mu);
				bound = g_bound;
				if (g_folder[0]) {
					std::strncpy(cur_folder, g_folder, MAX_PATH - 1);
				}
				if (g_stem[0]) {
					std::strncpy(cur_stem, g_stem, 63);
				}
			}
			if (bound && cur_folder[0] && _stricmp(cur_folder, projects) == 0 &&
				cur_stem[0] && ini_exists_in(projects, cur_stem))
			{
				return;
			}

			char seen_stem[64]{};
			{
				std::lock_guard lock(g_mu);
				if (g_seen_stem[0]) {
					std::strncpy(seen_stem, g_seen_stem, 63);
				}
			}

			char caption[64]{};
			caption_project_stem(caption, 64);
			char lastp[MAX_PATH]{};
			char cmdline[MAX_PATH]{};
			char last_stem[64]{};
			char cmd_stem[64]{};
			if (try_last_project_file(lastp, MAX_PATH)) {
				stem_from_leaf(lastp, last_stem, 64);
			}
			if (try_command_line_3dr(cmdline, MAX_PATH)) {
				stem_from_leaf(cmdline, cmd_stem, 64);
			}

			auto bind_local = [&](const char* stem, const char* why) -> bool
			{
				if (!stem || !stem[0] || generic_caption(stem)) {
					return false;
				}
				char file[MAX_PATH]{};
				sprintf_s(file, "%s\\%s.ini", projects, stem);
				const bool have_ini = file_exists_a(file);
				char folder[MAX_PATH]{};
				const bool have_3dr = try_3dr(projects, stem, folder, MAX_PATH);
				if (!have_ini && !have_3dr) {
					return false;
				}
				shared::common::log("Project",
					std::format("compiled INI loaded stem='{}' via={} from '{}'",
						stem, why ? why : "-", file),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				apply_folder_stem(projects, stem);
				return true;
			};

			if (bind_local(caption, "caption")) {
				return;
			}
			if (bind_local(seen_stem, "CreateFile")) {
				return;
			}
			if (bind_local(last_stem, "lastProject")) {
				return;
			}
			if (bind_local(cmd_stem, "command line")) {
				return;
			}

			char lone_folder[MAX_PATH]{};
			char lone_stem[64]{};
			if (lone_3dr_in_dir(projects, lone_folder, lone_stem, MAX_PATH, 64) &&
				bind_local(lone_stem, "lone .3dr"))
			{
				return;
			}
			if (lone_ini_in_dir(projects, lone_stem) &&
				bind_local(lone_stem, "lone .ini"))
			{
				return;
			}

			static bool logged_miss = false;
			if (!logged_miss)
			{
				logged_miss = true;
				shared::common::log("Project",
					std::format("compiled INI none in '{}'", projects),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
		}
	}

	const char* stem()
	{
		std::lock_guard lock(g_mu);
		return g_stem[0] ? g_stem : nullptr;
	}

	const char* scene_folder()
	{
		std::lock_guard lock(g_mu);
		return g_folder[0] ? g_folder : nullptr;
	}

	const char* ini_path()
	{
		std::lock_guard lock(g_mu);
		if (!g_ini[0]) {
			rebuild_ini_unlocked();
		}
		return g_ini[0] ? g_ini : nullptr;
	}

	void note_path(const char* path)
	{
		if (!looks_like_3dr_a(path)) {
			return;
		}
		wchar_t w[MAX_PATH]{};
		MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH);
		apply_3dr_path_w(w, true);
	}

	void note_path_w(const wchar_t* path)
	{
		apply_3dr_path_w(path, true);
	}

	void poll()
	{
		if (shared::globals::skip_remix) {
			return;
		}
		if (shared::globals::is_editor_host) {
			resolve_from_caption_or_cwd();
			return;
		}
		resolve_compiled_game();
	}

	void install_hooks()
	{
		if (g_hooks) {
			return;
		}
		HMODULE k32 = GetModuleHandleA("kernel32.dll");
		if (!k32) {
			return;
		}
		bool ok = false;
		FARPROC fw = GetProcAddress(k32, "CreateFileW");
		if (fw && shared::utils::hook::detour(reinterpret_cast<DWORD>(fw),
			reinterpret_cast<void*>(CreateFileW_hk),
			reinterpret_cast<void**>(&CreateFileW_og)))
		{
			ok = true;
		}
		FARPROC fa = GetProcAddress(k32, "CreateFileA");
		if (fa && shared::utils::hook::detour(reinterpret_cast<DWORD>(fa),
			reinterpret_cast<void*>(CreateFileA_hk),
			reinterpret_cast<void**>(&CreateFileA_og)))
		{
			ok = true;
		}
		g_hooks = ok;
	}

	void set_on_before_switch(void (*cb)())
	{
		g_before = cb;
	}

	void set_on_after_switch(void (*cb)())
	{
		g_after = cb;
	}
}
