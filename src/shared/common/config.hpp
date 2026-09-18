#pragma once

namespace shared::common
{
	class config
	{
	public:
		static config& get();

		void load(const std::string& ini_path);
		void save_launch_backend(const char* backend);
		const std::string& ini_file() const { return ini_path_; }
		bool is_loaded() const { return loaded_; }

		int get_int(const char* section, const char* key, int default_val) const;
		std::string get_string(const char* section, const char* key, const char* default_val) const;
		float get_float(const char* section, const char* key, float default_val) const;
		bool get_bool(const char* section, const char* key, bool default_val) const;

		struct ffp_settings
		{
			bool enabled = true;
			int albedo_stage = 0;
		} ffp;

		// Read the shown camera's world position from iCameraLocation (object+0x50)
		// and move it out of the per-object worlds into the view Remix sees.
		struct camera_settings
		{
			bool world_space_position = true;
		} camera;

		struct skinning_settings
		{
			bool enabled = false;
		} skinning;

		struct diagnostics_settings
		{
			bool enabled = true;
			bool auto_capture = true;
			int delay_ms = 50000;
			int log_frames = 3;

			// Log categories (defaults, overridable from ImGui at runtime)
			bool log_draw_calls = true;
			bool log_vs_constants = true;
			bool log_vertex_data = true;
			bool log_declarations = true;
			bool log_textures = true;
			bool log_present_info = true;
		} diagnostics;

		struct remix_settings
		{
			bool enabled = true;
			std::string dll_name = "d3d9_remix.dll";
		} remix;

		struct chain_settings
		{
			std::string preload;   // semicolon-separated DLLs/ASIs loaded before d3d9 chain
			std::string postload;  // semicolon-separated DLLs/ASIs loaded after init
		} chain;

		struct tracer_settings
		{
			int backtrace_depth = 8;
			std::string output_dir = "captures";
		} tracer;

		struct video_settings
		{
			bool match_window = true;
			int width = 0;
			int height = 0;
			int scale = 100;
		} video;

		struct ui_settings
		{
			int scale = 100;
			int hud_scale = 100;
			int font_size = 13;
			std::string font_name = "Segoe UI";
			int list_width = 0;   // 0 = stock left pane
			int row_height = 0;   // 0 = stock
			int check_size = 0;   // 0 = stock state-image / checkbox
			bool show_object_ids = true; // object-list "00032Particles" prefix
		} ui;

	private:
		std::string ini_path_;
		bool loaded_ = false;

		void parse_all();
	};
}
