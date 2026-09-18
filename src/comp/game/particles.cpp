#include "std_include.hpp"
#include "particles.hpp"
#include "particles_dialog.hpp"
#include "camera.hpp"
#include "../editor_frame.hpp"
#include "../editor_settings.hpp"
#include "../project_file.hpp"

#include "shared/common/ffp_state.hpp"
#include "shared/common/remix_api.hpp"
#include "shared/globals.hpp"
#include "shared/utils/hooking.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace comp::game::particles
{
	static std::uint64_t g_pending_invalidate = 0;

	namespace
	{
		constexpr std::uintptr_t host_preferred_base = 0x00400000;
		constexpr int host_max_objects = 4096;
		constexpr int host_header_size = 0x2930;
		constexpr int host_off_child_count = 0x291C;
		constexpr int host_off_child_array = 0x2920;
		constexpr int host_off_parent_handle = 0x2924;
		constexpr int host_off_linked = 0x2928;
		constexpr int host_child_stride = 8;
		constexpr int host_child_cap = 256;

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

		constexpr int plugin_off_shown = 0x04;
		constexpr int plugin_off_active = 0x0C;
		constexpr int plugin_off_dir = 0x24;
		constexpr int plugin_off_name = 0x424;
		constexpr int plugin_off_quat = 0x488;
		constexpr int plugin_off_pos = 0x498;
		constexpr int plugin_off_live_count = 0x4D8;
		// Dialog GET (Properties DIALOGEX + ObjectPropertiesWrite):
		//   0x40A/0x40B/0x40C Gravity XYZ → +0x570/+0x574/+0x578
		//   0x40D Air (0 → -0.001) → +0x57C
		//   0x416 Opacity → +0x5BC  (not +0x574; that slot is gravity Y)
		constexpr int plugin_off_grav = 0x570;
		constexpr int plugin_off_air = 0x57C;
		constexpr int plugin_off_opacity = 0x5BC;
		constexpr int plugin_off_scale_init = 0x580;
		constexpr int plugin_off_scale_final = 0x584;
		// Remix minSize/maxSize are USDA centimeters. Dialog 1.00 → spawn 14 cm,
		// target 20 cm. Speed/gravity are meters in 3D Rad, centimeters in Remix.
		constexpr float k_rad_to_remix_cm = 1.0f;
		constexpr float k_spawn_vis = 14.0f;
		constexpr float k_target_vis = 20.0f;
		constexpr float k_spawn_cm_floor = 4.0f;
		constexpr float k_target_cm_floor = 6.0f;
		constexpr float k_ms_to_cms = 100.0f;
		// Dialog m/s → Remix cm/s, half vs the Particles parameters (was ×5).
		constexpr float k_speed_param = 0.5f;
		// kb.h: Scale +0x580/+0x584. Initial RGB +0x588, Final RGB +0x594 (3 floats each).
		constexpr int plugin_off_rgb_init = 0x588;
		constexpr int plugin_off_rgb_final = 0x594;
		// GET 0x40E Lifetime → +0x598. 0x40F/0x410 Speed min/max → +0x59C/+0x5A0.
		// +0x5A0 is Speed max, not TTL (that swap made Speed change lifetime).
		constexpr int plugin_off_lifetime = 0x598;
		constexpr int plugin_off_speed_min = 0x59C;
		constexpr int plugin_off_speed_max = 0x5A0;
		constexpr int plugin_off_emit_min = 0x5A4;
		constexpr int plugin_off_emit_max = 0x5A8;
		constexpr int plugin_off_frequency = 0x5B4;
		constexpr int plugin_off_reflect = 0x5C0;
		constexpr int plugin_off_burnout = 0x5C4;
		constexpr int plugin_off_bone = 0x5C8;
		constexpr int plugin_off_tex = 0x5CC;
		// Internal "Frame rate" sits in the emit/frequency gap. 0 = use 10 fps
		// when a numbered sequence exists under data\<leaf>\.
		constexpr int plugin_off_anim_fps = 0x5AC;
		constexpr int plugin_off_bone_dlg = 0xDD0;
		// GET 0x412 Timer (s) → +0xE34. 0 = emit for as long as Active.
		constexpr int plugin_off_timer = 0xE34;
		constexpr int plugin_instance_size = 0x1E5C;
		constexpr int remix_particle_cap = 16;
		constexpr float k_pos_limit = 10000.0f;
		// Remix 1.5.2 DrawInstance only serializes ParticleSystemEXT as sType 25.
		// sType 24 is DEPRECATED_LEGACY_PARTICLE_SYSTEM and the 32-bit bridge skips it.
		constexpr remixapi_StructType k_particle_ext_stype =
			static_cast<remixapi_StructType>(25);

		struct remixapi_AnimatedFloat1D_152 {
			float* pData;
			uint32_t numberElements;
		};
		struct remixapi_AnimatedFloat2D_152 {
			remixapi_Float2D* pData;
			uint32_t numberElements;
		};
		struct remixapi_AnimatedFloat3D_152 {
			remixapi_Float3D* pData;
			uint32_t numberElements;
		};
		struct remixapi_AnimatedFloat4D_152 {
			remixapi_Float4D* pData;
			uint32_t numberElements;
		};
		// Official remix-1.5.2 remix_c.h InstanceInfoParticleSystemEXT (animated curves).
		struct remixapi_InstanceInfoParticleSystemEXT_152 {
			remixapi_StructType sType;
			void* pNext;
			uint32_t maxNumParticles;
			remixapi_Bool useTurbulence;
			remixapi_Bool alignParticlesToVelocity;
			remixapi_Bool useSpawnTexcoords;
			remixapi_Bool enableCollisionDetection;
			remixapi_Bool enableMotionTrail;
			remixapi_Bool hideEmitter;
			remixapi_Bool restrictVelocityX;
			remixapi_Bool restrictVelocityY;
			remixapi_Bool restrictVelocityZ;
			remixapi_AnimatedFloat4D_152 minColor;
			remixapi_AnimatedFloat4D_152 maxColor;
			remixapi_AnimatedFloat1D_152 minRotationSpeed;
			remixapi_AnimatedFloat1D_152 maxRotationSpeed;
			remixapi_AnimatedFloat2D_152 minSize;
			remixapi_AnimatedFloat2D_152 maxSize;
			remixapi_AnimatedFloat3D_152 maxVelocity;
			remixapi_Float3D attractorPosition;
			float minTimeToLive;
			float maxTimeToLive;
			float initialVelocityFromNormal;
			float initialVelocityConeAngleDegrees;
			float dragCoefficient;
			float initialRotationDeviationDegrees;
			float gravityForce;
			float turbulenceFrequency;
			float turbulenceForce;
			float spawnRatePerSecond;
			float collisionThickness;
			float collisionRestitution;
			float motionTrailMultiplier;
			float initialVelocityFromMotion;
			float spawnBurstDuration;
			float attractorRadius;
			float attractorForce;
			uint8_t billboardType;
			uint8_t spriteSheetMode;
			uint8_t collisionMode;
			uint8_t randomFlipAxis;
		};

		constexpr int mesh_off_location = 0xCF8;
		constexpr int skin_off_location = 0x15A8;
		constexpr int cam_off_location = 0x50;
		constexpr int body_off_location = 0x5B4;

		HMODULE host_module = nullptr;
		const char* host_layout_tag = nullptr;
		char host_exe_name[64]{};
		const int* host_count_at = nullptr;
		void* const* host_list_at = nullptr;
		HMODULE const* host_hmod_at = nullptr;
		bool host_list_ok = false;

		int captured = 0;
		UINT work_frame = 0xFFFFFFFFu;
		bool work_allowed = false;

		struct slot_emitter
		{
			std::uint64_t identity = 0;
			int slot = -1;
			remixapi_MaterialHandle mat = nullptr;
			remixapi_MeshHandle mesh = nullptr;
			bool ready = false;
			std::uint8_t sheet_rows = 1;
			std::uint8_t sheet_cols = 1;
			std::uint8_t sheet_fps = 0;
			char tex_key[MAX_PATH]{};
		};
		slot_emitter emitters[remix_particle_cap]{};
		bool emitter_ready = false;
		bool emitter_failed = false;
		wchar_t emitter_tex_w[MAX_PATH]{};

		struct captured_particle
		{
			D3DXVECTOR3 pos{};
			float quat[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
			D3DXVECTOR3 rgb_init{ 1.0f, 1.0f, 1.0f };
			D3DXVECTOR3 rgb_final{ 1.0f, 1.0f, 1.0f };
			float opacity = 1.0f;
			float frequency = 20.0f;
			float lifetime = 1.0f;
			float speed_min = 1.0f;
			float speed_max = 2.0f;
			float emit_min = 0.0f;
			float emit_max = 45.0f;
			D3DXVECTOR3 grav{ 0.0f, 0.0f, 0.0f };
			float air = -0.001f;
			float scale_init = 1.0f;
			float scale_final = 1.0f;
			int burnout = 1;
			int reflect = 1;
			int bone = 0;
			int live_count = 0;
			int shown = 1;
			int active = 1;
			float timer = 0.0f;
			int slot = -1;
			bool collide = false;
			char title[64]{};
			char tex[80]{};
			char tex_path[MAX_PATH]{};
			char sheet_path[MAX_PATH]{};
			std::uint8_t sheet_rows = 1;
			std::uint8_t sheet_cols = 1;
			std::uint8_t sheet_fps = 0;
			std::uint8_t sheet_mode = 0;
			std::uint64_t identity = 0;
			std::uint64_t content = 0;
			const void* plugin_ptr = nullptr;
		};

		struct cached_plugin
		{
			int slot = -1;
			int parent_slot = -1;
			int bone_id = 0;
			int object_id = -1;
			const std::uint8_t* host = nullptr;
			const std::uint8_t* plugin = nullptr;
			char title[64]{};
			bool have_last = false;
			D3DXVECTOR3 last_pos{};
			float last_quat[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
			char sheet_leaf[80]{};
			char sheet_path[MAX_PATH]{};
			float sheet_anim_fps = -1.0f;
			std::uint8_t sheet_rows = 1;
			std::uint8_t sheet_cols = 1;
			std::uint8_t sheet_fps = 0;
			bool sheet_ready = false;
		};

		cached_plugin cached_plugins[remix_particle_cap]{};
		int cached_plugin_n = 0;
		int cached_host_count = -1;

		constexpr std::uint64_t k_ident_tag = 0x50544C0000000000ull;
		constexpr char k_legacy_ini_name[] = "rtx-particles.ini";
		std::mutex g_store_mu;
		std::map<std::string, bool> g_collide;
		std::map<std::string, remix_ext_params> g_ext;
		std::map<std::uint64_t, std::string> g_id_section;
		std::map<std::uint64_t, int> g_ident_oid;
		int g_slot_oid[host_max_objects]{};
		std::map<const void*, int> g_plugin_oid;
		int g_sync_host_count = -1;
		int g_sync_list_count = -1;
		std::uint64_t g_list_oid_fp = 0;
		char g_bound_ini[MAX_PATH]{};
		int g_bound_plugin_n = -1;
		int g_bound_applied = -1;
		std::map<HWND, const void*> g_hwnd_plugin;
		std::map<HWND, const void*> g_hwnd_host;
		captured_particle g_last_parts[remix_particle_cap]{};
		int g_last_n = 0;
		char g_ini_path[MAX_PATH]{};
		char g_maps_ini[MAX_PATH]{};
		bool g_ini_loaded = false;
		bool g_loaded_ok = false;
		char g_loaded_stem[64]{};
		bool g_allow_empty_persist = false;
		bool g_host_ready_for_stem = false;
		bool g_ui_hooks = false;
		bool g_project_watch = false;

		std::string section_id_from_identity(std::uint64_t identity);
		std::string section_from_object_id(int oid);
		int object_id_from_section_name(const char* sec);
		bool is_hash_section_name(const char* sec);
		void refresh_object_ids_from_list();
		void sync_live_plugins_with_ini(bool persist = true);
		void maybe_sync_live_plugins(bool persist);

		int leading_decimal_id(const char* text)
		{
			if (!text) {
				return -1;
			}
			const char* p = text;
			while (*p == ' ' || *p == '\t') {
				++p;
			}
			if (*p < '0' || *p > '9') {
				return -1;
			}
			int id = 0;
			int n = 0;
			while (p[n] >= '0' && p[n] <= '9' && n < 8)
			{
				id = id * 10 + (p[n] - '0');
				n++;
			}
			return n > 0 ? id : -1;
		}

		int object_id_from_legacy_section(const char* sec)
		{
			const int oid = leading_decimal_id(sec);
			if (oid < 0 || oid >= host_max_objects) {
				return -1;
			}
			const char* p = sec;
			while (*p == ' ' || *p == '\t') {
				++p;
			}
			while (*p >= '0' && *p <= '9') {
				++p;
			}
			return _stricmp(p, "Particles") == 0 ? oid : -1;
		}

		std::uint64_t parse_hash_field(const char* hx)
		{
			if (!hx || !hx[0]) {
				return 0;
			}
			char* end = nullptr;
			std::uint64_t hv = std::strtoull(hx, &end, 16);
			if (hv && hv < 0x100000000ull) {
				hv = k_ident_tag | static_cast<std::uint32_t>(hv);
			}
			return hv;
		}

		int object_id_from_disk_section(const char* sec, const char* path)
		{
			if (!sec || !sec[0]) {
				return -1;
			}
			if (path && path[0])
			{
				const int named = GetPrivateProfileIntA(sec, "ObjectId", -1, path);
				if (named >= 0 && named < host_max_objects) {
					return named;
				}
			}
			const int from_name = object_id_from_section_name(sec);
			if (from_name >= 0) {
				return from_name;
			}
			return object_id_from_legacy_section(sec);
		}

		void bind_identity_object_id_unlocked(std::uint64_t identity, int oid)
		{
			if (!identity || oid < 0 || oid >= host_max_objects) {
				return;
			}
			const std::string neu = section_from_object_id(oid);
			std::string old;
			const auto prev = g_id_section.find(identity);
			if (prev != g_id_section.end()) {
				old = prev->second;
			}
			const std::string hashed = section_id_from_identity(identity);
			auto steal = [&](const std::string& from)
			{
				if (from.empty() || from == neu) {
					return;
				}
				if (object_id_from_section_name(from.c_str()) >= 0) {
					return;
				}
				if (auto it = g_collide.find(from); it != g_collide.end())
				{
					if (g_collide.find(neu) == g_collide.end()) {
						g_collide[neu] = it->second;
					}
					g_collide.erase(it);
				}
				if (auto it = g_ext.find(from); it != g_ext.end())
				{
					if (g_ext.find(neu) == g_ext.end()) {
						g_ext[neu] = it->second;
					}
					g_ext.erase(it);
				}
			};
			steal(old);
			steal(hashed);
			g_ident_oid[identity] = oid;
			g_id_section[identity] = neu;
		}

		std::string section_id_from_identity(std::uint64_t identity)
		{
			char buf[48]{};
			sprintf_s(buf, "Particles_%llX", static_cast<unsigned long long>(identity));
			return buf;
		}

		std::string section_from_object_id(int oid)
		{
			char buf[32]{};
			sprintf_s(buf, "Particles_%05d", oid);
			return buf;
		}

		int object_id_from_section_name(const char* sec)
		{
			if (!sec || _strnicmp(sec, "Particles_", 10) != 0) {
				return -1;
			}
			const char* p = sec + 10;
			if (!*p) {
				return -1;
			}
			int n = 0;
			int oid = 0;
			while (p[n] >= '0' && p[n] <= '9' && n < 8)
			{
				oid = oid * 10 + (p[n] - '0');
				n++;
			}
			if (n == 0 || p[n] != 0 || oid < 0 || oid >= host_max_objects) {
				return -1;
			}
			return oid;
		}

		std::uint64_t identity_from_section_name(const char* sec)
		{
			if (!sec || !sec[0]) {
				return 0;
			}
			if (_strnicmp(sec, "Particles_", 10) == 0)
			{
				if (object_id_from_section_name(sec) >= 0) {
					return 0;
				}
				char* end = nullptr;
				const unsigned long long hv = std::strtoull(sec + 10, &end, 16);
				if (hv && end && *end == 0) {
					return hv;
				}
			}
			if (_strnicmp(sec, "id", 2) == 0)
			{
				char* end = nullptr;
				const unsigned long lo = std::strtoul(sec + 2, &end, 16);
				if (lo && end && *end == 0) {
					return k_ident_tag | lo;
				}
			}
			return 0;
		}

		int slot_from_identity(std::uint64_t identity);
		std::uint64_t identity_of_plugin(const void* plugin, const void* host);
		std::string section_key_for_unlocked(std::uint64_t identity);

		bool collide_of_section(const std::string& key)
		{
			std::lock_guard lock(g_store_mu);
			const auto it = g_collide.find(key);
			return it != g_collide.end() && it->second;
		}

		bool collide_of_id(std::uint64_t identity)
		{
			if (!identity) {
				return false;
			}
			std::lock_guard lock(g_store_mu);
			const std::string key = section_key_for_unlocked(identity);
			const auto it = g_collide.find(key);
			return it != g_collide.end() && it->second;
		}

		void set_collide_id(std::uint64_t identity, bool on)
		{
			if (!identity) {
				return;
			}
			std::lock_guard lock(g_store_mu);
			const std::string key = section_key_for_unlocked(identity);
			g_collide[key] = on;
			const auto ex = g_ext.find(key);
			if (ex != g_ext.end()) {
				ex->second.collide = on;
			}
		}

		void register_section_id(std::uint64_t identity, const char* /*name*/)
		{
			if (!identity) {
				return;
			}
			std::lock_guard lock(g_store_mu);
			const auto oid_it = g_ident_oid.find(identity);
			if (oid_it != g_ident_oid.end() && oid_it->second >= 0) {
				bind_identity_object_id_unlocked(identity, oid_it->second);
				const std::string key = section_from_object_id(oid_it->second);
				if (g_collide.find(key) == g_collide.end()) {
					g_collide[key] = false;
				}
				return;
			}
			if (g_id_section.find(identity) != g_id_section.end()) {
				return;
			}
			g_id_section[identity] = section_id_from_identity(identity);
		}

		bool file_exists_a(const char* path)
		{
			if (!path || !path[0]) {
				return false;
			}
			const DWORD a = GetFileAttributesA(path);
			return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
		}

		void sync_ini_path_from_project()
		{
			g_ini_path[0] = 0;
			if (const char* p = project_file::ini_path(); p && p[0]) {
				std::strncpy(g_ini_path, p, MAX_PATH - 1);
			}
		}

		bool same_ini_path(const char* a, const char* b)
		{
			return a && b && a[0] && b[0] && _stricmp(a, b) == 0;
		}

		void claim_maps_ini_unlocked(const char* path)
		{
			g_maps_ini[0] = 0;
			if (path && path[0]) {
				std::strncpy(g_maps_ini, path, MAX_PATH - 1);
			}
		}

		void clear_particle_maps_unlocked()
		{
			g_collide.clear();
			g_ext.clear();
			g_id_section.clear();
			g_ident_oid.clear();
			g_plugin_oid.clear();
			g_maps_ini[0] = 0;
			g_ini_loaded = false;
			g_loaded_ok = false;
			g_loaded_stem[0] = 0;
			g_allow_empty_persist = false;
			g_host_ready_for_stem = false;
			g_bound_ini[0] = 0;
			g_bound_plugin_n = -1;
			g_bound_applied = -1;
			g_sync_host_count = -1;
			g_sync_list_count = -1;
			g_list_oid_fp = 0;
		}

		void drop_live_plugin_cache()
		{
			cached_plugin_n = 0;
			cached_host_count = -1;
			for (auto& rec : cached_plugins) {
				rec = {};
			}
			std::memset(g_slot_oid, 0xFF, sizeof(g_slot_oid));
			host_list_ok = false;
		}

		bool maps_belong_to_current_ini_unlocked()
		{
			return same_ini_path(g_maps_ini, g_ini_path);
		}

		int disk_particle_section_count(const char* path)
		{
			if (!path || !path[0] || !file_exists_a(path)) {
				return 0;
			}
			char names[8192]{};
			const DWORD n = GetPrivateProfileSectionNamesA(names, sizeof(names), path);
			if (n == 0) {
				return 0;
			}
			int count = 0;
			for (char* p = names; *p; p += std::strlen(p) + 1)
			{
				if (object_id_from_section_name(p) >= 0 ||
					is_hash_section_name(p) ||
					object_id_from_legacy_section(p) >= 0)
				{
					++count;
				}
			}
			return count;
		}

		bool ini_is_current_stem_unlocked()
		{
			const char* stem = project_file::stem();
			const char* ini = project_file::ini_path();
			if (!stem || !stem[0] || !ini || !ini[0] || !g_ini_path[0]) {
				return false;
			}
			if (!same_ini_path(g_ini_path, ini)) {
				return false;
			}
			if (g_loaded_stem[0] && _stricmp(g_loaded_stem, stem) != 0) {
				return false;
			}
			return true;
		}

		void snapshot_ini_bak_once_unlocked()
		{
			if (!g_ini_path[0] || !file_exists_a(g_ini_path)) {
				return;
			}
			char bak[MAX_PATH]{};
			sprintf_s(bak, "%s.bak", g_ini_path);
			if (file_exists_a(bak)) {
				return;
			}
			if (CopyFileA(g_ini_path, bak, TRUE))
			{
				shared::common::log("Particles",
					std::format("snapshot {} → {}", g_ini_path, bak));
			}
		}

		void migrate_legacy_ini()
		{
			sync_ini_path_from_project();
			if (!g_ini_path[0] || file_exists_a(g_ini_path)) {
				return;
			}
			const char* folder = project_file::scene_folder();
			if (!folder || !folder[0]) {
				return;
			}
			char legacy[MAX_PATH]{};
			sprintf_s(legacy, "%s\\%s", folder, k_legacy_ini_name);
			if (!file_exists_a(legacy)) {
				return;
			}
			if (CopyFileA(legacy, g_ini_path, TRUE))
			{
				shared::common::log("Particles",
					std::format("migrated {} → {}", legacy, g_ini_path));
			}
		}

		float ini_f(const char* sec, const char* key, float def, const char* path)
		{
			char t[64]{};
			GetPrivateProfileStringA(sec, key, "", t, 64, path);
			if (!t[0]) {
				return def;
			}
			return static_cast<float>(std::atof(t));
		}

		void ini_rgba(const char* sec, const char* kr, float c[4], const char* path)
		{
			static const char* suf[4] = { "R", "G", "B", "A" };
			for (int i = 0; i < 4; i++)
			{
				char key[32]{};
				sprintf_s(key, "%s%s", kr, suf[i]);
				c[i] = ini_f(sec, key, c[i], path);
			}
		}

		void write_f(const char* sec, const char* key, float v, const char* path)
		{
			char t[32]{};
			sprintf_s(t, "%.6g", static_cast<double>(v));
			WritePrivateProfileStringA(sec, key, t, path);
		}

		void write_rgba(const char* sec, const char* kr, const float c[4], const char* path)
		{
			static const char* suf[4] = { "R", "G", "B", "A" };
			for (int i = 0; i < 4; i++)
			{
				char key[32]{};
				sprintf_s(key, "%s%s", kr, suf[i]);
				write_f(sec, key, c[i], path);
			}
		}

		void write_ext_unlocked(const char* sec, const remix_ext_params& p)
		{
			WritePrivateProfileStringA(sec, "Remix", p.has_override ? "1" : "0", g_ini_path);
			if (!p.has_override) {
				return;
			}
			write_rgba(sec, "SpawnMin", p.min_spawn_color, g_ini_path);
			write_rgba(sec, "SpawnMax", p.max_spawn_color, g_ini_path);
			write_f(sec, "RotMin", p.min_rot_speed, g_ini_path);
			write_f(sec, "RotMax", p.max_rot_speed, g_ini_path);
			write_f(sec, "SizeMin", p.min_spawn_size, g_ini_path);
			write_f(sec, "SizeMax", p.max_spawn_size, g_ini_path);
			write_f(sec, "TtlMin", p.min_ttl, g_ini_path);
			write_f(sec, "TtlMax", p.max_ttl, g_ini_path);
			WritePrivateProfileStringA(sec, "HideEmitter", p.hide_emitter ? "1" : "0", g_ini_path);
			write_f(sec, "Cone", p.cone_deg, g_ini_path);
			write_f(sec, "VelMotion", p.vel_from_motion, g_ini_path);
			write_f(sec, "VelNormal", p.vel_from_normal, g_ini_path);
			char mp[16]{};
			sprintf_s(mp, "%u", p.max_particles);
			WritePrivateProfileStringA(sec, "MaxParticles", mp, g_ini_path);
			write_f(sec, "SpawnRate", p.spawn_rate, g_ini_path);
			WritePrivateProfileStringA(sec, "UseSpawnUV", p.use_spawn_uv ? "1" : "0", g_ini_path);
			write_rgba(sec, "TgtMin", p.min_target_color, g_ini_path);
			write_rgba(sec, "TgtMax", p.max_target_color, g_ini_path);
			write_f(sec, "TgtRotMin", p.min_target_rot, g_ini_path);
			write_f(sec, "TgtRotMax", p.max_target_rot, g_ini_path);
			write_f(sec, "TgtSizeMin", p.min_target_size, g_ini_path);
			write_f(sec, "TgtSizeMax", p.max_target_size, g_ini_path);
			WritePrivateProfileStringA(sec, "AlignMotion", p.align_motion ? "1" : "0", g_ini_path);
			char bb[8]{};
			sprintf_s(bb, "%u", static_cast<unsigned>(p.billboard));
			WritePrivateProfileStringA(sec, "Billboard", bb, g_ini_path);
			WritePrivateProfileStringA(sec, "MotionTrail", p.motion_trail ? "1" : "0", g_ini_path);
			write_f(sec, "TrailMult", p.trail_mult, g_ini_path);
			write_f(sec, "Bounce", p.restitution, g_ini_path);
			write_f(sec, "Thickness", p.thickness, g_ini_path);
			write_f(sec, "Grav", p.gravity_force, g_ini_path);
			WritePrivateProfileStringA(sec, "GravOverride", p.grav_override ? "1" : "0", g_ini_path);
			write_f(sec, "MaxSpeed", p.max_speed, g_ini_path);
			write_f(sec, "TurbF", p.turb_force, g_ini_path);
			write_f(sec, "TurbHz", p.turb_freq, g_ini_path);
			WritePrivateProfileStringA(sec, "TurbOn", p.use_turbulence ? "1" : "0", g_ini_path);
			char sh[16]{};
			sprintf_s(sh, "%u", static_cast<unsigned>(p.sheet_rows));
			WritePrivateProfileStringA(sec, "SheetRows", sh, g_ini_path);
			sprintf_s(sh, "%u", static_cast<unsigned>(p.sheet_cols));
			WritePrivateProfileStringA(sec, "SheetCols", sh, g_ini_path);
			sprintf_s(sh, "%u", static_cast<unsigned>(p.sheet_fps));
			WritePrivateProfileStringA(sec, "SheetFps", sh, g_ini_path);
			sprintf_s(sh, "%u", static_cast<unsigned>(p.sheet_mode));
			WritePrivateProfileStringA(sec, "SheetMode", sh, g_ini_path);
			sprintf_s(sh, "%u", static_cast<unsigned>(p.collision_mode));
			WritePrivateProfileStringA(sec, "CollisionMode", sh, g_ini_path);
			WritePrivateProfileStringA(sec, "Animation",
				p.animation[0] ? p.animation : "", g_ini_path);
			WritePrivateProfileStringA(sec, "AnimationFiles",
				p.animation_files[0] ? p.animation_files : "", g_ini_path);
			WritePrivateProfileStringA(sec, "SheetFile",
				p.sheet_file[0] ? p.sheet_file : "", g_ini_path);
		}

		void load_ext_section(const char* sec, const char* path, remix_ext_params& p)
		{
			p.has_override = GetPrivateProfileIntA(sec, "Remix", 0, path) != 0;
			if (!p.has_override) {
				return;
			}
			ini_rgba(sec, "SpawnMin", p.min_spawn_color, path);
			ini_rgba(sec, "SpawnMax", p.max_spawn_color, path);
			p.min_rot_speed = ini_f(sec, "RotMin", p.min_rot_speed, path);
			p.max_rot_speed = ini_f(sec, "RotMax", p.max_rot_speed, path);
			p.min_spawn_size = ini_f(sec, "SizeMin", p.min_spawn_size, path);
			p.max_spawn_size = ini_f(sec, "SizeMax", p.max_spawn_size, path);
			p.min_ttl = ini_f(sec, "TtlMin", p.min_ttl, path);
			p.max_ttl = ini_f(sec, "TtlMax", p.max_ttl, path);
			p.hide_emitter = GetPrivateProfileIntA(sec, "HideEmitter", 1, path) != 0;
			p.cone_deg = ini_f(sec, "Cone", p.cone_deg, path);
			p.vel_from_motion = ini_f(sec, "VelMotion", p.vel_from_motion, path);
			p.vel_from_normal = ini_f(sec, "VelNormal", p.vel_from_normal, path);
			p.max_particles = static_cast<std::uint32_t>(
				GetPrivateProfileIntA(sec, "MaxParticles", 2048, path));
			p.spawn_rate = ini_f(sec, "SpawnRate", p.spawn_rate, path);
			p.use_spawn_uv = GetPrivateProfileIntA(sec, "UseSpawnUV", 0, path) != 0;
			ini_rgba(sec, "TgtMin", p.min_target_color, path);
			ini_rgba(sec, "TgtMax", p.max_target_color, path);
			p.min_target_rot = ini_f(sec, "TgtRotMin", p.min_target_rot, path);
			p.max_target_rot = ini_f(sec, "TgtRotMax", p.max_target_rot, path);
			p.min_target_size = ini_f(sec, "TgtSizeMin", p.min_target_size, path);
			p.max_target_size = ini_f(sec, "TgtSizeMax", p.max_target_size, path);
			p.align_motion = GetPrivateProfileIntA(sec, "AlignMotion", 0, path) != 0;
			p.billboard = static_cast<std::uint8_t>(
				GetPrivateProfileIntA(sec, "Billboard", 0, path));
			p.motion_trail = GetPrivateProfileIntA(sec, "MotionTrail", 0, path) != 0;
			p.trail_mult = ini_f(sec, "TrailMult", p.trail_mult, path);
			p.restitution = ini_f(sec, "Bounce", p.restitution, path);
			p.thickness = ini_f(sec, "Thickness", p.thickness, path);
			p.gravity_force = ini_f(sec, "Grav", p.gravity_force, path);
			{
				char t[32]{};
				GetPrivateProfileStringA(sec, "GravOverride", "", t, 32, path);
				if (t[0]) {
					p.grav_override = std::atoi(t) != 0;
				}
				else
				{
					GetPrivateProfileStringA(sec, "Grav", "", t, 32, path);
					p.grav_override = t[0] != 0;
				}
			}
			p.max_speed = ini_f(sec, "MaxSpeed", p.max_speed, path);
			p.turb_force = ini_f(sec, "TurbF", p.turb_force, path);
			p.turb_freq = ini_f(sec, "TurbHz", p.turb_freq, path);
			p.use_turbulence = GetPrivateProfileIntA(sec, "TurbOn", 0, path) != 0;
			p.sheet_rows = static_cast<std::uint8_t>(
				GetPrivateProfileIntA(sec, "SheetRows", p.sheet_rows, path));
			p.sheet_cols = static_cast<std::uint8_t>(
				GetPrivateProfileIntA(sec, "SheetCols", p.sheet_cols, path));
			p.sheet_fps = static_cast<std::uint8_t>(
				GetPrivateProfileIntA(sec, "SheetFps", p.sheet_fps, path));
			p.sheet_mode = static_cast<std::uint8_t>(
				GetPrivateProfileIntA(sec, "SheetMode", p.sheet_mode, path));
			p.collision_mode = static_cast<std::uint8_t>(
				GetPrivateProfileIntA(sec, "CollisionMode", p.collision_mode, path));
			GetPrivateProfileStringA(sec, "Animation", "", p.animation, 80, path);
			GetPrivateProfileStringA(sec, "AnimationFiles", "", p.animation_files, 512, path);
			GetPrivateProfileStringA(sec, "SheetFile", "", p.sheet_file, 260, path);
		}

		void host_particle_sheet_dir(char* dest, int cap)
		{
			if (!dest || cap <= 0) {
				return;
			}
			dest[0] = 0;
			const std::string root = shared::globals::host_data_root();
			if (root.empty()) {
				return;
			}
			char rtx[MAX_PATH]{};
			sprintf_s(rtx, "%s\\rtx_comp", root.c_str());
			CreateDirectoryA(rtx, nullptr);
			sprintf_s(dest, static_cast<size_t>(cap), "%s\\particle_sheets\\", rtx);
			CreateDirectoryA(dest, nullptr);
		}

		bool persist_ini_allowed()
		{
			if (shared::globals::skip_remix) {
				return false;
			}
			if (shared::globals::is_editor_host) {
				return true;
			}
			if (!shared::globals::is_compiled_host) {
				return false;
			}
			const char* folder = project_file::scene_folder();
			if (!folder || !folder[0]) {
				return false;
			}
			const std::string root = shared::globals::host_data_root();
			char expect[MAX_PATH]{};
			sprintf_s(expect, "%s\\3DRad_res\\projects", root.c_str());
			return _stricmp(folder, expect) == 0;
		}

		bool editor_ini_allowed()
		{
			return shared::globals::is_editor_host && !shared::globals::skip_remix;
		}

		bool is_hash_section_name(const char* sec)
		{
			return sec && _strnicmp(sec, "Particles_", 10) == 0 &&
				identity_from_section_name(sec) != 0;
		}

		bool ensure_ini_file_unlocked()
		{
			if (!g_ini_path[0]) {
				return false;
			}
			if (file_exists_a(g_ini_path)) {
				return true;
			}
			HANDLE h = CreateFileA(g_ini_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
				CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				return file_exists_a(g_ini_path);
			}
			const char hdr[] = "; RTX Remix particles (3D Rad)\r\n";
			DWORD nw = 0;
			WriteFile(h, hdr, static_cast<DWORD>(sizeof(hdr) - 1), &nw, nullptr);
			CloseHandle(h);
			shared::common::log("Particles",
				std::format("created project ini {}", g_ini_path));
			return true;
		}

		void apply_disk_section_unlocked(std::uint64_t identity, const char* src_sec,
			const char* path, bool* had_collide, bool* had_remix)
		{
			if (!src_sec || !path) {
				return;
			}
			int oid = -1;
			if (identity)
			{
				const auto it = g_ident_oid.find(identity);
				if (it != g_ident_oid.end()) {
					oid = it->second;
				}
			}
			if (oid < 0) {
				oid = object_id_from_disk_section(src_sec, path);
			}
			std::string key;
			if (oid >= 0) {
				key = section_from_object_id(oid);
			}
			else if (identity) {
				key = section_id_from_identity(identity);
			}
			else {
				return;
			}
			if (identity) {
				g_id_section[identity] = key;
				if (oid >= 0) {
					g_ident_oid[identity] = oid;
				}
			}
			const int collide = GetPrivateProfileIntA(src_sec, "Collide", -1, path);
			if (collide >= 0) {
				g_collide[key] = collide != 0;
				if (had_collide) {
					*had_collide = true;
				}
			}
			remix_ext_params ext{};
			if (collide >= 0) {
				ext.collide = collide != 0;
			}
			load_ext_section(src_sec, path, ext);
			if (ext.has_override)
			{
				if (collide >= 0) {
					ext.collide = collide != 0;
				}
				g_ext[key] = ext;
				g_collide[key] = ext.collide;
				if (had_remix) {
					*had_remix = true;
				}
			}
		}

		void load_identity_section_from_disk(std::uint64_t identity, const char* path)
		{
			if (!identity || !path || !path[0] || !file_exists_a(path)) {
				return;
			}
			bool had_collide = false;
			bool had_remix = false;
			int oid = -1;
			{
				std::lock_guard lock(g_store_mu);
				const auto it = g_ident_oid.find(identity);
				if (it != g_ident_oid.end()) {
					oid = it->second;
				}
			}
			if (oid >= 0)
			{
				const std::string oid_key = section_from_object_id(oid);
				std::lock_guard lock(g_store_mu);
				apply_disk_section_unlocked(identity, oid_key.c_str(), path,
					&had_collide, &had_remix);
			}
			const std::string hash_key = section_id_from_identity(identity);
			{
				std::lock_guard lock(g_store_mu);
				apply_disk_section_unlocked(identity, hash_key.c_str(), path,
					&had_collide, &had_remix);
			}
			char names[4096]{};
			const DWORD n = GetPrivateProfileSectionNamesA(names, sizeof(names), path);
			if (n != 0)
			{
				for (char* p = names; *p; p += std::strlen(p) + 1)
				{
					const int disk_oid = object_id_from_disk_section(p, path);
					if (oid >= 0 && disk_oid == oid)
					{
						std::lock_guard lock(g_store_mu);
						apply_disk_section_unlocked(identity, p, path, &had_collide, &had_remix);
						continue;
					}
					if (oid >= 0) {
						continue;
					}
					if (is_hash_section_name(p) && identity_from_section_name(p) != identity) {
						continue;
					}
					char hx[32]{};
					GetPrivateProfileStringA(p, "Hash", "", hx, 32, path);
					if (!hx[0]) {
						continue;
					}
					const std::uint64_t hv = parse_hash_field(hx);
					if (hv != identity) {
						continue;
					}
					std::lock_guard lock(g_store_mu);
					apply_disk_section_unlocked(identity, p, path, &had_collide, &had_remix);
					shared::common::log("Particles",
						std::format("migrated leftover [{}] Hash={} -> [{}]",
							p, hx, hash_key));
				}
			}
			shared::common::log("Particles",
				std::format("ini load {} section={} collide={} remix={} path={}",
					(had_collide || had_remix) ? "ok" : "miss (defaults)",
					oid >= 0 ? section_from_object_id(oid) : hash_key,
					had_collide ? 1 : 0, had_remix ? 1 : 0, path));
		}

		void load_ini_from_path(const char* path)
		{
			std::lock_guard lock(g_store_mu);
			g_collide.clear();
			g_ext.clear();
			g_id_section.clear();
			g_ident_oid.clear();
			g_plugin_oid.clear();
			g_sync_host_count = -1;
			g_sync_list_count = -1;
			g_list_oid_fp = 0;
			g_maps_ini[0] = 0;
			if (!path || !path[0]) {
				g_ini_loaded = false;
				g_loaded_ok = false;
				g_loaded_stem[0] = 0;
				return;
			}
			claim_maps_ini_unlocked(path);
			std::strncpy(g_ini_path, path, MAX_PATH - 1);
			int sections = 0;
			char names[4096]{};
			const DWORD n = GetPrivateProfileSectionNamesA(names, sizeof(names), path);
			if (n != 0)
			{
				for (char* p = names; *p; p += std::strlen(p) + 1)
				{
					const int oid = object_id_from_disk_section(p, path);
					std::uint64_t hv = 0;
					if (oid < 0)
					{
						hv = identity_from_section_name(p);
						if (!hv)
						{
							char hx[32]{};
							GetPrivateProfileStringA(p, "Hash", "", hx, 32, path);
							hv = parse_hash_field(hx);
						}
					}
					else
					{
						char hx[32]{};
						GetPrivateProfileStringA(p, "Hash", "", hx, 32, path);
						hv = parse_hash_field(hx);
					}
					if (oid < 0 && !hv) {
						continue;
					}
					++sections;
					const std::string key = oid >= 0
						? section_from_object_id(oid)
						: section_id_from_identity(hv);
					const int v = GetPrivateProfileIntA(p, "Collide", 0, path);
					g_collide[key] = v != 0;
					remix_ext_params ext{};
					ext.collide = v != 0;
					load_ext_section(p, path, ext);
					if (ext.has_override) {
						g_ext[key] = ext;
					}
					if (hv) {
						g_id_section[hv] = key;
						if (oid >= 0) {
							g_ident_oid[hv] = oid;
						}
					}
				}
			}
			g_ini_loaded = true;
			g_loaded_ok = true;
			g_allow_empty_persist = false;
			g_host_ready_for_stem = false;
			g_loaded_stem[0] = 0;
			if (const char* stem = project_file::stem(); stem && stem[0]) {
				std::strncpy(g_loaded_stem, stem, 63);
			}
			shared::common::log("Particles",
				std::format("loaded {} collide / {} remix section(s) from {}",
					static_cast<int>(g_collide.size()),
					static_cast<int>(g_ext.size()), path));
			(void)sections;
		}

		void write_ini_unlocked()
		{
			if (!persist_ini_allowed()) {
				return;
			}
			sync_ini_path_from_project();
			if (!g_ini_path[0]) {
				return;
			}
			if (!g_loaded_ok || !ini_is_current_stem_unlocked())
			{
				shared::common::log("Particles",
					std::format("skip write stem not loaded maps='{}' path='{}'",
						g_maps_ini[0] ? g_maps_ini : "-", g_ini_path),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return;
			}
			if (!maps_belong_to_current_ini_unlocked())
			{
				shared::common::log("Particles",
					std::format("skip write maps belong to '{}' not '{}'",
						g_maps_ini[0] ? g_maps_ini : "-", g_ini_path),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return;
			}
			std::map<std::string, std::uint64_t> secs;
			auto take = [&](const std::string& key, std::uint64_t live)
			{
				if (key.empty() || object_id_from_section_name(key.c_str()) < 0) {
					return;
				}
				auto it = secs.find(key);
				if (it == secs.end()) {
					secs[key] = live;
				}
				else if (!it->second && live) {
					it->second = live;
				}
			};
			for (int i = 0; i < cached_plugin_n; i++)
			{
				const auto ident = identity_of_plugin(cached_plugins[i].plugin,
					cached_plugins[i].host);
				if (!ident) {
					continue;
				}
				int oid = cached_plugins[i].object_id;
				if (oid < 0)
				{
					const auto it = g_ident_oid.find(ident);
					if (it != g_ident_oid.end()) {
						oid = it->second;
					}
				}
				if (oid < 0 && cached_plugins[i].slot >= 0 &&
					cached_plugins[i].slot < host_max_objects)
				{
					oid = g_slot_oid[cached_plugins[i].slot];
				}
				cached_plugins[i].object_id = oid;
				if (oid >= 0) {
					bind_identity_object_id_unlocked(ident, oid);
					take(section_from_object_id(oid), ident);
				}
			}
			for (const auto& kv : g_ident_oid) {
				if (kv.second >= 0) {
					take(section_from_object_id(kv.second), kv.first);
				}
			}
			for (const auto& kv : g_collide) {
				take(kv.first, 0);
			}
			for (const auto& kv : g_ext) {
				take(kv.first, 0);
			}
			const int disk_n = disk_particle_section_count(g_ini_path);
			const int live_n = cached_plugin_n;
			if ((secs.empty() || live_n <= 0) && disk_n > 0)
			{
				shared::common::log("Particles",
					std::format(
						"skip empty overwrite of {} ({} disk section(s), maps={}, live={})",
						g_ini_path, disk_n, static_cast<int>(secs.size()), live_n),
					shared::common::LOG_TYPE::LOG_TYPE_WARN, true);
				return;
			}
			if (secs.empty())
			{
				return;
			}
			snapshot_ini_bak_once_unlocked();
			if (!ensure_ini_file_unlocked()) {
				return;
			}
			for (const auto& s : secs)
			{
				const char* sec = s.first.c_str();
				const auto c = g_collide.find(s.first);
				const bool on = c != g_collide.end() && c->second;
				WritePrivateProfileStringA(sec, "Collide", on ? "1" : "0", g_ini_path);
				WritePrivateProfileStringA(sec, "Name", "Particles", g_ini_path);
				WritePrivateProfileStringA(sec, "Slot", nullptr, g_ini_path);
				const int oid = object_id_from_section_name(sec);
				if (oid >= 0)
				{
					char idbuf[16]{};
					sprintf_s(idbuf, "%d", oid);
					WritePrivateProfileStringA(sec, "ObjectId", idbuf, g_ini_path);
				}
				std::uint64_t live = s.second;
				if (!live)
				{
					for (const auto& idsec : g_id_section)
					{
						if (idsec.second == s.first)
						{
							live = idsec.first;
							break;
						}
					}
				}
				if (live)
				{
					char hx[32]{};
					sprintf_s(hx, "%llX", static_cast<unsigned long long>(live));
					WritePrivateProfileStringA(sec, "Hash", hx, g_ini_path);
					g_id_section[live] = s.first;
				}
				const auto ex = g_ext.find(s.first);
				if (ex != g_ext.end()) {
					write_ext_unlocked(sec, ex->second);
				}
			}
			{
				if (secs.empty() || cached_plugin_n <= 0 ||
					(!g_host_ready_for_stem && !g_allow_empty_persist))
				{
					shared::common::log("Particles",
						std::format("skip prune-all {} — live={} host_ready={}",
							g_ini_path, cached_plugin_n, g_host_ready_for_stem ? 1 : 0),
						shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				}
				else
				{
				char names[8192]{};
				const DWORD nn = GetPrivateProfileSectionNamesA(names, sizeof(names), g_ini_path);
				if (nn != 0)
				{
					int pruned = 0;
					for (char* p = names; *p; p += std::strlen(p) + 1)
					{
						const int disk_oid = object_id_from_section_name(p);
						const bool hash_sec = is_hash_section_name(p);
						const bool legacy = object_id_from_legacy_section(p) >= 0;
						if (disk_oid < 0 && !hash_sec && !legacy) {
							continue;
						}
						if (secs.find(p) != secs.end()) {
							continue;
						}
						WritePrivateProfileStringA(p, nullptr, nullptr, g_ini_path);
						++pruned;
						shared::common::log("Particles",
							std::format("pruned duplicate ini section [{}]", p));
					}
					if (pruned) {
						shared::common::log("Particles",
							std::format("removed {} stale particle section(s) from {}",
								pruned, g_ini_path));
					}
				}
				}
			}
			for (auto it = g_collide.begin(); it != g_collide.end(); )
			{
				if (object_id_from_section_name(it->first.c_str()) < 0) {
					it = g_collide.erase(it);
				}
				else {
					++it;
				}
			}
			for (auto it = g_ext.begin(); it != g_ext.end(); )
			{
				if (object_id_from_section_name(it->first.c_str()) < 0) {
					it = g_ext.erase(it);
				}
				else {
					++it;
				}
			}
			shared::common::log("Particles",
				std::format("saved {} particle section(s) to {}",
					static_cast<int>(secs.size()), g_ini_path));
			WritePrivateProfileStringA(nullptr, nullptr, nullptr, g_ini_path);
		}

		void drop_overrides_and_load()
		{
			migrate_legacy_ini();
			sync_ini_path_from_project();
			load_ini_from_path(g_ini_path[0] ? g_ini_path : "");
		}

		void on_before_project_switch()
		{
			drop_live_plugin_cache();
			std::lock_guard lock(g_store_mu);
			clear_particle_maps_unlocked();
		}

		void on_after_project_switch()
		{
			drop_overrides_and_load();
		}

		void bind_project_watch()
		{
			if (g_project_watch) {
				return;
			}
			g_project_watch = true;
			project_file::set_on_before_switch(on_before_project_switch);
			project_file::set_on_after_switch(on_after_project_switch);
		}

		struct dlg_frame
		{
			DLGPROC orig = nullptr;
			LPARAM lp = 0;
		};
		thread_local std::vector<dlg_frame> t_dlg_stack;
		HHOOK g_cbt_hook = nullptr;
		HHOOK g_wndret_hook = nullptr;
		DWORD g_dialog_hook_tid = 0;
		int ensure_host_count();
		bool stash_properties_ptrs_from_dlg(HWND dlg, bool force = false);
		void forget_hwnd_unlocked(HWND dlg);
		void bind_and_inject_dialog(HWND hwnd, LPARAM lp);
		bool capture_list_row_plugin(const void*& host, const void*& plugin, int* oid_out);

		DLGPROC current_dlg_orig()
		{
			return t_dlg_stack.empty() ? nullptr : t_dlg_stack.back().orig;
		}

		LPARAM current_dlg_lp()
		{
			return t_dlg_stack.empty() ? 0 : t_dlg_stack.back().lp;
		}

		INT_PTR CALLBACK chained_dlgproc(HWND h, UINT m, WPARAM w, LPARAM l)
		{
			auto orig = reinterpret_cast<DLGPROC>(GetPropA(h, "vreOrigDlg"));
			if (!orig) {
				orig = current_dlg_orig();
				if (orig) {
					SetPropA(h, "vreOrigDlg", reinterpret_cast<HANDLE>(orig));
				}
			}
			if (m == WM_INITDIALOG)
			{
				const LPARAM arg = l ? l : current_dlg_lp();
				if (arg) {
					SetPropA(h, "vrePDlgArg", reinterpret_cast<HANDLE>(arg));
				}
			}
			const INT_PTR r = orig ? orig(h, m, w, l) : FALSE;
			if (m == WM_INITDIALOG) {
				bind_and_inject_dialog(h, l ? l : current_dlg_lp());
			}
			if (m == WM_NCDESTROY) {
				forget_properties_hwnd(h);
				RemovePropA(h, "vreOrigDlg");
				RemovePropA(h, "vrePDlgArg");
				RemovePropA(h, "vrePHost");
				RemovePropA(h, "vrePPlug");
				RemovePropA(h, "vrePTick");
				RemovePropA(h, "vrePBind");
				RemovePropA(h, "vrePInj");
			}
			return r;
		}

		LRESULT CALLBACK cbt_proc(int code, WPARAM wparam, LPARAM lparam)
		{
			if (code == HCBT_DESTROYWND && wparam) {
				forget_properties_hwnd(reinterpret_cast<HWND>(wparam));
			}
			return CallNextHookEx(g_cbt_hook, code, wparam, lparam);
		}

		LRESULT CALLBACK wndret_proc(int code, WPARAM wparam, LPARAM lparam)
		{
			if (code >= 0)
			{
				const auto* msg = reinterpret_cast<CWPRETSTRUCT*>(lparam);
				if (msg && msg->message == WM_INITDIALOG && msg->hwnd) {
					bind_and_inject_dialog(msg->hwnd,
						msg->lParam ? msg->lParam : current_dlg_lp());
				}
			}
			return CallNextHookEx(g_wndret_hook, code, wparam, lparam);
		}

		void ensure_dialog_thread_hooks()
		{
			HWND ed = editor_frame::editor_hwnd();
			if (!ed) {
				ed = editor_frame::wrapper_hwnd();
			}
			DWORD tid = GetCurrentThreadId();
			if (ed && IsWindow(ed)) {
				tid = GetWindowThreadProcessId(ed, nullptr);
			}
			if (!tid) {
				return;
			}
			if (g_dialog_hook_tid == tid && g_wndret_hook && g_cbt_hook) {
				return;
			}
			if (g_cbt_hook) {
				UnhookWindowsHookEx(g_cbt_hook);
				g_cbt_hook = nullptr;
			}
			if (g_wndret_hook) {
				UnhookWindowsHookEx(g_wndret_hook);
				g_wndret_hook = nullptr;
			}
			g_dialog_hook_tid = tid;
			HMODULE mod = shared::globals::dll_hmodule;
			g_cbt_hook = SetWindowsHookExA(WH_CBT, cbt_proc, mod, tid);
			g_wndret_hook = SetWindowsHookExA(WH_CALLWNDPROCRET, wndret_proc, mod, tid);
			shared::common::log("Particles",
				std::format("dialog thread hooks tid={} cbt={} wndret={}",
					tid, g_cbt_hook ? 1 : 0, g_wndret_hook ? 1 : 0));
		}

		void bind_and_inject_dialog(HWND hwnd, LPARAM lp)
		{
			if (!hwnd || !IsWindow(hwnd)) {
				return;
			}
			if (GetPropA(hwnd, "vrePInj")) {
				return;
			}
			LPARAM arg = lp ? lp : current_dlg_lp();
			if (!arg)
			{
				const void* host = nullptr;
				const void* plugin = nullptr;
				if (capture_list_row_plugin(host, plugin, nullptr) && plugin) {
					arg = reinterpret_cast<LPARAM>(const_cast<void*>(plugin));
				}
			}
			if (arg) {
				SetPropA(hwnd, "vrePDlgArg", reinterpret_cast<HANDLE>(arg));
			}
			stash_properties_ptrs_from_dlg(hwnd, true);
			inject_collide_checkbox(hwnd);
			SetPropA(hwnd, "vrePInj", reinterpret_cast<HANDLE>(1));
		}

		BOOL CALLBACK enum_props_dlg(HWND hwnd, LPARAM)
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid == GetCurrentProcessId() && is_particles_properties_dialog(hwnd)) {
				inject_collide_checkbox(hwnd);
				sync_mapped_from_properties_dialog(hwnd, false);
			}
			return TRUE;
		}

		void poll_all_properties_hwnds()
		{
			EnumWindows(enum_props_dlg, 0);
			HWND roots[2]{ editor_frame::wrapper_hwnd(), editor_frame::editor_hwnd() };
			for (HWND root : roots)
			{
				if (!root || !IsWindow(root)) {
					continue;
				}
				EnumChildWindows(root, enum_props_dlg, 0);
			}
		}

		bool read_f3(const std::uint8_t* base, const int off, D3DXVECTOR3& out);
		bool bind_host();
		void rescan_plugins(const int count);
		bool cache_still_valid(const int count);
		int slot_of_plugin(const void* plugin, const int count);
		int slot_of_host(const void* host, const int count);
		int slot_from_handle(const void* handle, const int count);
		bool slot_is_particles(int slot, const int count);

		std::uint64_t identity_of_plugin(const void* plugin, const void* host)
		{
			const auto p = static_cast<std::uint32_t>(
				reinterpret_cast<std::uintptr_t>(plugin));
			const auto h = static_cast<std::uint32_t>(
				reinterpret_cast<std::uintptr_t>(host));
			// Lights-style: overlay and DrawInstance key off the plugin pointer
			// of this Properties hwnd / host row. Host is fallback only.
			const std::uint32_t lo = p ? p : (h ? h : 1u);
			return k_ident_tag | lo;
		}

		std::uint64_t identity_of_slot(int slot)
		{
			if (slot < 0 || !host_list_at || !host_count_at) {
				return 0;
			}
			const int count = *host_count_at;
			if (slot >= count) {
				return 0;
			}
			for (int i = 0; i < cached_plugin_n; i++)
			{
				if (cached_plugins[i].slot == slot) {
					return identity_of_plugin(cached_plugins[i].plugin, cached_plugins[i].host);
				}
			}
			const void* host = host_list_at[slot];
			const std::uint8_t* plugin = nullptr;
			if (host) {
				plugin = *reinterpret_cast<const std::uint8_t* const*>(host);
			}
			return identity_of_plugin(plugin, host);
		}

		int slot_from_identity(std::uint64_t identity)
		{
			if (!identity) {
				return -1;
			}
			for (int i = 0; i < g_last_n; i++)
			{
				if (g_last_parts[i].identity == identity) {
					return g_last_parts[i].slot;
				}
			}
			for (int i = 0; i < cached_plugin_n; i++)
			{
				const auto id = identity_of_plugin(cached_plugins[i].plugin,
					cached_plugins[i].host);
				if (id == identity) {
					return cached_plugins[i].slot;
				}
			}
			return -1;
		}

		int slot_from_section_name(const char* sec)
		{
			if (!sec || !(sec[0] >= '0' && sec[0] <= '9')) {
				return -1;
			}
			int digits = 0;
			int slot = 0;
			while (sec[digits] >= '0' && sec[digits] <= '9' && digits < 8)
			{
				slot = slot * 10 + (sec[digits] - '0');
				digits++;
			}
			return digits ? slot : -1;
		}

		std::string name_for_slot_unlocked(int slot)
		{
			if (slot < 0) {
				return "Particles";
			}
			for (int i = 0; i < g_last_n; i++)
			{
				if (g_last_parts[i].slot == slot && g_last_parts[i].title[0]) {
					return g_last_parts[i].title;
				}
			}
			for (int i = 0; i < cached_plugin_n; i++)
			{
				if (cached_plugins[i].slot == slot && cached_plugins[i].title[0]) {
					return cached_plugins[i].title;
				}
			}
			return "Particles";
		}

		std::string section_key_for_unlocked(std::uint64_t identity)
		{
			const auto oid_it = g_ident_oid.find(identity);
			if (oid_it != g_ident_oid.end() && oid_it->second >= 0)
			{
				const std::string key = section_from_object_id(oid_it->second);
				g_id_section[identity] = key;
				return key;
			}
			const auto it = g_id_section.find(identity);
			if (it != g_id_section.end() && !it->second.empty()) {
				return it->second;
			}
			return section_id_from_identity(identity);
		}

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

		bool path_is_particles(const char* path)
		{
			if (path_has_folder(path, "Particles")) {
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
			return _strnicmp(leaf, "particle", 8) == 0;
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
			host_module = mod;
			host_layout_tag = layout.tag;
			take_module_leaf(mod, host_exe_name, 64);
			host_count_at = count_at;
			host_list_at = list_at;
			host_hmod_at = hmod_at;
			host_list_ok = true;

			int hits = 0;
			const int count = *count_at;
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
				if (path_is_particles(path)) {
					hits++;
				}
			}
			shared::common::log("Particles", std::format(
				"host exe={} layout={} base=0x{:X} n={} Particles plugins={}",
				host_exe_name[0] ? host_exe_name : "?",
				layout.tag,
				reinterpret_cast<std::uintptr_t>(mod),
				count, hits));
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
			return false;
		}

		bool finite3(const float* v)
		{
			return v && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
		}

		bool sane_pos(const D3DXVECTOR3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
				std::fabs(v.x) < k_pos_limit && std::fabs(v.y) < k_pos_limit &&
				std::fabs(v.z) < k_pos_limit;
		}

		bool significant_pos(const D3DXVECTOR3& v)
		{
			return sane_pos(v) && (v.x * v.x + v.y * v.y + v.z * v.z) >= 1.0e-4f;
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

		bool read_f(const std::uint8_t* base, const int off, float& out)
		{
			if (!base || !memory_readable(base + off, sizeof(float))) {
				return false;
			}
			const float v = *reinterpret_cast<const float*>(base + off);
			if (!std::isfinite(v)) {
				return false;
			}
			out = v;
			return true;
		}

		bool read_i(const std::uint8_t* base, const int off, int& out)
		{
			if (!base || !memory_readable(base + off, sizeof(int))) {
				return false;
			}
			out = *reinterpret_cast<const int*>(base + off);
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

		float map_remix_size_cm(const float rad_scale, const float vis, const float floor_cm)
		{
			if (rad_scale <= 0.0001f) {
				return 0.0f;
			}
			return std::max(floor_cm, rad_scale * k_rad_to_remix_cm * vis);
		}

		void map_remix_spawn_target_cm(const float scale_init, const float scale_final, float& spawn_cm, float& target_cm)
		{
			spawn_cm = map_remix_size_cm(scale_init, k_spawn_vis, k_spawn_cm_floor);
			target_cm = map_remix_size_cm(scale_final, k_target_vis, k_target_cm_floor);
			const float d_rad = scale_init - scale_final;
			const float d_cm = spawn_cm - target_cm;
			if (d_rad * d_cm < 0.0f) {
				spawn_cm = target_cm;
			}
		}

		void clamp_ext_colors(remix_ext_params& ext);

		void fill_ext_from_particle(const captured_particle& src, remix_ext_params& ext)
		{
			const float a = std::clamp(src.opacity, 0.0f, 1.0f);
			ext.min_spawn_color[0] = src.rgb_init.x;
			ext.min_spawn_color[1] = src.rgb_init.y;
			ext.min_spawn_color[2] = src.rgb_init.z;
			ext.min_spawn_color[3] = a;
			ext.max_spawn_color[0] = src.rgb_init.x;
			ext.max_spawn_color[1] = src.rgb_init.y;
			ext.max_spawn_color[2] = src.rgb_init.z;
			ext.max_spawn_color[3] = a;
			ext.min_target_color[0] = src.rgb_final.x;
			ext.min_target_color[1] = src.rgb_final.y;
			ext.min_target_color[2] = src.rgb_final.z;
			ext.min_target_color[3] = a;
			ext.max_target_color[0] = src.rgb_final.x;
			ext.max_target_color[1] = src.rgb_final.y;
			ext.max_target_color[2] = src.rgb_final.z;
			ext.max_target_color[3] = a;
			float s0 = 14.0f;
			float s1 = 20.0f;
			map_remix_spawn_target_cm(src.scale_init, src.scale_final, s0, s1);
			ext.min_spawn_size = s0;
			ext.max_spawn_size = s0;
			ext.min_target_size = s1;
			ext.max_target_size = s1;
			ext.min_ttl = src.lifetime;
			ext.max_ttl = src.lifetime;
			ext.hide_emitter = true;
			ext.cone_deg = std::clamp(std::max(src.emit_min, src.emit_max), 0.0f, 50.0f);
			ext.vel_from_motion = 0.0f;
			ext.vel_from_normal = 0.5f * (src.speed_min + src.speed_max) *
				k_ms_to_cms * k_speed_param;
			ext.max_particles = 2048;
			ext.spawn_rate = src.frequency;
			ext.use_spawn_uv = false;
			ext.min_rot_speed = 0.0f;
			ext.max_rot_speed = 0.0f;
			ext.min_target_rot = 0.0f;
			ext.max_target_rot = 0.0f;
			ext.align_motion = false;
			ext.billboard = 0;
			ext.motion_trail = src.burnout != 0;
			ext.trail_mult = 1.0f;
			ext.restitution = 0.5f;
			ext.thickness = 5.0f;
			ext.collide = src.collide;
			ext.gravity_force = std::clamp(src.grav.y * k_ms_to_cms, -980.0f, 980.0f);
			ext.grav_override = false;
			ext.max_speed = src.speed_max > 0.0f
				? src.speed_max * k_ms_to_cms * k_speed_param : 0.0f;
			ext.turb_force = 0.0f;
			ext.turb_freq = 0.05f;
			ext.use_turbulence = false;
			ext.sheet_rows = src.sheet_rows ? src.sheet_rows : 1;
			ext.sheet_cols = src.sheet_cols ? src.sheet_cols : 1;
			ext.sheet_fps = src.sheet_fps;
			ext.sheet_mode = src.sheet_mode;
			ext.collision_mode = 0;
			clamp_ext_colors(ext);
			ext.has_override = false;
		}

		void clamp_rgba(float c[4])
		{
			if (!c) {
				return;
			}
			for (int i = 0; i < 4; i++)
			{
				if (!std::isfinite(c[i])) {
					c[i] = (i == 3) ? 1.0f : 1.0f;
				}
				c[i] = std::clamp(c[i], 0.0f, 1.0f);
			}
			if (c[0] + c[1] + c[2] < 0.02f) {
				c[0] = c[1] = c[2] = 1.0f;
			}
		}

		void clamp_ext_colors(remix_ext_params& ext)
		{
			clamp_rgba(ext.min_spawn_color);
			clamp_rgba(ext.max_spawn_color);
			clamp_rgba(ext.min_target_color);
			clamp_rgba(ext.max_target_color);
		}

		void merge_mapped_from_src(remix_ext_params& ext, const captured_particle& src)
		{
			if (ext.has_override)
			{
				ext.collide = src.collide;
				return;
			}
			fill_ext_from_particle(src, ext);
			ext.collide = src.collide;
			ext.has_override = false;
		}

		bool mapped_close(const remix_ext_params& a, const remix_ext_params& b)
		{
			auto eq4 = [](const float x[4], const float y[4])
			{
				return std::fabs(x[0] - y[0]) < 0.002f && std::fabs(x[1] - y[1]) < 0.002f &&
					std::fabs(x[2] - y[2]) < 0.002f && std::fabs(x[3] - y[3]) < 0.002f;
			};
			return eq4(a.min_spawn_color, b.min_spawn_color) &&
				eq4(a.max_spawn_color, b.max_spawn_color) &&
				eq4(a.min_target_color, b.min_target_color) &&
				eq4(a.max_target_color, b.max_target_color) &&
				std::fabs(a.spawn_rate - b.spawn_rate) < 0.05f &&
				std::fabs(a.min_ttl - b.min_ttl) < 0.02f &&
				std::fabs(a.max_ttl - b.max_ttl) < 0.02f &&
				std::fabs(a.cone_deg - b.cone_deg) < 0.2f &&
				std::fabs(a.vel_from_normal - b.vel_from_normal) < 0.5f &&
				std::fabs(a.max_speed - b.max_speed) < 0.5f &&
				std::fabs(a.gravity_force - b.gravity_force) < 0.5f &&
				std::fabs(a.min_spawn_size - b.min_spawn_size) < 0.2f &&
				std::fabs(a.max_target_size - b.max_target_size) < 0.2f &&
				a.collide == b.collide;
		}

		void quat_to_basis(const float q[4], D3DXVECTOR3& x, D3DXVECTOR3& y, D3DXVECTOR3& z)
		{
			const D3DXVECTOR3 i{ 1.0f, 0.0f, 0.0f };
			const D3DXVECTOR3 j{ 0.0f, 1.0f, 0.0f };
			const D3DXVECTOR3 k{ 0.0f, 0.0f, 1.0f };
			quat_rotate(q, i, x);
			quat_rotate(q, j, y);
			quat_rotate(q, k, z);
		}

		void unit_rgb(D3DXVECTOR3& c)
		{
			if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z)) {
				c = { 1.0f, 1.0f, 1.0f };
				return;
			}
			const float m = std::max(std::max(std::fabs(c.x), std::fabs(c.y)), std::fabs(c.z));
			if (m > 1.5f && m <= 255.5f) {
				c.x /= 255.0f;
				c.y /= 255.0f;
				c.z /= 255.0f;
			}
			c.x = std::clamp(c.x, 0.0f, 1.0f);
			c.y = std::clamp(c.y, 0.0f, 1.0f);
			c.z = std::clamp(c.z, 0.0f, 1.0f);
		}

		bool rgb_looks_like_color(const D3DXVECTOR3& c)
		{
			return c.x >= 0.0f && c.x <= 1.0f &&
				c.y >= 0.0f && c.y <= 1.0f &&
				c.z >= 0.0f && c.z <= 1.0f &&
				(c.x + c.y + c.z) > 0.02f;
		}

		void read_dialog_rgb(const std::uint8_t* plugin, const int off, D3DXVECTOR3& out)
		{
			out = { 1.0f, 1.0f, 1.0f };
			if (!plugin) {
				return;
			}
			D3DXVECTOR3 f{};
			if (read_f3(plugin, off, f)) {
				unit_rgb(f);
				if (rgb_looks_like_color(f)) {
					out = f;
					return;
				}
			}
			if (!memory_readable(plugin + off, sizeof(COLORREF))) {
				return;
			}
			const COLORREF cr = *reinterpret_cast<const COLORREF*>(plugin + off);
			out = {
				GetRValue(cr) / 255.0f,
				GetGValue(cr) / 255.0f,
				GetBValue(cr) / 255.0f };
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
			int found = -1;
			int hits = 0;
			for (int i = 0; i < count; i++)
			{
				const auto* host = static_cast<const std::uint8_t*>(host_list_at[i]);
				if (!host || !memory_readable(host, sizeof(void*))) {
					continue;
				}
				if (*reinterpret_cast<void* const*>(host) == plugin)
				{
					hits++;
					if (found < 0) {
						found = i;
					}
				}
			}
			return hits == 1 ? found : -1;
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
			D3DXVECTOR3 best{};
			float best_len2 = -1.0f;
			const auto consider = [&](const int off, const int need)
			{
				D3DXVECTOR3 v{};
				if (!try_offset_pos(obj, off, need, v)) {
					return;
				}
				const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
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

		bool resolve_world_pos(const cached_plugin& rec, const std::uint8_t* plugin,
			const int count, D3DXVECTOR3& world, float q[4], bool& have_q)
		{
			have_q = read_quat(plugin, plugin_off_quat, q);
			D3DXVECTOR3 local{};
			const bool have_local = read_f3(plugin, plugin_off_pos, local) && sane_pos(local);
			if (!have_local) {
				local = {};
			}

			D3DXVECTOR3 parent{};
			float pq[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
			bool have_parent = false;
			if (rec.host && memory_readable(rec.host + host_off_parent_handle, sizeof(void*)))
			{
				const auto* impact = *reinterpret_cast<const std::uint8_t* const*>(
					rec.host + host_off_parent_handle);
				if (try_impact_pos(impact, parent) && significant_pos(parent)) {
					have_parent = true;
					if (rec.parent_slot >= 0 && rec.parent_slot < count && host_list_at)
					{
						const auto* phost = static_cast<const std::uint8_t*>(host_list_at[rec.parent_slot]);
						if (phost && memory_readable(phost, sizeof(void*)))
						{
							const auto* pplugin = *reinterpret_cast<const std::uint8_t* const*>(phost);
							read_quat(pplugin, plugin_off_quat, pq);
						}
					}
				}
			}

			if (have_parent)
			{
				D3DXVECTOR3 offset = local;
				const float n2 = pq[0] * pq[0] + pq[1] * pq[1] + pq[2] * pq[2] + pq[3] * pq[3];
				if (n2 > 0.81f && n2 < 1.21f) {
					quat_rotate(pq, local, offset);
				}
				world = { parent.x + offset.x, parent.y + offset.y, parent.z + offset.z };
				if (sane_pos(world)) {
					return true;
				}
			}
			if (have_local && sane_pos(local))
			{
				world = local;
				return true;
			}
			(void)count;
			return false;
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
				if (!rec.host || !memory_readable(rec.host, sizeof(void*))) {
					return false;
				}
				const auto* live_plugin = *reinterpret_cast<const std::uint8_t* const*>(rec.host);
				if (live_plugin != rec.plugin) {
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
			struct pose_keep
			{
				const std::uint8_t* plugin = nullptr;
				bool have_last = false;
				D3DXVECTOR3 last_pos{};
				float last_quat[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
				int object_id = -1;
			};
			pose_keep keep[remix_particle_cap]{};
			int keep_n = 0;
			for (int i = 0; i < cached_plugin_n && keep_n < remix_particle_cap; i++)
			{
				const auto& rec = cached_plugins[i];
				if (!rec.plugin) {
					continue;
				}
				auto& k = keep[keep_n++];
				k.plugin = rec.plugin;
				k.have_last = rec.have_last;
				k.last_pos = rec.last_pos;
				k.last_quat[0] = rec.last_quat[0];
				k.last_quat[1] = rec.last_quat[1];
				k.last_quat[2] = rec.last_quat[2];
				k.last_quat[3] = rec.last_quat[3];
				k.object_id = rec.object_id;
			}

			cached_plugin_n = 0;
			cached_host_count = count;
			for (int i = 0; i < count && cached_plugin_n < remix_particle_cap; i++)
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
				if (!path_is_particles(path)) {
					continue;
				}
				const auto* plugin = *reinterpret_cast<const std::uint8_t* const*>(host_obj);
				if (!plugin || !memory_readable(plugin, 0x5D0)) {
					continue;
				}
				auto& rec = cached_plugins[cached_plugin_n++];
				rec = {};
				rec.slot = i;
				rec.host = host_obj;
				rec.plugin = plugin;
				rec.parent_slot = find_parent_slot(i, count, rec.bone_id);
				for (int k = 0; k < keep_n; k++)
				{
					if (keep[k].plugin != plugin) {
						continue;
					}
					rec.have_last = keep[k].have_last;
					rec.last_pos = keep[k].last_pos;
					rec.last_quat[0] = keep[k].last_quat[0];
					rec.last_quat[1] = keep[k].last_quat[1];
					rec.last_quat[2] = keep[k].last_quat[2];
					rec.last_quat[3] = keep[k].last_quat[3];
					rec.object_id = keep[k].object_id;
					break;
				}
				if (rec.object_id < 0)
				{
					const auto pit = g_plugin_oid.find(plugin);
					if (pit != g_plugin_oid.end()) {
						rec.object_id = pit->second;
					}
				}
				if (memory_readable(plugin + plugin_off_name, 8)) {
					take_title(rec.title, reinterpret_cast<const char*>(plugin + plugin_off_name));
				}
			}
		}

		std::uint64_t mix_u64(std::uint64_t h, const std::uint64_t x)
		{
			h ^= x;
			h *= 1099511628211ull;
			return h;
		}

		std::uint64_t mix_cstr(std::uint64_t h, const char* s)
		{
			if (!s) {
				return h;
			}
			for (; *s; ++s) {
				h = mix_u64(h, static_cast<unsigned char>(*s));
			}
			return h;
		}

		std::uint64_t mix_f(std::uint64_t h, const float v)
		{
			const auto q = static_cast<std::uint32_t>(std::llround(static_cast<double>(v) * 1000.0));
			return mix_u64(h, q);
		}

		int parse_list_slot(const char* text)
		{
			return leading_decimal_id(text);
		}

		int object_list_item_count()
		{
			HWND list = editor_settings::object_list_hwnd();
			if (!list) {
				return 0;
			}
			const LRESULT n = SendMessageA(list, LB_GETCOUNT, 0, 0);
			if (n < 0) {
				return 0;
			}
			return static_cast<int>(n);
		}

		void refresh_object_ids_from_host_slots()
		{
			std::memset(g_slot_oid, 0xFF, sizeof(g_slot_oid));
			g_list_oid_fp = 0;
			for (int i = 0; i < cached_plugin_n; i++)
			{
				const int slot = cached_plugins[i].slot;
				if (slot < 0 || slot >= host_max_objects) {
					continue;
				}
				g_slot_oid[slot] = slot;
				cached_plugins[i].object_id = slot;
				g_list_oid_fp ^= static_cast<std::uint64_t>(slot) + 1ull;
			}
		}

		void refresh_object_ids_from_list()
		{
			HWND list = editor_settings::object_list_hwnd();
			if (!list) {
				if (!shared::globals::is_editor_host) {
					refresh_object_ids_from_host_slots();
				}
				return;
			}
			const int n = static_cast<int>(SendMessageA(list, LB_GETCOUNT, 0, 0));
			if (n <= 0) {
				return;
			}
			std::memset(g_slot_oid, 0xFF, sizeof(g_slot_oid));
			g_list_oid_fp = 0;
			int count = host_max_objects;
			if (host_count_at && memory_readable(host_count_at, sizeof(int)))
			{
				const int live = *host_count_at;
				if (live > 0 && live <= host_max_objects) {
					count = live;
				}
			}
			std::uint64_t fp = 0;
			for (int i = 0; i < n; i++)
			{
				char text[256]{};
				if (!editor_settings::object_list_item_text(i, text, 256) || !text[0]) {
					continue;
				}
				if (!std::strstr(text, "Particles") && !std::strstr(text, "particles")) {
					continue;
				}
				const int oid = leading_decimal_id(text);
				if (oid < 0 || oid >= host_max_objects) {
					continue;
				}
				fp ^= static_cast<std::uint64_t>(oid) + 1ull;
				const auto data = SendMessageA(list, LB_GETITEMDATA, static_cast<WPARAM>(i), 0);
				int host_slot = -1;
				const auto value = static_cast<std::uintptr_t>(data);
				if (value < static_cast<std::uintptr_t>(count)) {
					host_slot = static_cast<int>(value);
				}
				else {
					host_slot = slot_of_host(reinterpret_cast<void*>(data), count);
				}
				if (host_slot >= 0 && host_slot < host_max_objects) {
					g_slot_oid[host_slot] = oid;
				}
			}
			g_list_oid_fp = fp;
		}

		void move_section_maps_unlocked(const std::string& from, const std::string& to)
		{
			if (from.empty() || from == to) {
				return;
			}
			if (auto it = g_collide.find(from); it != g_collide.end())
			{
				g_collide[to] = it->second;
				g_collide.erase(it);
			}
			if (auto it = g_ext.find(from); it != g_ext.end())
			{
				g_ext[to] = it->second;
				g_ext.erase(it);
			}
		}

		void drop_particle_section_unlocked(int oid, std::uint64_t ident)
		{
			if (oid < 0 || oid >= host_max_objects) {
				return;
			}
			const std::string key = section_from_object_id(oid);
			g_collide.erase(key);
			g_ext.erase(key);
			if (ident) {
				g_ident_oid.erase(ident);
				g_id_section.erase(ident);
			}
			else
			{
				for (auto it = g_ident_oid.begin(); it != g_ident_oid.end(); )
				{
					if (it->second != oid) {
						++it;
						continue;
					}
					g_id_section.erase(it->first);
					it = g_ident_oid.erase(it);
				}
			}
			shared::common::log("Particles",
				std::format("oid compact drop {:05d} ident=0x{:X}",
					oid, ident));
		}

		void rename_particle_ini_section_unlocked(int old_oid, int new_oid, std::uint64_t ident)
		{
			if (old_oid < 0 || new_oid < 0 || old_oid == new_oid) {
				return;
			}
			const std::string from = section_from_object_id(old_oid);
			const std::string to = section_from_object_id(new_oid);
			move_section_maps_unlocked(from, to);
			if (ident) {
				g_ident_oid[ident] = new_oid;
				g_id_section[ident] = to;
			}
			shared::common::log("Particles",
				std::format("{} {:05d} -> {:05d} ident=0x{:X}",
					new_oid > old_oid ? "oid insert" : "oid compact",
					old_oid, new_oid, ident));
		}

		void inherit_clone_params_unlocked(int src_oid, int clone_oid, std::uint64_t clone_ident)
		{
			if (src_oid < 0 || clone_oid < 0 || src_oid == clone_oid) {
				return;
			}
			const std::string from = section_from_object_id(src_oid);
			const std::string to = section_from_object_id(clone_oid);
			if (auto it = g_collide.find(from); it != g_collide.end()) {
				g_collide[to] = it->second;
			}
			if (auto it = g_ext.find(from); it != g_ext.end()) {
				g_ext[to] = it->second;
			}
			if (clone_ident) {
				g_ident_oid[clone_ident] = clone_oid;
				g_id_section[clone_ident] = to;
			}
			shared::common::log("Particles",
				std::format("oid inherit {:05d} from {:05d} ident=0x{:X}",
					clone_oid, src_oid, clone_ident));
		}

		void delete_owned_ini_section_unlocked(const char* sec)
		{
			if (!sec || !sec[0] || !persist_ini_allowed() || !g_ini_path[0]) {
				return;
			}
			if (!maps_belong_to_current_ini_unlocked()) {
				return;
			}
			WritePrivateProfileStringA(sec, nullptr, nullptr, g_ini_path);
		}

		bool prune_vacated_particle_oids_unlocked()
		{
			if (!g_loaded_ok || !ini_is_current_stem_unlocked() ||
				!maps_belong_to_current_ini_unlocked())
			{
				return false;
			}
			std::set<int> live;
			for (int i = 0; i < cached_plugin_n; i++)
			{
				const int oid = cached_plugins[i].object_id;
				if (oid >= 0 && oid < host_max_objects) {
					live.insert(oid);
				}
			}
			for (int s = 0; s < host_max_objects; s++)
			{
				if (g_slot_oid[s] >= 0) {
					live.insert(g_slot_oid[s]);
				}
			}
			if (live.empty() && !g_allow_empty_persist)
			{
				shared::common::log("Particles",
					"skip oid prune — live ObjectId set empty (host not ready)",
					shared::common::LOG_TYPE::LOG_TYPE_STATUS, true);
				return false;
			}
			if (!g_host_ready_for_stem && !g_allow_empty_persist) {
				return false;
			}

			std::set<int> vacated;
			auto consider = [&](int oid)
			{
				if (oid >= 0 && oid < host_max_objects && live.find(oid) == live.end()) {
					vacated.insert(oid);
				}
			};
			for (const auto& kv : g_collide) {
				consider(object_id_from_section_name(kv.first.c_str()));
			}
			for (const auto& kv : g_ext) {
				consider(object_id_from_section_name(kv.first.c_str()));
			}
			for (const auto& kv : g_ident_oid) {
				consider(kv.second);
			}
			for (const auto& kv : g_id_section) {
				consider(object_id_from_section_name(kv.second.c_str()));
			}

			std::vector<std::string> disk_secs;
			const bool can_disk = persist_ini_allowed() && g_ini_path[0] &&
				maps_belong_to_current_ini_unlocked() && file_exists_a(g_ini_path);
			// Name-only disk pass: drop leftover [Particles_%05d] / hash
			// sections. Never GetPrivateProfileIntA on unrelated project
			// sections — that re-parses the whole INI per name.
			if (can_disk)
			{
				char names[8192]{};
				const DWORD nn = GetPrivateProfileSectionNamesA(
					names, sizeof(names), g_ini_path);
				if (nn != 0)
				{
					for (char* p = names; *p; p += std::strlen(p) + 1)
					{
						const int oid = object_id_from_section_name(p);
						if (oid >= 0)
						{
							if (live.find(oid) == live.end()) {
								vacated.insert(oid);
								disk_secs.emplace_back(p);
							}
							continue;
						}
						if (!is_hash_section_name(p) &&
							object_id_from_legacy_section(p) < 0)
						{
							continue;
						}
						bool still_mapped = false;
						for (const auto& kv : g_id_section)
						{
							if (kv.second == p) {
								still_mapped = true;
								break;
							}
						}
						if (!still_mapped) {
							disk_secs.emplace_back(p);
						}
					}
				}
			}

			if (vacated.empty() && disk_secs.empty()) {
				return false;
			}

			bool wrote = false;
			for (int oid : vacated)
			{
				const std::string key = section_from_object_id(oid);
				g_collide.erase(key);
				g_ext.erase(key);
				for (auto it = g_ident_oid.begin(); it != g_ident_oid.end(); )
				{
					if (it->second != oid) {
						++it;
						continue;
					}
					g_id_section.erase(it->first);
					it = g_ident_oid.erase(it);
				}
				for (auto it = g_id_section.begin(); it != g_id_section.end(); )
				{
					if (object_id_from_section_name(it->second.c_str()) != oid) {
						++it;
						continue;
					}
					it = g_id_section.erase(it);
				}
				delete_owned_ini_section_unlocked(key.c_str());
				wrote = true;
				shared::common::log("Particles",
					std::format("oid prune vacated {:05d}", oid));
			}
			for (const auto& sec : disk_secs)
			{
				if (object_id_from_section_name(sec.c_str()) >= 0) {
					continue;
				}
				delete_owned_ini_section_unlocked(sec.c_str());
				wrote = true;
			}
			if (can_disk && wrote) {
				WritePrivateProfileStringA(nullptr, nullptr, nullptr, g_ini_path);
			}
			return wrote;
		}

		bool compact_object_ids_unlocked()
		{
			if (cached_plugin_n <= 0)
			{
				return false;
			}
			struct oid_remap
			{
				int old_oid = -1;
				int new_oid = -1;
				std::uint64_t ident = 0;
			};
			std::map<const void*, int> now;
			std::vector<oid_remap> remaps;
			for (int i = 0; i < cached_plugin_n; i++)
			{
				auto& rec = cached_plugins[i];
				if (!rec.plugin) {
					continue;
				}
				int neu = -1;
				if (rec.slot >= 0 && rec.slot < host_max_objects) {
					neu = g_slot_oid[rec.slot];
				}
				int old = rec.object_id;
				if (old < 0)
				{
					const auto pit = g_plugin_oid.find(rec.plugin);
					if (pit != g_plugin_oid.end()) {
						old = pit->second;
					}
				}
				const auto ident = identity_of_plugin(rec.plugin, rec.host);
				now[rec.plugin] = neu;
				if (old >= 0 && neu >= 0 && old != neu) {
					remaps.push_back({ old, neu, ident });
				}
				rec.object_id = neu;
			}

			std::vector<oid_remap> dropped;
			for (const auto& kv : g_plugin_oid)
			{
				if (now.find(kv.first) != now.end() || kv.second < 0) {
					continue;
				}
				dropped.push_back({ kv.second, -1, identity_of_plugin(kv.first, nullptr) });
			}

			bool insert = false;
			for (const auto& r : remaps) {
				if (r.new_oid > r.old_oid) {
					insert = true;
					break;
				}
			}
			if (insert)
			{
				remaps.erase(std::remove_if(remaps.begin(), remaps.end(),
					[](const oid_remap& r) { return r.new_oid < r.old_oid; }),
					remaps.end());
				dropped.clear();
			}
			else
			{
				for (const auto& d : dropped) {
					drop_particle_section_unlocked(d.old_oid, d.ident);
				}
				if (!dropped.empty() && g_collide.empty() && g_ext.empty()) {
					g_allow_empty_persist = true;
				}
			}

			std::sort(remaps.begin(), remaps.end(),
				[insert](const oid_remap& a, const oid_remap& b) {
					return insert ? a.old_oid > b.old_oid : a.old_oid < b.old_oid;
				});
			for (const auto& r : remaps) {
				rename_particle_ini_section_unlocked(r.old_oid, r.new_oid, r.ident);
			}

			bool inherited = false;
			if (insert && !remaps.empty())
			{
				std::set<int> new_oids;
				for (const auto& r : remaps) {
					new_oids.insert(r.new_oid);
				}
				for (const auto& src : remaps)
				{
					if (src.new_oid != src.old_oid + 1 || new_oids.count(src.old_oid)) {
						continue;
					}
					const int clone_oid = src.old_oid;
					for (int i = 0; i < cached_plugin_n; i++)
					{
						auto& rec = cached_plugins[i];
						if (rec.object_id != clone_oid || !rec.plugin) {
							continue;
						}
						const auto clone_ident = identity_of_plugin(rec.plugin, rec.host);
						if (clone_ident == src.ident) {
							continue;
						}
						inherit_clone_params_unlocked(src.new_oid, clone_oid, clone_ident);
						inherited = true;
						break;
					}
				}
			}

			bool pruned = false;
			if (!insert)
			{
				bool downward = false;
				for (const auto& r : remaps)
				{
					if (r.new_oid < r.old_oid) {
						downward = true;
						break;
					}
				}
				if (!dropped.empty() || downward) {
					pruned = prune_vacated_particle_oids_unlocked();
				}
			}

			g_plugin_oid = std::move(now);
			return !dropped.empty() || !remaps.empty() || inherited || pruned;
		}

		int bind_live_plugins_unlocked()
		{
			int applied = 0;
			for (int i = 0; i < cached_plugin_n; i++)
			{
				auto& rec = cached_plugins[i];
				const auto ident = identity_of_plugin(rec.plugin, rec.host);
				if (!ident) {
					continue;
				}
				int oid = rec.object_id;
				if (oid < 0 && rec.slot >= 0 && rec.slot < host_max_objects) {
					oid = g_slot_oid[rec.slot];
				}
				rec.object_id = oid;
				if (oid >= 0)
				{
					bind_identity_object_id_unlocked(ident, oid);
					const std::string key = section_from_object_id(oid);
					if (g_ext.find(key) != g_ext.end() || g_collide.find(key) != g_collide.end()) {
						++applied;
					}
				}
				else
				{
					const std::string hk = section_id_from_identity(ident);
					const auto mapped = g_id_section.find(ident);
					if (mapped != g_id_section.end() &&
						(g_ext.find(mapped->second) != g_ext.end() ||
							g_collide.find(mapped->second) != g_collide.end()))
					{
						++applied;
					}
					else if (g_ext.find(hk) != g_ext.end() || g_collide.find(hk) != g_collide.end())
					{
						g_id_section[ident] = hk;
						++applied;
					}
				}
			}
			return applied;
		}

		void maybe_log_project_loaded_unlocked(int applied)
		{
			if (!g_ini_path[0]) {
				return;
			}
			if (_stricmp(g_bound_ini, g_ini_path) == 0 &&
				g_bound_plugin_n == cached_plugin_n &&
				g_bound_applied == applied)
			{
				return;
			}
			std::strncpy(g_bound_ini, g_ini_path, MAX_PATH - 1);
			g_bound_plugin_n = cached_plugin_n;
			g_bound_applied = applied;
			int sections = 0;
			std::map<std::string, int> uniq;
			for (const auto& kv : g_collide) {
				uniq[kv.first] = 1;
			}
			for (const auto& kv : g_ext) {
				uniq[kv.first] = 1;
			}
			sections = static_cast<int>(uniq.size());
			const char* stem = project_file::stem();
			shared::common::log("Particles",
				std::format("project loaded stem={} ini={} sections={} applied={}",
					(stem && stem[0]) ? stem : "-", g_ini_path, sections, applied));
		}

		void note_list_sync_state(int host_count)
		{
			g_sync_host_count = host_count;
			g_sync_list_count = object_list_item_count();
		}

		bool host_or_list_count_changed(int host_count)
		{
			const int list_n = object_list_item_count();
			if (host_count != g_sync_host_count) {
				return true;
			}
			if (list_n <= 0) {
				return g_sync_list_count < 0;
			}
			return list_n != g_sync_list_count;
		}

		void maybe_sync_live_plugins(bool persist)
		{
			int host = cached_host_count;
			if (host_count_at && memory_readable(host_count_at, sizeof(int)))
			{
				const int live = *host_count_at;
				if (live > 0 && live <= host_max_objects) {
					host = live;
				}
			}
			if (!host_or_list_count_changed(host)) {
				return;
			}
			sync_live_plugins_with_ini(persist);
		}

		void sync_live_plugins_with_ini(bool persist)
		{
			if (bind_host() && host_count_at && memory_readable(host_count_at, sizeof(int)))
			{
				const int count = *host_count_at;
				if (count > 0 && count <= host_max_objects && !cache_still_valid(count)) {
					rescan_plugins(count);
				}
			}
			refresh_object_ids_from_list();
			{
				std::lock_guard lock(g_store_mu);
				bool shifted = false;
				if (editor_ini_allowed()) {
					shifted = compact_object_ids_unlocked();
				}
				else
				{
					for (int i = 0; i < cached_plugin_n; i++)
					{
						auto& rec = cached_plugins[i];
						if (rec.slot >= 0 && rec.slot < host_max_objects) {
							rec.object_id = rec.slot;
							g_slot_oid[rec.slot] = rec.slot;
						}
					}
				}
				const int applied = bind_live_plugins_unlocked();
				if (!g_host_ready_for_stem && g_loaded_ok &&
					maps_belong_to_current_ini_unlocked())
				{
					bool overlap = false;
					for (int i = 0; i < cached_plugin_n; i++)
					{
						const int oid = cached_plugins[i].object_id;
						if (oid < 0) {
							continue;
						}
						const std::string key = section_from_object_id(oid);
						if (g_collide.find(key) != g_collide.end() ||
							g_ext.find(key) != g_ext.end())
						{
							overlap = true;
							break;
						}
					}
					if (overlap || (g_collide.empty() && g_ext.empty())) {
						g_host_ready_for_stem = true;
					}
				}
				if (persist && shifted && g_host_ready_for_stem && g_loaded_ok &&
					cached_plugin_n > 0) {
					write_ini_unlocked();
				}
				maybe_log_project_loaded_unlocked(applied);
			}
			note_list_sync_state(cached_host_count);
		}

		bool slot_is_particles(int slot, const int count)
		{
			if (slot < 0 || slot >= count || !host_hmod_at || !host_list_at) {
				return false;
			}
			const HMODULE plugin_mod = host_hmod_at[slot];
			if (!plugin_mod) {
				return false;
			}
			char path[MAX_PATH]{};
			if (!GetModuleFileNameA(plugin_mod, path, MAX_PATH) || !path[0]) {
				return false;
			}
			if (!path_is_particles(path)) {
				return false;
			}
			const auto* host = static_cast<const std::uint8_t*>(host_list_at[slot]);
			if (!host || !memory_readable(host, sizeof(void*))) {
				return false;
			}
			const auto* plugin = *reinterpret_cast<const std::uint8_t* const*>(host);
			return plugin && memory_readable(plugin, 16);
		}

		int ensure_host_count()
		{
			if (!bind_host() || !host_count_at) {
				return -1;
			}
			const int count = *host_count_at;
			if (count <= 0 || count > host_max_objects) {
				return -1;
			}
			if (!cache_still_valid(count)) {
				rescan_plugins(count);
			}
			return count;
		}

		bool ptrs_from_handle(void* handle, const void*& host, const void*& plugin)
		{
			host = nullptr;
			plugin = nullptr;
			if (!handle) {
				return false;
			}
			const int count = ensure_host_count();
			if (count < 0 || !host_list_at) {
				return false;
			}
			for (int i = 0; i < count; i++)
			{
				if (host_list_at[i] != handle || !slot_is_particles(i, count)) {
					continue;
				}
				host = handle;
				if (memory_readable(handle, sizeof(void*))) {
					plugin = *reinterpret_cast<void* const*>(handle);
				}
				return host && plugin;
			}
			const void* found_host = nullptr;
			int hits = 0;
			for (int i = 0; i < count; i++)
			{
				const auto* h = static_cast<const std::uint8_t*>(host_list_at[i]);
				if (!h || !memory_readable(h, sizeof(void*)) || !slot_is_particles(i, count)) {
					continue;
				}
				if (*reinterpret_cast<void* const*>(h) == handle)
				{
					hits++;
					found_host = h;
				}
			}
			if (hits >= 1 && found_host)
			{
				host = found_host;
				plugin = handle;
				return true;
			}
			const auto value = reinterpret_cast<std::uintptr_t>(handle);
			if (value < static_cast<std::uintptr_t>(count) &&
				slot_is_particles(static_cast<int>(value), count))
			{
				host = host_list_at[static_cast<int>(value)];
				if (host && memory_readable(host, sizeof(void*))) {
					plugin = *reinterpret_cast<void* const*>(host);
				}
				return host && plugin;
			}
			if (memory_readable(handle, sizeof(void*)))
			{
				void* inner = *reinterpret_cast<void* const*>(handle);
				if (inner && inner != handle)
				{
					for (int i = 0; i < count; i++)
					{
						if (!slot_is_particles(i, count)) {
							continue;
						}
						if (host_list_at[i] == inner)
						{
							host = inner;
							if (memory_readable(inner, sizeof(void*))) {
								plugin = *reinterpret_cast<void* const*>(inner);
							}
							return host && plugin;
						}
						const auto* h = static_cast<const std::uint8_t*>(host_list_at[i]);
						if (h && memory_readable(h, sizeof(void*)) &&
							*reinterpret_cast<void* const*>(h) == inner)
						{
							host = h;
							plugin = inner;
							return true;
						}
					}
				}
			}
			return false;
		}

		bool capture_list_row_plugin(const void*& host, const void*& plugin, int* oid_out)
		{
			host = nullptr;
			plugin = nullptr;
			if (oid_out) {
				*oid_out = -1;
			}
			HWND list = editor_settings::object_list_hwnd();
			if (!list) {
				return false;
			}
			const int sel = editor_settings::object_list_cursel();
			if (sel < 0) {
				return false;
			}
			char text[256]{};
			if (!editor_settings::object_list_item_text(sel, text, 256) || !text[0]) {
				return false;
			}
			if (!std::strstr(text, "Particles") && !std::strstr(text, "particles")) {
				return false;
			}
			const int oid = leading_decimal_id(text);
			if (oid_out) {
				*oid_out = oid;
			}
			const auto data = SendMessageA(list, LB_GETITEMDATA, static_cast<WPARAM>(sel), 0);
			if (data && ptrs_from_handle(reinterpret_cast<void*>(data), host, plugin) && plugin) {
				return true;
			}
			refresh_object_ids_from_list();
			const int count = ensure_host_count();
			if (oid >= 0 && count > 0)
			{
				for (int i = 0; i < count; i++)
				{
					if (g_slot_oid[i] != oid || !slot_is_particles(i, count)) {
						continue;
					}
					host = host_list_at[i];
					if (host && memory_readable(host, sizeof(void*))) {
						plugin = *reinterpret_cast<void* const*>(host);
					}
					return host && plugin;
				}
			}
			return false;
		}

		void forget_hwnd_unlocked(HWND dlg)
		{
			if (!dlg) {
				return;
			}
			g_hwnd_plugin.erase(dlg);
			g_hwnd_host.erase(dlg);
		}

		void stash_properties_ptrs(HWND dlg, const void* host, const void* plugin)
		{
			if (!dlg) {
				return;
			}
			if (host) {
				SetPropA(dlg, "vrePHost", const_cast<void*>(host));
				g_hwnd_host[dlg] = host;
			}
			if (plugin) {
				SetPropA(dlg, "vrePPlug", const_cast<void*>(plugin));
				g_hwnd_plugin[dlg] = plugin;
			}
		}

		bool stash_properties_ptrs_from_dlg(HWND dlg, bool force)
		{
			if (!dlg || !IsWindow(dlg)) {
				return false;
			}
			void* candidates[4]{
				GetPropA(dlg, "vrePDlgArg"),
				reinterpret_cast<void*>(GetWindowLongPtrA(dlg, DWLP_USER)),
				reinterpret_cast<void*>(GetWindowLongPtrA(dlg, GWLP_USERDATA)),
				nullptr
			};
			for (void* handle : candidates)
			{
				const void* host = nullptr;
				const void* plugin = nullptr;
				if (ptrs_from_handle(handle, host, plugin))
				{
					stash_properties_ptrs(dlg, host, plugin);
					return true;
				}
			}
			if (!force)
			{
				const HANDLE stashed_host = GetPropA(dlg, "vrePHost");
				const HANDLE stashed_plug = GetPropA(dlg, "vrePPlug");
				if (stashed_host && stashed_plug) {
					g_hwnd_host[dlg] = stashed_host;
					g_hwnd_plugin[dlg] = stashed_plug;
					return true;
				}
			}
			{
				std::lock_guard lock(g_store_mu);
				const auto ip = g_hwnd_plugin.find(dlg);
				const auto ih = g_hwnd_host.find(dlg);
				if (ip != g_hwnd_plugin.end() && ip->second) {
					if (ih != g_hwnd_host.end()) {
						stash_properties_ptrs(dlg, ih->second, ip->second);
					}
					else {
						stash_properties_ptrs(dlg, nullptr, ip->second);
					}
					return true;
				}
			}
			if (is_particles_properties_dialog(dlg))
			{
				const void* host = nullptr;
				const void* plugin = nullptr;
				int oid = -1;
				if (capture_list_row_plugin(host, plugin, &oid) && plugin)
				{
					stash_properties_ptrs(dlg, host, plugin);
					if (oid >= 0) {
						std::lock_guard lock(g_store_mu);
						bind_identity_object_id_unlocked(
							identity_of_plugin(plugin, host), oid);
					}
					shared::common::log("Particles",
						std::format("bound from list row oid={} plugin=0x{:X} host=0x{:X}",
							oid,
							reinterpret_cast<std::uintptr_t>(plugin),
							reinterpret_cast<std::uintptr_t>(host)));
					return true;
				}
			}
			return false;
		}

		std::uint64_t identity_from_stashed_dlg(HWND dlg)
		{
			if (!stash_properties_ptrs_from_dlg(dlg, false)) {
				return 0;
			}
			const void* host = GetPropA(dlg, "vrePHost");
			const void* plugin = GetPropA(dlg, "vrePPlug");
			{
				std::lock_guard lock(g_store_mu);
				const auto ip = g_hwnd_plugin.find(dlg);
				if (ip != g_hwnd_plugin.end() && ip->second) {
					plugin = ip->second;
				}
				const auto ih = g_hwnd_host.find(dlg);
				if (ih != g_hwnd_host.end() && ih->second) {
					host = ih->second;
				}
			}
			if (!plugin && host && memory_readable(host, sizeof(void*))) {
				plugin = *reinterpret_cast<void* const*>(host);
			}
			return identity_of_plugin(plugin, host);
		}

		void build_tex_path(const std::uint8_t* plugin, char* dest, const int cap)
		{
			if (!dest || cap <= 0) {
				return;
			}
			dest[0] = 0;
			char dir[MAX_PATH]{};
			char leaf[80]{};
			if (memory_readable(plugin + plugin_off_dir, 8)) {
				std::strncpy(dir, reinterpret_cast<const char*>(plugin + plugin_off_dir), MAX_PATH - 1);
			}
			if (memory_readable(plugin + plugin_off_tex, 8)) {
				std::strncpy(leaf, reinterpret_cast<const char*>(plugin + plugin_off_tex), 79);
			}
			if (!leaf[0]) {
				std::strncpy(leaf, "particle_default", 79);
			}
			if (dir[0])
			{
				std::snprintf(dest, static_cast<std::size_t>(cap), "%sdata\\%s.dds", dir, leaf);
			}
			else
			{
				std::snprintf(dest, static_cast<std::size_t>(cap),
					"%s\\3DRad_res\\objects\\Particles\\data\\%s.dds",
					shared::globals::host_data_root().c_str(), leaf);
			}
		}

		void join_data_dir(const char* plugin_dir, char* dest, const int cap)
		{
			if (!dest || cap <= 0) {
				return;
			}
			dest[0] = 0;
			if (plugin_dir && plugin_dir[0]) {
				std::snprintf(dest, static_cast<std::size_t>(cap), "%sdata\\", plugin_dir);
			}
			else {
				std::snprintf(dest, static_cast<std::size_t>(cap),
					"%s\\3DRad_res\\objects\\Particles\\data\\",
					shared::globals::host_data_root().c_str());
			}
		}

		bool pack_sequence_sheet(char paths[][MAX_PATH], const int n, const char* out_path)
		{
			if (!paths || n < 2 || !out_path || !out_path[0]) {
				return false;
			}
			auto* dev = shared::globals::d3d_device;
			if (!dev)
			{
				shared::common::log("Particles",
					"pack sprite sheet failed: no D3D device",
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return false;
			}
			D3DXIMAGE_INFO info{};
			HRESULT hr = D3DXGetImageInfoFromFileA(paths[0], &info);
			if (FAILED(hr) || info.Width == 0 || info.Height == 0)
			{
				shared::common::log("Particles",
					std::format("pack sprite sheet GetImageInfo hr=0x{:08X} {}",
						static_cast<unsigned>(hr), paths[0]),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return false;
			}
			const UINT aw = info.Width * static_cast<UINT>(n);
			const UINT ah = info.Height;
			IDirect3DSurface9* dest = nullptr;
			hr = dev->CreateOffscreenPlainSurface(aw, ah, D3DFMT_A8R8G8B8,
				D3DPOOL_SYSTEMMEM, &dest, nullptr);
			if (FAILED(hr) || !dest)
			{
				hr = dev->CreateOffscreenPlainSurface(aw, ah, D3DFMT_X8R8G8B8,
					D3DPOOL_SYSTEMMEM, &dest, nullptr);
			}
			if (FAILED(hr) || !dest)
			{
				shared::common::log("Particles",
					std::format("pack sprite sheet CreateOffscreenPlainSurface hr=0x{:08X} {}x{} n={}",
						static_cast<unsigned>(hr), aw, ah, n),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return false;
			}
			bool ok = true;
			for (int i = 0; i < n; i++)
			{
				RECT rc{};
				rc.left = static_cast<LONG>(i * info.Width);
				rc.top = 0;
				rc.right = rc.left + static_cast<LONG>(info.Width);
				rc.bottom = static_cast<LONG>(info.Height);
				hr = D3DXLoadSurfaceFromFileA(dest, nullptr, &rc, paths[i],
					nullptr, D3DX_FILTER_NONE, 0, nullptr);
				if (FAILED(hr))
				{
					hr = D3DXLoadSurfaceFromFileA(dest, nullptr, &rc, paths[i],
						nullptr, D3DX_FILTER_LINEAR, 0, nullptr);
				}
				if (FAILED(hr))
				{
					shared::common::log("Particles",
						std::format("pack sprite sheet LoadSurface hr=0x{:08X} frame {} {}",
							static_cast<unsigned>(hr), i, paths[i]),
						shared::common::LOG_TYPE::LOG_TYPE_WARN);
					ok = false;
					break;
				}
			}
			if (ok)
			{
				hr = D3DXSaveSurfaceToFileA(out_path, D3DXIFF_DDS, dest, nullptr, nullptr);
				ok = SUCCEEDED(hr);
				if (!ok)
				{
					shared::common::log("Particles",
						std::format("pack sprite sheet SaveSurface hr=0x{:08X} {}",
							static_cast<unsigned>(hr), out_path),
						shared::common::LOG_TYPE::LOG_TYPE_WARN);
				}
			}
			dest->Release();
			return ok;
		}

		void resolve_sprite_sheet(const char* plugin_dir, const char* leaf,
			float plugin_fps, std::uint64_t identity, char* out_path, const int cap,
			std::uint8_t& rows, std::uint8_t& cols, std::uint8_t& fps)
		{
			rows = 1;
			cols = 1;
			fps = 0;
			if (!out_path || cap <= 0) {
				return;
			}
			out_path[0] = 0;
			char data_dir[MAX_PATH]{};
			join_data_dir(plugin_dir, data_dir, MAX_PATH);
			char use_leaf[80]{};
			std::strncpy(use_leaf, (leaf && leaf[0]) ? leaf : "particle_default", 79);

			char seq[16][MAX_PATH]{};
			int n = 0;
			char first[MAX_PATH]{};
			std::snprintf(first, MAX_PATH, "%s%s.dds", data_dir, use_leaf);
			if (file_exists_a(first) && n < 16) {
				std::strncpy(seq[n++], first, MAX_PATH - 1);
			}
			for (int i = 1; i <= 16 && n < 16; i++)
			{
				char frame[MAX_PATH]{};
				std::snprintf(frame, MAX_PATH, "%s%s\\%04d.dds", data_dir, use_leaf, i);
				if (!file_exists_a(frame)) {
					break;
				}
				std::strncpy(seq[n++], frame, MAX_PATH - 1);
			}

			if (n <= 0)
			{
				std::strncpy(out_path, first, static_cast<std::size_t>(cap) - 1);
				return;
			}
			if (n == 1)
			{
				std::strncpy(out_path, seq[0], static_cast<std::size_t>(cap) - 1);
				rows = 1;
				cols = 1;
				fps = 0;
				return;
			}

			rows = 1;
			cols = static_cast<std::uint8_t>(n);
			float use_fps = plugin_fps;
			if (!(use_fps >= 1.0f && use_fps <= 60.0f)) {
				use_fps = 10.0f;
			}
			fps = static_cast<std::uint8_t>(use_fps + 0.5f);
			if (!fps) {
				fps = 10;
			}

			char sheet_dir[MAX_PATH]{};
			host_particle_sheet_dir(sheet_dir, MAX_PATH);
			char packed[MAX_PATH]{};
			const unsigned lo = static_cast<unsigned>(identity & 0xFFFFFFFFu);
			if (lo) {
				std::snprintf(packed, MAX_PATH, "%sp%08X_%s_r%uc%u.dds", sheet_dir, lo, use_leaf,
					static_cast<unsigned>(rows), static_cast<unsigned>(cols));
			}
			else {
				std::snprintf(packed, MAX_PATH, "%s%s_r%uc%u.dds", sheet_dir, use_leaf,
					static_cast<unsigned>(rows), static_cast<unsigned>(cols));
			}
			if (file_exists_a(packed) || pack_sequence_sheet(seq, n, packed))
			{
				std::strncpy(out_path, packed, static_cast<std::size_t>(cap) - 1);
				return;
			}
			std::strncpy(out_path, seq[0], static_cast<std::size_t>(cap) - 1);
			rows = 1;
			cols = 1;
			fps = 0;
		}

		int collect_into(captured_particle* out, const int max_n)
		{
			if (!out || max_n <= 0 || !bind_host()) {
				return 0;
			}
			const int count = *host_count_at;
			if (count <= 0 || count > host_max_objects) {
				cached_plugin_n = 0;
				cached_host_count = -1;
				return 0;
			}
			const bool cache_ok = cache_still_valid(count);
			if (!cache_ok) {
				rescan_plugins(count);
			}
			maybe_sync_live_plugins(true);

			int n = 0;
			for (int i = 0; i < cached_plugin_n && n < max_n; i++)
			{
				auto& rec = cached_plugins[i];
				const auto* plugin = rec.plugin;
				if (!plugin) {
					continue;
				}

				int shown = 1;
				int active = 1;
				read_i(plugin, plugin_off_shown, shown);
				read_i(plugin, plugin_off_active, active);
				// Play can zero editor shown while ObjectRun still simulates if
				// active. Skip only when the object would not run at all.
				if (active == 0) {
					continue;
				}

				D3DXVECTOR3 world{};
				float q[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
				bool have_q = read_quat(plugin, plugin_off_quat, q);
				if (!resolve_world_pos(rec, plugin, count, world, q, have_q) || !sane_pos(world))
				{
					if (rec.have_last) {
						world = rec.last_pos;
						if (!have_q) {
							q[0] = rec.last_quat[0];
							q[1] = rec.last_quat[1];
							q[2] = rec.last_quat[2];
							q[3] = rec.last_quat[3];
							have_q = true;
						}
					}
					else if (!read_f3(plugin, plugin_off_pos, world) || !sane_pos(world))
					{
						world = {};
					}
				}
				else
				{
					rec.have_last = true;
					rec.last_pos = world;
				}
				if (!have_q) {
					have_q = read_quat(plugin, plugin_off_quat, q);
				}
				if (have_q) {
					rec.last_quat[0] = q[0];
					rec.last_quat[1] = q[1];
					rec.last_quat[2] = q[2];
					rec.last_quat[3] = q[3];
				}

				auto& slot = out[n++];
				slot = {};
				slot.pos = world;
				slot.quat[0] = q[0];
				slot.quat[1] = q[1];
				slot.quat[2] = q[2];
				slot.quat[3] = q[3];
				slot.shown = shown;
				slot.active = active;
				slot.slot = rec.slot;
				take_title(slot.title, rec.title);
				read_dialog_rgb(plugin, plugin_off_rgb_init, slot.rgb_init);
				read_dialog_rgb(plugin, plugin_off_rgb_final, slot.rgb_final);
				read_f(plugin, plugin_off_opacity, slot.opacity);
				read_f(plugin, plugin_off_frequency, slot.frequency);
				read_f(plugin, plugin_off_lifetime, slot.lifetime);
				read_f(plugin, plugin_off_speed_min, slot.speed_min);
				read_f(plugin, plugin_off_speed_max, slot.speed_max);
				read_f(plugin, plugin_off_emit_min, slot.emit_min);
				read_f(plugin, plugin_off_emit_max, slot.emit_max);
				read_f3(plugin, plugin_off_grav, slot.grav);
				read_f(plugin, plugin_off_air, slot.air);
				read_f(plugin, plugin_off_scale_init, slot.scale_init);
				read_f(plugin, plugin_off_scale_final, slot.scale_final);
				read_f(plugin, plugin_off_timer, slot.timer);
				read_i(plugin, plugin_off_burnout, slot.burnout);
				read_i(plugin, plugin_off_reflect, slot.reflect);
				if (!read_i(plugin, plugin_off_bone_dlg, slot.bone)) {
					read_i(plugin, plugin_off_bone, slot.bone);
				}
				read_i(plugin, plugin_off_live_count, slot.live_count);
				if (memory_readable(plugin + plugin_off_tex, 8)) {
					std::strncpy(slot.tex, reinterpret_cast<const char*>(plugin + plugin_off_tex), 79);
				}
				build_tex_path(plugin, slot.tex_path, MAX_PATH);
				slot.identity = identity_of_plugin(plugin, rec.host);
				slot.plugin_ptr = plugin;
				{
					char dir[MAX_PATH]{};
					if (memory_readable(plugin + plugin_off_dir, 8)) {
						std::strncpy(dir, reinterpret_cast<const char*>(plugin + plugin_off_dir), MAX_PATH - 1);
					}
					float anim_fps = 0.0f;
					read_f(plugin, plugin_off_anim_fps, anim_fps);
					const char* leaf = slot.tex[0] ? slot.tex : "particle_default";
					if (rec.sheet_ready &&
						std::fabs(rec.sheet_anim_fps - anim_fps) < 0.01f &&
						_stricmp(rec.sheet_leaf, leaf) == 0)
					{
						std::strncpy(slot.sheet_path, rec.sheet_path, MAX_PATH - 1);
						slot.sheet_rows = rec.sheet_rows;
						slot.sheet_cols = rec.sheet_cols;
						slot.sheet_fps = rec.sheet_fps;
					}
					else
					{
						resolve_sprite_sheet(dir, leaf, anim_fps, slot.identity,
							slot.sheet_path, MAX_PATH,
							slot.sheet_rows, slot.sheet_cols, slot.sheet_fps);
						std::strncpy(rec.sheet_leaf, leaf, 79);
						std::strncpy(rec.sheet_path, slot.sheet_path, MAX_PATH - 1);
						rec.sheet_anim_fps = anim_fps;
						rec.sheet_rows = slot.sheet_rows;
						rec.sheet_cols = slot.sheet_cols;
						rec.sheet_fps = slot.sheet_fps;
						rec.sheet_ready = true;
					}
					if (slot.sheet_cols > 1 || slot.sheet_rows > 1) {
						slot.sheet_mode = 0;
					}
				}
				if (!(slot.frequency > 0.0f)) {
					slot.frequency = 20.0f;
				}
				if (!(slot.lifetime > 0.0f)) {
					slot.lifetime = 1.0f;
				}
				if (!(slot.opacity >= 0.0f && slot.opacity <= 1.0f)) {
					slot.opacity = 0.8f;
				}
				if (!(std::fabs(slot.air) > 0.0000005f)) {
					slot.air = -0.001f;
				}
				register_section_id(slot.identity, slot.title);
				slot.collide = collide_of_id(slot.identity);
				std::uint64_t h = 1469598103934665603ull;
				h = mix_f(h, slot.pos.x);
				h = mix_f(h, slot.pos.y);
				h = mix_f(h, slot.pos.z);
				h = mix_f(h, slot.rgb_init.x);
				h = mix_f(h, slot.frequency);
				h = mix_f(h, slot.lifetime);
				h = mix_f(h, slot.grav.x);
				h = mix_f(h, slot.grav.y);
				h = mix_f(h, slot.grav.z);
				h = mix_f(h, slot.air);
				h = mix_u64(h, slot.collide ? 1ull : 0ull);
				h = mix_u64(h, slot.sheet_rows);
				h = mix_u64(h, slot.sheet_cols);
				h = mix_u64(h, slot.sheet_fps);
				slot.content = h;
			}
			return n;
		}

		void to_wide(const char* src, wchar_t* dest, const int cap)
		{
			if (!dest || cap <= 0) {
				return;
			}
			dest[0] = 0;
			if (!src || !src[0]) {
				return;
			}
			MultiByteToWideChar(CP_ACP, 0, src, -1, dest, cap);
			dest[cap - 1] = 0;
		}

		void destroy_one_emitter(slot_emitter& e)
		{
			auto& api = shared::common::remix_api::get();
			if (api.is_initialized())
			{
				if (e.mesh && api.m_bridge.DestroyMesh) {
					api.m_bridge.DestroyMesh(e.mesh);
				}
				if (e.mat && api.m_bridge.DestroyMaterial) {
					api.m_bridge.DestroyMaterial(e.mat);
				}
			}
			e = {};
		}

		void destroy_emitter_unlocked()
		{
			for (auto& e : emitters) {
				destroy_one_emitter(e);
			}
			emitter_ready = false;
			emitter_tex_w[0] = 0;
		}

		bool ensure_emitter_for(const captured_particle& src, remixapi_MeshHandle& out_mesh)
		{
			out_mesh = nullptr;
			const std::uint64_t identity = src.identity;
			if (!identity) {
				return false;
			}
			if (g_pending_invalidate)
			{
				const std::uint64_t drop = g_pending_invalidate;
				g_pending_invalidate = 0;
				for (auto& e : emitters) {
					if (e.identity == drop) {
						destroy_one_emitter(e);
					}
				}
			}
			auto& api = shared::common::remix_api::get();
			if (!camera::remix_api_init_allowed() || !api.is_initialized() ||
				!api.m_bridge.CreateMaterial || !api.m_bridge.CreateMesh ||
				!api.m_bridge.DrawInstance)
			{
				return false;
			}

			slot_emitter* slot = nullptr;
			slot_emitter* empty = nullptr;
			wchar_t tex_w[MAX_PATH]{};
			const char* albedo = src.sheet_path[0] ? src.sheet_path : src.tex_path;
			std::uint8_t rows = src.sheet_rows ? src.sheet_rows : 1;
			std::uint8_t cols = src.sheet_cols ? src.sheet_cols : 1;
			std::uint8_t fps = src.sheet_fps;
			{
				remix_ext_params overlay{};
				if (remix_ext_of(identity, overlay) && overlay.has_override)
				{
					if (overlay.sheet_file[0]) {
						albedo = overlay.sheet_file;
					}
					if (overlay.sheet_rows) {
						rows = overlay.sheet_rows;
					}
					if (overlay.sheet_cols) {
						cols = overlay.sheet_cols;
					}
					if (overlay.sheet_fps) {
						fps = overlay.sheet_fps;
					}
				}
			}
			to_wide(albedo, tex_w, MAX_PATH);

			for (auto& e : emitters)
			{
				if (e.identity == identity && e.ready && e.mesh)
				{
					if (e.sheet_rows == rows && e.sheet_cols == cols && e.sheet_fps == fps &&
						_stricmp(e.tex_key, albedo ? albedo : "") == 0)
					{
						e.slot = src.slot;
						out_mesh = e.mesh;
						return true;
					}
					destroy_one_emitter(e);
					slot = &e;
					break;
				}
				if (!empty && !e.ready) {
					empty = &e;
				}
			}
			if (!slot) {
				slot = empty;
			}
			if (!slot) {
				shared::common::log("Particles",
					std::format("no free emitter slot for ident=0x{:X} - not stealing another mesh",
						identity),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return false;
			}

			remixapi_MaterialInfoOpaqueEXT opaque{};
			opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
			opaque.useDrawCallAlphaState = 0;
			opaque.blendType_hasvalue = 1;
			opaque.blendType_value = 3;
			opaque.albedoConstant = { 1.0f, 1.0f, 1.0f };
			opaque.opacityConstant = 1.0f;
			opaque.roughnessConstant = 1.0f;
			opaque.metallicConstant = 0.0f;
			opaque.roughnessTexture = L"";
			opaque.metallicTexture = L"";
			opaque.heightTexture = L"";

			remixapi_MaterialInfo mat{};
			mat.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
			mat.pNext = &opaque;
			mat.hash = identity ^ 1ull;
			mat.hash = mix_cstr(mat.hash, albedo ? albedo : "");
			mat.hash = mix_u64(mat.hash, rows);
			mat.hash = mix_u64(mat.hash, cols);
			mat.hash = mix_u64(mat.hash, fps);
			if (!mat.hash) {
				mat.hash = k_ident_tag | 1ull;
			}
			mat.normalTexture = L"";
			mat.tangentTexture = L"";
			mat.emissiveTexture = L"";
			mat.emissiveIntensity = 0.0f;
			mat.emissiveColorConstant = { 1.0f, 1.0f, 1.0f };
			mat.filterMode = 1;
			mat.wrapModeU = 1;
			mat.wrapModeV = 1;

			auto try_create_mat = [&](const wchar_t* tex, const char* key,
				const std::uint8_t r, const std::uint8_t c, const std::uint8_t f) -> bool
			{
				mat.hash = identity ^ 1ull;
				mat.hash = mix_cstr(mat.hash, key ? key : "");
				mat.hash = mix_u64(mat.hash, r);
				mat.hash = mix_u64(mat.hash, c);
				mat.hash = mix_u64(mat.hash, f);
				if (!mat.hash) {
					mat.hash = k_ident_tag | 1ull;
				}
				mat.albedoTexture = (tex && tex[0]) ? tex : L"";
				mat.spriteSheetRow = r;
				mat.spriteSheetCol = c;
				mat.spriteSheetFps = f;
				return api.m_bridge.CreateMaterial(&mat, &slot->mat) ==
					REMIXAPI_ERROR_CODE_SUCCESS && slot->mat;
			};

			const bool have_file = albedo && albedo[0] && file_exists_a(albedo);
			bool mat_ok = false;
			if (have_file) {
				mat_ok = try_create_mat(tex_w, albedo, rows, cols, fps);
			}
			if (!mat_ok)
			{
				if (albedo && albedo[0]) {
					shared::common::log("Particles",
						std::format("CreateMaterial textured failed ident=0x{:X} albedo={} - untextured fallback",
							identity, albedo),
						shared::common::LOG_TYPE::LOG_TYPE_WARN);
				}
				albedo = "";
				rows = 1;
				cols = 1;
				fps = 0;
				tex_w[0] = 0;
				mat_ok = try_create_mat(L"", "", 1, 1, 0);
			}
			if (!mat_ok)
			{
				shared::common::log("Particles",
					std::format("CreateMaterial failed ident=0x{:X} (textured and untextured)",
						identity),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				*slot = {};
				return false;
			}
			shared::common::log("Particles",
				std::format("CreateMaterial ident=0x{:X} albedo={} spriteSheet {}x{} @ {} fps hash=0x{:X}",
					identity, albedo ? albedo : "",
					static_cast<int>(rows), static_cast<int>(cols), static_cast<int>(fps),
					mat.hash));

			remixapi_HardcodedVertex verts[4]{};
			std::uint32_t indices[6]{};
			const float q = 0.005f;
			const auto make_v = [](const float x, const float y, const float z,
				const float u, const float v)
			{
				remixapi_HardcodedVertex vert{};
				vert.position[0] = x;
				vert.position[1] = y;
				vert.position[2] = z;
				vert.normal[0] = 0.0f;
				vert.normal[1] = 0.0f;
				vert.normal[2] = 1.0f;
				vert.texcoord[0] = u;
				vert.texcoord[1] = v;
				vert.color = 0xFFFFFFFF;
				return vert;
			};
			verts[0] = make_v(-q, -q, 0.0f, 0.0f, 0.0f);
			verts[1] = make_v(q, -q, 0.0f, 1.0f, 0.0f);
			verts[2] = make_v(-q, q, 0.0f, 0.0f, 1.0f);
			verts[3] = make_v(q, q, 0.0f, 1.0f, 1.0f);
			indices[0] = 0;
			indices[1] = 1;
			indices[2] = 3;
			indices[3] = 0;
			indices[4] = 3;
			indices[5] = 2;
			remixapi_MeshInfoSurfaceTriangles triangles{};
			triangles.vertices_values = verts;
			triangles.vertices_count = 4;
			triangles.indices_values = indices;
			triangles.indices_count = 6;
			triangles.skinning_hasvalue = FALSE;
			triangles.material = slot->mat;

			remixapi_MeshInfo mesh{};
			mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
			mesh.hash = identity ^ 2ull;
			mesh.hash = mix_cstr(mesh.hash, albedo ? albedo : "");
			mesh.hash = mix_u64(mesh.hash, rows);
			mesh.hash = mix_u64(mesh.hash, cols);
			mesh.hash = mix_u64(mesh.hash, fps);
			if (!mesh.hash) {
				mesh.hash = k_ident_tag | 2ull;
			}
			mesh.surfaces_values = &triangles;
			mesh.surfaces_count = 1;
			if (api.m_bridge.CreateMesh(&mesh, &slot->mesh) != REMIXAPI_ERROR_CODE_SUCCESS ||
				!slot->mesh)
			{
				if (api.m_bridge.DestroyMaterial) {
					api.m_bridge.DestroyMaterial(slot->mat);
				}
				*slot = {};
				shared::common::log("Particles",
					std::format("CreateMesh failed ident=0x{:X}", identity),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return false;
			}

			slot->identity = identity;
			slot->slot = src.slot;
			slot->ready = true;
			slot->sheet_rows = rows;
			slot->sheet_cols = cols;
			slot->sheet_fps = fps;
			if (albedo) {
				std::strncpy(slot->tex_key, albedo, MAX_PATH - 1);
			}
			emitter_ready = true;
			out_mesh = slot->mesh;
			shared::common::log("Particles",
				std::format("Remix emitter mesh+material sprite sheet ident=0x{:X} albedo={} "
					"rows={} cols={} fps={}",
					identity, albedo ? albedo : "",
					static_cast<int>(rows), static_cast<int>(cols), static_cast<int>(fps)));
			return true;
		}

		void submit_remix(const captured_particle* parts, const int n)
		{
			auto& api = shared::common::remix_api::get();
			if (!camera::remix_api_init_allowed() || !api.is_initialized() ||
				!api.m_bridge.DrawInstance)
			{
				return;
			}
			if (n <= 0) {
				return;
			}

			for (int i = 0; i < n; i++)
			{
				const auto& src = parts[i];
				remixapi_MeshHandle mesh = nullptr;
				if (!ensure_emitter_for(src, mesh) || !mesh) {
					continue;
				}
				D3DXVECTOR3 x{ 1.0f, 0.0f, 0.0f };
				D3DXVECTOR3 y{ 0.0f, 1.0f, 0.0f };
				D3DXVECTOR3 z{ 0.0f, 0.0f, 1.0f };
				quat_to_basis(src.quat, x, y, z);

				// Plugin quat +0x488 as world columns, then RH Rx(+90°) about
				// local X: x′=x, y′=−z, z′=y. Remix emits along instance +Z.
				remixapi_Transform xf{};
				xf.matrix[0][0] = x.x;
				xf.matrix[1][0] = x.y;
				xf.matrix[2][0] = x.z;
				xf.matrix[0][1] = -z.x;
				xf.matrix[1][1] = -z.y;
				xf.matrix[2][1] = -z.z;
				xf.matrix[0][2] = y.x;
				xf.matrix[1][2] = y.y;
				xf.matrix[2][2] = y.z;
				xf.matrix[0][3] = src.pos.x;
				xf.matrix[1][3] = src.pos.y;
				xf.matrix[2][3] = src.pos.z;

				remix_ext_params ext{};
				fill_ext_from_particle(src, ext);
				const float live_grav = ext.gravity_force;
				bool used_overlay = false;
				{
					std::lock_guard lock(g_store_mu);
					const auto overlay_id = identity_of_plugin(src.plugin_ptr, nullptr);
					const auto id = overlay_id ? overlay_id : src.identity;
					const std::string key = section_key_for_unlocked(id);
					auto it = g_ext.find(key);
					if (it != g_ext.end() && it->second.has_override) {
						ext = it->second;
						used_overlay = true;
					}
				}
				if (!used_overlay) {
					fill_ext_from_particle(src, ext);
				}
				else if (!ext.grav_override) {
					ext.gravity_force = live_grav;
				}
				ext.collide = src.collide;
				if (!used_overlay)
				{
					if (src.frequency > 0.01f) {
						ext.spawn_rate = src.frequency;
					}
					else if (ext.spawn_rate < 0.01f) {
						ext.spawn_rate = 20.0f;
					}
					if (ext.min_ttl < 0.01f) {
						ext.min_ttl = src.lifetime;
					}
					if (ext.max_ttl < 0.01f) {
						ext.max_ttl = src.lifetime;
					}
				}
				else
				{
					if (ext.spawn_rate < 0.01f && src.frequency > 0.01f) {
						ext.spawn_rate = src.frequency;
					}
					if (ext.min_ttl < 0.01f) {
						ext.min_ttl = src.lifetime;
					}
					if (ext.max_ttl < 0.01f) {
						ext.max_ttl = src.lifetime;
					}
				}
				if (src.scale_final > 0.001f &&
					(ext.min_target_size < 0.5f || ext.max_target_size < 0.5f))
				{
					float s0 = 14.0f;
					float s1 = 20.0f;
					map_remix_spawn_target_cm(src.scale_init, src.scale_final, s0, s1);
					if (ext.min_target_size < 0.5f) {
						ext.min_target_size = s1;
					}
					if (ext.max_target_size < 0.5f) {
						ext.max_target_size = s1;
					}
				}
				clamp_ext_colors(ext);
				const float drag = std::clamp(std::fabs(src.air), 0.0f, 0.02f);

				remixapi_Float4D color_min[4]{
					{ ext.min_spawn_color[0], ext.min_spawn_color[1],
						ext.min_spawn_color[2], ext.min_spawn_color[3] },
					{ ext.min_spawn_color[0], ext.min_spawn_color[1],
						ext.min_spawn_color[2], ext.min_spawn_color[3] },
					{ ext.min_target_color[0], ext.min_target_color[1],
						ext.min_target_color[2], ext.min_target_color[3] },
					{ ext.min_target_color[0], ext.min_target_color[1],
						ext.min_target_color[2], ext.min_target_color[3] },
				};
				remixapi_Float4D color_max[4]{
					{ ext.max_spawn_color[0], ext.max_spawn_color[1],
						ext.max_spawn_color[2], ext.max_spawn_color[3] },
					{ ext.max_spawn_color[0], ext.max_spawn_color[1],
						ext.max_spawn_color[2], ext.max_spawn_color[3] },
					{ ext.max_target_color[0], ext.max_target_color[1],
						ext.max_target_color[2], ext.max_target_color[3] },
					{ ext.max_target_color[0], ext.max_target_color[1],
						ext.max_target_color[2], ext.max_target_color[3] },
				};
				remixapi_Float2D size_min[2]{
					{ ext.min_spawn_size, ext.min_spawn_size },
					{ ext.min_target_size, ext.min_target_size },
				};
				remixapi_Float2D size_max[2]{
					{ ext.max_spawn_size, ext.max_spawn_size },
					{ ext.max_target_size, ext.max_target_size },
				};
				float rot_min[2]{ ext.min_rot_speed, ext.min_target_rot };
				float rot_max[2]{ ext.max_rot_speed, ext.max_target_rot };
				const float vmax = ext.max_speed;
				remixapi_Float3D vel_keys[2]{
					{ vmax, vmax, vmax },
					{ vmax, vmax, vmax },
				};

				remixapi_InstanceInfoObjectPickingEXT pick{};
				pick.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_OBJECT_PICKING_EXT;
				pick.pNext = nullptr;
				pick.objectPickingValue = static_cast<std::uint32_t>(src.identity & 0xFFFFFFFFu);
				if (!pick.objectPickingValue) {
					pick.objectPickingValue = 1;
				}

				remixapi_InstanceInfoParticleSystemEXT_152 ps{};
				ps.sType = k_particle_ext_stype;
				ps.pNext = &pick;
				ps.maxNumParticles = ext.max_particles ? ext.max_particles : 2048;
				ps.useTurbulence = ext.use_turbulence ? TRUE : FALSE;
				ps.alignParticlesToVelocity = ext.align_motion ? TRUE : FALSE;
				ps.useSpawnTexcoords = ext.use_spawn_uv ? TRUE : FALSE;
				ps.enableCollisionDetection = ext.collide ? TRUE : FALSE;
				ps.enableMotionTrail = ext.motion_trail ? TRUE : FALSE;
				ps.hideEmitter = ext.hide_emitter ? TRUE : FALSE;
				ps.restrictVelocityX = FALSE;
				ps.restrictVelocityY = FALSE;
				ps.restrictVelocityZ = FALSE;
				ps.minColor = { color_min, 4 };
				ps.maxColor = { color_max, 4 };
				ps.minRotationSpeed = { rot_min, 2 };
				ps.maxRotationSpeed = { rot_max, 2 };
				ps.minSize = { size_min, 2 };
				ps.maxSize = { size_max, 2 };
				ps.maxVelocity = { vel_keys, 2 };
				ps.attractorPosition = { 0.0f, 0.0f, 0.0f };
				ps.minTimeToLive = ext.min_ttl;
				ps.maxTimeToLive = ext.max_ttl;
				ps.initialVelocityFromNormal = ext.vel_from_normal;
				ps.initialVelocityConeAngleDegrees = ext.cone_deg;
				ps.dragCoefficient = drag;
				ps.initialRotationDeviationDegrees = 0.0f;
				ps.gravityForce = ext.gravity_force;
				ps.turbulenceFrequency = ext.turb_freq;
				ps.turbulenceForce = ext.turb_force;
				ps.spawnRatePerSecond = ext.spawn_rate;
				ps.collisionThickness = ext.thickness;
				ps.collisionRestitution = ext.restitution;
				ps.motionTrailMultiplier = ext.trail_mult;
				ps.initialVelocityFromMotion = ext.vel_from_motion;
				// Screenshot-era path: 0 restarts the USD burst each DrawInstance
				// (every frame) so fountains stay continuous. A huge duration
				// can stall or disable spawn.
				ps.spawnBurstDuration = 0.0f;
				ps.attractorRadius = 0.0f;
				ps.attractorForce = 0.0f;
				ps.billboardType = ext.billboard;
				ps.spriteSheetMode = ext.sheet_mode;
				ps.collisionMode = ext.collision_mode;
				ps.randomFlipAxis = 0;

				remixapi_InstanceInfo inst{};
				inst.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
				inst.pNext = &ps;
				// Particle Emitter category + hideEmitter on the stub mesh.
				// Spawned particles keep PARTICLE_EMITTER; never HIDDEN / IGNORE.
				inst.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE_EMITTER;
				if (!src.reflect) {
					inst.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_LIGHTS;
				}
				inst.mesh = mesh;
				inst.transform = xf;
				inst.doubleSided = TRUE;
				const auto status = api.m_bridge.DrawInstance(&inst);
				if (status != REMIXAPI_ERROR_CODE_SUCCESS)
				{
					shared::common::log("Particles", std::format(
						"DrawInstance slot={} ident=0x{:X} sType={} failed code={}",
						src.slot, src.identity,
						static_cast<int>(k_particle_ext_stype), static_cast<int>(status)),
						shared::common::LOG_TYPE::LOG_TYPE_WARN);
				}
			}
		}

		void log_capture_once(const captured_particle* parts, const int n)
		{
			static int last_n = -1;
			static std::uint64_t last_ident = 0;
			static UINT last_log_frame = 0xFFFFFFFFu;
			std::uint64_t ident = static_cast<std::uint64_t>(n) + 1;
			for (int i = 0; i < n; i++) {
				ident = mix_u64(ident, parts[i].identity);
			}
			const UINT frame = shared::common::ffp_state::get().frame_count();
			const bool changed = n != last_n || ident != last_ident;
			const bool heartbeat = !changed && n > 0 &&
				(frame % 60u) == 0u && frame != last_log_frame;
			if (!changed && !heartbeat) {
				return;
			}
			last_n = n;
			last_ident = ident;
			last_log_frame = frame;
			if (changed)
			{
				shared::common::log("Particles", std::format(
					"Captured {} Particles object(s) host={} layout={} - DrawInstance+ParticleSystemEXT sType=25 {}",
					n,
					host_exe_name[0] ? host_exe_name : "unbound",
					host_layout_tag ? host_layout_tag : "-",
					emitter_ready ? "fired" : (emitter_failed ? "stub" : "pending")));
				for (int i = 0; i < n && i < 8; i++)
				{
					const auto& P = parts[i];
					shared::common::log("Particles", std::format(
						"  [{}] '{}' shown={} active={} pos=({:.3f},{:.3f},{:.3f}) "
						"rate={:.2f}/s life={:.2f}s spd={:.2f}..{:.2f} emit={:.1f}..{:.1f}deg "
						"rgb0=({:.2f},{:.2f},{:.2f}) rgb1=({:.2f},{:.2f},{:.2f}) a={:.2f} "
						"grav=({:.2f},{:.2f},{:.2f}) gravityForce={:.1f} air={:.4f} "
						"scale={:.2f}->{:.2f} remix_cm={:.2f}->{:.2f} "
						"live={} tex={} burnout={} bone={} collide={}",
						i, P.title[0] ? P.title : "Particles",
						P.shown, P.active,
						P.pos.x, P.pos.y, P.pos.z,
						P.frequency, P.lifetime, P.speed_min, P.speed_max,
						P.emit_min, P.emit_max,
						P.rgb_init.x, P.rgb_init.y, P.rgb_init.z,
						P.rgb_final.x, P.rgb_final.y, P.rgb_final.z, P.opacity,
						P.grav.x, P.grav.y, P.grav.z,
						std::clamp(P.grav.y * k_ms_to_cms, -980.0f, 980.0f),
						P.air,
						P.scale_init, P.scale_final,
						map_remix_size_cm(P.scale_init, k_spawn_vis, k_spawn_cm_floor),
						map_remix_size_cm(P.scale_final, k_target_vis, k_target_cm_floor),
						P.live_count,
						P.tex[0] ? P.tex : "-", P.burnout, P.bone,
						P.collide ? 1 : 0));
					if (std::fabs(P.grav.x) > 1.0e-4f || std::fabs(P.grav.z) > 1.0e-4f) {
						shared::common::log("Particles", std::format(
							"  [{}] wind X/Z=({:.3f},{:.3f}) ignored (Remix gravityForce is scalar Y)",
							i, P.grav.x, P.grav.z));
					}
				}
			}
			else if (heartbeat && n > 0)
			{
				shared::common::log("Particles", std::format(
					"live n={} api={}",
					n, emitter_ready ? 1 : 0));
			}
		}
	}

	void on_frame()
	{
		const UINT frame = shared::common::ffp_state::get().frame_count();
		const bool allowed = camera::scene_conversion_allowed();
		if (frame == work_frame && allowed == work_allowed) {
			return;
		}
		work_frame = frame;
		work_allowed = allowed;

		bind_project_watch();
		install_ui_hooks();
		project_file::poll();
		char ini[MAX_PATH]{};
		bool loaded = false;
		char maps_ini[MAX_PATH]{};
		{
			std::lock_guard lock(g_store_mu);
			sync_ini_path_from_project();
			loaded = g_ini_loaded;
			if (g_ini_path[0]) {
				std::strncpy(ini, g_ini_path, MAX_PATH - 1);
			}
			if (g_maps_ini[0]) {
				std::strncpy(maps_ini, g_maps_ini, MAX_PATH - 1);
			}
		}
		if (ini[0] && (!loaded || !same_ini_path(maps_ini, ini)))
		{
			migrate_legacy_ini();
			load_ini_from_path(ini);
		}
		if (!allowed)
		{
			captured = 0;
			return;
		}
		static int poll_props;
		if ((++poll_props % 15) == 0) {
			poll_all_properties_hwnds();
		}

		captured_particle found[remix_particle_cap]{};
		const int n = collect_into(found, remix_particle_cap);
		captured = n;
		g_last_n = n;
		for (int i = 0; i < n; i++) {
			g_last_parts[i] = found[i];
		}
		submit_remix(found, n);
		log_capture_once(found, n);
	}

	int captured_count()
	{
		return captured;
	}

	void prepare_reset()
	{
		// Device still alive — DestroyMesh here, not after Reset (CreateLight AV).
		destroy_emitter_unlocked();
		emitter_failed = false;
	}

	void reset()
	{
		captured = 0;
		work_frame = 0xFFFFFFFFu;
		work_allowed = false;
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
		for (auto& e : emitters) {
			e = {};
		}
		emitter_ready = false;
		emitter_tex_w[0] = 0;
		{
			std::lock_guard lock(g_store_mu);
			g_hwnd_plugin.clear();
			g_hwnd_host.clear();
			g_ident_oid.clear();
			g_id_section.clear();
			g_plugin_oid.clear();
			g_sync_host_count = -1;
			g_sync_list_count = -1;
			g_list_oid_fp = 0;
			std::memset(g_slot_oid, 0xFF, sizeof(g_slot_oid));
			g_bound_ini[0] = 0;
			g_bound_plugin_n = -1;
			g_bound_applied = -1;
			g_maps_ini[0] = 0;
			g_ini_loaded = false;
			g_loaded_ok = false;
			g_loaded_stem[0] = 0;
			g_allow_empty_persist = false;
			g_host_ready_for_stem = false;
		}
	}

	void install_ui_hooks()
	{
		if (shared::globals::skip_remix || !shared::globals::is_editor_host) {
			return;
		}
		bind_project_watch();
		project_file::install_hooks();
		ensure_dialog_thread_hooks();
		g_ui_hooks = true;
	}

	DLGPROC chain_dlgproc(DLGPROC orig, LPARAM lp)
	{
		if (shared::globals::skip_remix) {
			return orig;
		}
		ensure_dialog_thread_hooks();
		LPARAM bind_lp = lp;
		if (!bind_lp)
		{
			const void* host = nullptr;
			const void* plugin = nullptr;
			if (capture_list_row_plugin(host, plugin, nullptr) && plugin) {
				bind_lp = reinterpret_cast<LPARAM>(const_cast<void*>(plugin));
			}
		}
		t_dlg_stack.push_back({ orig, bind_lp });
		return chained_dlgproc;
	}

	void unchain_dlgproc()
	{
		if (!t_dlg_stack.empty()) {
			t_dlg_stack.pop_back();
		}
	}

	void note_dialog_created(HWND hwnd)
	{
		bind_and_inject_dialog(hwnd, current_dlg_lp());
	}

	void forget_properties_hwnd(HWND dlg)
	{
		if (!dlg) {
			return;
		}
		std::lock_guard lock(g_store_mu);
		forget_hwnd_unlocked(dlg);
	}

	void poll_properties_dialog()
	{
		poll_all_properties_hwnds();
	}

	void note_project_file(const char* path)
	{
		bind_project_watch();
		project_file::note_path(path);
	}

	void note_project_file_w(const wchar_t* path)
	{
		bind_project_watch();
		project_file::note_path_w(path);
	}

	const char* project_ini_path()
	{
		bind_project_watch();
		project_file::poll();
		sync_ini_path_from_project();
		return g_ini_path[0] ? g_ini_path : nullptr;
	}

	const char* current_project_stem()
	{
		return project_file::stem();
	}

	const char* scene_folder()
	{
		return project_file::scene_folder();
	}

	void reload_project_ini()
	{
		bind_project_watch();
		project_file::poll();
		drop_overrides_and_load();
	}

	void save_project_ini()
	{
		if (!persist_ini_allowed()) {
			return;
		}
		sync_live_plugins_with_ini(false);
		std::lock_guard lock(g_store_mu);
		if (g_maps_ini[0]) {
			std::strncpy(g_ini_path, g_maps_ini, MAX_PATH - 1);
		}
		else
		{
			sync_ini_path_from_project();
			if (g_ini_path[0]) {
				claim_maps_ini_unlocked(g_ini_path);
				g_ini_loaded = true;
			}
		}
		write_ini_unlocked();
	}

	void prepare_properties_ini(std::uint64_t identity)
	{
		if (!editor_ini_allowed() || !identity) {
			return;
		}
		bind_project_watch();
		project_file::poll();
		sync_live_plugins_with_ini(true);
		char path[MAX_PATH]{};
		{
			std::lock_guard lock(g_store_mu);
			sync_ini_path_from_project();
			if (!g_ini_path[0])
			{
				shared::common::log("Particles",
					"properties ini skipped - no project .3dr path (caption/CreateFile)",
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				return;
			}
			if (!g_maps_ini[0]) {
				claim_maps_ini_unlocked(g_ini_path);
				g_ini_loaded = true;
			}
			if (!maps_belong_to_current_ini_unlocked()) {
				return;
			}
			std::strncpy(path, g_ini_path, MAX_PATH - 1);
		}
		if (file_exists_a(path)) {
			load_identity_section_from_disk(identity, path);
		}
	}

	bool collide_enabled(std::uint64_t identity)
	{
		return collide_of_id(identity);
	}

	void set_collide(std::uint64_t identity, bool on)
	{
		set_collide_id(identity, on);
	}

	std::uint64_t identity_for_properties_dialog(HWND dlg)
	{
		const std::uint64_t identity = identity_from_stashed_dlg(dlg);
		if (!identity)
		{
			shared::common::log("Particles",
				std::format("properties bind miss hwnd=0x{:X} lParam=0x{:X} dwlp=0x{:X} userdata=0x{:X}",
					reinterpret_cast<std::uintptr_t>(dlg),
					reinterpret_cast<std::uintptr_t>(GetPropA(dlg, "vrePDlgArg")),
					static_cast<std::uintptr_t>(GetWindowLongPtrA(dlg, DWLP_USER)),
					static_cast<std::uintptr_t>(GetWindowLongPtrA(dlg, GWLP_USERDATA))),
				shared::common::LOG_TYPE::LOG_TYPE_WARN);
			return 0;
		}
		register_section_id(identity, "Particles");
		const void* host = GetPropA(dlg, "vrePHost");
		const void* plugin = GetPropA(dlg, "vrePPlug");
		{
			std::lock_guard lock(g_store_mu);
			const auto ip = g_hwnd_plugin.find(dlg);
			if (ip != g_hwnd_plugin.end() && ip->second) {
				plugin = ip->second;
			}
			const auto ih = g_hwnd_host.find(dlg);
			if (ih != g_hwnd_host.end() && ih->second) {
				host = ih->second;
			}
		}
		maybe_sync_live_plugins(false);
		const int count = ensure_host_count();
		int slot = (count >= 0) ? slot_of_host(host, count) : -1;
		if (slot < 0 && count >= 0) {
			slot = slot_of_plugin(plugin, count);
		}
		int oid = -1;
		if (slot >= 0 && slot < host_max_objects) {
			oid = g_slot_oid[slot];
		}
		{
			std::lock_guard lock(g_store_mu);
			if (oid >= 0) {
				bind_identity_object_id_unlocked(identity, oid);
			}
		}
		const auto prev = GetPropA(dlg, "vrePBind");
		if (prev != plugin)
		{
			SetPropA(dlg, "vrePBind", const_cast<void*>(plugin));
			shared::common::log("Particles",
				std::format("properties dlg hwnd=0x{:X} host=0x{:X} plugin=0x{:X} "
					"section={} ident=0x{:X} objectId={}",
					reinterpret_cast<std::uintptr_t>(dlg),
					reinterpret_cast<std::uintptr_t>(host),
					reinterpret_cast<std::uintptr_t>(plugin),
					oid >= 0 ? section_from_object_id(oid) : section_id_from_identity(identity),
					identity, oid));
		}
		return identity;
	}

	void register_plugin_section(std::uint64_t identity, int /*slot*/, const char* name)
	{
		register_section_id(identity, name);
	}

	bool remix_ext_of(std::uint64_t identity, remix_ext_params& out)
	{
		if (!identity) {
			return false;
		}
		std::lock_guard lock(g_store_mu);
		const std::string key = section_key_for_unlocked(identity);
		auto it = g_ext.find(key);
		if (it == g_ext.end()) {
			return false;
		}
		out = it->second;
		return true;
	}

	bool load_remix_for_identity(std::uint64_t identity, remix_ext_params& out)
	{
		out = remix_ext_params{};
		if (!identity) {
			return false;
		}
		char path[MAX_PATH]{};
		bool loaded = false;
		{
			std::lock_guard lock(g_store_mu);
			sync_ini_path_from_project();
			loaded = g_ini_loaded;
			if (g_ini_path[0]) {
				std::strncpy(path, g_ini_path, MAX_PATH - 1);
			}
		}
		if (!loaded && path[0]) {
			migrate_legacy_ini();
			load_ini_from_path(path);
		}
		{
			std::lock_guard lock(g_store_mu);
			const std::string key = section_key_for_unlocked(identity);
			auto it = g_ext.find(key);
			if (it != g_ext.end() && it->second.has_override) {
				out = it->second;
				return true;
			}
			if (g_ini_path[0])
			{
				remix_ext_params disk{};
				load_ext_section(key.c_str(), g_ini_path, disk);
				if (disk.has_override)
				{
					g_ext[key] = disk;
					g_collide[key] = disk.collide;
					out = disk;
					return true;
				}
			}
		}
		remix_defaults_for(identity, out);
		out.collide = collide_of_id(identity);
		return false;
	}

	void set_remix_ext(std::uint64_t identity, const remix_ext_params& p)
	{
		if (!identity) {
			return;
		}
		std::lock_guard lock(g_store_mu);
		const std::string key = section_key_for_unlocked(identity);
		g_ext[key] = p;
		g_collide[key] = p.collide;
		if (!g_maps_ini[0] && g_ini_path[0]) {
			claim_maps_ini_unlocked(g_ini_path);
			g_ini_loaded = true;
		}
	}

	void remix_defaults_for(std::uint64_t identity, remix_ext_params& out)
	{
		out = remix_ext_params{};
		if (!identity) {
			return;
		}
		for (int i = 0; i < g_last_n; i++)
		{
			if (g_last_parts[i].identity == identity)
			{
				fill_ext_from_particle(g_last_parts[i], out);
				return;
			}
		}
	}

	const char* particle_animation_dir()
	{
		static char dir[MAX_PATH]{};
		sprintf_s(dir, "%s\\3DRad_res\\objects\\Particles\\data\\animation",
			shared::globals::host_data_root().c_str());
		return dir;
	}

	bool apply_animation_frames(std::uint64_t identity, const char* const* paths, int n)
	{
		if (!identity || !paths || n <= 0) {
			return false;
		}
		char seq[16][MAX_PATH]{};
		int m = 0;
		for (int i = 0; i < n && m < 16; i++)
		{
			if (!paths[i] || !paths[i][0]) {
				continue;
			}
			std::strncpy(seq[m++], paths[i], MAX_PATH - 1);
		}
		if (m <= 0) {
			return false;
		}
		qsort(seq, static_cast<std::size_t>(m), MAX_PATH,
			[](const void* a, const void* b) -> int
			{
				return _stricmp(static_cast<const char*>(a), static_cast<const char*>(b));
			});

		char stem[80]{};
		{
			const char* leaf = seq[0];
			for (const char* p = seq[0]; *p; p++) {
				if (*p == '\\' || *p == '/') {
					leaf = p + 1;
				}
			}
			std::strncpy(stem, leaf, 79);
			char* dot = std::strrchr(stem, '.');
			if (dot) {
				*dot = 0;
			}
			const std::size_t len = std::strlen(stem);
			if (len > 5)
			{
				char* us = stem + len;
				while (us > stem && us[-1] >= '0' && us[-1] <= '9') {
					--us;
				}
				if (us > stem && (us[-1] == '_' || us[-1] == '-') && us < stem + len)
				{
					us[-1] = 0;
				}
			}
			if (!stem[0]) {
				std::strncpy(stem, "custom", 79);
			}
			else
			{
				bool digits_only = true;
				for (const char* c = stem; *c; ++c)
				{
					if (*c < '0' || *c > '9') {
						digits_only = false;
						break;
					}
				}
				if (digits_only)
				{
					const char* slash = nullptr;
					for (const char* q = seq[0]; *q; ++q) {
						if (*q == '\\' || *q == '/') {
							slash = q;
						}
					}
					if (slash)
					{
						const char* start = seq[0];
						for (const char* q = seq[0]; q < slash; ++q) {
							if (*q == '\\' || *q == '/') {
								start = q + 1;
							}
						}
						char parent[80]{};
						const std::size_t pn = static_cast<std::size_t>(slash - start);
						if (pn > 0 && pn < 79)
						{
							std::memcpy(parent, start, pn);
							parent[pn] = 0;
							std::strncpy(stem, parent, 79);
						}
					}
					if (!stem[0] || (stem[0] >= '0' && stem[0] <= '9')) {
						std::strncpy(stem, "custom", 79);
					}
				}
			}
		}

		remix_ext_params p{};
		load_remix_for_identity(identity, p);
		p.has_override = true;
		std::strncpy(p.animation, stem, 79);
		p.animation_files[0] = 0;
		for (int i = 0; i < m; i++)
		{
			const char* leaf = seq[i];
			for (const char* q = seq[i]; *q; q++) {
				if (*q == '\\' || *q == '/') {
					leaf = q + 1;
				}
			}
			if (p.animation_files[0]) {
				std::strncat(p.animation_files, "|", 511);
			}
			std::strncat(p.animation_files, leaf, 511);
		}

		char sheet_dir[MAX_PATH]{};
		host_particle_sheet_dir(sheet_dir, MAX_PATH);
		char packed[MAX_PATH]{};
		const auto lo = static_cast<std::uint32_t>(identity & 0xFFFFFFFFu);
		if (m == 1)
		{
			std::strncpy(p.sheet_file, seq[0], 259);
			p.sheet_rows = 1;
			p.sheet_cols = 1;
			if (!p.sheet_fps) {
				p.sheet_fps = 0;
			}
		}
		else
		{
			std::snprintf(packed, MAX_PATH, "%sp%08X_%s_r1c%d.dds",
				sheet_dir, lo, stem, m);
			if (!pack_sequence_sheet(seq, m, packed))
			{
				shared::common::log("Particles",
					std::format("pack sprite sheet failed ident=0x{:X} n={} - using first frame",
						identity, m),
					shared::common::LOG_TYPE::LOG_TYPE_WARN);
				std::strncpy(p.sheet_file, seq[0], 259);
				p.sheet_rows = 1;
				p.sheet_cols = 1;
			}
			else
			{
				std::strncpy(p.sheet_file, packed, 259);
				p.sheet_rows = 1;
				p.sheet_cols = static_cast<std::uint8_t>(m);
				if (!p.sheet_fps) {
					p.sheet_fps = 10;
				}
			}
		}
		p.sheet_mode = 0;
		set_remix_ext(identity, p);
		g_pending_invalidate = identity;
		save_project_ini();
		reload_remix_pane_if_open(identity);
		shared::common::log("Particles",
			std::format("animation sequence ident=0x{:X} stem={} frames={} sheet={} "
				"rows={} cols={} fps={}",
				identity, stem, m, p.sheet_file,
				static_cast<int>(p.sheet_rows), static_cast<int>(p.sheet_cols),
				static_cast<int>(p.sheet_fps)));
		return true;
	}

	void sync_mapped_from_properties_dialog(HWND dlg, bool force_save)
	{
		if (!is_particles_properties_dialog(dlg)) {
			return;
		}
		const std::uint64_t id = identity_for_properties_dialog(dlg);
		if (!id) {
			return;
		}
		const DWORD now = GetTickCount();
		const HANDLE tick_prev = GetPropA(dlg, "vrePTick");
		const DWORD last = tick_prev ? static_cast<DWORD>(reinterpret_cast<UINT_PTR>(tick_prev)) : 0;
		if (!force_save && last && now - last < 120) {
			return;
		}
		SetPropA(dlg, "vrePTick", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(now ? now : 1)));

		bool collide = false;
		HWND chk = GetDlgItem(dlg, 0x7E80);
		if (chk) {
			collide = SendMessageA(chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
		}

		remix_ext_params prev{};
		const bool had = remix_ext_of(id, prev) && prev.has_override;
		if (had)
		{
			if (prev.collide != collide)
			{
				prev.collide = collide;
				set_remix_ext(id, prev);
			}
			set_collide(id, collide);
			if (force_save) {
				save_project_ini();
			}
			return;
		}
		set_collide(id, collide);
		if (force_save) {
			save_project_ini();
		}
	}
}
