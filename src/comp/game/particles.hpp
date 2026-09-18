#pragma once

#include <cstdint>

namespace comp::game::particles
{
	// 3D Rad Particles v1.16 plugin → Remix 1.5.2 DrawInstance + ParticleSystemEXT.
	// Emission = quat +0x488 world basis, then one RH Rx(+90°) about local X
	// (x′=x, y′=−z, z′=y). Gravity GET 0x40A–0x40C → +0x570/574/578,
	// Y×100 → gravityForce. Lifetime GET 0x40E → +0x598. Speed GET 0x40F/0x410
	// → +0x59C/+0x5A0 (not TTL). Launch = avg(min,max) × 0.5 × 100 cm/s.
	// Colors +0x588/+0x594. Scale +0x580/+0x584 → ~14/20 cm. hideEmitter.
	// sType 25. No CreateLight. Per-object `{scene}\{stem}.ini` from the
	// loaded .3dr basename (any project). Timer GET 0x412 → +0xE34 is emit
	// window; Lifetime is TTL.

	struct remix_ext_params
	{
		bool has_override = false;
		float min_spawn_color[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
		float max_spawn_color[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
		float min_rot_speed = 0.0f;
		float max_rot_speed = 0.0f;
		float min_spawn_size = 14.0f;
		float max_spawn_size = 14.0f;
		float min_ttl = 1.0f;
		float max_ttl = 1.0f;
		bool hide_emitter = true;
		float cone_deg = 0.0f;
		float vel_from_motion = 0.0f;
		float vel_from_normal = 0.0f;
		std::uint32_t max_particles = 2048;
		float spawn_rate = 20.0f;
		bool use_spawn_uv = false;
		float min_target_color[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
		float max_target_color[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
		float min_target_rot = 0.0f;
		float max_target_rot = 0.0f;
		float min_target_size = 20.0f;
		float max_target_size = 20.0f;
		bool align_motion = false;
		std::uint8_t billboard = 0;
		bool motion_trail = false;
		float trail_mult = 1.0f;
		float restitution = 0.5f;
		float thickness = 5.0f;
		bool collide = false;
		float gravity_force = 0.0f;
		bool grav_override = false;
		float max_speed = 0.0f;
		float turb_force = 0.0f;
		float turb_freq = 0.05f;
		bool use_turbulence = false;
		std::uint8_t sheet_rows = 0;
		std::uint8_t sheet_cols = 0;
		std::uint8_t sheet_fps = 0;
		std::uint8_t sheet_mode = 0;
		std::uint8_t collision_mode = 0;
		char animation[80]{};
		char animation_files[512]{};
		char sheet_file[260]{};
	};

	void on_frame();
	int captured_count();
	void prepare_reset();
	void reset();

	void install_ui_hooks();
	DLGPROC chain_dlgproc(DLGPROC orig, LPARAM lp = 0);
	void unchain_dlgproc();
	void note_dialog_created(HWND hwnd);
	void forget_properties_hwnd(HWND dlg);
	void poll_properties_dialog();

	void note_project_file(const char* path);
	void note_project_file_w(const wchar_t* path);
	const char* project_ini_path();
	const char* current_project_stem();
	const char* scene_folder();
	void reload_project_ini();
	void save_project_ini();
	void prepare_properties_ini(std::uint64_t identity);

	bool collide_enabled(std::uint64_t identity);
	void set_collide(std::uint64_t identity, bool on);
	std::uint64_t identity_for_properties_dialog(HWND dlg);
	void register_plugin_section(std::uint64_t identity, int slot, const char* name);

	bool remix_ext_of(std::uint64_t identity, remix_ext_params& out);
	bool load_remix_for_identity(std::uint64_t identity, remix_ext_params& out);
	void set_remix_ext(std::uint64_t identity, const remix_ext_params& p);
	void remix_defaults_for(std::uint64_t identity, remix_ext_params& out);
	bool apply_animation_frames(std::uint64_t identity, const char* const* paths, int n);
	const char* particle_animation_dir();

	void sync_mapped_from_properties_dialog(HWND dlg, bool force_save = true);
	void reload_remix_pane_if_open(std::uint64_t identity);

	void open_remix_pane(HWND owner, std::uint64_t identity);
	bool is_remix_pane_hwnd(HWND hwnd);
	bool filter_remix_pane_message(MSG* msg);
}
