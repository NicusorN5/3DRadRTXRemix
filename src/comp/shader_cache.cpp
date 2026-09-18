#include "std_include.hpp"
#include "shader_cache.hpp"

namespace comp::shader_cache
{
	namespace
	{
		constexpr wchar_t k_wiped_env[] = L"RTX_COMP_CACHE_WIPED";

		bool is_dot_dir(const wchar_t* name)
		{
			return name && name[0] == L'.' &&
				(name[1] == 0 || (name[1] == L'.' && name[2] == 0));
		}

		void log_line(const wchar_t* install_root, const char* msg)
		{
			if (!msg) {
				return;
			}
			OutputDebugStringA("RtxCompClearShaders: ");
			OutputDebugStringA(msg);
			OutputDebugStringA("\n");
			if (!install_root || !install_root[0]) {
				return;
			}
			wchar_t dir[MAX_PATH]{};
			wchar_t path[MAX_PATH]{};
			if (swprintf_s(dir, L"%s\\rtx_comp", install_root) < 0 ||
				swprintf_s(path, L"%s\\clear_shaders.log", dir) < 0)
			{
				return;
			}
			CreateDirectoryW(dir, nullptr);
			HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
				OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				return;
			}
			DWORD nw = 0;
			WriteFile(h, msg, static_cast<DWORD>(strlen(msg)), &nw, nullptr);
			WriteFile(h, "\r\n", 2, &nw, nullptr);
			CloseHandle(h);
		}

		void wipe_tree(const wchar_t* dir, stats& st, int depth)
		{
			if (!dir || !dir[0] || depth > 24) {
				return;
			}
			wchar_t spec[MAX_PATH]{};
			if (swprintf_s(spec, L"%s\\*", dir) < 0) {
				return;
			}
			WIN32_FIND_DATAW fd{};
			HANDLE h = FindFirstFileW(spec, &fd);
			if (h == INVALID_HANDLE_VALUE) {
				return;
			}
			do
			{
				if (is_dot_dir(fd.cFileName)) {
					continue;
				}
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
					continue;
				}
				wchar_t child[MAX_PATH]{};
				if (swprintf_s(child, L"%s\\%s", dir, fd.cFileName) < 0) {
					continue;
				}
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				{
					wipe_tree(child, st, depth + 1);
					SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
					if (RemoveDirectoryW(child)) {
						++st.dirs_removed;
					}
					else if (GetLastError() != ERROR_DIR_NOT_EMPTY) {
						++st.failed;
					}
					else {
						++st.failed;
					}
				}
				else
				{
					SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
					if (DeleteFileW(child)) {
						++st.deleted;
					}
					else {
						++st.failed;
					}
				}
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}

		void wipe_dxvk(const wchar_t* dir, stats& st)
		{
			if (!dir || !dir[0]) {
				return;
			}
			wchar_t spec[MAX_PATH]{};
			if (swprintf_s(spec, L"%s\\*.dxvk-cache", dir) < 0) {
				return;
			}
			WIN32_FIND_DATAW fd{};
			HANDLE h = FindFirstFileW(spec, &fd);
			if (h == INVALID_HANDLE_VALUE) {
				return;
			}
			do
			{
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					continue;
				}
				wchar_t child[MAX_PATH]{};
				if (swprintf_s(child, L"%s\\%s", dir, fd.cFileName) < 0) {
					continue;
				}
				SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
				if (DeleteFileW(child)) {
					++st.deleted;
				}
				else {
					++st.failed;
				}
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}

		void nvidia_cache_dirs(wchar_t gl[MAX_PATH], wchar_t dx[MAX_PATH])
		{
			wchar_t local[MAX_PATH]{};
			GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
			swprintf_s(gl, MAX_PATH, L"%s\\NVIDIA\\GLCache", local);
			swprintf_s(dx, MAX_PATH, L"%s\\NVIDIA\\DXCache", local);
		}

		void try_remove_and_recreate(const wchar_t* dir, stats& st)
		{
			if (!dir || !dir[0]) {
				return;
			}
			SetFileAttributesW(dir, FILE_ATTRIBUTE_NORMAL);
			if (RemoveDirectoryW(dir)) {
				++st.dirs_removed;
			}
			CreateDirectoryW(dir, nullptr);
		}

		void skip_spaces(const char*& p)
		{
			while (p && *p == ' ') {
				++p;
			}
		}

		bool take_token(const char*& p, char* out, int cap)
		{
			if (!out || cap <= 0) {
				return false;
			}
			out[0] = 0;
			skip_spaces(p);
			if (!p || !*p) {
				return false;
			}
			if (*p == '"')
			{
				++p;
				int i = 0;
				while (*p && *p != '"' && i + 1 < cap) {
					out[i++] = *p++;
				}
				out[i] = 0;
				if (*p == '"') {
					++p;
				}
				return i > 0;
			}
			int i = 0;
			while (*p && *p != ' ' && i + 1 < cap) {
				out[i++] = *p++;
			}
			out[i] = 0;
			return i > 0;
		}

		void wait_for_pid(DWORD pid)
		{
			if (!pid) {
				return;
			}
			HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
			if (h)
			{
				WaitForSingleObject(h, INFINITE);
				CloseHandle(h);
			}
			for (int i = 0; i < 40; ++i)
			{
				HANDLE alive = OpenProcess(SYNCHRONIZE, FALSE, pid);
				if (!alive) {
					break;
				}
				const DWORD wait = WaitForSingleObject(alive, 250);
				CloseHandle(alive);
				if (wait != WAIT_TIMEOUT) {
					break;
				}
			}
			Sleep(400);
		}

		void relaunch(const wchar_t* exe, const wchar_t* cwd)
		{
			if (!exe || !exe[0]) {
				return;
			}
			SetEnvironmentVariableW(k_wiped_env, L"1");
			SetEnvironmentVariableW(L"RTX_COMP_EDITOR_WARMED", nullptr);

			wchar_t cmd[MAX_PATH * 2]{};
			swprintf_s(cmd, L"\"%s\"", exe);

			STARTUPINFOW si{};
			si.cb = sizeof(si);
			PROCESS_INFORMATION pi{};
			if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr,
				(cwd && cwd[0]) ? cwd : nullptr, &si, &pi))
			{
				return;
			}
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
	}

	bool host_is_wipe_helper()
	{
		char path[MAX_PATH]{};
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		const char* slash = strrchr(path, '\\');
		const char* leaf = slash ? slash + 1 : path;
		if (_stricmp(leaf, "rundll32.exe") == 0) {
			return true;
		}
		char env[8]{};
		return GetEnvironmentVariableA("RTX_COMP_CACHE_WIPE_HELPER", env, 8) > 0;
	}

	stats wipe_now(const wchar_t* install_root)
	{
		stats st{};
		wchar_t gl[MAX_PATH]{};
		wchar_t dx[MAX_PATH]{};
		nvidia_cache_dirs(gl, dx);
		wipe_tree(gl, st, 0);
		wipe_tree(dx, st, 0);
		try_remove_and_recreate(gl, st);
		try_remove_and_recreate(dx, st);
		if (install_root && install_root[0])
		{
			wipe_dxvk(install_root, st);
			wchar_t trex[MAX_PATH]{};
			if (swprintf_s(trex, L"%s\\.trex", install_root) >= 0) {
				wipe_dxvk(trex, st);
			}
		}
		return st;
	}

	bool spawn_post_exit_helper(DWORD pid, const wchar_t* relaunch_exe,
		const wchar_t* cwd, const wchar_t* install_root)
	{
		if (!pid || !relaunch_exe || !relaunch_exe[0]) {
			return false;
		}

		wchar_t sys[MAX_PATH]{};
		GetSystemDirectoryW(sys, MAX_PATH);
		wchar_t rundll[MAX_PATH]{};
		if (swprintf_s(rundll, L"%s\\rundll32.exe", sys) < 0) {
			return false;
		}

		wchar_t dll[MAX_PATH]{};
		if (!GetModuleFileNameW(shared::globals::dll_hmodule, dll, MAX_PATH) || !dll[0]) {
			return false;
		}

		const wchar_t* work = (cwd && cwd[0]) ? cwd : L"";
		const wchar_t* root = (install_root && install_root[0]) ? install_root : L"";

		wchar_t cmd[4096]{};
		if (swprintf_s(cmd,
			L"\"%s\" \"%s\",RtxCompClearShadersWipe %lu \"%s\" \"%s\" \"%s\"",
			rundll, dll, static_cast<unsigned long>(pid), relaunch_exe, work, root) < 0)
		{
			return false;
		}

		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION pi{};
		if (!CreateProcessW(rundll, cmd, nullptr, nullptr, FALSE,
			DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
			nullptr, nullptr, &si, &pi))
		{
			return false;
		}
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		return true;
	}

	void run_exported_wipe(const char* cmd)
	{
		DWORD pid = 0;
		char exe_a[MAX_PATH]{};
		char cwd_a[MAX_PATH]{};
		char root_a[MAX_PATH]{};
		const char* p = cmd ? cmd : "";
		char pid_tok[32]{};
		if (take_token(p, pid_tok, 32)) {
			pid = static_cast<DWORD>(strtoul(pid_tok, nullptr, 10));
		}
		take_token(p, exe_a, MAX_PATH);
		take_token(p, cwd_a, MAX_PATH);
		take_token(p, root_a, MAX_PATH);

		wchar_t exe[MAX_PATH]{};
		wchar_t cwd[MAX_PATH]{};
		wchar_t root[MAX_PATH]{};
		MultiByteToWideChar(CP_ACP, 0, exe_a, -1, exe, MAX_PATH);
		MultiByteToWideChar(CP_ACP, 0, cwd_a, -1, cwd, MAX_PATH);
		MultiByteToWideChar(CP_ACP, 0, root_a, -1, root, MAX_PATH);

		char line[512]{};
		sprintf_s(line, "helper wait pid=%lu exe=%s root=%s",
			static_cast<unsigned long>(pid), exe_a, root_a);
		log_line(root, line);

		wait_for_pid(pid);

		stats last{};
		for (int attempt = 0; attempt < 48; ++attempt)
		{
			last = {};
			last = wipe_now(root);
			if (last.failed == 0) {
				break;
			}
			Sleep(250);
		}

		sprintf_s(line, "helper wipe deleted=%d failed=%d dirs=%d",
			last.deleted, last.failed, last.dirs_removed);
		log_line(root, line);

		relaunch(exe, cwd);
		log_line(root, "helper relaunched editor (no --rtx-comp-warmed)");
	}
}

extern "C" void CALLBACK RtxCompClearShadersWipe(HWND, HINSTANCE, LPSTR cmd, int)
{
	comp::shader_cache::run_exported_wipe(cmd);
}
