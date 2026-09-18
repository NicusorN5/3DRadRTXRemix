#include "std_include.hpp"
#include "compiler_inject.hpp"
#include "project_file.hpp"
#include <commctrl.h>
#include <shlobj.h>
#include <mutex>
#include <thread>
#include <deque>
#include <vector>
#include <cctype>

namespace comp::compiler_inject
{
	namespace fs = std::filesystem;

	static std::mutex g_mu;
	static std::deque<fs::path> g_queue;
	static std::set<std::wstring> g_done;
	static std::set<std::wstring> g_logged_dest;
	static fs::path g_last_copy_dir;
	static volatile LONG g_worker_started = 0;

	static std::wstring lower_copy(std::wstring s)
	{
		for (auto& c : s) {
			c = static_cast<wchar_t>(::towlower(c));
		}
		return s;
	}

	static std::wstring norm_key(const fs::path& p)
	{
		std::error_code ec;
		const auto abs = fs::absolute(p, ec);
		return lower_copy((ec ? p : abs).wstring());
	}

	static std::string narrow_path(const fs::path& p)
	{
		const std::wstring w = p.wstring();
		if (w.empty()) {
			return {};
		}
		const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (n <= 1) {
			return {};
		}
		std::string s(static_cast<std::size_t>(n - 1), '\0');
		WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
		return s;
	}

	static bool is_console_hwnd(HWND hwnd)
	{
		char title[128]{};
		GetWindowTextA(hwnd, title, 128);
		return std::strstr(title, "RTX-Comp") != nullptr;
	}

	static bool class_is_list(const char* cls)
	{
		if (!cls || !cls[0]) {
			return false;
		}
		return std::strcmp(cls, "SysListView32") == 0 ||
			_stricmp(cls, "ListBox") == 0;
	}

	static std::string stem_from_project_row(const char* text)
	{
		if (!text || !text[0]) {
			return {};
		}
		const char* p = text;
		while (*p == ' ' || *p == '\t') {
			++p;
		}
		if (std::isdigit(static_cast<unsigned char>(p[0])) &&
			std::isdigit(static_cast<unsigned char>(p[1])) &&
			std::isdigit(static_cast<unsigned char>(p[2])) &&
			std::isdigit(static_cast<unsigned char>(p[3])) &&
			std::isdigit(static_cast<unsigned char>(p[4])) &&
			p[5] && !std::isdigit(static_cast<unsigned char>(p[5])))
		{
			p += 5;
		}
		std::string s(p);
		while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
			s.pop_back();
		}
		if (s.size() > 4)
		{
			const char* ext = s.c_str() + s.size() - 4;
			if (_stricmp(ext, ".3dr") == 0 || _stricmp(ext, ".ini") == 0) {
				s.resize(s.size() - 4);
			}
		}
		if (s.empty() || _stricmp(s.c_str(), "Untitled") == 0) {
			return {};
		}
		return s;
	}

	static fs::path projects_dir()
	{
		return fs::path(shared::globals::root_path) / "3DRad_res" / "projects";
	}

	static bool project_3dr_exists(const std::string& stem)
	{
		if (stem.empty()) {
			return false;
		}
		std::error_code ec;
		return fs::exists(projects_dir() / (stem + ".3dr"), ec);
	}

	static int list_project_hits(HWND hwnd)
	{
		char cls[64]{};
		GetClassNameA(hwnd, cls, 64);
		int hits = 0;
		if (_stricmp(cls, "ListBox") == 0)
		{
			const int n = static_cast<int>(SendMessageA(hwnd, LB_GETCOUNT, 0, 0));
			for (int i = 0; i < n && i < 512; i++)
			{
				char text[256]{};
				if (SendMessageA(hwnd, LB_GETTEXT, i, reinterpret_cast<LPARAM>(text)) <= 0) {
					continue;
				}
				const auto stem = stem_from_project_row(text);
				if (project_3dr_exists(stem)) {
					++hits;
				}
			}
			return hits;
		}
		if (std::strcmp(cls, "SysListView32") == 0)
		{
			const int n = static_cast<int>(SendMessageA(hwnd, LVM_GETITEMCOUNT, 0, 0));
			for (int i = 0; i < n && i < 512; i++)
			{
				char text[256]{};
				LVITEMA item{};
				item.mask = LVIF_TEXT;
				item.iItem = i;
				item.pszText = text;
				item.cchTextMax = 256;
				SendMessageA(hwnd, LVM_GETITEMTEXTA, i, reinterpret_cast<LPARAM>(&item));
				const auto stem = stem_from_project_row(text);
				if (project_3dr_exists(stem)) {
					++hits;
				}
			}
			return hits;
		}
		return 0;
	}

	struct list_scan
	{
		HWND best = nullptr;
		int best_hits = 0;
	};

	static BOOL CALLBACK enum_project_list_child(HWND hwnd, LPARAM lp)
	{
		auto* ctx = reinterpret_cast<list_scan*>(lp);
		char cls[64]{};
		GetClassNameA(hwnd, cls, 64);
		if (!class_is_list(cls)) {
			return TRUE;
		}
		const int hits = list_project_hits(hwnd);
		if (hits > ctx->best_hits) {
			ctx->best_hits = hits;
			ctx->best = hwnd;
		}
		return TRUE;
	}

	static BOOL CALLBACK enum_project_list_top(HWND hwnd, LPARAM lp)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || is_console_hwnd(hwnd)) {
			return TRUE;
		}
		EnumChildWindows(hwnd, enum_project_list_child, lp);
		enum_project_list_child(hwnd, lp);
		return TRUE;
	}

	static HWND find_compiler_project_list()
	{
		list_scan ctx{};
		EnumWindows(enum_project_list_top, reinterpret_cast<LPARAM>(&ctx));
		return (ctx.best_hits > 0) ? ctx.best : nullptr;
	}

	static void collect_list_stems(HWND list, std::vector<std::string>& out)
	{
		if (!list) {
			return;
		}
		char cls[64]{};
		GetClassNameA(list, cls, 64);
		auto push = [&](const char* text)
		{
			const auto stem = stem_from_project_row(text);
			if (stem.empty()) {
				return;
			}
			for (const auto& e : out) {
				if (_stricmp(e.c_str(), stem.c_str()) == 0) {
					return;
				}
			}
			out.push_back(stem);
		};
		if (_stricmp(cls, "ListBox") == 0)
		{
			const int n = static_cast<int>(SendMessageA(list, LB_GETCOUNT, 0, 0));
			for (int i = 0; i < n && i < 512; i++)
			{
				char text[256]{};
				if (SendMessageA(list, LB_GETTEXT, i, reinterpret_cast<LPARAM>(text)) > 0) {
					push(text);
				}
			}
			return;
		}
		if (std::strcmp(cls, "SysListView32") == 0)
		{
			const int n = static_cast<int>(SendMessageA(list, LVM_GETITEMCOUNT, 0, 0));
			for (int i = 0; i < n && i < 512; i++)
			{
				char text[256]{};
				LVITEMA item{};
				item.mask = LVIF_TEXT;
				item.iItem = i;
				item.pszText = text;
				item.cchTextMax = 256;
				SendMessageA(list, LVM_GETITEMTEXTA, i, reinterpret_cast<LPARAM>(&item));
				if (text[0]) {
					push(text);
				}
			}
		}
	}

	static std::string stem_from_compiled_folder(const fs::path& dest)
	{
		auto name = dest.filename().wstring();
		if (name.empty() && dest.has_parent_path()) {
			name = dest.parent_path().filename().wstring();
		}
		std::string s = narrow_path(name);
		// compiledProject\scary_YYYYMMDDHHMMSS
		if (s.size() > 15)
		{
			const auto under = s.rfind('_');
			if (under != std::string::npos && s.size() - under == 15)
			{
				bool digits = true;
				for (std::size_t i = under + 1; i < s.size(); i++) {
					if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
						digits = false;
						break;
					}
				}
				if (digits) {
					s.resize(under);
				}
			}
		}
		return s;
	}

	static void collect_compile_stems(const fs::path& dest, std::vector<std::string>& stems)
	{
		HWND list = find_compiler_project_list();
		if (list) {
			collect_list_stems(list, stems);
			shared::common::log("Compile",
				std::format("project-list hwnd=0x{:X} rows={}",
					reinterpret_cast<std::uintptr_t>(list),
					static_cast<int>(stems.size())),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}
		else {
			shared::common::log("Compile",
				"compiler project-list not found - using folder / lastProject fallbacks",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}

		auto push = [&](const std::string& stem)
		{
			if (stem.empty()) {
				return;
			}
			for (const auto& e : stems) {
				if (_stricmp(e.c_str(), stem.c_str()) == 0) {
					return;
				}
			}
			stems.push_back(stem);
		};

		push(stem_from_compiled_folder(dest));

		char last[MAX_PATH]{};
		const auto last_txt = fs::path(shared::globals::root_path) /
			"3DRad_res" / "system" / "lastProject.txt";
		if (FILE* f = nullptr; fopen_s(&f, narrow_path(last_txt).c_str(), "r") == 0 && f)
		{
			if (fgets(last, MAX_PATH, f))
			{
				char* nl = std::strpbrk(last, "\r\n");
				if (nl) {
					*nl = 0;
				}
				const char* slash = std::strrchr(last, '\\');
				const char* name = slash ? slash + 1 : last;
				push(stem_from_project_row(name));
			}
			fclose(f);
		}
	}

	static bool is_install_root(const fs::path& dir)
	{
		return fs::exists(dir / "3DRad.exe") && fs::exists(dir / "3DRad_compiler.exe");
	}

	static bool looks_like_compiled_game(const fs::path& dir)
	{
		std::error_code ec;
		if (!fs::is_directory(dir, ec) || is_install_root(dir)) {
			return false;
		}
		if (!fs::exists(dir / "dll3impact.dll", ec)) {
			return false;
		}
		if (!fs::is_directory(dir / "3DRad_res", ec)) {
			return false;
		}

		for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
		{
			if (!it->is_regular_file(ec)) {
				continue;
			}
			const auto name = lower_copy(it->path().filename().wstring());
			if (name.size() < 4 || name.compare(name.size() - 4, 4, L".exe") != 0) {
				continue;
			}
			if (name == L"nvremixlauncher32.exe") {
				continue;
			}
			return true;
		}
		return false;
	}

	static bool already_injected(const fs::path& dir)
	{
		std::error_code ec;
		return fs::exists(dir / "d3d9_remix.dll", ec) && fs::exists(dir / "d3d9.dll", ec);
	}

	struct copy_stats
	{
		int copied = 0;
		int skipped = 0;
		int failed = 0;
	};

	static const char* trex_skip_reason(const fs::path& name)
	{
		const auto w = lower_copy(name.wstring());
		if (w.size() >= 4 && w.compare(w.size() - 4, 4, L".dmp") == 0) {
			return ".dmp dump";
		}
		if (w.find(L"dlss5") != std::wstring::npos) {
			return "dlss5";
		}
		if (w.rfind(L"d3d9.dll.", 0) == 0) {
			return "d3d9.dll.* backup";
		}
		if (w.rfind(L"nvremixbridge.exe_", 0) == 0) {
			return "NvRemixBridge dump";
		}
		return nullptr;
	}

	static void copy_file_logged(const fs::path& src, const fs::path& dst, copy_stats& st,
		const bool skip_if_exists = false)
	{
		std::error_code ec;
		if (skip_if_exists && fs::exists(dst, ec) && !ec) {
			++st.skipped;
			return;
		}
		if (!fs::exists(src, ec)) {
			++st.skipped;
			shared::common::log("Compile",
				std::format("skip missing {}", src.string()),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		fs::create_directories(dst.parent_path(), ec);
		fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
		if (ec) {
			++st.failed;
			shared::common::log("Compile",
				std::format("copy failed {} -> {} ({})",
					src.string(), dst.string(), ec.message()),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}
		++st.copied;
		shared::common::log("Compile",
			std::format("copied {} -> {}", src.filename().string(), dst.string()),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}

	static void copy_tree(const fs::path& src, const fs::path& dst, const bool skip_log_dirs, copy_stats& st)
	{
		std::error_code ec;
		if (!fs::exists(src, ec)) {
			++st.skipped;
			shared::common::log("Compile",
				std::format("skip missing dir {}", src.string()),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return;
		}
		fs::create_directories(dst, ec);
		shared::common::log("Compile",
			std::format("copying dir {} -> {}", src.string(), dst.string()),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		for (fs::directory_iterator it(src, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
		{
			const auto name = it->path().filename();
			const auto wname = lower_copy(name.wstring());
			if (skip_log_dirs && (wname == L"captures" || wname == L"logs")) {
				++st.skipped;
				shared::common::log("Compile",
					std::format("skip dir {} (captures/logs)", (dst / name).string()),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				continue;
			}
			if (it->is_symlink(ec) || (GetFileAttributesW(it->path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))
			{
				++st.skipped;
				shared::common::log("Compile",
					std::format("skip symlink {}", name.string()),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				continue;
			}
			if (it->is_directory(ec)) {
				copy_tree(it->path(), dst / name, skip_log_dirs, st);
				continue;
			}
			if (!it->is_regular_file(ec)) {
				++st.skipped;
				shared::common::log("Compile",
					std::format("skip non-file {}", name.string()),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				continue;
			}
			if (const auto* why = trex_skip_reason(name)) {
				++st.skipped;
				shared::common::log("Compile",
					std::format("skip {} ({})", name.string(), why),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				continue;
			}
			fs::copy_file(it->path(), dst / name, fs::copy_options::overwrite_existing, ec);
			if (ec) {
				++st.failed;
				shared::common::log("Compile",
					std::format("copy failed {}: {}", name.string(), ec.message()),
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				continue;
			}
			++st.copied;
			shared::common::log("Compile",
				std::format("copied {}", (dst / name).string()),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}
	}

	static void copy_project_inis(const fs::path& dest, copy_stats& st)
	{
		std::vector<std::string> stems;
		collect_compile_stems(dest, stems);

		const fs::path src_projects = projects_dir();
		const fs::path dst_projects = dest / "3DRad_res" / "projects";
		int copied_ini = 0;
		for (const auto& stem : stems)
		{
			const fs::path src = src_projects / (stem + ".ini");
			std::error_code ec;
			if (!fs::exists(src, ec)) {
				shared::common::log("Compile",
					std::format("no ini for list project '{}' ({})",
						stem, narrow_path(src)),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				continue;
			}
			copy_file_logged(src, dst_projects / (stem + ".ini"), st);
			copy_file_logged(src, dest / (stem + ".ini"), st);
			++copied_ini;
		}

		if (copied_ini == 0)
		{
			project_file::poll();
			if (const char* proj_ini = project_file::ini_path();
				proj_ini && proj_ini[0] && fs::exists(proj_ini))
			{
				const fs::path src(proj_ini);
				copy_file_logged(src, dst_projects / src.filename(), st);
				copy_file_logged(src, dest / src.filename(), st);
				++copied_ini;
			}
		}

		if (copied_ini == 0) {
			shared::common::log("Compile",
				"no scene-folder <project>.ini to copy",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}
		else {
			shared::common::log("Compile",
				std::format("copied {} project ini file(s) into {}",
					copied_ini, narrow_path(dst_projects)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}
	}

	static void inject(const fs::path& dest)
	{
		try
		{
		const auto src_root = fs::path(shared::globals::root_path);
		copy_stats st{};
		shared::common::log("Compile",
			std::format("injecting Remix runtime into {}", narrow_path(dest)),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

		char self[MAX_PATH]{};
		GetModuleFileNameA(shared::globals::dll_hmodule, self, MAX_PATH);
		if (self[0]) {
			copy_file_logged(self, dest / "d3d9.dll", st);
		}

		copy_file_logged(src_root / "d3d9_remix.dll", dest / "d3d9_remix.dll", st);
		copy_file_logged(src_root / "remix-comp-proxy.ini", dest / "remix-comp-proxy.ini", st);
		copy_file_logged(src_root / "dxvk.conf", dest / "dxvk.conf", st);
		copy_file_logged(src_root / "rtx.conf", dest / "rtx.conf", st, true);
		copy_file_logged(src_root / "user.conf", dest / "user.conf", st, true);
		copy_file_logged(src_root / "NvRemixLauncher32.exe", dest / "NvRemixLauncher32.exe", st);

		copy_project_inis(dest, st);

		if (fs::exists(src_root / ".trex")) {
			shared::common::log("Compile", "copying .trex runtime...",
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			copy_tree(src_root / ".trex", dest / ".trex", false, st);
		}
		else {
			++st.skipped;
			shared::common::log("Compile",
				std::format("skip missing dir {}", (src_root / ".trex").string()),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}
		if (fs::exists(src_root / "rtx-remix")) {
			copy_tree(src_root / "rtx-remix", dest / "rtx-remix", true, st);
		}

		const auto type = st.failed
			? shared::common::LOG_TYPE::LOG_TYPE_ERROR
			: shared::common::LOG_TYPE::LOG_TYPE_GREEN;
		shared::common::log("Compile",
			std::format("inject {}: {} copied, {} skipped, {} failed - {}",
				st.failed ? "finished with errors" : "ready",
				st.copied, st.skipped, st.failed, narrow_path(dest)),
			type, true);
		}
		catch (const std::exception& e)
		{
			shared::common::log("Compile",
				std::format("inject aborted: {}", e.what()),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
		catch (...)
		{
			shared::common::log("Compile",
				"inject aborted (unknown exception)",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
	}

	static void log_dest_once(const fs::path& dir, const char* how)
	{
		if (dir.empty()) {
			return;
		}
		const auto key = norm_key(dir);
		{
			std::lock_guard lock(g_mu);
			if (!g_logged_dest.insert(key).second) {
				return;
			}
		}
		shared::common::log("Compile",
			std::format("compile dest ({}): {}", how, narrow_path(dir)),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}

	static void enqueue(fs::path dir)
	{
		std::error_code ec;
		if (dir.empty()) {
			return;
		}
		if (fs::is_regular_file(dir, ec)) {
			dir = dir.parent_path();
		}
		dir = fs::absolute(dir, ec);
		if (ec) {
			return;
		}

		const auto key = norm_key(dir);
		if (already_injected(dir))
		{
			bool first = false;
			{
				std::lock_guard lock(g_mu);
				first = g_done.insert(key).second;
			}
			if (first)
			{
				copy_stats st{};
				char self[MAX_PATH]{};
				GetModuleFileNameA(shared::globals::dll_hmodule, self, MAX_PATH);
				if (self[0]) {
					copy_file_logged(self, dir / "d3d9.dll", st);
				}
				const auto src_root = fs::path(shared::globals::root_path);
				copy_file_logged(src_root / "d3d9_remix.dll", dir / "d3d9_remix.dll", st);
				shared::common::log("Compile",
					std::format("refresh proxy d3d9.dll (runtime already present, rtx.conf kept): {} ({} copied)",
						dir.string(), st.copied),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
			return;
		}
		if (!looks_like_compiled_game(dir)) {
			return;
		}

		{
			std::lock_guard lock(g_mu);
			if (g_done.contains(key)) {
				return;
			}
			for (const auto& q : g_queue) {
				if (norm_key(q) == key) {
					return;
				}
			}
			g_queue.push_back(dir);
		}
		shared::common::log("Compile",
			std::format("queued Remix inject: {}", dir.string()),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}

	static void scan_parent(const fs::path& parent)
	{
		std::error_code ec;
		if (!fs::is_directory(parent, ec)) {
			return;
		}
		for (fs::directory_iterator it(parent, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
		{
			if (it->is_directory(ec)) {
				enqueue(it->path());
			}
		}
	}

	static fs::path desktop_dir()
	{
		wchar_t buf[MAX_PATH]{};
		if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, buf))) {
			return buf;
		}
		return {};
	}

	static void drain_queue()
	{
		for (;;)
		{
			fs::path job;
			{
				std::lock_guard lock(g_mu);
				if (g_queue.empty()) {
					return;
				}
				job = g_queue.front();
				g_queue.pop_front();
			}
			if (already_injected(job)) {
				shared::common::log("Compile",
					std::format("skip inject (already has d3d9.dll + d3d9_remix.dll): {}", job.string()),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				std::lock_guard lock(g_mu);
				g_done.insert(norm_key(job));
				continue;
			}
			if (!looks_like_compiled_game(job)) {
				shared::common::log("Compile",
					std::format("skip inject (not a compiled-game folder yet): {}", job.string()),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				std::lock_guard lock(g_mu);
				g_done.insert(norm_key(job));
				continue;
			}
			inject(job);
			std::lock_guard lock(g_mu);
			g_done.insert(norm_key(job));
		}
	}

	static bool text_is_compile_done(const wchar_t* a, const wchar_t* b)
	{
		auto has = [](const wchar_t* s) {
			return s && wcsstr(s, L"Stand-alone executable generation completed");
		};
		return has(a) || has(b);
	}

	static bool text_is_compile_done_a(const char* a, const char* b)
	{
		auto has = [](const char* s) {
			return s && strstr(s, "Stand-alone executable generation completed");
		};
		return has(a) || has(b);
	}

	using SHFileOperationW_t = int (WINAPI*)(LPSHFILEOPSTRUCTW);
	using MessageBoxW_t = int (WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
	using MessageBoxA_t = int (WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
	static SHFileOperationW_t SHFileOperationW_og = nullptr;
	static MessageBoxW_t MessageBoxW_og = nullptr;
	static MessageBoxA_t MessageBoxA_og = nullptr;

	static fs::path first_path_list(LPCWSTR list)
	{
		if (!list || !list[0]) {
			return {};
		}
		return list;
	}

	static int WINAPI SHFileOperationW_hk(LPSHFILEOPSTRUCTW op)
	{
		const int r = SHFileOperationW_og ? SHFileOperationW_og(op) : 0;
		if (op && op->wFunc == FO_COPY && r == 0)
		{
			try
			{
				auto dest = first_path_list(op->pTo);
				if (!dest.empty())
				{
					std::error_code ec;
					if (fs::is_regular_file(dest, ec)) {
						dest = dest.parent_path();
					}
					{
						std::lock_guard lock(g_mu);
						g_last_copy_dir = dest;
					}
					log_dest_once(dest, "SHFileOperation");
					enqueue(dest);
				}
			}
			catch (...) {
				shared::common::log("Compile",
					"SHFileOperation watcher threw",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
		}
		return r;
	}

	static void on_compile_success_dialog()
	{
		fs::path last;
		{
			std::lock_guard lock(g_mu);
			last = g_last_copy_dir;
		}
		shared::common::log("Compile",
			"compile success dialog — scanning dest / compiledProject / Desktop",
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		if (!last.empty()) {
			log_dest_once(last, "success dialog");
		}
		else {
			shared::common::log("Compile",
				"no SHFileOperation dest yet — scanning default folders",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}
		enqueue(last);
		scan_parent(fs::path(shared::globals::root_path) / "3DRad_res" / "compiledProject");
		scan_parent(desktop_dir());
	}

	static bool text_is_crt_abort_w(LPCWSTR a, LPCWSTR b)
	{
		auto has = [](LPCWSTR s) {
			return s && (wcsstr(s, L"Runtime Error") ||
				wcsstr(s, L"terminate it in an unusual way"));
		};
		return has(a) || has(b);
	}

	static bool text_is_crt_abort_a(LPCSTR a, LPCSTR b)
	{
		auto has = [](LPCSTR s) {
			return s && (strstr(s, "Runtime Error") ||
				strstr(s, "terminate it in an unusual way"));
		};
		return has(a) || has(b);
	}

	static int WINAPI MessageBoxW_hk(HWND hwnd, LPCWSTR text, LPCWSTR caption, UINT type)
	{
		if (!text_is_crt_abort_w(text, caption) && text_is_compile_done(text, caption)) {
			try {
				on_compile_success_dialog();
			}
			catch (...) {
				shared::common::log("Compile",
					"compile-success handler threw",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
		}
		return MessageBoxW_og ? MessageBoxW_og(hwnd, text, caption, type) : 0;
	}

	static int WINAPI MessageBoxA_hk(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type)
	{
		if (!text_is_crt_abort_a(text, caption) && text_is_compile_done_a(text, caption)) {
			try {
				on_compile_success_dialog();
			}
			catch (...) {
				shared::common::log("Compile",
					"compile-success handler threw",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
		}
		return MessageBoxA_og ? MessageBoxA_og(hwnd, text, caption, type) : 0;
	}

	static void install_hooks()
	{
		if (const auto st = MH_Initialize();
			st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
		{
			shared::common::log("Compile",
				std::format("MinHook init failed: {}", static_cast<int>(st)),
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}

		const auto shell = GetProcAddress(GetModuleHandleA("shell32.dll"), "SHFileOperationW");
		const auto mbw = GetProcAddress(GetModuleHandleA("user32.dll"), "MessageBoxW");
		const auto mba = GetProcAddress(GetModuleHandleA("user32.dll"), "MessageBoxA");

		if (shell) {
			shared::utils::hook::detour(reinterpret_cast<DWORD>(shell), SHFileOperationW_hk,
				reinterpret_cast<void**>(&SHFileOperationW_og));
			shared::common::log("Compile", "hooked SHFileOperationW (compile dest watcher)");
		}
		else {
			shared::common::log("Compile", "SHFileOperationW not found",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
		if (mbw) {
			shared::utils::hook::detour(reinterpret_cast<DWORD>(mbw), MessageBoxW_hk,
				reinterpret_cast<void**>(&MessageBoxW_og));
			shared::common::log("Compile", "hooked MessageBoxW (compile success dialog)");
		}
		else {
			shared::common::log("Compile", "MessageBoxW not found",
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}
		if (mba) {
			shared::utils::hook::detour(reinterpret_cast<DWORD>(mba), MessageBoxA_hk,
				reinterpret_cast<void**>(&MessageBoxA_og));
			shared::common::log("Compile", "hooked MessageBoxA (compile success dialog)");
		}

		shared::common::log("Compile",
			"inject watcher started — Remix runtime will be copied into output folders.",
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}

	static DWORD WINAPI worker(LPVOID)
	{
		install_hooks();

		const auto compiled = fs::path(shared::globals::root_path) / "3DRad_res" / "compiledProject";
		const auto desktop = desktop_dir();
		shared::common::log("Compile",
			std::format("watching {} and {}", compiled.string(), desktop.string()),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

		for (;;)
		{
			try
			{
				scan_parent(compiled);
				scan_parent(desktop);
				{
					std::lock_guard lock(g_mu);
					if (!g_last_copy_dir.empty()) {
						enqueue(g_last_copy_dir);
					}
				}
				drain_queue();
			}
			catch (const std::exception& e)
			{
				shared::common::log("Compile",
					std::format("inject watcher exception: {}", e.what()),
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
			catch (...)
			{
				shared::common::log("Compile",
					"inject watcher exception (unknown)",
					shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}
			Sleep(750);
		}
	}

	void start()
	{
		if (InterlockedCompareExchange(&g_worker_started, 1, 0) != 0) {
			shared::common::log("Compile", "inject watcher already running");
			return;
		}
		if (const auto t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr); t) {
			shared::common::log("Compile", "inject watcher thread created");
			CloseHandle(t);
		}
		else {
			shared::common::log("Compile", "failed to start inject watcher thread",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
	}
}
