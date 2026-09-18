#include "std_include.hpp"
#include "runtime_update.hpp"

#include <winhttp.h>
#include <cctype>
#include <cstdint>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace comp::runtime_update
{
	namespace fs = std::filesystem;

	static std::string lower_copy(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

	static std::wstring utf8_to_wide(const std::string& s)
	{
		if (s.empty()) {
			return {};
		}
		const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
		if (n <= 0) {
			return std::wstring(s.begin(), s.end());
		}
		std::wstring out(static_cast<size_t>(n - 1), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
		return out;
	}

	static bool iequals(const std::string& a, const char* b)
	{
		return lower_copy(a) == lower_copy(b);
	}

	static fs::path install_root()
	{
		return fs::path(shared::globals::root_path);
	}

	static fs::path proxy_d3d9_path()
	{
		return install_root() / "d3d9.dll";
	}

	static bool is_proxy_d3d9(const fs::path& dest)
	{
		if (!iequals(dest.filename().string(), "d3d9.dll")) {
			return false;
		}
		return lower_copy(dest.parent_path().string()) == lower_copy(install_root().string());
	}

	enum class pe_machine { unknown, i386, amd64 };

	static pe_machine read_pe_machine(const fs::path& path)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f) {
			return pe_machine::unknown;
		}
		IMAGE_DOS_HEADER dos{};
		f.read(reinterpret_cast<char*>(&dos), sizeof(dos));
		if (!f || dos.e_magic != IMAGE_DOS_SIGNATURE) {
			return pe_machine::unknown;
		}
		f.seekg(dos.e_lfanew);
		DWORD sig = 0;
		f.read(reinterpret_cast<char*>(&sig), sizeof(sig));
		if (!f || sig != IMAGE_NT_SIGNATURE) {
			return pe_machine::unknown;
		}
		IMAGE_FILE_HEADER fh{};
		f.read(reinterpret_cast<char*>(&fh), sizeof(fh));
		if (!f) {
			return pe_machine::unknown;
		}
		if (fh.Machine == IMAGE_FILE_MACHINE_I386) {
			return pe_machine::i386;
		}
		if (fh.Machine == IMAGE_FILE_MACHINE_AMD64) {
			return pe_machine::amd64;
		}
		return pe_machine::unknown;
	}

	static uintmax_t file_size_or_0(const fs::path& p)
	{
		std::error_code ec;
		const auto n = fs::file_size(p, ec);
		return ec ? 0 : n;
	}

	struct winhttp_handle
	{
		HINTERNET h = nullptr;
		winhttp_handle() = default;
		explicit winhttp_handle(HINTERNET hh) : h(hh) {}
		~winhttp_handle() { if (h) { WinHttpCloseHandle(h); } }
		winhttp_handle(const winhttp_handle&) = delete;
		winhttp_handle& operator=(const winhttp_handle&) = delete;
		winhttp_handle(winhttp_handle&& o) noexcept : h(o.h) { o.h = nullptr; }
		winhttp_handle& operator=(winhttp_handle&& o) noexcept
		{
			if (this != &o)
			{
				if (h) {
					WinHttpCloseHandle(h);
				}
				h = o.h;
				o.h = nullptr;
			}
			return *this;
		}
		explicit operator bool() const { return h != nullptr; }
	};

	static void last_winhttp_error(std::string& error, const char* what)
	{
		error = std::format("{} (WinHTTP 0x{:X})", what, GetLastError());
	}

	static bool http_fetch(const std::string& url, std::string* text_out, const fs::path* file_out,
		const status_fn& status, const char* label, std::string& error)
	{
		error.clear();
		if (url.empty() || (!text_out && !file_out))
		{
			error = "internal: empty download";
			return false;
		}

		const std::wstring wurl = utf8_to_wide(url);
		URL_COMPONENTS uc{};
		uc.dwStructSize = sizeof(uc);
		wchar_t host[256]{};
		wchar_t path[2048]{};
		wchar_t extra[1024]{};
		uc.lpszHostName = host;
		uc.dwHostNameLength = 256;
		uc.lpszUrlPath = path;
		uc.dwUrlPathLength = 2048;
		uc.lpszExtraInfo = extra;
		uc.dwExtraInfoLength = 1024;
		if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
		{
			last_winhttp_error(error, "Bad download URL");
			return false;
		}

		std::wstring full_path = path;
		full_path += extra;

		winhttp_handle session(WinHttpOpen(
			L"3DRadRTX-proxy/0.0.1",
			WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS,
			0));
		if (!session)
		{
			session = winhttp_handle(WinHttpOpen(
				L"3DRadRTX-proxy/0.0.1",
				WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
				WINHTTP_NO_PROXY_NAME,
				WINHTTP_NO_PROXY_BYPASS,
				0));
		}
		if (!session)
		{
			last_winhttp_error(error, "WinHttpOpen failed");
			return false;
		}

		DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
		protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
		WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
		WinHttpSetTimeouts(session.h, 30000, 30000, 30000, 600000);

		winhttp_handle connect(WinHttpConnect(session.h, host, uc.nPort, 0));
		if (!connect)
		{
			last_winhttp_error(error, "WinHttpConnect failed");
			return false;
		}

		DWORD flags = WINHTTP_FLAG_REFRESH;
		if (uc.nScheme == INTERNET_SCHEME_HTTPS) {
			flags |= WINHTTP_FLAG_SECURE;
		}

		winhttp_handle request(WinHttpOpenRequest(
			connect.h, L"GET", full_path.c_str(), nullptr,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
		if (!request)
		{
			last_winhttp_error(error, "WinHttpOpenRequest failed");
			return false;
		}

		DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
		WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

		std::wstring headers = L"User-Agent: 3DRadRTX-proxy/0.0.1\r\n";
		if (text_out) {
			headers += L"Accept: application/vnd.github+json\r\n";
		}
		else {
			headers += L"Accept: application/octet-stream\r\n";
		}

		if (!WinHttpSendRequest(request.h, headers.c_str(), static_cast<DWORD>(-1),
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
		{
			last_winhttp_error(error, "WinHttpSendRequest failed");
			return false;
		}
		if (!WinHttpReceiveResponse(request.h, nullptr))
		{
			last_winhttp_error(error, "WinHttpReceiveResponse failed");
			return false;
		}

		DWORD status_code = 0;
		DWORD status_size = sizeof(status_code);
		WinHttpQueryHeaders(request.h,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size, WINHTTP_NO_HEADER_INDEX);
		if (status_code < 200 || status_code > 299)
		{
			error = std::format("HTTP {} from {}", status_code, url);
			return false;
		}

		std::ofstream file;
		if (file_out)
		{
			fs::create_directories(file_out->parent_path());
			file.open(*file_out, std::ios::binary | std::ios::trunc);
			if (!file)
			{
				error = std::format("Could not write {}", file_out->string());
				return false;
			}
		}

		std::string text;
		std::vector<char> buf(64 * 1024);
		uintmax_t total = 0;
		uintmax_t last_report = 0;
		for (;;)
		{
			DWORD avail = 0;
			if (!WinHttpQueryDataAvailable(request.h, &avail))
			{
				last_winhttp_error(error, "WinHttpQueryDataAvailable failed");
				return false;
			}
			if (avail == 0) {
				break;
			}
			if (avail > buf.size()) {
				buf.resize(avail);
			}
			DWORD got = 0;
			if (!WinHttpReadData(request.h, buf.data(), avail, &got))
			{
				last_winhttp_error(error, "WinHttpReadData failed");
				return false;
			}
			if (got == 0) {
				break;
			}
			if (file_out) {
				file.write(buf.data(), static_cast<std::streamsize>(got));
			}
			else {
				text.append(buf.data(), got);
			}
			total += got;
			if (file_out && status && total - last_report >= 512 * 1024)
			{
				last_report = total;
				const double mb = static_cast<double>(total) / (1024.0 * 1024.0);
				status(std::format("{} {:.1f} MB…", label ? label : "Downloading", mb).c_str());
			}
		}

		if (file_out)
		{
			file.close();
			if (total == 0)
			{
				error = "Downloaded empty file";
				return false;
			}
		}
		else if (text_out)
		{
			*text_out = std::move(text);
		}
		return true;
	}

	struct gh_asset
	{
		std::string name;
		std::string url;
	};

	static std::vector<gh_asset> parse_download_assets(const std::string& json)
	{
		std::vector<gh_asset> out;
		size_t pos = 0;
		while ((pos = json.find("\"browser_download_url\"", pos)) != std::string::npos)
		{
			const size_t colon = json.find(':', pos);
			const size_t q1 = json.find('"', colon == std::string::npos ? pos : colon + 1);
			if (q1 == std::string::npos) {
				break;
			}
			const size_t q2 = json.find('"', q1 + 1);
			if (q2 == std::string::npos) {
				break;
			}
			gh_asset a;
			a.url = json.substr(q1 + 1, q2 - q1 - 1);
			const auto slash = a.url.find_last_of('/');
			a.name = slash == std::string::npos ? a.url : a.url.substr(slash + 1);
			out.push_back(std::move(a));
			pos = q2 + 1;
		}
		return out;
	}

	static bool pick_dxvk_asset(const std::vector<gh_asset>& assets, gh_asset& out)
	{
		for (const auto& a : assets)
		{
			const auto n = lower_copy(a.name);
			if (n.size() < 8) {
				continue;
			}
			if (n.rfind("dxvk-", 0) != 0) {
				continue;
			}
			if (n.find(".tar.gz") == std::string::npos) {
				continue;
			}
			if (n.find(".sha") != std::string::npos || n.find(".asc") != std::string::npos) {
				continue;
			}
			out = a;
			return true;
		}
		return false;
	}

	static bool pick_remix_asset(const std::vector<gh_asset>& assets, gh_asset& out)
	{
		auto is_release_zip = [](const std::string& n) {
			return n.find("release.zip") != std::string::npos &&
				n.find("debug") == std::string::npos &&
				n.find("symbol") == std::string::npos &&
				n.find("toolkit") == std::string::npos &&
				n.find("crc") == std::string::npos;
		};

		for (const auto& a : assets)
		{
			if (lower_copy(a.name) == "remix-1.5.2-release.zip")
			{
				out = a;
				return true;
			}
		}
		for (const auto& a : assets)
		{
			const auto n = lower_copy(a.name);
			if (n.find("remix-1.5.") != std::string::npos && is_release_zip(n))
			{
				out = a;
				return true;
			}
		}
		for (const auto& a : assets)
		{
			const auto n = lower_copy(a.name);
			if (n.rfind("remix-", 0) == 0 && is_release_zip(n))
			{
				out = a;
				return true;
			}
		}
		return false;
	}

	static bool github_json(const std::string& api_url, std::string& json, std::string& error, const status_fn& status)
	{
		if (status) {
			status("Contacting GitHub…");
		}
		return http_fetch(api_url, &json, nullptr, status, "GitHub", error);
	}

	static bool run_tar_extract(const fs::path& archive, const fs::path& dest, std::string& error)
	{
		std::error_code ec;
		fs::create_directories(dest, ec);

		wchar_t sys[MAX_PATH]{};
		GetSystemDirectoryW(sys, MAX_PATH);
		const std::wstring tar = std::wstring(sys) + L"\\tar.exe";
		if (GetFileAttributesW(tar.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			error = "Windows tar.exe not found (needed to extract DXVK/Remix archives)";
			return false;
		}

		std::wstring cmd = L"\"" + tar + L"\" -xf \"" + archive.wstring() + L"\" -C \"" + dest.wstring() + L"\"";
		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION pi{};
		std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
		cmdline.push_back(0);
		if (!CreateProcessW(tar.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, dest.c_str(), &si, &pi))
		{
			error = std::format("CreateProcess tar.exe failed (0x{:X})", GetLastError());
			return false;
		}
		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD code = 1;
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (code != 0)
		{
			error = std::format("tar.exe failed with exit code {}", code);
			return false;
		}
		return true;
	}

	static bool trex_has_usd(const fs::path& trex)
	{
		std::error_code ec;
		for (fs::directory_iterator it(trex, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
		{
			if (!it->is_regular_file(ec)) {
				continue;
			}
			const auto n = lower_copy(it->path().filename().string());
			if (n.rfind("usd_", 0) == 0 && n.size() >= 4 && n.compare(n.size() - 4, 4, ".dll") == 0) {
				return true;
			}
		}
		return false;
	}

	static bool name_is_d3d9(const fs::path& p)
	{
		return iequals(p.filename().string(), "d3d9.dll");
	}

	static bool path_has_trex(const fs::path& p)
	{
		for (const auto& part : p)
		{
			if (iequals(part.string(), ".trex")) {
				return true;
			}
		}
		return false;
	}

	static bool skip_trex_file(const fs::path& name)
	{
		const auto n = lower_copy(name.string());
		if (n.size() >= 4 && n.compare(n.size() - 4, 4, ".dmp") == 0) {
			return true;
		}
		if (n.find("dlss5") != std::string::npos) {
			return true;
		}
		if (n.rfind("d3d9.dll.", 0) == 0) {
			return true;
		}
		if (n.rfind("nvremixbridge.exe_", 0) == 0) {
			return true;
		}
		return false;
	}

	static void copy_tree_skip_junk(const fs::path& src, const fs::path& dst, std::string& error)
	{
		std::error_code ec;
		fs::create_directories(dst, ec);
		for (fs::recursive_directory_iterator it(src, fs::directory_options::skip_permission_denied, ec);
			!ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			const auto rel = fs::relative(it->path(), src, ec);
			if (ec) {
				continue;
			}
			if (it->is_directory(ec)) {
				fs::create_directories(dst / rel, ec);
				continue;
			}
			if (!it->is_regular_file(ec)) {
				continue;
			}
			if (skip_trex_file(it->path().filename())) {
				continue;
			}
			const auto out = dst / rel;
			if (is_proxy_d3d9(out))
			{
				error = "refusing to overwrite proxy d3d9.dll";
				return;
			}
			fs::create_directories(out.parent_path(), ec);
			fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				error = std::format("copy failed {} ({})", out.string(), ec.message());
				return;
			}
		}
	}

	static bool backup_trex_renderer(const fs::path& trex_d3d9)
	{
		std::error_code ec;
		if (!fs::exists(trex_d3d9, ec)) {
			return false;
		}
		const auto bak_once = trex_d3d9.parent_path() / "d3d9.dll.1.5.2.bak";
		if (!fs::exists(bak_once, ec))
		{
			fs::copy_file(trex_d3d9, bak_once, fs::copy_options::skip_existing, ec);
		}
		const auto bak = trex_d3d9.parent_path() / "d3d9.dll.bak";
		fs::copy_file(trex_d3d9, bak, fs::copy_options::overwrite_existing, ec);
		return !ec;
	}

	struct remix_layout
	{
		fs::path bridge;       // 32-bit interposer
		fs::path trex;         // .trex directory
		fs::path launcher;
		bool trex_d3d9_64 = false;
		bool has_usd = false;
		bool complete = false;
		std::string reason;
	};

	static remix_layout inspect_extracted(const fs::path& root)
	{
		remix_layout L;
		std::error_code ec;
		std::vector<fs::path> trex_dirs;
		std::vector<fs::path> d3d9s;
		std::vector<fs::path> launchers;

		for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
			!ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			if (it->is_directory(ec) && iequals(it->path().filename().string(), ".trex"))
			{
				trex_dirs.push_back(it->path());
			}
			else if (it->is_regular_file(ec))
			{
				if (name_is_d3d9(it->path())) {
					d3d9s.push_back(it->path());
				}
				if (iequals(it->path().filename().string(), "nvremixlauncher32.exe")) {
					launchers.push_back(it->path());
				}
			}
		}

		for (const auto& t : trex_dirs)
		{
			if (fs::exists(t / "NvRemixBridge.exe", ec) || fs::exists(t / "d3d9.dll", ec))
			{
				L.trex = t;
				break;
			}
		}
		if (L.trex.empty() && !trex_dirs.empty()) {
			L.trex = trex_dirs.front();
		}

		fs::path bridge;
		for (const auto& p : d3d9s)
		{
			if (path_has_trex(p)) {
				continue;
			}
			if (read_pe_machine(p) != pe_machine::i386) {
				continue;
			}
			if (file_size_or_0(p) > 40ull * 1024 * 1024) {
				continue;
			}
			bridge = p;
			break;
		}
		L.bridge = bridge;

		if (!launchers.empty()) {
			L.launcher = launchers.front();
		}

		if (!L.trex.empty())
		{
			const auto trex_d3d9 = L.trex / "d3d9.dll";
			L.trex_d3d9_64 = read_pe_machine(trex_d3d9) == pe_machine::amd64;
			L.has_usd = trex_has_usd(L.trex);
		}

		if (L.bridge.empty())
		{
			L.reason = "zip has no 32-bit Remix bridge (d3d9.dll outside .trex). Aborting Remix update — looks 64-bit-only.";
			return L;
		}
		if (L.trex.empty())
		{
			L.reason = "zip has no .trex renderer tree. Aborting Remix update.";
			return L;
		}
		if (!L.trex_d3d9_64 && fs::exists(L.trex / "d3d9.dll", ec))
		{
			L.reason = ".trex\\d3d9.dll is not 64-bit; refusing to mix it into the renderer tree.";
			return L;
		}

		L.complete = true;
		return L;
	}

	static bool install_dxvk_from_extract(const fs::path& extracted, std::string& error, const status_fn& status)
	{
		std::error_code ec;
		fs::path found;
		for (fs::recursive_directory_iterator it(extracted, fs::directory_options::skip_permission_denied, ec);
			!ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			if (!it->is_regular_file(ec) || !name_is_d3d9(it->path())) {
				continue;
			}
			const auto parent = lower_copy(it->path().parent_path().filename().string());
			if (parent == "x32" || parent == "win32" || parent == "x86")
			{
				found = it->path();
				break;
			}
		}
		if (found.empty())
		{
			error = "DXVK archive had no x32/d3d9.dll";
			return false;
		}
		if (read_pe_machine(found) != pe_machine::i386)
		{
			error = "DXVK d3d9.dll is not 32-bit";
			return false;
		}

		const auto dest = install_root() / "d3d9_dxvk.dll";
		if (is_proxy_d3d9(dest))
		{
			error = "internal: dxvk dest resolved to proxy";
			return false;
		}
		if (status) {
			status("Installing d3d9_dxvk.dll…");
		}
		fs::copy_file(found, dest, fs::copy_options::overwrite_existing, ec);
		if (ec)
		{
			error = std::format("Could not write d3d9_dxvk.dll ({})", ec.message());
			return false;
		}
		shared::common::log("Update",
			std::format("Installed vanilla DXVK 32-bit d3d9 → {}", dest.string()),
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		return true;
	}

	static bool try_local_dxvk(const status_fn& status)
	{
		const auto dest = install_root() / "d3d9_dxvk.dll";
		std::error_code ec;
		if (fs::exists(dest, ec) && read_pe_machine(dest) == pe_machine::i386) {
			return true;
		}

		const char* guesses[] = {
			"dxvk\\x32\\d3d9.dll",
			"x32\\d3d9.dll",
			"win32\\d3d9.dll",
		};
		for (const char* rel : guesses)
		{
			const auto src = install_root() / rel;
			if (!fs::exists(src, ec)) {
				continue;
			}
			if (read_pe_machine(src) != pe_machine::i386) {
				continue;
			}
			if (status) {
				status("Copying local 32-bit DXVK d3d9.dll…");
			}
			fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
			if (!ec) {
				return true;
			}
		}
		return false;
	}

	static bool update_dxvk(result& r, const status_fn& status)
	{
		if (status) {
			status("Downloading DXVK…");
		}

		std::string json, err;
		gh_asset asset;
		bool got = false;
		if (github_json("https://api.github.com/repos/doitsujin/dxvk/releases/latest", json, err, status))
		{
			got = pick_dxvk_asset(parse_download_assets(json), asset);
		}
		if (!got)
		{
			asset.name = "dxvk-3.1.tar.gz";
			asset.url = "https://github.com/doitsujin/dxvk/releases/download/v3.1/dxvk-3.1.tar.gz";
			shared::common::log("Update",
				std::format("GitHub DXVK API failed ({}); trying {}", err, asset.url),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}

		wchar_t tmp[MAX_PATH]{};
		GetTempPathW(MAX_PATH, tmp);
		const fs::path work = fs::path(tmp) / "3DRadRTX_update" / "dxvk";
		std::error_code ec;
		fs::remove_all(work, ec);
		fs::create_directories(work, ec);
		const fs::path archive = work / asset.name;
		const fs::path extracted = work / "out";

		if (status) {
			status(std::format("Downloading DXVK ({})…", asset.name).c_str());
		}
		if (!http_fetch(asset.url, nullptr, &archive, status, "Downloading DXVK…", err))
		{
			if (try_local_dxvk(status))
			{
				r.dxvk_ok = true;
				r.warning += "DXVK download failed; using existing/local d3d9_dxvk.dll. " + err + " ";
				return true;
			}
			r.error += "DXVK: " + err + " ";
			return false;
		}

		if (status) {
			status("Extracting DXVK…");
		}
		if (!run_tar_extract(archive, extracted, err))
		{
			r.error += "DXVK extract: " + err + " ";
			return false;
		}
		if (!install_dxvk_from_extract(extracted, err, status))
		{
			r.error += "DXVK: " + err + " ";
			return false;
		}
		fs::remove_all(work, ec);
		r.dxvk_ok = true;
		return true;
	}

	static bool update_remix(result& r, const status_fn& status)
	{
		if (status) {
			status("Downloading RTX Remix…");
		}

		std::string json, err;
		gh_asset asset;
		bool got = false;

		if (github_json("https://api.github.com/repos/NVIDIAGameWorks/rtx-remix/releases/tags/remix-1.5.2", json, err, status))
		{
			got = pick_remix_asset(parse_download_assets(json), asset);
		}
		if (!got)
		{
			json.clear();
			if (github_json("https://api.github.com/repos/NVIDIAGameWorks/rtx-remix/releases/latest", json, err, status))
			{
				got = pick_remix_asset(parse_download_assets(json), asset);
			}
		}
		if (!got)
		{
			asset.name = "remix-1.5.2-release.zip";
			asset.url = "https://github.com/NVIDIAGameWorks/rtx-remix/releases/download/remix-1.5.2/remix-1.5.2-release.zip";
			shared::common::log("Update",
				std::format("GitHub Remix API failed ({}); trying {}", err, asset.url),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
		}

		wchar_t tmp[MAX_PATH]{};
		GetTempPathW(MAX_PATH, tmp);
		const fs::path work = fs::path(tmp) / "3DRadRTX_update" / "remix";
		std::error_code ec;
		fs::remove_all(work, ec);
		fs::create_directories(work, ec);
		const fs::path archive = work / asset.name;
		const fs::path extracted = work / "out";

		if (status) {
			status(std::format("Downloading Remix ({})…", asset.name).c_str());
		}
		if (!http_fetch(asset.url, nullptr, &archive, status, "Downloading Remix…", err))
		{
			r.error += "Remix: " + err + " ";
			r.remix_skipped = true;
			return false;
		}

		if (status) {
			status("Extracting Remix…");
		}
		if (!run_tar_extract(archive, extracted, err))
		{
			r.error += "Remix extract: " + err + " ";
			r.remix_skipped = true;
			return false;
		}

		if (status) {
			status("Checking Remix layout…");
		}
		const auto layout = inspect_extracted(extracted);
		if (!layout.complete)
		{
			r.remix_skipped = true;
			r.warning += layout.reason.empty() ? "Remix zip failed safety checks. " : layout.reason + " ";
			shared::common::log("Update", r.warning, shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			return false;
		}

		const auto dest_root = install_root();
		const auto dest_bridge = dest_root / "d3d9_remix.dll";
		const auto dest_trex = dest_root / ".trex";

		if (is_proxy_d3d9(dest_bridge))
		{
			r.remix_skipped = true;
			r.warning += "Refusing to overwrite the proxy d3d9.dll. ";
			return false;
		}

		const bool new_has_usd = layout.has_usd;
		const auto zip_l = lower_copy(asset.name);
		const bool known_152 = zip_l.find("1.5.2") != std::string::npos || zip_l.find("1.5.") != std::string::npos;
		const bool copy_trex = new_has_usd || (known_152 && layout.trex_d3d9_64);

		if (!copy_trex)
		{
			r.remix_skipped = true;
			r.warning += "Remix zip looks incomplete (no usd_*.dll, not a 1.5.x runtime). "
				"Kept existing .trex. 32-bit bridge will still be updated. ";
		}

		if (status) {
			status("Installing Remix bridge…");
		}
		fs::copy_file(layout.bridge, dest_bridge, fs::copy_options::overwrite_existing, ec);
		if (ec)
		{
			r.error += std::format("Could not write d3d9_remix.dll ({}) ", ec.message());
			r.remix_skipped = true;
			return false;
		}

		if (!layout.launcher.empty())
		{
			const auto dest_launch = dest_root / "NvRemixLauncher32.exe";
			fs::copy_file(layout.launcher, dest_launch, fs::copy_options::overwrite_existing, ec);
		}

		if (copy_trex)
		{
			backup_trex_renderer(dest_trex / "d3d9.dll");
			if (status) {
				status("Installing .trex renderer…");
			}
			std::string copy_err;
			copy_tree_skip_junk(layout.trex, dest_trex, copy_err);
			if (!copy_err.empty())
			{
				r.error += copy_err + " ";
				r.remix_skipped = true;
				return false;
			}
			if (fs::exists(dest_root / "d3d9.dll", ec) && read_pe_machine(dest_root / "d3d9.dll") == pe_machine::amd64)
			{
				r.warning += "Note: a 64-bit d3d9.dll in the game folder would be wrong; proxy was not replaced. ";
			}
			r.remix_ok = true;
			shared::common::log("Update",
				std::format("Installed Remix bridge → {} and .trex from {}", dest_bridge.string(), asset.name),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
		else
		{
			r.remix_ok = false;
			shared::common::log("Update",
				std::format("Updated d3d9_remix.dll only; kept existing .trex ({})", r.warning),
				shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			if (fs::exists(dest_trex / "d3d9.dll", ec) || trex_has_usd(dest_trex))
			{
				// Bridge updated, trex kept — still a usable Remix install.
				r.remix_ok = true;
			}
		}

		fs::remove_all(work, ec);
		return r.remix_ok;
	}

	result run(const status_fn& status)
	{
		result r;
		try
		{
			update_dxvk(r, status);
			update_remix(r, status);

			if (r.dxvk_ok && r.remix_ok && r.error.empty())
			{
				r.status = "Updated DXVK and RTX Remix. The proxy d3d9.dll was not replaced.";
			}
			else if (r.dxvk_ok && r.remix_skipped)
			{
				r.status = "DXVK updated. Remix skipped (existing .trex kept if present).";
				if (!r.warning.empty()) {
					r.status += " " + r.warning;
				}
			}
			else if (r.dxvk_ok)
			{
				r.status = "DXVK updated.";
				if (!r.error.empty()) {
					r.status += " Remix: " + r.error;
				}
			}
			else if (r.remix_ok)
			{
				r.status = "Remix updated. DXVK failed: " + r.error;
			}
			else
			{
				if (r.status.empty()) {
					r.status = r.error.empty() ? "Update failed." : r.error;
				}
			}

			if (!r.dxvk_ok && !r.remix_ok && r.error.empty()) {
				r.error = "Update failed.";
			}
		}
		catch (const std::exception& ex)
		{
			r.error = std::string("Update crashed: ") + ex.what();
			r.status = r.error;
			shared::common::log("Update", r.error, shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
		catch (...)
		{
			r.error = "Update crashed (unknown error).";
			r.status = r.error;
			shared::common::log("Update", r.error, shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
		return r;
	}
}
