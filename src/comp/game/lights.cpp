#include "std_include.hpp"
#include "lights.hpp"
#include "camera.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/remix_api.hpp"
#include "shared/globals.hpp"

namespace comp::game::lights
{
	namespace
	{
		constexpr std::uintptr_t preferred_base = 0x10000000;
		constexpr std::uintptr_t host_preferred_base = 0x00400000;
		// iLightDirectionalSet dest + iLightDirectionalColor source.
		// +0x00 dir xyz, +0x0C color rgba, +0x2C enabled (set to 1 by Set).
		constexpr std::uintptr_t dir_light_va = 0x100ED650;
		constexpr int dir_off_color = 0x0C;
		constexpr int dir_off_enabled = 0x2C;
		// SkinMesh shader upload only — 3 slots, filled when PointLight
		// ObjectRun has plugin+0x550 != -1. Empty while objects exist.
		constexpr std::uintptr_t local_light_va = 0x101B3C80;
		constexpr int local_stride = 0x3C;
		constexpr int local_off_color = 0x0C;
		constexpr int local_off_range = 0x20;
		constexpr int local_slots = 3;
		constexpr int d3d_light_cap = 8;
		constexpr int remix_light_cap = 32;
		// Emitter size only. Range factor must not drive this.
		constexpr float k_remix_sphere_radius = 0.1f;
		constexpr float k_rect_size = 0.2f;
		constexpr float k_disk_radius = 0.12f;
		constexpr float k_cyl_radius = 0.05f;
		constexpr float k_cyl_length = 0.35f;
		constexpr float k_range_max = 500.0f;
		constexpr float k_radiance_per_world = 0.15f;
		constexpr float k_radiance_min = 2.0f;
		// World coords past this are heap garbage (gizmo / SkinMesh +0x15A8
		// on a small plugin). Remix drops those SetLights.
		constexpr float k_pos_limit = 10000.0f;
		// Working overlay was SetLight-only. CreateLight + DrawLightInstance
		// with NaN/huge pos zeroed lights; CreateLight on Reset AVed the bridge.
		constexpr bool k_emit_remix_api_spheres = false;

		enum class light_shape
		{
			sphere,
			rect,
			disk,
			cylinder,
		};

		// Host plugin tables live in the THIN EXE, not dll3impact. Editor
		// (3DRad.exe) and compiled player (3drad_player.exe renamed) use the
		// same host+0 / +0x291A layout but different BSS addresses. Never
		// rebase editor VAs off scary.exe — SizeOfImage still covers those
		// RVAs as unrelated BSS, or bind fails and we emit empty iLightLocal*
		// slots (origin, rgb=0).
		struct host_layout
		{
			std::uintptr_t count_va;
			std::uintptr_t list_va;
			std::uintptr_t hmod_va;
			const char* tag;
		};

		constexpr host_layout editor_host_layout{
			0x00450460, 0x00454468, 0x0044AE58, "3DRad.exe" };
		// 3drad_player.exe (110592 bytes, preferred 0x400000):
		//   cmp eax,[0x44445C] then mov eax,[eax*4+0x448460]; type +0x291A
		//   HMODULE[] via GetProcAddress ObjectRun at [esi*4+0x43EE58]
		constexpr host_layout player_host_layout{
			0x0044445C, 0x00448460, 0x0043EE58, "3drad_player" };
		constexpr int host_max_objects = 4096;
		constexpr int host_header_size = 0x2930;
		constexpr int host_off_type = 0x291A;
		constexpr int host_off_child_count = 0x291C;
		constexpr int host_off_child_array = 0x2920;
		constexpr int host_off_parent_handle = 0x2924;
		constexpr int host_off_linked = 0x2928;
		constexpr int host_child_stride = 8;
		constexpr int host_child_cap = 256;

		constexpr int plugin_off_shown = 0x04;
		constexpr int plugin_off_active = 0x0C;
		constexpr int plugin_off_quat = 0x484;
		constexpr int point_off_pos = 0x494;
		constexpr int point_off_xform = 0x4C8; // transform helper copy of +0x494
		constexpr int point_off_gizmo = 0x52C;
		constexpr int sun_off_dir = 0x50C;
		constexpr int point_off_rgb = 0x530;
		constexpr int point_off_range = 0x53C;
		constexpr int point_instance_size = 0x584;
		constexpr int sun_instance_size = 0x53C;
		constexpr int skin_off_mesh = 0x6DC;

		// iMeshLocation / iSkinMeshLocation / iCameraLocation / iBodyLocation.
		constexpr int mesh_off_location = 0xCF8;
		constexpr int skin_off_location = 0x15A8;
		constexpr int cam_off_location = 0x50;
		constexpr int body_off_location = 0x5B4;
		constexpr int impact_probe_size = 0x16B0;

		HMODULE engine_module = nullptr;
		HMODULE host_module = nullptr;
		const char* host_layout_tag = nullptr;
		char host_exe_name[64]{};
		const std::uint8_t* dir_at = nullptr;
		const std::uint8_t* local_at = nullptr;
		const int* host_count_at = nullptr;
		void* const* host_list_at = nullptr;
		HMODULE const* host_hmod_at = nullptr;
		bool host_list_ok = false;

		int captured = 0;
		int captured_sun = 0;
		int captured_point = 0;
		int last_d3d_enabled = 0;
		UINT captured_frame = 0xFFFFFFFFu;
		bool captured_allowed = false;

		struct captured_light
		{
			bool directional = false;
			D3DXVECTOR3 pos{};
			D3DXVECTOR3 dir{};
			D3DXVECTOR3 color{};
			float range = 1.0f;
			light_shape shape = light_shape::sphere;
			std::uint64_t identity = 0;
			std::uint64_t content = 0;
			char title[64]{};
		};

		captured_light frame_lights[remix_light_cap]{};

		struct api_slot
		{
			remixapi_LightHandle handle = nullptr;
			std::uint64_t identity = 0;
			std::uint64_t content = 0;
			bool in_use = false;
		};

		api_slot api_lights[remix_light_cap]{};

		enum class plugin_kind
		{
			other,
			point,
			sun,
			skin,
			camera,
			body,
		};

		struct cached_plugin
		{
			int slot = -1;
			int parent_slot = -1;
			int bone_id = 0;
			const std::uint8_t* host = nullptr;
			const std::uint8_t* plugin = nullptr;
			plugin_kind kind = plugin_kind::other;
			light_shape shape = light_shape::sphere;
			char title[64]{};
			bool have_last = false;
			D3DXVECTOR3 last_pos{};
			D3DXVECTOR3 last_dir{ 0.0f, -1.0f, 0.0f };
			D3DXVECTOR3 last_color{ 1.0f, 1.0f, 1.0f };
			float last_range = 25.0f;
			int last_shown = 1;
		};

		cached_plugin cached_plugins[remix_light_cap]{};
		int cached_plugin_n = 0;
		int cached_host_count = -1;

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

		void title_to_lower(const char* title, char* lower, const int cap)
		{
			if (!lower || cap <= 0) {
				return;
			}
			lower[0] = 0;
			if (!title) {
				return;
			}
			int i = 0;
			for (; i < cap - 1 && title[i]; i++)
			{
				const char c = title[i];
				lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
			}
			lower[i] = 0;
		}

		bool title_has_known_token(const char* title)
		{
			if (!title || !title[0]) {
				return false;
			}
			char lower[64]{};
			title_to_lower(title, lower, 64);
			// Only these tokens. Ignore two-letter blob hits like 'ht'.
			return std::strstr(lower, "sphere") ||
				std::strstr(lower, "rectangle") || std::strstr(lower, "rect") ||
				std::strstr(lower, "cylinder") ||
				std::strstr(lower, "disk") || std::strstr(lower, "disc") ||
				std::strstr(lower, "pointlight") || std::strstr(lower, "sunlight");
		}

		light_shape shape_from_title(const char* title)
		{
			if (!title || !title[0]) {
				return light_shape::sphere;
			}
			char lower[64]{};
			title_to_lower(title, lower, 64);
			if (std::strstr(lower, "rectangle") || std::strstr(lower, "rect")) {
				return light_shape::rect;
			}
			if (std::strstr(lower, "cylinder")) {
				return light_shape::cylinder;
			}
			if (std::strstr(lower, "disk") || std::strstr(lower, "disc")) {
				return light_shape::disk;
			}
			if (std::strstr(lower, "sphere")) {
				return light_shape::sphere;
			}
			return light_shape::sphere;
		}

		bool looks_ascii_name(const char* s, const int n)
		{
			if (!s || n < 2) {
				return false;
			}
			int letters = 0;
			for (int i = 0; i < n; i++)
			{
				const unsigned char c = static_cast<unsigned char>(s[i]);
				if (c == 0) {
					return letters >= 2;
				}
				if (c < 32 || c > 126) {
					return false;
				}
				if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
					letters++;
				}
			}
			return letters >= 2;
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

		void scan_blob_title(const std::uint8_t* blob, const int bytes, char* dest)
		{
			if (!blob || !dest || bytes < 4) {
				return;
			}
			for (int i = 0; i + 4 < bytes; i++)
			{
				if (!looks_ascii_name(reinterpret_cast<const char*>(blob + i),
					std::min(63, bytes - i)))
				{
					continue;
				}
				const char* s = reinterpret_cast<const char*>(blob + i);
				if (std::strstr(s, "\\") || std::strstr(s, ".dll") || std::strstr(s, ".spr")) {
					continue;
				}
				if (!title_has_known_token(s)) {
					continue;
				}
				take_title(dest, s);
				if (shape_from_title(dest) != light_shape::sphere ||
					_strnicmp(s, "PointLight", 10) == 0 ||
					_strnicmp(s, "SunLight", 8) == 0)
				{
					return;
				}
			}

			for (int i = 0; i + 8 < bytes; i += 2)
			{
				if (blob[i] < 32 || blob[i] > 126 || blob[i + 1] != 0) {
					continue;
				}
				char ascii[64]{};
				int n = 0;
				for (int k = 0; k < 63 && i + k * 2 + 1 < bytes; k++)
				{
					const unsigned char lo = blob[i + k * 2];
					const unsigned char hi = blob[i + k * 2 + 1];
					if (hi != 0 || lo < 32 || lo > 126) {
						break;
					}
					ascii[n++] = static_cast<char>(lo);
				}
				if (n >= 2 && looks_ascii_name(ascii, n) && title_has_known_token(ascii))
				{
					take_title(dest, ascii);
					if (shape_from_title(dest) != light_shape::sphere) {
						return;
					}
				}
			}
		}

		void read_host_title(const std::uint8_t* host, const std::uint8_t* plugin,
			const int plugin_bytes, char* dest)
		{
			dest[0] = 0;
			if (host && memory_readable(host, static_cast<SIZE_T>(host_header_size)))
			{
				for (int off = 4; off <= 0x20; off += 4)
				{
					if (!memory_readable(host + off, sizeof(void*))) {
						continue;
					}
					const auto* p = *reinterpret_cast<const char* const*>(host + off);
					if (p && memory_readable(p, 8) && looks_ascii_name(p, 63) &&
						title_has_known_token(p))
					{
						take_title(dest, p);
						if (shape_from_title(dest) != light_shape::sphere) {
							return;
						}
					}
				}
				char inline_name[64]{};
				scan_blob_title(host + 4, 0x80, inline_name);
				if (inline_name[0] && (dest[0] == 0 ||
					shape_from_title(inline_name) != light_shape::sphere))
				{
					take_title(dest, inline_name);
					if (shape_from_title(dest) != light_shape::sphere) {
						return;
					}
				}
			}
			if (plugin && plugin_bytes > 0 &&
				memory_readable(plugin, static_cast<SIZE_T>(plugin_bytes)))
			{
				char from_plugin[64]{};
				scan_blob_title(plugin, std::min(plugin_bytes, 0x80), from_plugin);
				if (from_plugin[0] && (dest[0] == 0 ||
					shape_from_title(from_plugin) != light_shape::sphere))
				{
					take_title(dest, from_plugin);
				}
			}
		}

		plugin_kind kind_from_path(const char* path)
		{
			if (path_has_folder(path, "PointLight")) {
				return plugin_kind::point;
			}
			if (path_has_folder(path, "SunLight")) {
				return plugin_kind::sun;
			}
			if (path_has_folder(path, "SkinMesh")) {
				return plugin_kind::skin;
			}
			if (path_has_folder(path, "Cam1StPerson") ||
				path_has_folder(path, "CamChase") ||
				path_has_folder(path, "Camera"))
			{
				return plugin_kind::camera;
			}
			if (path_has_folder(path, "Character") ||
				path_has_folder(path, "RigidBody") ||
				path_has_folder(path, "Ball") ||
				path_has_folder(path, "Car") ||
				path_has_folder(path, "PCar"))
			{
				return plugin_kind::body;
			}
			return plugin_kind::other;
		}

		bool bind_engine()
		{
			if (dir_at && local_at) {
				return true;
			}

			if (!engine_module)
			{
				engine_module = GetModuleHandleA("dll3impact.dll");
				if (!engine_module) {
					return false;
				}
			}

			const auto rebase = [](const std::uintptr_t preferred)
				{
					return reinterpret_cast<std::uintptr_t>(engine_module) +
						(preferred - preferred_base);
				};

			const auto* dir = reinterpret_cast<const std::uint8_t*>(rebase(dir_light_va));
			const auto* local = reinterpret_cast<const std::uint8_t*>(rebase(local_light_va));
			if (!memory_readable(dir, 0x30) ||
				!memory_readable(local, static_cast<SIZE_T>(local_slots * local_stride)))
			{
				return false;
			}

			static bool logged = false;
			if (!logged)
			{
				logged = true;
				shared::common::log("Lights", std::format(
					"dll3impact base=0x{:X} dir=0x{:X} local=0x{:X} (iLightDirectional* / iLightLocal*)",
					reinterpret_cast<std::uintptr_t>(engine_module),
					reinterpret_cast<std::uintptr_t>(dir),
					reinterpret_cast<std::uintptr_t>(local)));
			}

			dir_at = dir;
			local_at = local;
			return true;
		}

		void take_module_leaf(HMODULE mod, char* dest, const int cap)
		{
			if (!dest || cap <= 0) {
				return;
			}
			dest[0] = 0;
			if (!mod) {
				return;
			}
			char path[MAX_PATH]{};
			if (!GetModuleFileNameA(mod, path, MAX_PATH) || !path[0]) {
				return;
			}
			const char* leaf = path;
			for (const char* p = path; *p; p++) {
				if (*p == '\\' || *p == '/') {
					leaf = p + 1;
				}
			}
			std::strncpy(dest, leaf, static_cast<std::size_t>(cap) - 1);
			dest[cap - 1] = 0;
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

			const int count = *count_at;
			host_module = mod;
			host_layout_tag = layout.tag;
			take_module_leaf(mod, host_exe_name, 64);
			host_count_at = count_at;
			host_list_at = list_at;
			host_hmod_at = hmod_at;
			host_list_ok = true;

			int plugin_hits = 0;
			int point_hits = 0;
			int sun_hits = 0;
			for (int i = 0; i < count && i < 512; i++)
			{
				const HMODULE plugin_mod = hmod_at[i];
				if (!plugin_mod) {
					continue;
				}
				char path[MAX_PATH]{};
				if (!GetModuleFileNameA(plugin_mod, path, MAX_PATH) || !path[0]) {
					continue;
				}
				plugin_hits++;
				const plugin_kind kind = kind_from_path(path);
				if (kind == plugin_kind::point) {
					point_hits++;
				}
				else if (kind == plugin_kind::sun) {
					sun_hits++;
				}
			}

			shared::common::log("Lights", std::format(
				"host exe={} layout={} base=0x{:X} count=0x{:X} list=0x{:X} hmod=0x{:X} "
				"n={} plugins={} PointLight={} SunLight={}",
				host_exe_name[0] ? host_exe_name : "?",
				layout.tag,
				reinterpret_cast<std::uintptr_t>(mod),
				reinterpret_cast<std::uintptr_t>(count_at),
				reinterpret_cast<std::uintptr_t>(list_at),
				reinterpret_cast<std::uintptr_t>(hmod_at),
				count, plugin_hits, point_hits, sun_hits));
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
				cached_plugin_n = 0;
				cached_host_count = -1;
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

			static bool logged_fail = false;
			if (!logged_fail)
			{
				logged_fail = true;
				char leaf[64]{};
				take_module_leaf(self, leaf, 64);
				shared::common::log("Lights", std::format(
					"host tables not bound yet (exe={} editor={} player layout pending)",
					leaf[0] ? leaf : "?",
					editor ? 1 : 0));
			}
			return false;
		}

		bool finite3(const float* v)
		{
			return v && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
		}

		float vec_len2(const D3DXVECTOR3& v)
		{
			return v.x * v.x + v.y * v.y + v.z * v.z;
		}

		float color_strength(const float* rgb)
		{
			if (!finite3(rgb)) {
				return 0.0f;
			}
			return std::max(rgb[0], std::max(rgb[1], rgb[2]));
		}

		bool normalize_dir(const float* in, D3DXVECTOR3& out)
		{
			if (!finite3(in)) {
				return false;
			}
			const float len = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
			if (!(len > 1e-4f) || !std::isfinite(len)) {
				return false;
			}
			out = { in[0] / len, in[1] / len, in[2] / len };
			return true;
		}

		bool read_f3(const std::uint8_t* base, const int off, D3DXVECTOR3& out)
		{
			if (!base || !memory_readable(base + off, sizeof(float) * 3)) {
				return false;
			}
			const auto* v = reinterpret_cast<const float*>(base + off);
			if (!finite3(v)) {
				return false;
			}
			out = { v[0], v[1], v[2] };
			return true;
		}

		bool read_quat(const std::uint8_t* base, const int off, float q[4])
		{
			if (!base || !q || !memory_readable(base + off, sizeof(float) * 4)) {
				return false;
			}
			const auto* v = reinterpret_cast<const float*>(base + off);
			if (!std::isfinite(v[0]) || !std::isfinite(v[1]) ||
				!std::isfinite(v[2]) || !std::isfinite(v[3]))
			{
				return false;
			}
			const float n2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3];
			if (!(n2 > 0.81f && n2 < 1.21f)) {
				return false;
			}
			q[0] = v[0];
			q[1] = v[1];
			q[2] = v[2];
			q[3] = v[3];
			return true;
		}

		void quat_rotate(const float q[4], const D3DXVECTOR3& v, D3DXVECTOR3& out)
		{
			const float tx = 2.0f * (q[1] * v.z - q[2] * v.y);
			const float ty = 2.0f * (q[2] * v.x - q[0] * v.z);
			const float tz = 2.0f * (q[0] * v.y - q[1] * v.x);
			out.x = v.x + q[3] * tx + (q[1] * tz - q[2] * ty);
			out.y = v.y + q[3] * ty + (q[2] * tx - q[0] * tz);
			out.z = v.z + q[3] * tz + (q[0] * ty - q[1] * tx);
		}

		bool looks_like_host(const std::uint8_t* p)
		{
			if (!p || !memory_readable(p, static_cast<SIZE_T>(host_header_size))) {
				return false;
			}
			const auto* plugin = *reinterpret_cast<void* const*>(p);
			const auto type = *reinterpret_cast<const std::int16_t*>(p + host_off_type);
			return plugin && memory_readable(plugin, 16) && type >= 0 && type < 0x200;
		}

		int slot_of_host(const void* host, const int count)
		{
			if (!host || !host_list_at) {
				return -1;
			}
			for (int i = 0; i < count; i++)
			{
				if (host_list_at[i] == host) {
					return i;
				}
			}
			return -1;
		}

		int slot_of_plugin(const void* plugin, const int count)
		{
			if (!plugin || !host_list_at) {
				return -1;
			}
			for (int i = 0; i < count; i++)
			{
				const auto* host = static_cast<const std::uint8_t*>(host_list_at[i]);
				if (!host || !memory_readable(host, sizeof(void*))) {
					continue;
				}
				if (*reinterpret_cast<void* const*>(host) == plugin) {
					return i;
				}
			}
			return -1;
		}

		int slot_from_handle(const void* handle, const int count)
		{
			if (!handle) {
				return -1;
			}
			const auto value = reinterpret_cast<std::uintptr_t>(handle);
			if (value < static_cast<std::uintptr_t>(count)) {
				return static_cast<int>(value);
			}
			const int by_host = slot_of_host(handle, count);
			if (by_host >= 0) {
				return by_host;
			}
			return slot_of_plugin(handle, count);
		}

		bool sane_pos(const D3DXVECTOR3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
				std::fabs(v.x) < k_pos_limit && std::fabs(v.y) < k_pos_limit &&
				std::fabs(v.z) < k_pos_limit;
		}

		bool significant_pos(const D3DXVECTOR3& v)
		{
			return sane_pos(v) && vec_len2(v) >= 1.0e-4f;
		}

		bool try_offset_pos(const std::uint8_t* obj, const int off, const int need, D3DXVECTOR3& out)
		{
			if (!obj || !memory_readable(obj, static_cast<SIZE_T>(need))) {
				return false;
			}
			D3DXVECTOR3 v{};
			if (!read_f3(obj, off, v) || !sane_pos(v)) {
				return false;
			}
			out = v;
			return true;
		}

		bool try_impact_pos(const std::uint8_t* obj, D3DXVECTOR3& out)
		{
			if (!obj || !memory_readable(obj, 16)) {
				return false;
			}

			// Origin is a valid unparented spawn, but it is also what we get
			// when +0x15A8 / +0x50 is empty padding. Prefer the longest sane
			// vector; reject an all-zero hit so the caller can try the next
			// source (child+0x2924 vs parent plugin vs SkinMesh child).
			D3DXVECTOR3 best{};
			float best_len2 = -1.0f;
			const auto consider = [&](const int off, const int need)
			{
				D3DXVECTOR3 v{};
				if (!try_offset_pos(obj, off, need, v)) {
					return;
				}
				const float len2 = vec_len2(v);
				if (len2 > best_len2) {
					best = v;
					best_len2 = len2;
				}
			};

			if (memory_readable(obj, static_cast<SIZE_T>(skin_off_location + 12))) {
				consider(skin_off_location, skin_off_location + 12);
			}
			consider(mesh_off_location, mesh_off_location + 12);
			consider(body_off_location, body_off_location + 12);
			consider(cam_off_location, cam_off_location + 12);

			if (best_len2 < 1.0e-4f) {
				return false;
			}
			out = best;
			return true;
		}

		bool try_plugin_mesh_pos(const std::uint8_t* plugin, const int off, D3DXVECTOR3& out)
		{
			if (!plugin || !memory_readable(plugin + off, sizeof(void*))) {
				return false;
			}
			const auto* mesh = *reinterpret_cast<const std::uint8_t* const*>(plugin + off);
			return try_impact_pos(mesh, out);
		}

		bool plugin_world_pos(const std::uint8_t* plugin, const plugin_kind kind, D3DXVECTOR3& out)
		{
			if (!plugin) {
				return false;
			}

			if (kind == plugin_kind::skin)
			{
				if (try_plugin_mesh_pos(plugin, skin_off_mesh, out)) {
					return true;
				}
				if (try_offset_pos(plugin, skin_off_location, skin_off_location + 12, out)) {
					return true;
				}
			}
			if (kind == plugin_kind::body)
			{
				if (try_plugin_mesh_pos(plugin, skin_off_mesh, out)) {
					return true;
				}
				if (try_offset_pos(plugin, body_off_location, body_off_location + 12, out)) {
					return true;
				}
				if (try_offset_pos(plugin, cam_off_location, cam_off_location + 12, out)) {
					return true;
				}
			}
			if (kind == plugin_kind::point || kind == plugin_kind::sun)
			{
				if (try_plugin_mesh_pos(plugin, point_off_gizmo, out) && significant_pos(out)) {
					return true;
				}
				return read_f3(plugin, point_off_pos, out) && sane_pos(out);
			}
			if (kind == plugin_kind::camera &&
				read_f3(plugin, cam_off_location, out) && sane_pos(out))
			{
				return true;
			}
			return try_impact_pos(plugin, out);
		}

		bool plugin_world_quat(const std::uint8_t* plugin, float q[4])
		{
			return read_quat(plugin, plugin_off_quat, q);
		}

		bool host_world_pos(const int slot, const int count, D3DXVECTOR3& out, float q[4], bool& have_q)
		{
			have_q = false;
			if (slot < 0 || slot >= count || !host_list_at) {
				return false;
			}

			const auto* host = static_cast<const std::uint8_t*>(host_list_at[slot]);
			if (!host || !memory_readable(host, sizeof(void*))) {
				return false;
			}

			if (memory_readable(host + host_off_parent_handle, sizeof(void*)))
			{
				const auto* impact = *reinterpret_cast<const std::uint8_t* const*>(
					host + host_off_parent_handle);
				if (impact && try_impact_pos(impact, out) && significant_pos(out))
				{
					const auto* plugin = *reinterpret_cast<const std::uint8_t* const*>(host);
					have_q = plugin_world_quat(plugin, q);
					return true;
				}
			}

			const auto* plugin = *reinterpret_cast<const std::uint8_t* const*>(host);
			plugin_kind kind = plugin_kind::other;
			if (host_hmod_at)
			{
				char path[MAX_PATH]{};
				if (GetModuleFileNameA(host_hmod_at[slot], path, MAX_PATH) && path[0]) {
					kind = kind_from_path(path);
				}
			}

			if (!plugin_world_pos(plugin, kind, out) || !significant_pos(out)) {
				return false;
			}
			have_q = plugin_world_quat(plugin, q);
			return true;
		}

		int find_parent_slot(const int child_slot, const int count, int& bone_id)
		{
			bone_id = 0;
			if (child_slot < 0 || child_slot >= count || !host_list_at) {
				return -1;
			}

			const auto* child_host = static_cast<const std::uint8_t*>(host_list_at[child_slot]);
			if (child_host && memory_readable(child_host, static_cast<SIZE_T>(host_header_size)))
			{
				const int linked = slot_from_handle(
					*reinterpret_cast<void* const*>(child_host + host_off_linked), count);
				if (linked >= 0 && linked != child_slot) {
					return linked;
				}
				const int via_2924 = slot_from_handle(
					*reinterpret_cast<void* const*>(child_host + host_off_parent_handle), count);
				if (via_2924 >= 0 && via_2924 != child_slot) {
					return via_2924;
				}
			}

			for (int i = 0; i < count; i++)
			{
				if (i == child_slot) {
					continue;
				}
				const auto* host = static_cast<const std::uint8_t*>(host_list_at[i]);
				if (!host || !memory_readable(host + host_off_child_count, 8)) {
					continue;
				}
				const int nchild = *reinterpret_cast<const int*>(host + host_off_child_count);
				if (nchild <= 0 || nchild > host_child_cap) {
					continue;
				}
				const auto* arr = *reinterpret_cast<const std::uint8_t* const*>(
					host + host_off_child_array);
				if (!arr || !memory_readable(arr, static_cast<SIZE_T>(nchild * host_child_stride))) {
					continue;
				}
				for (int k = 0; k < nchild; k++)
				{
					const auto* entry = arr + k * host_child_stride;
					const int idx = *reinterpret_cast<const int*>(entry);
					const auto* as_ptr = *reinterpret_cast<void* const*>(entry);
					if (idx == child_slot || as_ptr == host_list_at[child_slot])
					{
						bone_id = *reinterpret_cast<const int*>(entry + 4);
						return i;
					}
				}
			}
			return -1;
		}

		bool parent_skin_child_pos(const int parent_slot, const int count,
			D3DXVECTOR3& out, float q[4], bool& have_q)
		{
			have_q = false;
			if (parent_slot < 0 || parent_slot >= count || !host_list_at) {
				return false;
			}

			const auto* host = static_cast<const std::uint8_t*>(host_list_at[parent_slot]);
			if (!host || !memory_readable(host + host_off_child_count, 8)) {
				return false;
			}

			const int nchild = *reinterpret_cast<const int*>(host + host_off_child_count);
			if (nchild <= 0 || nchild > host_child_cap) {
				return false;
			}
			const auto* arr = *reinterpret_cast<const std::uint8_t* const*>(
				host + host_off_child_array);
			if (!arr || !memory_readable(arr, static_cast<SIZE_T>(nchild * host_child_stride))) {
				return false;
			}

			for (int k = 0; k < nchild; k++)
			{
				const auto* entry = arr + k * host_child_stride;
				int idx = *reinterpret_cast<const int*>(entry);
				if (idx < 0 || idx >= count) {
					idx = slot_from_handle(*reinterpret_cast<void* const*>(entry), count);
				}
				if (idx < 0 || idx >= count || !host_hmod_at) {
					continue;
				}

				char path[MAX_PATH]{};
				if (!GetModuleFileNameA(host_hmod_at[idx], path, MAX_PATH) || !path[0]) {
					continue;
				}
				if (kind_from_path(path) != plugin_kind::skin) {
					continue;
				}

				const auto* child_host = static_cast<const std::uint8_t*>(host_list_at[idx]);
				if (!child_host || !memory_readable(child_host, sizeof(void*))) {
					continue;
				}
				const auto* child_plugin = *reinterpret_cast<const std::uint8_t* const*>(child_host);
				if (plugin_world_pos(child_plugin, plugin_kind::skin, out) && significant_pos(out))
				{
					have_q = plugin_world_quat(child_plugin, q);
					return true;
				}
			}
			return false;
		}

		bool resolve_world_pos(const cached_plugin& rec, const std::uint8_t* plugin,
			const int count, D3DXVECTOR3& world)
		{
			D3DXVECTOR3 local{};
			const bool have_local = read_f3(plugin, point_off_pos, local) && sane_pos(local);
			if (!have_local) {
				local = {};
			}

			D3DXVECTOR3 xform{};
			const bool have_xform = read_f3(plugin, point_off_xform, xform) && significant_pos(xform);

			D3DXVECTOR3 mesh{};
			const bool have_mesh = try_plugin_mesh_pos(plugin, point_off_gizmo, mesh) &&
				significant_pos(mesh);

			int bone_id = rec.bone_id;
			int parent_slot = find_parent_slot(rec.slot, count, bone_id);
			if (parent_slot < 0) {
				parent_slot = rec.parent_slot;
			}

			D3DXVECTOR3 parent{};
			float q[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
			bool have_q = false;
			bool have_parent = false;
			const char* parent_src = "-";

			// THIS light's host+0x2924 is the parent 3Impact object. Reading
			// the parent's own +0x2924 (grandparent / empty) was returning
			// origin, so every PointLight sat at (0,0,0).
			if (rec.host && memory_readable(rec.host + host_off_parent_handle, sizeof(void*)))
			{
				const auto* impact = *reinterpret_cast<const std::uint8_t* const*>(
					rec.host + host_off_parent_handle);
				if (try_impact_pos(impact, parent) && significant_pos(parent))
				{
					have_parent = true;
					parent_src = "child+0x2924";
					if (parent_slot >= 0 && parent_slot < count && host_list_at)
					{
						const auto* phost = static_cast<const std::uint8_t*>(host_list_at[parent_slot]);
						if (phost && memory_readable(phost, sizeof(void*)))
						{
							const auto* pplugin = *reinterpret_cast<const std::uint8_t* const*>(phost);
							have_q = plugin_world_quat(pplugin, q);
						}
					}
				}
			}

			if (!have_parent && parent_slot >= 0 &&
				host_world_pos(parent_slot, count, parent, q, have_q) &&
				significant_pos(parent))
			{
				have_parent = true;
				parent_src = "parent plugin";
			}

			if (!have_parent && parent_slot >= 0 &&
				parent_skin_child_pos(parent_slot, count, parent, q, have_q) &&
				significant_pos(parent))
			{
				have_parent = true;
				parent_src = "parent SkinMesh";
			}

			const char* world_src = "-";
			bool have_world = false;

			if (have_parent)
			{
				D3DXVECTOR3 offset = local;
				if (have_q) {
					quat_rotate(q, local, offset);
				}
				world = { parent.x + offset.x, parent.y + offset.y, parent.z + offset.z };
				if (sane_pos(world))
				{
					have_world = true;
					world_src = "parent+local";
				}
			}

			// Gizmo already includes bone attach. Only take it when it is a
			// real world (not origin padding) and disagrees with local.
			if (have_mesh)
			{
				const float dx = mesh.x - local.x;
				const float dy = mesh.y - local.y;
				const float dz = mesh.z - local.z;
				if (dx * dx + dy * dy + dz * dz > 0.0025f)
				{
					world = mesh;
					have_world = true;
					world_src = "gizmo";
				}
			}

			if (!have_world && have_xform)
			{
				world = xform;
				have_world = true;
				world_src = "+0x4C8";
			}
			if (!have_world && have_local && significant_pos(local))
			{
				world = local;
				have_world = true;
				world_src = "local";
			}
			if (!have_world && have_local)
			{
				world = local;
				have_world = sane_pos(world);
				world_src = "origin";
			}

			static bool logged[remix_light_cap]{};
			if (rec.slot >= 0)
			{
				const int li = rec.slot % remix_light_cap;
				if (!logged[li])
				{
					logged[li] = true;
					char parent_path[MAX_PATH]{};
					if (parent_slot >= 0 && parent_slot < count && host_hmod_at) {
						GetModuleFileNameA(host_hmod_at[parent_slot], parent_path, MAX_PATH);
					}
					const char* parent_leaf = parent_path;
					for (const char* p = parent_path; *p; p++) {
						if (*p == '\\' || *p == '/') {
							parent_leaf = p + 1;
						}
					}
					shared::common::log("Lights", std::format(
						"PointLight slot={} parent={} ({}) bone={} src={} via={} "
						"local=({:.2f},{:.2f},{:.2f}) parent=({:.2f},{:.2f},{:.2f}) "
						"gizmo=({:.2f},{:.2f},{:.2f}) xform=({:.2f},{:.2f},{:.2f}) "
						"world=({:.2f},{:.2f},{:.2f})",
						rec.slot, parent_slot, parent_leaf[0] ? parent_leaf : "-",
						bone_id, world_src, parent_src,
						local.x, local.y, local.z,
						parent.x, parent.y, parent.z,
						mesh.x, mesh.y, mesh.z,
						xform.x, xform.y, xform.z,
						world.x, world.y, world.z));
				}
			}
			(void)bone_id;
			return have_world && sane_pos(world);
		}

		bool resolve_world_dir(const cached_plugin& rec, const std::uint8_t* plugin,
			const int count, D3DXVECTOR3& world_dir)
		{
			D3DXVECTOR3 local{};
			if (!read_f3(plugin, sun_off_dir, local) || vec_len2(local) < 1e-8f) {
				local = { 0.0f, -1.0f, 0.0f };
			}

			int bone_id = rec.bone_id;
			int parent_slot = find_parent_slot(rec.slot, count, bone_id);
			if (parent_slot < 0) {
				parent_slot = rec.parent_slot;
			}
			(void)bone_id;

			if (parent_slot >= 0)
			{
				D3DXVECTOR3 parent{};
				float q[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
				bool have_q = false;
				if (host_world_pos(parent_slot, count, parent, q, have_q) && have_q)
				{
					quat_rotate(q, local, world_dir);
					return normalize_dir(&world_dir.x, world_dir);
				}
			}

			return normalize_dir(&local.x, world_dir);
		}

		void zero_d3d_light(D3DLIGHT9& light)
		{
			light = {};
			light.Range = 1.0f;
			light.Falloff = 1.0f;
			light.Attenuation0 = 1.0f;
		}

		void to_d3d(const captured_light& src, D3DLIGHT9& light)
		{
			zero_d3d_light(light);
			// Range factor is brightness × reach, not emitter size. Remix
			// converts SetLight POINT to a sphere with a *fixed* radius
			// (lightConversionSphereLightFixedRadius, default 4). Diffuse
			// carries RGB × intensity; Range is attenuation distance only.
			const float intensity = src.directional
				? 1.0f
				: std::min(20.0f, std::max(1.0f, src.range / 25.0f));
			light.Diffuse = {
				src.color.x * intensity,
				src.color.y * intensity,
				src.color.z * intensity,
				1.0f };
			light.Specular = { 0.0f, 0.0f, 0.0f, 1.0f };
			if (src.directional)
			{
				light.Type = D3DLIGHT_DIRECTIONAL;
				light.Direction = { src.dir.x, src.dir.y, src.dir.z };
				light.Ambient = {
					src.color.x * 0.15f,
					src.color.y * 0.15f,
					src.color.z * 0.15f,
					1.0f };
				light.Range = 1.0f;
			}
			else
			{
				light.Type = D3DLIGHT_POINT;
				light.Position = { src.pos.x, src.pos.y, src.pos.z };
				light.Range = src.range;
				light.Attenuation0 = 1.0f;
				light.Attenuation1 = (src.range > 1.0f) ? (1.0f / src.range) : 0.0f;
				light.Attenuation2 = 0.0f;
			}
		}

		float world_range(const float factor)
		{
			float r = 25.0f;
			if (std::isfinite(factor) && factor > 0.05f && factor <= 20.0f) {
				r = 25.0f * factor;
			}
			else if (std::isfinite(factor) && factor > 20.0f && factor <= 10000.0f) {
				r = factor;
			}
			if (r > k_range_max) {
				r = k_range_max;
			}
			return r;
		}

		std::uint64_t mix_u64(std::uint64_t h, const std::uint64_t x)
		{
			h ^= x;
			h *= 1099511628211ull;
			return h;
		}

		std::uint64_t mix_f(std::uint64_t h, const float v)
		{
			const auto q = static_cast<std::uint32_t>(std::llround(static_cast<double>(v) * 1000.0));
			return mix_u64(h, q);
		}

		std::uint64_t mix_v(std::uint64_t h, const D3DXVECTOR3& v)
		{
			h = mix_f(h, v.x);
			h = mix_f(h, v.y);
			h = mix_f(h, v.z);
			return h;
		}

		std::uint64_t identity_of(const std::uint8_t* plugin, const bool directional)
		{
			const auto ptr = static_cast<std::uint32_t>(
				reinterpret_cast<std::uintptr_t>(plugin));
			std::uint64_t id = directional ? 0x53554E3100000000ull : 0x504C000000000000ull;
			id |= ptr ? ptr : 1u;
			return id;
		}

		std::uint64_t content_of(const captured_light& L)
		{
			std::uint64_t h = 1469598103934665603ull;
			h = mix_u64(h, L.directional ? 1ull : 0ull);
			h = mix_u64(h, static_cast<std::uint64_t>(L.shape));
			h = mix_v(h, L.pos);
			h = mix_v(h, L.dir);
			h = mix_v(h, L.color);
			h = mix_f(h, L.range);
			return h;
		}

		bool add_directional(captured_light* out, int& n, const int max_n,
			const float* dir, const float* col, const std::uint64_t identity)
		{
			D3DXVECTOR3 ndir{};
			if (n >= max_n || color_strength(col) <= 0.01f || !normalize_dir(dir, ndir)) {
				return false;
			}

			auto& slot = out[n++];
			slot = {};
			slot.directional = true;
			slot.dir = ndir;
			slot.color = { col[0], col[1], col[2] };
			slot.range = 1.0f;
			slot.identity = identity ? identity : 0x53554E31ull;
			slot.content = content_of(slot);
			return true;
		}

		bool add_point(captured_light* out, int& n, const int max_n,
			const float* pos, const float* rgb, const float range, const std::uint64_t identity,
			const light_shape shape, const char* title)
		{
			if (n >= max_n || !finite3(pos)) {
				return false;
			}
			const D3DXVECTOR3 p{ pos[0], pos[1], pos[2] };
			if (!sane_pos(p)) {
				return false;
			}

			auto& slot = out[n++];
			slot = {};
			slot.directional = false;
			slot.pos = { pos[0], pos[1], pos[2] };
			slot.color = finite3(rgb) ? D3DXVECTOR3{ rgb[0], rgb[1], rgb[2] }
				: D3DXVECTOR3{ 1.0f, 1.0f, 1.0f };
			slot.range = range;
			slot.shape = shape;
			slot.identity = identity;
			take_title(slot.title, title);
			slot.content = content_of(slot);
			return true;
		}

		bool cache_still_valid(const int count)
		{
			if (cached_plugin_n <= 0 || cached_host_count != count) {
				return false;
			}
			for (int i = 0; i < cached_plugin_n; i++)
			{
				const auto& rec = cached_plugins[i];
				if (rec.slot < 0 || rec.slot >= count) {
					return false;
				}
				if (host_list_at[rec.slot] != rec.host) {
					return false;
				}
				if (!rec.plugin || !memory_readable(rec.plugin, 16)) {
					return false;
				}
			}
			return true;
		}

		void rescan_plugins(const int count)
		{
			cached_plugin_n = 0;
			cached_host_count = count;
			for (int i = 0; i < count && cached_plugin_n < remix_light_cap; i++)
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

				const plugin_kind kind = kind_from_path(path);
				if (kind != plugin_kind::point && kind != plugin_kind::sun) {
					continue;
				}

				const auto* plugin = *reinterpret_cast<const std::uint8_t* const*>(host_obj);
				const int need = (kind == plugin_kind::point) ? point_instance_size : sun_instance_size;
				if (!plugin || !memory_readable(plugin, static_cast<SIZE_T>(need))) {
					continue;
				}

				auto& rec = cached_plugins[cached_plugin_n++];
				rec = {};
				rec.slot = i;
				rec.host = host_obj;
				rec.plugin = plugin;
				rec.kind = kind;
				rec.parent_slot = find_parent_slot(i, count, rec.bone_id);
				read_host_title(host_obj, plugin, need, rec.title);
				rec.shape = shape_from_title(rec.title);
			}
		}

		int collect_host_objects(captured_light* out, const int max_n, int& sun_objects)
		{
			sun_objects = 0;
			if (!out || max_n <= 0 || !bind_host()) {
				return 0;
			}

			const int count = *host_count_at;
			if (count <= 0 || count > host_max_objects) {
				cached_plugin_n = 0;
				cached_host_count = -1;
				return 0;
			}

			if (!cache_still_valid(count)) {
				rescan_plugins(count);
			}

			int n = 0;
			for (int i = 0; i < cached_plugin_n && n < max_n; i++)
			{
				auto& rec = cached_plugins[i];
				const auto* plugin = rec.plugin;
				if (!plugin) {
					continue;
				}

				int shown = 1;
				if (memory_readable(plugin + plugin_off_shown, sizeof(int))) {
					shown = *reinterpret_cast<const int*>(plugin + plugin_off_shown);
				}

				if (rec.kind == plugin_kind::sun)
				{
					const auto* rgb = reinterpret_cast<const float*>(plugin + point_off_rgb);
					D3DXVECTOR3 dir{};
					if (!resolve_world_dir(rec, plugin, count, dir)) {
						if (rec.have_last) {
							dir = rec.last_dir;
						}
						else {
							continue;
						}
					}
					float col[3] = { 1.0f, 1.0f, 1.0f };
					if (finite3(rgb) && color_strength(rgb) > 0.01f)
					{
						col[0] = rgb[0];
						col[1] = rgb[1];
						col[2] = rgb[2];
					}
					else if (rec.have_last)
					{
						col[0] = rec.last_color.x;
						col[1] = rec.last_color.y;
						col[2] = rec.last_color.z;
					}
					if (shown == 0 && !rec.have_last) {
						continue;
					}
					sun_objects++;
					add_directional(out, n, max_n, &dir.x, col, identity_of(plugin, true));
					rec.have_last = true;
					rec.last_dir = dir;
					rec.last_color = { col[0], col[1], col[2] };
					continue;
				}

				const auto* rgb = reinterpret_cast<const float*>(plugin + point_off_rgb);
				float factor = 1.0f;
				if (memory_readable(plugin + point_off_range, sizeof(float))) {
					factor = *reinterpret_cast<const float*>(plugin + point_off_range);
				}
				D3DXVECTOR3 world{};
				if (!resolve_world_pos(rec, plugin, count, world) || !sane_pos(world)) {
					if (rec.have_last && sane_pos(rec.last_pos)) {
						world = rec.last_pos;
					}
					else {
						continue;
					}
				}

				float col[3] = { 1.0f, 1.0f, 1.0f };
				if (finite3(rgb) && color_strength(rgb) > 0.01f)
				{
					col[0] = rgb[0];
					col[1] = rgb[1];
					col[2] = rgb[2];
				}
				else if (rec.have_last)
				{
					col[0] = rec.last_color.x;
					col[1] = rec.last_color.y;
					col[2] = rec.last_color.z;
				}

				float range = world_range(factor);
				if (!(range > 0.05f) && rec.have_last) {
					range = rec.last_range;
				}

				// Keep emitting while the plugin is still in the list. Scripted
				// Show/Hide and RGB=0 pulses used to drop the Remix light.
				if (shown == 0 && !rec.have_last && color_strength(col) <= 0.01f) {
					continue;
				}

				add_point(out, n, max_n, &world.x, col, range,
					identity_of(plugin, false), rec.shape, rec.title);
				if (sane_pos(world))
				{
					rec.have_last = true;
					rec.last_pos = world;
					rec.last_color = { col[0], col[1], col[2] };
					rec.last_range = range;
					rec.last_shown = shown;
				}
			}

			static int logged_n = -1;
			static int logged_suns = -1;
			if (n != logged_n || sun_objects != logged_suns)
			{
				logged_n = n;
				logged_suns = sun_objects;
				shared::common::log("Lights", std::format(
					"host exe={} layout={} objects={} cached lights={} shown this frame={} SunLight plugins={}",
					host_exe_name[0] ? host_exe_name : "?",
					host_layout_tag ? host_layout_tag : "?",
					count, cached_plugin_n, n, sun_objects));
			}

			return n;
		}

		int collect_bss_points(captured_light* out, int n, const int max_n)
		{
			for (int i = 0; i < local_slots && n < max_n; i++)
			{
				const auto* rec = local_at + i * local_stride;
				const auto* pos = reinterpret_cast<const float*>(rec);
				const auto* rgb = reinterpret_cast<const float*>(rec + local_off_color);
				const float factor = *reinterpret_cast<const float*>(rec + local_off_range);
				// SkinMesh 3-nearest template is rgb=0 / origin until a slot
				// is assigned. Emitting those as SetLight POINT made compiled
				// games look unlit (black spheres at C).
				if (color_strength(rgb) <= 0.01f) {
					continue;
				}
				if (!finite3(pos) || !sane_pos({ pos[0], pos[1], pos[2] })) {
					continue;
				}
				add_point(out, n, max_n, pos, rgb, world_range(factor),
					0x504C3000ull + static_cast<std::uint64_t>(i),
					light_shape::sphere, nullptr);
			}
			return n;
		}

		int collect_into(captured_light* out, const int max_n)
		{
			if (!out || max_n <= 0 || !bind_engine()) {
				return 0;
			}

			int sun_objects = 0;
			int n = 0;

			captured_light host_pts[remix_light_cap]{};
			const int host_n = collect_host_objects(host_pts, max_n, sun_objects);

			bool have_plugin_sun = false;
			for (int i = 0; i < host_n && n < max_n; i++)
			{
				if (host_pts[i].directional) {
					have_plugin_sun = true;
				}
				out[n++] = host_pts[i];
			}

			// One directional from iLightDirectional* when no SunLight object
			// wrote a parent-resolved direction this frame. Engine default sun
			// and SunLight ObjectRun share this buffer — emit once.
			if (!have_plugin_sun)
			{
				const int enabled = *reinterpret_cast<const int*>(dir_at + dir_off_enabled);
				const auto* dir = reinterpret_cast<const float*>(dir_at);
				const auto* col = reinterpret_cast<const float*>(dir_at + dir_off_color);
				if (enabled == 1) {
					captured_light tmp[1]{};
					int tn = 0;
					if (add_directional(tmp, tn, 1, dir, col, 0x53554E31ull) && n < max_n)
					{
						for (int i = n; i > 0; i--) {
							out[i] = out[i - 1];
						}
						out[0] = tmp[0];
						n++;
					}
				}
			}
			else
			{
				// Parent rotation aims the sun — still refresh BSS into SetLight
				// if the plugin path missed a finite dir.
				(void)sun_objects;
			}

			int points = 0;
			int suns = 0;
			for (int i = 0; i < n; i++)
			{
				if (out[i].directional) {
					suns++;
				}
				else {
					points++;
				}
			}

			// BSS local slots are SkinMesh's 3 nearest lights, not the object
			// list. Use them only when the host walk found no PointLights.
			if (points == 0) {
				n = collect_bss_points(out, n, max_n);
			}

			return n;
		}

		void destroy_api_slot(api_slot& slot)
		{
			auto& api = shared::common::remix_api::get();
			if (slot.handle && api.is_initialized() && api.m_bridge.DestroyLight) {
				api.m_bridge.DestroyLight(slot.handle);
			}
			slot.handle = nullptr;
			slot.identity = 0;
			slot.content = 0;
			slot.in_use = false;
		}

		void fill_basis(const captured_light& src, D3DXVECTOR3& x_axis, D3DXVECTOR3& y_axis,
			D3DXVECTOR3& z_axis)
		{
			D3DXVECTOR3 z = src.dir;
			if (vec_len2(z) < 1e-8f) {
				z = { 0.0f, -1.0f, 0.0f };
			}
			normalize_dir(&z.x, z);
			D3DXVECTOR3 up = { 0.0f, 1.0f, 0.0f };
			if (std::fabs(z.x * up.x + z.y * up.y + z.z * up.z) > 0.95f) {
				up = { 1.0f, 0.0f, 0.0f };
			}
			x_axis = {
				up.y * z.z - up.z * z.y,
				up.z * z.x - up.x * z.z,
				up.x * z.y - up.y * z.x };
			normalize_dir(&x_axis.x, x_axis);
			y_axis = {
				z.y * x_axis.z - z.z * x_axis.y,
				z.z * x_axis.x - z.x * x_axis.z,
				z.x * x_axis.y - z.y * x_axis.x };
			normalize_dir(&y_axis.x, y_axis);
			z_axis = z;
		}

		bool fill_light_info(const captured_light& src, remixapi_LightInfo& info,
			remixapi_LightInfoSphereEXT& sphere, remixapi_LightInfoRectEXT& rect,
			remixapi_LightInfoDiskEXT& disk, remixapi_LightInfoCylinderEXT& cylinder)
		{
			info = {};
			info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			info.hash = src.identity;
			const float intensity = std::max(k_radiance_min, src.range * k_radiance_per_world);
			info.radiance = {
				src.color.x * intensity,
				src.color.y * intensity,
				src.color.z * intensity };

			D3DXVECTOR3 x_axis{}, y_axis{}, z_axis{};
			fill_basis(src, x_axis, y_axis, z_axis);
			const remixapi_Float3D pos{ src.pos.x, src.pos.y, src.pos.z };

			if (src.shape == light_shape::rect)
			{
				rect = {};
				rect.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT;
				rect.position = pos;
				rect.xAxis = { x_axis.x, x_axis.y, x_axis.z };
				rect.xSize = k_rect_size;
				rect.yAxis = { y_axis.x, y_axis.y, y_axis.z };
				rect.ySize = k_rect_size;
				rect.direction = { z_axis.x, z_axis.y, z_axis.z };
				rect.shaping_hasvalue = FALSE;
				rect.volumetricRadianceScale = 1.0f;
				info.pNext = &rect;
				return true;
			}
			if (src.shape == light_shape::disk)
			{
				disk = {};
				disk.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT;
				disk.position = pos;
				disk.xAxis = { x_axis.x, x_axis.y, x_axis.z };
				disk.xRadius = k_disk_radius;
				disk.yAxis = { y_axis.x, y_axis.y, y_axis.z };
				disk.yRadius = k_disk_radius;
				disk.direction = { z_axis.x, z_axis.y, z_axis.z };
				disk.shaping_hasvalue = FALSE;
				disk.volumetricRadianceScale = 1.0f;
				info.pNext = &disk;
				return true;
			}
			if (src.shape == light_shape::cylinder)
			{
				cylinder = {};
				cylinder.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT;
				cylinder.position = pos;
				cylinder.radius = k_cyl_radius;
				cylinder.axis = { y_axis.x, y_axis.y, y_axis.z };
				cylinder.axisLength = k_cyl_length;
				cylinder.volumetricRadianceScale = 1.0f;
				info.pNext = &cylinder;
				return true;
			}

			sphere = {};
			sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
			sphere.position = pos;
			sphere.radius = k_remix_sphere_radius;
			sphere.shaping_hasvalue = FALSE;
			sphere.volumetricRadianceScale = 1.0f;
			info.pNext = &sphere;
			return true;
		}

		void submit_remix_api(const captured_light* lights, const int n)
		{
			(void)lights;
			(void)n;
			if constexpr (!k_emit_remix_api_spheres) {
				return;
			}
			else
			{
			auto& api = shared::common::remix_api::get();
			const bool allowed = camera::remix_api_init_allowed() &&
				api.is_initialized() && api.m_bridge.CreateLight &&
				api.m_bridge.DrawLightInstance;
			if (!allowed)
			{
				return;
			}

			bool used[remix_light_cap]{};
			for (int i = 0; i < n && i < remix_light_cap; i++)
			{
				const auto& src = lights[i];
				if (src.directional) {
					continue;
				}
				api_slot* slot = nullptr;
				for (int s = 0; s < remix_light_cap; s++)
				{
					if (api_lights[s].in_use && api_lights[s].identity == src.identity)
					{
						slot = &api_lights[s];
						used[s] = true;
						break;
					}
				}
				if (!slot)
				{
					for (int s = 0; s < remix_light_cap; s++)
					{
						if (!api_lights[s].in_use)
						{
							slot = &api_lights[s];
							used[s] = true;
							break;
						}
					}
				}
				if (!slot) {
					continue;
				}

				remixapi_LightInfo info{};
				remixapi_LightInfoSphereEXT sphere{};
				remixapi_LightInfoRectEXT rect{};
				remixapi_LightInfoDiskEXT disk{};
				remixapi_LightInfoCylinderEXT cylinder{};
				fill_light_info(src, info, sphere, rect, disk, cylinder);

				const bool need_create = !slot->handle || slot->content != src.content;
				if (need_create)
				{
					remixapi_LightHandle created = nullptr;
					if (api.m_bridge.CreateLight(&info, &created) != REMIXAPI_ERROR_CODE_SUCCESS ||
						!created)
					{
						if (!slot->handle) {
							continue;
						}
					}
					else
					{
						if (slot->handle && slot->handle != created) {
							destroy_api_slot(*slot);
						}
						slot->handle = created;
						slot->identity = src.identity;
						slot->content = src.content;
						slot->in_use = true;
					}
				}

				if (slot->handle) {
					api.m_bridge.DrawLightInstance(slot->handle);
				}
			}

			for (int s = 0; s < remix_light_cap; s++)
			{
				if (api_lights[s].in_use && !used[s]) {
					destroy_api_slot(api_lights[s]);
				}
			}
			}
		}

		const char* shape_cstr(const light_shape s)
		{
			switch (s)
			{
			case light_shape::rect: return "rect";
			case light_shape::disk: return "disk";
			case light_shape::cylinder: return "cylinder";
			default: return "sphere";
			}
		}

		void log_capture_once(const captured_light* lights, const int n)
		{
			static int last_n = -1;
			static int last_sun = -1;
			static int last_pt = -1;
			static std::uint64_t last_ident = 0;
			static UINT last_log_frame = 0xFFFFFFFFu;

			std::uint64_t ident = static_cast<std::uint64_t>(n) + 1;
			for (int i = 0; i < n; i++) {
				ident = mix_u64(ident, lights[i].identity);
				ident = mix_u64(ident, lights[i].directional ? 1ull : 0ull);
				ident = mix_u64(ident, static_cast<std::uint64_t>(lights[i].shape));
			}

			const UINT frame = shared::common::ffp_state::get().frame_count();
			const bool count_changed = n != last_n || captured_sun != last_sun ||
				captured_point != last_pt || ident != last_ident;
			const bool heartbeat = !count_changed && n > 0 &&
				(frame % 60u) == 0u && frame != last_log_frame;
			if (!count_changed && !heartbeat) {
				return;
			}

			last_n = n;
			last_sun = captured_sun;
			last_pt = captured_point;
			last_ident = ident;
			last_log_frame = frame;

			if (count_changed)
			{
				shared::common::log("Lights", std::format(
					"Captured {} this frame (sun={} point={}) host={} layout={} - SetLight {} (Remix API lights off); fallback sun {}",
					n, captured_sun, captured_point,
					host_exe_name[0] ? host_exe_name : "unbound",
					host_layout_tag ? host_layout_tag : "-",
					n > 0 ? "fired" : "none",
					n == 0 ? "ON" : "off"));

				for (int i = 0; i < n && i < 8; i++)
				{
					const auto& L = lights[i];
					if (L.directional)
					{
						shared::common::log("Lights", std::format(
							"  [{}] Sun dir=({:.3f},{:.3f},{:.3f}) rgb=({:.3f},{:.3f},{:.3f})",
							i, L.dir.x, L.dir.y, L.dir.z, L.color.x, L.color.y, L.color.z));
					}
					else
					{
						shared::common::log("Lights", std::format(
							"  [{}] {} '{}' pos=({:.3f},{:.3f},{:.3f}) rgb=({:.3f},{:.3f},{:.3f}) range={:.2f}",
							i, shape_cstr(L.shape), L.title[0] ? L.title : "",
							L.pos.x, L.pos.y, L.pos.z, L.color.x, L.color.y, L.color.z, L.range));
					}
				}
			}
			else if (heartbeat)
			{
				const captured_light* pt = nullptr;
				for (int i = 0; i < n; i++)
				{
					if (!lights[i].directional) {
						pt = &lights[i];
						break;
					}
				}
				if (pt)
				{
					shared::common::log("Lights", std::format(
						"live sun={} pt={} first='{}' pos=({:.3f},{:.3f},{:.3f}) rgb=({:.3f},{:.3f},{:.3f}) range={:.1f}",
						captured_sun, captured_point,
						pt->title[0] ? pt->title : "",
						pt->pos.x, pt->pos.y, pt->pos.z,
						pt->color.x, pt->color.y, pt->color.z, pt->range));
				}
			}
		}
	}

	void on_frame()
	{
		const UINT frame = shared::common::ffp_state::get().frame_count();
		const bool allowed = camera::scene_conversion_allowed();
		if (frame == captured_frame && allowed == captured_allowed) {
			return;
		}
		captured_frame = frame;
		captured_allowed = allowed;

		if (!allowed)
		{
			captured = 0;
			captured_sun = 0;
			captured_point = 0;
			shared::common::ffp_state::get().set_scene_lights(nullptr, 0);
			return;
		}

		captured_light found[remix_light_cap]{};
		const int n = collect_into(found, remix_light_cap);
		captured = n;
		captured_sun = 0;
		captured_point = 0;
		for (int i = 0; i < n; i++)
		{
			if (found[i].directional) {
				captured_sun++;
			}
			else {
				captured_point++;
			}
			frame_lights[i] = found[i];
		}

		submit_remix_api(found, n);

		D3DLIGHT9 d3d[d3d_light_cap]{};
		const int d3d_n = std::min(n, d3d_light_cap);
		for (int i = 0; i < d3d_n; i++) {
			to_d3d(found[i], d3d[i]);
		}
		shared::common::ffp_state::get().set_scene_lights(d3d, d3d_n);

		if (IDirect3DDevice9* dev = shared::globals::d3d_device)
		{
			for (int i = 0; i < d3d_n; i++)
			{
				dev->SetLight(static_cast<DWORD>(i), &d3d[i]);
				dev->LightEnable(static_cast<DWORD>(i), TRUE);
			}
			for (int i = d3d_n; i < last_d3d_enabled && i < d3d_light_cap; i++) {
				dev->LightEnable(static_cast<DWORD>(i), FALSE);
			}
			last_d3d_enabled = d3d_n;
		}

		log_capture_once(found, n);
	}

	int captured_count()
	{
		return captured;
	}

	int captured_suns()
	{
		return captured_sun;
	}

	int captured_points()
	{
		return captured_point;
	}

	void reset()
	{
		captured = 0;
		captured_sun = 0;
		captured_point = 0;
		captured_frame = 0xFFFFFFFFu;
		captured_allowed = false;
		last_d3d_enabled = 0;
		dir_at = nullptr;
		local_at = nullptr;
		engine_module = nullptr;
		host_module = nullptr;
		host_layout_tag = nullptr;
		host_exe_name[0] = 0;
		host_count_at = nullptr;
		host_list_at = nullptr;
		host_hmod_at = nullptr;
		host_list_ok = false;
		cached_plugin_n = 0;
		cached_host_count = -1;
		for (auto& rec : cached_plugins) {
			rec = {};
		}
		for (auto& slot : api_lights)
		{
			if constexpr (k_emit_remix_api_spheres) {
				destroy_api_slot(slot);
			}
			else {
				slot = {};
			}
		}
		shared::common::ffp_state::get().set_scene_lights(nullptr, 0);
	}
}
