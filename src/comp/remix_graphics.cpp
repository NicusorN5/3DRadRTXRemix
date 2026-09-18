#include "std_include.hpp"
#include "remix_graphics.hpp"
#include "shared/common/remix_api.hpp"

#include <commctrl.h>
#include <uxtheme.h>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace comp::remix_graphics
{
	namespace
	{
		constexpr int k_id_group = 0x7F00;
		constexpr int k_id_first = 0x7F10;
		constexpr char k_prop[] = "vreRtxGfx";
		constexpr char k_prop_host[] = "vreRtxGfxHost";

		struct choice
		{
			const char* label;
			const char* value;
		};

		enum class widget
		{
			combo,
			check,
			header,
			slider
		};

		struct field
		{
			widget kind;
			int id;
			const char* label;
			const char* key; // null for headers / derived
			const choice* choices;
			int n_choices;
			const char* def;
		};

		constexpr choice k_preset[] = {
			// NVIDIA GraphicsPreset: Ultra=0 High=1 Medium=2 Low=3 Custom=4 Auto=5
			{"Ultra", "0"}, {"High", "1"}, {"Medium", "2"}, {"Low", "3"}, {"Custom", "4"}
		};
		constexpr choice k_dlss_preset[] = {
			{"Disabled", "0"}, {"Enabled", "1"}, {"Custom", "2"}
		};
		constexpr choice k_nrc[] = {
			{"Medium", "0"}, {"High", "1"}, {"Ultra", "2"}
		};
		constexpr choice k_min_b[] = { {"0", "0"}, {"1", "1"} };
		constexpr choice k_max_b[] = {
			{"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"},
			{"5", "5"}, {"6", "6"}, {"7", "7"}, {"8", "8"}
		};
		constexpr choice k_particle[] = {
			{"None", "0"}, {"Low", "1"}, {"High", "2"}
		};
		constexpr choice k_upscaler[] = {
			{"None", "0"}, {"DLSS", "1"}
		};
		constexpr choice k_dlss_mode[] = {
			{"Ultra Performance", "0"}, {"Performance", "1"}, {"Balanced", "2"},
			{"Quality", "3"}, {"Full Resolution", "5"}, {"Auto", "4"}
		};
		constexpr choice k_reflex[] = {
			{"Disabled", "0"}, {"Enabled", "1"}, {"Enabled + Boost", "2"}
		};
		constexpr choice k_rr_model[] = {
			{"CNN", "0"}, {"Transformer", "1"}
		};
		constexpr choice k_volq[] = {
			{"Low", "32"}, {"Medium", "16"}, {"High", "8"}, {"Ultra", "4"}, {"Insane", "3"}
		};

		enum ids
		{
			id_hdr_gen = k_id_first,
			id_bright,
			id_dlss_preset,
			id_upscaler,
			id_rr,
			id_rr_model,
			id_dlss_mode,
			id_dlfg,
			id_reflex,
			id_hdr_gfx,
			id_preset,
			id_minb,
			id_maxb,
			id_particle,
			id_nrc,
			id_vol,
			id_volq,
			id_postfx,
			id_motion,
			id_ca,
			id_vignette,
			id_last
		};

		constexpr field k_fields[] = {
			{widget::header, id_hdr_gen, "General", nullptr, nullptr, 0, nullptr},
			{widget::slider, id_bright, "Brightness", "rtx.userBrightness", nullptr, 0, "50"},
			{widget::combo, id_dlss_preset, "DLSS Preset", "rtx.dlssPreset", k_dlss_preset, 3, "2"},
			{widget::combo, id_upscaler, "Upscaler Type", "rtx.upscalerType", k_upscaler, 2, "1"},
			{widget::check, id_rr, "Ray Reconstruction", "rtx.enableRayReconstruction", nullptr, 0, "True"},
			{widget::combo, id_rr_model, "Ray Reconstruction Model", "rtx.rayreconstruction.model", k_rr_model, 2, "1"},
			{widget::combo, id_dlss_mode, "DLSS Mode", "rtx.qualityDLSS", k_dlss_mode, 6, "4"},
			{widget::check, id_dlfg, "Enable DLSS Frame Generation", "rtx.dlfg.enable", nullptr, 0, "True"},
			{widget::combo, id_reflex, "Reflex", "rtx.reflexMode", k_reflex, 3, "1"},
			{widget::header, id_hdr_gfx, "Graphics", nullptr, nullptr, 0, nullptr},
			{widget::combo, id_preset, "Graphics Preset", "rtx.graphicsPreset", k_preset, 5, "4"},
			{widget::combo, id_minb, "Min Light Bounces", "rtx.pathMinBounces", k_min_b, 2, "0"},
			{widget::combo, id_maxb, "Max Light Bounces", "rtx.pathMaxBounces", k_max_b, 8, "2"},
			{widget::combo, id_particle, "Particle Light", nullptr, k_particle, 3, "0"},
			{widget::combo, id_nrc, "RTX Neural Radiance Cache Quality",
				"rtx.neuralRadianceCache.qualityPreset", k_nrc, 3, "0"},
			{widget::check, id_vol, "Enable Volumetric Lighting", "rtx.volumetrics.enable", nullptr, 0, "True"},
			{widget::combo, id_volq, "Volumetrics Quality", nullptr, k_volq, 5, "8"},
			{widget::check, id_postfx, "Enable Post Effects", "rtx.postfx.enable", nullptr, 0, "True"},
			{widget::check, id_motion, "Enable Motion Blur", "rtx.postfx.enableMotionBlur", nullptr, 0, "True"},
			{widget::check, id_ca, "Enable Chromatic Aberration",
				"rtx.postfx.enableChromaticAberration", nullptr, 0, "True"},
			{widget::check, id_vignette, "Enable Vignette", "rtx.postfx.enableVignette", nullptr, 0, "True"},
		};

		std::map<std::string, std::string> g_values;
		std::map<std::string, std::string> g_key_from;
		std::vector<std::string> g_written_paths;
		HINSTANCE inst()
		{
			return shared::globals::dll_hmodule;
		}

		std::string trim_copy(const std::string& s)
		{
			size_t a = 0;
			while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) {
				a++;
			}
			size_t b = s.size();
			while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) {
				b--;
			}
			return s.substr(a, b - a);
		}

		bool key_line(const std::string& line, const std::string& key)
		{
			size_t i = 0;
			while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
				i++;
			}
			if (i < line.size() && (line[i] == '#' || line[i] == ';')) {
				return false;
			}
			if (line.compare(i, key.size(), key) != 0) {
				return false;
			}
			const char n = (i + key.size() < line.size()) ? line[i + key.size()] : 0;
			return n == 0 || n == ' ' || n == '\t' || n == '=';
		}

		void parse_conf(const std::string& path, bool overlay)
		{
			std::ifstream in(path);
			if (!in) {
				return;
			}
			std::string line;
			while (std::getline(in, line))
			{
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				size_t i = 0;
				while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
					i++;
				}
				if (i >= line.size() || line[i] == '#' || line[i] == ';') {
					continue;
				}
				const auto eq = line.find('=', i);
				if (eq == std::string::npos) {
					continue;
				}
				std::string key = trim_copy(line.substr(i, eq - i));
				std::string val = trim_copy(line.substr(eq + 1));
				if (key.size() >= 2 && key.front() == '"' && key.back() == '"') {
					key = key.substr(1, key.size() - 2);
				}
				if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
					val = val.substr(1, val.size() - 2);
				}
				if (key.empty()) {
					continue;
				}
				if (overlay || g_values.find(key) == g_values.end()) {
					g_values[key] = val;
					g_key_from[key] = path;
				}
			}
		}

		bool file_exists(const std::string& p)
		{
			const DWORD a = GetFileAttributesA(p.c_str());
			return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
		}

		bool dir_exists(const std::string& p)
		{
			const DWORD a = GetFileAttributesA(p.c_str());
			return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
		}

		std::string module_dir(HMODULE mod)
		{
			char path[MAX_PATH]{};
			if (!GetModuleFileNameA(mod, path, MAX_PATH) || !path[0]) {
				return {};
			}
			char* slash = std::strrchr(path, '\\');
			if (slash) {
				*slash = 0;
			}
			return path;
		}

		// Editor: C:\3DRadRTX. Compiled player: this exe's folder, never install.
		std::string conf_root()
		{
			return shared::globals::host_data_root();
		}

		std::string getv(const char* key, const char* def)
		{
			const auto it = g_values.find(key);
			if (it != g_values.end() && !it->second.empty()) {
				return it->second;
			}
			return def ? def : "";
		}

		int particle_level_from_conf()
		{
			const bool resolve = _stricmp(getv("rtx.enableUnorderedResolveInIndirectRays", "True").c_str(), "True") == 0
				|| getv("rtx.enableUnorderedResolveInIndirectRays", "True") == "1";
			const bool emissive = _stricmp(getv("rtx.enableUnorderedEmissiveParticlesInIndirectRays", "False").c_str(), "True") == 0
				|| getv("rtx.enableUnorderedEmissiveParticlesInIndirectRays", "False") == "1";
			if (!resolve) {
				return 0;
			}
			return emissive ? 2 : 1;
		}

		int nearest_choice(const choice* ch, int n, const std::string& val)
		{
			if (!ch || n <= 0) {
				return 0;
			}
			for (int i = 0; i < n; i++)
			{
				if (_stricmp(ch[i].value, val.c_str()) == 0) {
					return i;
				}
			}
			for (int i = 0; i < n; i++)
			{
				if (ch[i].label && _stricmp(ch[i].label, val.c_str()) == 0) {
					return i;
				}
			}
			char* end = nullptr;
			const float have = std::strtof(val.c_str(), &end);
			if (end != val.c_str())
			{
				int best = 0;
				float best_d = 1e9f;
				for (int i = 0; i < n; i++)
				{
					const float v = std::strtof(ch[i].value, nullptr);
					const float d = std::fabs(v - have);
					if (d < best_d) {
						best_d = d;
						best = i;
					}
				}
				return best;
			}
			return 0;
		}

		bool bool_on(const std::string& v)
		{
			return _stricmp(v.c_str(), "True") == 0 || v == "1";
		}

		HWND make_ctrl(HWND dlg, const char* cls, const char* text, DWORD style,
			int x, int y, int w, int h, int id, HFONT font)
		{
			HWND c = CreateWindowExA(0, cls, text ? text : "",
				style | WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
				x, y, w, h, dlg,
				reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
				inst(), nullptr);
			if (c && font) {
				SendMessageA(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
			}
			return c;
		}

		void fill_combo(HWND cb, const choice* ch, int n, int sel)
		{
			if (!cb || !ch) {
				return;
			}
			SendMessageA(cb, CB_RESETCONTENT, 0, 0);
			for (int i = 0; i < n; i++) {
				SendMessageA(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ch[i].label));
			}
			if (sel < 0 || sel >= n) {
				sel = 0;
			}
			SendMessageA(cb, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
		}

		int combo_sel(HWND dlg, int id)
		{
			HWND cb = GetDlgItem(dlg, id);
			if (!cb) {
				return 0;
			}
			const int s = static_cast<int>(SendMessageA(cb, CB_GETCURSEL, 0, 0));
			return s < 0 ? 0 : s;
		}

		std::string combo_value(HWND dlg, const field& f)
		{
			const int s = combo_sel(dlg, f.id);
			if (!f.choices || f.n_choices <= 0) {
				return f.def ? f.def : "";
			}
			if (s >= 0 && s < f.n_choices) {
				return f.choices[s].value;
			}
			return f.def ? f.def : f.choices[0].value;
		}

		std::string check_value(HWND dlg, int id)
		{
			HWND c = GetDlgItem(dlg, id);
			if (!c) {
				return "False";
			}
			return SendMessageA(c, BM_GETCHECK, 0, 0) == BST_CHECKED ? "True" : "False";
		}

		bool upsert_file(const std::string& path, const std::vector<std::pair<std::string, std::string>>& kv)
		{
			std::string text;
			{
				std::ifstream in(path);
				if (in) {
					text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
				}
			}

			std::map<std::string, bool> seen;
			for (const auto& p : kv) {
				seen[p.first] = false;
			}

			std::string out;
			out.reserve(text.size() + 512);
			size_t start = 0;
			while (start <= text.size())
			{
				size_t end = text.find_first_of("\r\n", start);
				if (end == std::string::npos) {
					end = text.size();
				}
				const std::string line = text.substr(start, end - start);
				bool replaced = false;
				for (const auto& p : kv)
				{
					if (key_line(line, p.first))
					{
						seen[p.first] = true;
						out += p.first;
						out += " = ";
						out += p.second;
						replaced = true;
						break;
					}
				}
				if (!replaced) {
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

			bool added = false;
			for (const auto& p : kv)
			{
				if (seen[p.first]) {
					continue;
				}
				if (!out.empty() && out.back() != '\n') {
					out += '\n';
				}
				if (!added)
				{
					out += "\n# RTX Remix graphics (Display Options / editor launcher)\n";
					added = true;
				}
				out += p.first;
				out += " = ";
				out += p.second;
				out += '\n';
			}

			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			if (!file) {
				return false;
			}
			file.write(out.data(), static_cast<std::streamsize>(out.size()));
			file.flush();
			file.close();
			if (!file) {
				return false;
			}

			HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h != INVALID_HANDLE_VALUE)
			{
				FlushFileBuffers(h);
				CloseHandle(h);
			}
			return true;
		}

		void write_kv_set(const std::vector<std::pair<std::string, std::string>>& kv)
		{
			g_written_paths.clear();
			const std::string root = conf_root();
			const std::string user = root + "\\user.conf";
			const std::string rtx = root + "\\rtx.conf";
			const bool u = upsert_file(user, kv);
			const bool r = upsert_file(rtx, kv);
			if (u) {
				g_written_paths.push_back(user);
			}
			if (r) {
				g_written_paths.push_back(rtx);
			}
			shared::common::log("RemixGfx",
				std::format("wrote {} keys -> {} ({}) and {} ({})",
					kv.size(), user, u ? "ok" : "fail", rtx, r ? "ok" : "fail"),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

			const std::string trex_dir = root + "\\.trex";
			const std::string trex_u = trex_dir + "\\user.conf";
			const std::string trex_r = trex_dir + "\\rtx.conf";
			if (dir_exists(trex_dir) || file_exists(trex_u) || file_exists(trex_r))
			{
				if (upsert_file(trex_u, kv)) {
					g_written_paths.push_back(trex_u);
				}
				if (upsert_file(trex_r, kv)) {
					g_written_paths.push_back(trex_r);
				}
			}

			auto& api = shared::common::remix_api::get();
			if (api.is_initialized())
			{
				for (const auto& p : kv) {
					shared::common::remix_api::set_config_variable(p.first.c_str(), p.second.c_str());
				}
			}
		}

		const char* source_leaf(const char* key)
		{
			if (!key) {
				return "(derived)";
			}
			const auto src = g_key_from.find(key);
			if (src == g_key_from.end()) {
				return "(default)";
			}
			const auto slash = src->second.find_last_of("\\/");
			return (slash == std::string::npos)
				? src->second.c_str()
				: src->second.c_str() + slash + 1;
		}

		void log_control(const field& f, const std::string& val, int idx)
		{
			const char* label = "-";
			if (f.choices && idx >= 0 && idx < f.n_choices && f.choices[idx].label) {
				label = f.choices[idx].label;
			}
			else if (f.kind == widget::check) {
				label = bool_on(val) ? "On" : "Off";
			}
			else if (f.kind == widget::slider) {
				label = "slider";
			}
			const char* key = f.key ? f.key
				: (f.id == id_particle ? "rtx.enableUnorderedResolveInIndirectRays"
					: (f.id == id_volq ? "rtx.volumetrics.froxelGridResolutionScale" : "(none)"));
			shared::common::log("RemixGfx",
				std::format("ctrl {} key={} file={} idx={} label={} src={}",
					f.label ? f.label : "?",
					key, val, idx, label, source_leaf(f.key ? f.key : key)),
				shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
		}

		void apply_field(HWND dlg, const field& f, bool log)
		{
			HWND c = GetDlgItem(dlg, f.id);
			if (!c) {
				return;
			}
			if (f.kind == widget::combo && f.choices)
			{
				std::string val = f.def ? f.def : "";
				if (f.key) {
					val = getv(f.key, f.def);
				}
				else if (f.id == id_particle) {
					val = std::to_string(particle_level_from_conf());
				}
				else if (f.id == id_volq) {
					val = getv("rtx.volumetrics.froxelGridResolutionScale", f.def);
				}
				const int idx = nearest_choice(f.choices, f.n_choices, val);
				fill_combo(c, f.choices, f.n_choices, idx);
				if (log) {
					log_control(f, val, idx);
				}
			}
			else if (f.kind == widget::slider && f.key)
			{
				const std::string val = getv(f.key, f.def);
				const int v = std::atoi(val.c_str());
				SendMessageA(c, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
				SendMessageA(c, TBM_SETPOS, TRUE, v < 0 ? 0 : (v > 100 ? 100 : v));
				HWND lab = GetDlgItem(dlg, f.id + 0x81);
				if (lab)
				{
					char t[16]{};
					sprintf_s(t, "%d", v < 0 ? 0 : (v > 100 ? 100 : v));
					SetWindowTextA(lab, t);
				}
				if (log) {
					log_control(f, val, v);
				}
			}
			else if (f.kind == widget::combo && f.id == id_volq && f.choices)
			{
				const std::string val = getv("rtx.volumetrics.froxelGridResolutionScale", f.def);
				const int idx = nearest_choice(f.choices, f.n_choices, val);
				fill_combo(c, f.choices, f.n_choices, idx);
				if (log) {
					log_control(f, val, idx);
				}
			}
			else if (f.kind == widget::check && f.key)
			{
				const std::string val = getv(f.key, f.def);
				SendMessageA(c, BM_SETCHECK,
					bool_on(val) ? BST_CHECKED : BST_UNCHECKED, 0);
				if (log) {
					log_control(f, val, bool_on(val) ? 1 : 0);
				}
			}
		}
	}

	HWND controls_root(HWND dlg)
	{
		if (!dlg) {
			return nullptr;
		}
		if (GetDlgItem(dlg, id_preset)) {
			return dlg;
		}
		HWND host = static_cast<HWND>(GetPropA(dlg, k_prop_host));
		if (host && GetDlgItem(host, id_preset)) {
			return host;
		}
		HWND nested = GetDlgItem(dlg, 4011);
		if (nested && GetDlgItem(nested, id_preset)) {
			return nested;
		}
		return dlg;
	}

	void bind_controls_host(HWND dlg, HWND host)
	{
		if (dlg && host) {
			SetPropA(dlg, k_prop_host, host);
		}
	}

	void commit_conf_before_remix()
	{
		for (const auto& p : g_written_paths)
		{
			HANDLE h = CreateFileA(p.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h != INVALID_HANDLE_VALUE)
			{
				FlushFileBuffers(h);
				CloseHandle(h);
			}
		}
		shared::common::log("RemixGfx",
			std::format("flushed {} conf path(s) — no delay before Remix",
				g_written_paths.size()),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}

	bool compiled_should_defer_remix()
	{
		if (shared::globals::skip_remix ||
			shared::globals::is_editor_host ||
			!shared::globals::is_compiled_host)
		{
			return false;
		}
		return true;
	}

	void load_from_disk()
	{
		const std::string root = conf_root();
		static std::string loaded_root;
		if (loaded_root == root && !g_values.empty()) {
			return;
		}
		g_values.clear();
		g_key_from.clear();
		const std::string trex = root + "\\.trex";

		struct hit
		{
			const char* tag;
			std::string path;
			bool overlay;
		};
		// Remix runtime: rtx.conf is defaults, user.conf overlays (user wins).
		// Local folder only; .trex fills missing keys and never overrides.
		const hit files[] = {
			{"rtx", root + "\\rtx.conf", true},
			{"user", root + "\\user.conf", true},
			{"trex_rtx", trex + "\\rtx.conf", false},
			{"trex_user", trex + "\\user.conf", false},
		};

		std::string loaded;
		int n = 0;
		bool overlayed = false;
		for (const auto& f : files)
		{
			if (!file_exists(f.path))
			{
				if (!loaded.empty()) {
					loaded += " ";
				}
				loaded += f.tag;
				loaded += "=miss";
				continue;
			}
			const size_t before = g_values.size();
			parse_conf(f.path, f.overlay);
			++n;
			if (!loaded.empty()) {
				loaded += " ";
			}
			loaded += f.tag;
			if (!f.overlay) {
				loaded += "=fill";
			}
			else if (!overlayed) {
				loaded += "=base";
				overlayed = true;
			}
			else {
				loaded += "=overlay";
			}
			loaded += "(";
			loaded += std::to_string(g_values.size() - (f.overlay ? 0 : before));
			loaded += ")";
		}

		loaded_root = root;
		shared::common::log("RemixGfx",
			std::format("load_from_disk host={} root={} files={} keys={} {}",
				shared::globals::host_kind_name(), root, n, g_values.size(), loaded),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

		std::string applied;
		for (const auto& f : k_fields)
		{
			if (!f.key) {
				continue;
			}
			const auto it = g_values.find(f.key);
			if (it == g_values.end()) {
				continue;
			}
			if (!applied.empty()) {
				applied += ", ";
			}
			applied += f.key;
			applied += "=";
			applied += it->second;
			const auto src = g_key_from.find(f.key);
			if (src != g_key_from.end())
			{
				const auto slash = src->second.find_last_of("\\/");
				applied += " <";
				applied += (slash == std::string::npos) ? src->second : src->second.substr(slash + 1);
				const bool trex_file = src->second.find("\\.trex\\") != std::string::npos;
				applied += trex_file ? " fill" : " won";
				applied += ">";
			}
		}
		shared::common::log("RemixGfx",
			std::format("keys applied to dialog: {}",
				applied.empty() ? "(none — using per-control defaults)" : applied),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}

	HFONT segoe_ui(int point_size, bool bold, int dpi)
	{
		if (dpi <= 0) {
			dpi = 96;
		}
		LOGFONTA lf{};
		lf.lfHeight = -MulDiv(point_size, dpi, 72);
		lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
		lf.lfCharSet = DEFAULT_CHARSET;
		lf.lfQuality = CLEARTYPE_QUALITY;
		lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
		strcpy_s(lf.lfFaceName, "Segoe UI");
		return CreateFontIndirectA(&lf);
	}

	int window_dpi(HWND hwnd)
	{
		using fn_t = UINT(WINAPI*)(HWND);
		static fn_t fn = nullptr;
		static bool tried = false;
		if (!tried)
		{
			tried = true;
			HMODULE u = GetModuleHandleA("user32.dll");
			if (u) {
				fn = reinterpret_cast<fn_t>(GetProcAddress(u, "GetDpiForWindow"));
			}
		}
		if (fn && hwnd) {
			const UINT d = fn(hwnd);
			if (d) {
				return static_cast<int>(d);
			}
		}
		HDC dc = GetDC(hwnd);
		const int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
		if (dc) {
			ReleaseDC(hwnd, dc);
		}
		return d > 0 ? d : 96;
	}

	void clamp_to_work_area(HWND hwnd)
	{
		if (!hwnd) {
			return;
		}
		HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi{};
		mi.cbSize = sizeof(mi);
		if (!GetMonitorInfoA(mon, &mi)) {
			return;
		}
		RECT wr{};
		GetWindowRect(hwnd, &wr);
		int w = wr.right - wr.left;
		int h = wr.bottom - wr.top;
		const int max_w = (mi.rcWork.right - mi.rcWork.left) * 9 / 10;
		const int max_h = (mi.rcWork.bottom - mi.rcWork.top) * 9 / 10;
		if (w > max_w) {
			w = max_w;
		}
		if (h > max_h) {
			h = max_h;
		}
		int x = wr.left;
		int y = wr.top;
		if (x + w > mi.rcWork.right) {
			x = mi.rcWork.right - w;
		}
		if (y + h > mi.rcWork.bottom) {
			y = mi.rcWork.bottom - h;
		}
		if (x < mi.rcWork.left) {
			x = mi.rcWork.left;
		}
		if (y < mi.rcWork.top) {
			y = mi.rcWork.top;
		}
		SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
	}

	int create_controls(HWND dlg, int x, int y, int width, HFONT font)
	{
		if (!dlg || width < 120) {
			return 0;
		}
		if (GetPropA(dlg, k_prop))
		{
			apply_to_dialog(dlg);
			RECT r{};
			HWND g = GetDlgItem(dlg, k_id_group);
			if (g)
			{
				GetWindowRect(g, &r);
				return r.bottom - r.top;
			}
			return 0;
		}

		INITCOMMONCONTROLSEX icc{};
		icc.dwSize = sizeof(icc);
		icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
		InitCommonControlsEx(&icc);

		const int dpi = window_dpi(dlg);
		const int s = (dpi <= 0) ? 96 : dpi;
		auto px = [s](int v) { return MulDiv(v, s, 96); };

		const int pad = px(12);
		const int label_h = px(16);
		const int combo_h = px(22);
		const int check_h = px(22);
		const int slider_h = px(28);
		const int hdr = px(18);
		const int sb = GetSystemMetrics(SM_CXVSCROLL);
		const int inner_w = (std::max)(80, width - pad * 2 - sb);
		const int col_w = inner_w;

		char kn[32]{};
		GetClassNameA(dlg, kn, 32);
		const bool dark = std::strcmp(kn, "RtxCompGfxHost") == 0;

		HFONT header_font = segoe_ui(9, true, dpi);
		if (!header_font) {
			header_font = font;
		}

		make_ctrl(dlg, "STATIC", "User Graphics Settings",
			SS_LEFT, x + pad, y + px(4), inner_w, hdr, k_id_group, header_font);

		int left_x = x + pad;
		int ly = y + px(4) + hdr + px(8);

		for (const auto& f : k_fields)
		{
			int& cy = ly;
			const int cx = left_x;

			if (f.kind == widget::header)
			{
				make_ctrl(dlg, "STATIC", f.label, SS_LEFT,
					left_x, cy, inner_w, hdr, f.id, header_font);
				cy += hdr + px(4);
				continue;
			}

			if (f.kind == widget::combo)
			{
				make_ctrl(dlg, "STATIC", f.label, SS_LEFT | SS_ENDELLIPSIS,
					cx, cy, col_w, label_h, f.id + 0x80, font);
				HWND cb = make_ctrl(dlg, "ComboBox", "",
					CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
					cx, cy + label_h + px(2), col_w, combo_h, f.id, font);
				if (cb)
				{
					SendMessageA(cb, CB_SETMINVISIBLE, 10, 0);
					SendMessageA(cb, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), combo_h - px(6));
					if (f.choices)
					{
						std::string val = f.def ? f.def : "";
						if (f.key) {
							val = getv(f.key, f.def);
						}
						else if (f.id == id_particle) {
							val = std::to_string(particle_level_from_conf());
						}
						else if (f.id == id_volq) {
							val = getv("rtx.volumetrics.froxelGridResolutionScale", f.def);
						}
						fill_combo(cb, f.choices, f.n_choices,
							nearest_choice(f.choices, f.n_choices, val));
					}
				}
				cy += label_h + px(2) + combo_h + px(10);
			}
			else if (f.kind == widget::slider)
			{
				make_ctrl(dlg, "STATIC", f.label, SS_LEFT | SS_ENDELLIPSIS,
					cx, cy, col_w - px(40), label_h, f.id + 0x80, font);
				const int cur = std::atoi(getv(f.key, f.def).c_str());
				char num[16]{};
				sprintf_s(num, "%d", cur < 0 ? 0 : (cur > 100 ? 100 : cur));
				make_ctrl(dlg, "STATIC", num, SS_RIGHT,
					cx + col_w - px(36), cy, px(36), label_h, f.id + 0x81, font);
				HWND tb = CreateWindowExA(0, TRACKBAR_CLASS, "",
					WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
					cx, cy + label_h, col_w, slider_h, dlg,
					reinterpret_cast<HMENU>(static_cast<INT_PTR>(f.id)),
					inst(), nullptr);
				if (tb)
				{
					if (font) {
						SendMessageA(tb, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
					}
					SendMessageA(tb, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
					SendMessageA(tb, TBM_SETPOS, TRUE, cur < 0 ? 0 : (cur > 100 ? 100 : cur));
				}
				cy += label_h + slider_h + px(10);
			}
			else if (f.kind == widget::check)
			{
				HWND ck = make_ctrl(dlg, "BUTTON", f.label,
					BS_AUTOCHECKBOX | BS_LEFT | BS_VCENTER | WS_TABSTOP,
					cx, cy + px(4), col_w, check_h, f.id, font);
				if (ck)
				{
					if (dark) {
						SetWindowTheme(ck, L"", L"");
					}
					SetWindowTextA(ck, f.label ? f.label : "");
					if (f.key) {
						SendMessageA(ck, BM_SETCHECK,
							bool_on(getv(f.key, f.def)) ? BST_CHECKED : BST_UNCHECKED, 0);
					}
				}
				cy += check_h + px(10);
			}
		}

		const int used = ly - y + pad;
		SetPropA(dlg, k_prop, reinterpret_cast<HANDLE>(1));
		apply_to_dialog(dlg);
		update_dependent_enables(dlg);
		return used;
	}

	void apply_to_dialog(HWND dlg)
	{
		HWND root = controls_root(dlg);
		if (!root || !GetDlgItem(root, id_preset)) {
			static HWND last_skip = nullptr;
			if (dlg && dlg != last_skip)
			{
				last_skip = dlg;
				shared::common::log("RemixGfx",
					std::format("apply_to_dialog skipped — no remix controls hwnd=0x{:X}",
						reinterpret_cast<std::uintptr_t>(dlg)),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
			}
			return;
		}
		static HWND last_applied = nullptr;
		const bool log = (root != last_applied);
		int n = 0;
		for (const auto& f : k_fields)
		{
			if (f.kind == widget::header) {
				continue;
			}
			apply_field(root, f, log);
			++n;
		}
		update_dependent_enables(root);
		if (!log) {
			return;
		}
		last_applied = root;
		const std::string gp = getv("rtx.graphicsPreset", "?");
		const std::string dp = getv("rtx.dlssPreset", "?");
		const int gp_i = nearest_choice(k_preset, 5, gp);
		const int dp_i = nearest_choice(k_dlss_preset, 3, dp);
		shared::common::log("RemixGfx",
			std::format(
				"apply_to_dialog controls={} graphicsPreset={} ({}) dlssPreset={} ({}) "
				"gp_src={} dlss_src={} root={}",
				n, gp, (gp_i >= 0 && gp_i < 5) ? k_preset[gp_i].label : "?",
				dp, (dp_i >= 0 && dp_i < 3) ? k_dlss_preset[dp_i].label : "?",
				source_leaf("rtx.graphicsPreset"),
				source_leaf("rtx.dlssPreset"),
				conf_root()),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
	}

	void set_enabled(HWND dlg, bool on)
	{
		HWND root = controls_root(dlg);
		if (!root) {
			return;
		}
		for (int id = k_id_group; id < id_last + 0x90; id++)
		{
			HWND c = GetDlgItem(root, id);
			if (c) {
				EnableWindow(c, on ? TRUE : FALSE);
			}
		}
		if (on) {
			update_dependent_enables(root);
		}
	}

	void update_dependent_enables(HWND dlg)
	{
		HWND root = controls_root(dlg);
		if (!root || !GetDlgItem(root, id_upscaler)) {
			return;
		}
		const bool dlss = combo_sel(root, id_upscaler) == 1;
		EnableWindow(GetDlgItem(root, id_dlss_mode), dlss ? TRUE : FALSE);
		EnableWindow(GetDlgItem(root, id_dlfg), dlss ? TRUE : FALSE);

		HWND rr = GetDlgItem(root, id_rr);
		const bool rr_on = rr && SendMessageA(rr, BM_GETCHECK, 0, 0) == BST_CHECKED;
		EnableWindow(GetDlgItem(root, id_rr_model), rr_on ? TRUE : FALSE);

		HWND vol = GetDlgItem(root, id_vol);
		const bool vol_on = vol && SendMessageA(vol, BM_GETCHECK, 0, 0) == BST_CHECKED;
		EnableWindow(GetDlgItem(root, id_volq), vol_on ? TRUE : FALSE);

		HWND fx = GetDlgItem(root, id_postfx);
		const bool fx_on = fx && SendMessageA(fx, BM_GETCHECK, 0, 0) == BST_CHECKED;
		EnableWindow(GetDlgItem(root, id_motion), fx_on ? TRUE : FALSE);
		EnableWindow(GetDlgItem(root, id_ca), fx_on ? TRUE : FALSE);
		EnableWindow(GetDlgItem(root, id_vignette), fx_on ? TRUE : FALSE);
	}

	bool write_from_dialog(HWND dlg)
	{
		HWND root = controls_root(dlg);
		if (!root || !GetDlgItem(root, id_preset)) {
			return false;
		}

		std::vector<std::pair<std::string, std::string>> kv;
		kv.reserve(40);
		for (const auto& f : k_fields)
		{
			if (f.kind == widget::header) {
				continue;
			}
			if (f.id == id_particle)
			{
				const int lv = combo_sel(root, id_particle);
				kv.emplace_back("rtx.enableUnorderedResolveInIndirectRays", lv >= 1 ? "True" : "False");
				kv.emplace_back("rtx.enableUnorderedEmissiveParticlesInIndirectRays", lv >= 2 ? "True" : "False");
				continue;
			}
			if (f.id == id_volq)
			{
				kv.emplace_back("rtx.volumetrics.froxelGridResolutionScale", combo_value(root, f));
				kv.emplace_back("rtx.volumetrics.froxelDepthSlices", "48");
				continue;
			}
			if (!f.key) {
				continue;
			}
			if (f.kind == widget::combo) {
				kv.emplace_back(f.key, combo_value(root, f));
			}
			else if (f.kind == widget::check) {
				kv.emplace_back(f.key, check_value(root, f.id));
			}
			else if (f.kind == widget::slider)
			{
				HWND tb = GetDlgItem(root, f.id);
				int v = tb ? static_cast<int>(SendMessageA(tb, TBM_GETPOS, 0, 0)) : 50;
				if (v < 0) {
					v = 0;
				}
				if (v > 100) {
					v = 100;
				}
				kv.emplace_back(f.key, std::to_string(v));
			}
		}

		const int reflex_i = combo_sel(root, id_reflex);
		kv.emplace_back("rtx.isReflexEnabled", reflex_i == 0 ? "False" : "True");
		kv.emplace_back("rtx.integrateIndirectMode", "2");

		std::string gp_w = "?";
		std::string dp_w = "?";
		for (const auto& p : kv)
		{
			if (p.first == "rtx.graphicsPreset") {
				gp_w = p.second;
			}
			else if (p.first == "rtx.dlssPreset") {
				dp_w = p.second;
			}
		}
		shared::common::log("RemixGfx",
			std::format("write_from_dialog graphicsPreset={} dlssPreset={} keys={} root={}",
				gp_w, dp_w, kv.size(), conf_root()),
			shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);

		write_kv_set(kv);
		return true;
	}

	void on_host_hscroll(HWND host)
	{
		if (!host) {
			return;
		}
		HWND tb = GetDlgItem(host, id_bright);
		if (!tb) {
			return;
		}
		int v = static_cast<int>(SendMessageA(tb, TBM_GETPOS, 0, 0));
		if (v < 0) {
			v = 0;
		}
		if (v > 100) {
			v = 100;
		}
		HWND lab = GetDlgItem(host, id_bright + 0x81);
		if (lab)
		{
			char t[16]{};
			sprintf_s(t, "%d", v);
			SetWindowTextA(lab, t);
		}
	}
}
