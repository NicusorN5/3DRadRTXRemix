#include "std_include.hpp"
#include "fog.hpp"
#include "camera.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/remix_api.hpp"
#include "shared/globals.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>

namespace comp::game::fog
{
	namespace
	{
		constexpr std::uintptr_t host_preferred_base = 0x00400000;
		constexpr int host_max_objects = 4096;
		constexpr int fog_cap = 8;
		constexpr int plugin_probe = 0x600;
		constexpr int plugin_off_shown = 0x04;
		constexpr int plugin_off_enable_start = 0x08;
		constexpr int plugin_off_active = 0x0C;
		constexpr int plugin_off_name = 0x424;
		constexpr std::uint64_t k_ident_tag = 0x464F470000000000ull;

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

		HMODULE host_module = nullptr;
		const char* host_layout_tag = nullptr;
		const int* host_count_at = nullptr;
		void* const* host_list_at = nullptr;
		HMODULE const* host_hmod_at = nullptr;
		bool host_list_ok = false;

		int captured = 0;
		UINT work_frame = 0xFFFFFFFFu;
		bool work_allowed = false;
		bool applied_once = false;
		std::uint64_t last_content = 0;
		bool logged_offsets = false;

		int off_rgb = -1;
		int off_start = -1;
		int off_end = -1;
		int off_color_dword = -1;

		struct captured_fog
		{
			std::uint64_t identity = 0;
			int object_id = -1;
			int shown = 1;
			int active = 1;
			int enable_start = 1;
			D3DXVECTOR3 color{ 0.5f, 0.5f, 0.5f };
			float start = 1.0f;
			float end = 100.0f;
			char title[64]{};
		};

		captured_fog frame_fogs[fog_cap]{};

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

		bool path_is_fog(const char* path)
		{
			if (path_has_folder(path, "Fog")) {
				return true;
			}
			if (!path || !path[0]) {
				return false;
			}
			const char* leaf = path;
			for (const char* p = path; *p; p++) {
				if (*p == '\\' || *p == '/') {
					leaf = p + 1;
				}
			}
			return _stricmp(leaf, "fog.dll") == 0;
		}

		void take_title(char* dest, const char* src)
		{
			if (!dest) {
				return;
			}
			dest[0] = 0;
			if (!src || !src[0]) {
				return;
			}
			std::strncpy(dest, src, 63);
			dest[63] = 0;
		}

		bool host_tables_sane(const int* count_at, void* const* list_at,
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

		bool try_bind_host_layout(HMODULE mod, const host_layout& layout)
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
			if (!host_tables_sane(count_at, list_at, hmod_at)) {
				return false;
			}
			host_module = mod;
			host_layout_tag = layout.tag;
			host_count_at = count_at;
			host_list_at = list_at;
			host_hmod_at = hmod_at;
			host_list_ok = true;
			return true;
		}

		bool bind_host()
		{
			if (host_list_ok)
			{
				if (host_tables_sane(host_count_at, host_list_at, host_hmod_at)) {
					return true;
				}
				host_list_ok = false;
				host_count_at = nullptr;
				host_list_at = nullptr;
				host_hmod_at = nullptr;
			}

			HMODULE editor = GetModuleHandleA("3DRad.exe");
			if (!editor) {
				editor = GetModuleHandleA("3DRadRT.exe");
			}
			if (editor && try_bind_host_layout(editor, editor_host_layout)) {
				return true;
			}
			const HMODULE self = GetModuleHandleA(nullptr);
			if (self && self != editor &&
				try_bind_host_layout(self, player_host_layout))
			{
				return true;
			}
			return false;
		}

		bool finite_fog_dist(const float v)
		{
			return std::isfinite(v) && v >= 0.0f && v <= 1.0e6f;
		}

		bool looks_rgb(const float r, const float g, const float b)
		{
			return std::isfinite(r) && std::isfinite(g) && std::isfinite(b) &&
				r >= 0.0f && r <= 1.05f &&
				g >= 0.0f && g <= 1.05f &&
				b >= 0.0f && b <= 1.05f;
		}

		void discover_layout(const std::uint8_t* plugin)
		{
			if (off_start >= 0 && off_end >= 0) {
				return;
			}
			if (!plugin || !memory_readable(plugin, plugin_probe)) {
				return;
			}

			int best_start = -1;
			int best_end = -1;
			int best_score = -1;
			for (int off = 0x40; off + 8 <= plugin_probe; off += 4)
			{
				const float a = *reinterpret_cast<const float*>(plugin + off);
				const float b = *reinterpret_cast<const float*>(plugin + off + 4);
				if (!finite_fog_dist(a) || !finite_fog_dist(b) || b <= a) {
					continue;
				}
				int score = 1;
				if (std::fabs(a - 1.0f) < 0.001f) {
					score += 8;
				}
				if (std::fabs(b - 100.0f) < 0.05f) {
					score += 8;
				}
				if (b <= 1000.0f) {
					score += 2;
				}
				if (off >= 0x480 && off <= 0x560) {
					score += 3;
				}
				if (score > best_score)
				{
					best_score = score;
					best_start = off;
					best_end = off + 4;
				}
			}

			if (best_start < 0) {
				return;
			}
			off_start = best_start;
			off_end = best_end;

			if (best_start >= 12)
			{
				const float r = *reinterpret_cast<const float*>(plugin + best_start - 12);
				const float g = *reinterpret_cast<const float*>(plugin + best_start - 8);
				const float b = *reinterpret_cast<const float*>(plugin + best_start - 4);
				if (looks_rgb(r, g, b)) {
					off_rgb = best_start - 12;
				}
			}
			if (off_rgb < 0 && best_start >= 4)
			{
				const std::uint32_t packed =
					*reinterpret_cast<const std::uint32_t*>(plugin + best_start - 4);
				const int r = packed & 0xFF;
				const int g = (packed >> 8) & 0xFF;
				const int b = (packed >> 16) & 0xFF;
				if (r <= 255 && g <= 255 && b <= 255 && (r + g + b) > 0) {
					off_color_dword = best_start - 4;
				}
			}
		}

		std::uint64_t identity_of_plugin(const void* plugin)
		{
			const auto p = static_cast<std::uint32_t>(
				reinterpret_cast<std::uintptr_t>(plugin));
			return k_ident_tag | (p ? p : 1u);
		}

		bool read_i32(const std::uint8_t* p, const int off, int& out)
		{
			if (!p || off < 0 || !memory_readable(p + off, 4)) {
				return false;
			}
			out = *reinterpret_cast<const int*>(p + off);
			return true;
		}

		bool read_f32(const std::uint8_t* p, const int off, float& out)
		{
			if (!p || off < 0 || !memory_readable(p + off, 4)) {
				return false;
			}
			out = *reinterpret_cast<const float*>(p + off);
			return std::isfinite(out);
		}

		void apply_remix(const captured_fog* live, const int n)
		{
			const captured_fog* pick = nullptr;
			for (int i = 0; i < n; i++)
			{
				if (live[i].active || live[i].enable_start) {
					pick = &live[i];
					if (live[i].active) {
						break;
					}
				}
			}

			IDirect3DDevice9* dev = shared::globals::d3d_device;
			if (!pick)
			{
				if (dev) {
					dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
				}
				if (applied_once)
				{
					shared::common::remix_api::set_config_variable(
						"rtx.volumetrics.enableFogRemap", "False");
					applied_once = false;
					last_content = 0;
					shared::common::log("Fog", "Fog live n=0 (disabled remap, rtx.conf not written)",
						shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				}
				return;
			}

			const float start = pick->start;
			const float end = pick->end > start ? pick->end : start + 1.0f;
			const D3DXVECTOR3 c = pick->color;
			const bool on = pick->active != 0;

			if (dev)
			{
				const DWORD col = D3DCOLOR_COLORVALUE(c.x, c.y, c.z, 1.0f);
				const DWORD start_bits = *reinterpret_cast<const DWORD*>(&start);
				const DWORD end_bits = *reinterpret_cast<const DWORD*>(&end);
				dev->SetRenderState(D3DRS_FOGENABLE, on ? TRUE : FALSE);
				dev->SetRenderState(D3DRS_FOGCOLOR, col);
				dev->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
				dev->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
				dev->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
				dev->SetRenderState(D3DRS_FOGSTART, start_bits);
				dev->SetRenderState(D3DRS_FOGEND, end_bits);
			}

			char start_s[32];
			char end_s[32];
			char color_s[80];
			std::snprintf(start_s, sizeof(start_s), "%.4f", start);
			std::snprintf(end_s, sizeof(end_s), "%.4f", end);
			std::snprintf(color_s, sizeof(color_s), "%.4f, %.4f, %.4f", c.x, c.y, c.z);

			shared::common::remix_api::set_config_variable(
				"rtx.volumetrics.enableFogRemap", on ? "True" : "False");
			shared::common::remix_api::set_config_variable(
				"rtx.volumetrics.enableFogColorRemap", "True");
			shared::common::remix_api::set_config_variable(
				"rtx.volumetrics.enableFogMaxDistanceRemap", "True");
			shared::common::remix_api::set_config_variable(
				"rtx.volumetrics.fogRemapMaxDistanceMinMeters", start_s);
			shared::common::remix_api::set_config_variable(
				"rtx.volumetrics.fogRemapMaxDistanceMaxMeters", end_s);
			shared::common::remix_api::set_config_variable(
				"rtx.volumetrics.transmittanceColor", color_s);
			shared::common::remix_api::set_config_variable(
				"rtx.enableFog", on ? "True" : "False");
			applied_once = true;

			const std::uint64_t content =
				(static_cast<std::uint64_t>(pick->identity) ^
					(on ? 1ull : 0ull) << 1) ^
				(static_cast<std::uint64_t>(static_cast<int>(start * 1000.0f)) << 8) ^
				(static_cast<std::uint64_t>(static_cast<int>(end * 1000.0f)) << 20) ^
				(static_cast<std::uint64_t>(static_cast<int>(c.x * 255.0f))) ^
				(static_cast<std::uint64_t>(static_cast<int>(c.y * 255.0f)) << 32) ^
				(static_cast<std::uint64_t>(static_cast<int>(c.z * 255.0f)) << 40);
			if (content != last_content)
			{
				last_content = content;
				shared::common::log("Fog",
					std::format(
						"Fog live n={} start={:.3f} end={:.3f} color=({:.3f},{:.3f},{:.3f}) "
						"active={} oid={} id={:X} rgb+0x{:X} start+0x{:X} end+0x{:X}",
						n, start, end, c.x, c.y, c.z, on ? 1 : 0,
						pick->object_id,
						static_cast<unsigned long long>(pick->identity),
						static_cast<unsigned>(off_rgb >= 0 ? off_rgb : off_color_dword),
						static_cast<unsigned>(off_start),
						static_cast<unsigned>(off_end)),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		}

		void capture_plugins()
		{
			captured = 0;
			if (shared::globals::skip_remix || !bind_host() || !host_count_at) {
				return;
			}
			const int count = *host_count_at;
			if (count <= 0 || count > host_max_objects) {
				return;
			}

			for (int i = 0; i < count && captured < fog_cap; i++)
			{
				const auto* host_obj = static_cast<const std::uint8_t*>(host_list_at[i]);
				const HMODULE plugin_mod = host_hmod_at[i];
				if (!host_obj || !plugin_mod || !memory_readable(host_obj, sizeof(void*))) {
					continue;
				}
				char path[MAX_PATH]{};
				if (!GetModuleFileNameA(plugin_mod, path, MAX_PATH) || !path[0]) {
					continue;
				}
				if (!path_is_fog(path)) {
					continue;
				}
				const auto* plugin = *reinterpret_cast<const std::uint8_t* const*>(host_obj);
				if (!plugin || !memory_readable(plugin, 0x40)) {
					continue;
				}
				discover_layout(plugin);

				auto& rec = frame_fogs[captured];
				rec = {};
				rec.identity = identity_of_plugin(plugin);
				rec.object_id = i;
				read_i32(plugin, plugin_off_shown, rec.shown);
				read_i32(plugin, plugin_off_enable_start, rec.enable_start);
				read_i32(plugin, plugin_off_active, rec.active);
				if (memory_readable(plugin + plugin_off_name, 8)) {
					take_title(rec.title, reinterpret_cast<const char*>(plugin + plugin_off_name));
				}
				if (off_rgb >= 0)
				{
					float r = rec.color.x, g = rec.color.y, b = rec.color.z;
					if (read_f32(plugin, off_rgb, r) &&
						read_f32(plugin, off_rgb + 4, g) &&
						read_f32(plugin, off_rgb + 8, b) &&
						looks_rgb(r, g, b))
					{
						rec.color = { r, g, b };
					}
				}
				else if (off_color_dword >= 0 && memory_readable(plugin + off_color_dword, 4))
				{
					const std::uint32_t packed =
						*reinterpret_cast<const std::uint32_t*>(plugin + off_color_dword);
					rec.color.x = static_cast<float>(packed & 0xFF) / 255.0f;
					rec.color.y = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
					rec.color.z = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
				}
				read_f32(plugin, off_start, rec.start);
				read_f32(plugin, off_end, rec.end);
				if (!finite_fog_dist(rec.start)) {
					rec.start = 1.0f;
				}
				if (!finite_fog_dist(rec.end) || rec.end <= rec.start) {
					rec.end = rec.start + 99.0f;
				}
				captured++;
			}

			if (!logged_offsets && captured > 0 && off_start >= 0)
			{
				logged_offsets = true;
				shared::common::log("Fog",
					std::format(
						"Fog struct offsets shown=+0x{:X} enableStart=+0x{:X} active=+0x{:X} "
						"name=+0x{:X} rgb=+0x{:X} start=+0x{:X} end=+0x{:X} (plugin heap)",
						plugin_off_shown, plugin_off_enable_start, plugin_off_active,
						plugin_off_name,
						off_rgb >= 0 ? off_rgb : off_color_dword,
						off_start, off_end),
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
			}
		}
	}

	void on_frame()
	{
		if (shared::globals::skip_remix) {
			captured = 0;
			return;
		}

		const UINT frame = shared::common::ffp_state::get().frame_count();
		const bool allowed = camera::scene_conversion_allowed();
		if (frame == work_frame && allowed == work_allowed) {
			return;
		}
		work_frame = frame;
		work_allowed = allowed;
		if (!allowed) {
			captured = 0;
			return;
		}

		capture_plugins();
		apply_remix(frame_fogs, captured);
	}

	int captured_count()
	{
		return captured;
	}

	void reset()
	{
		captured = 0;
		work_frame = 0xFFFFFFFFu;
		applied_once = false;
		last_content = 0;
		logged_offsets = false;
		off_rgb = -1;
		off_start = -1;
		off_end = -1;
		off_color_dword = -1;
		host_list_ok = false;
	}

	void on_project_before()
	{
		if (applied_once)
		{
			shared::common::remix_api::set_config_variable(
				"rtx.volumetrics.enableFogRemap", "False");
			applied_once = false;
		}
		captured = 0;
		work_frame = 0xFFFFFFFFu;
		last_content = 0;
		host_list_ok = false;
		host_module = nullptr;
		host_layout_tag = nullptr;
		host_count_at = nullptr;
		host_list_at = nullptr;
		host_hmod_at = nullptr;
	}

	void on_project_after()
	{
		host_list_ok = false;
		work_frame = 0xFFFFFFFFu;
		captured = 0;
	}
}
