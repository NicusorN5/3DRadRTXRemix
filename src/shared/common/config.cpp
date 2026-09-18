#include "std_include.hpp"
#include "config.hpp"
#include "../globals.hpp"

namespace shared::common
{
	config& config::get()
	{
		static config instance;
		return instance;
	}

	void config::load(const std::string& path)
	{
		ini_path_ = path;

		// Check if the INI file actually exists — GetPrivateProfileInt silently
		// returns defaults for missing files, making it look like settings are ignored.
		if (GetFileAttributesA(ini_path_.c_str()) == INVALID_FILE_ATTRIBUTES)
		{
			log("Config", std::format("INI NOT FOUND: {} — using all defaults!", ini_path_),
				LOG_TYPE::LOG_TYPE_ERROR, true);
			loaded_ = false;
			return;
		}

		loaded_ = true;
		parse_all();
	}

	void config::save_launch_backend(const char* backend)
	{
		std::string path = ini_path_;
		if (path.empty()) {
			path = shared::globals::root_path + "\\remix-comp-proxy.ini";
		}
		if (!backend || !backend[0]) {
			backend = "remix";
		}
		WritePrivateProfileStringA("Launch", "Backend", backend, path.c_str());
	}

	int config::get_int(const char* section, const char* key, int default_val) const
	{
		if (!loaded_) return default_val;
		return GetPrivateProfileIntA(section, key, default_val, ini_path_.c_str());
	}

	std::string config::get_string(const char* section, const char* key, const char* default_val) const
	{
		if (!loaded_) return default_val;
		char buf[512];
		GetPrivateProfileStringA(section, key, default_val, buf, sizeof(buf), ini_path_.c_str());
		return buf;
	}

	float config::get_float(const char* section, const char* key, float default_val) const
	{
		auto str = get_string(section, key, "");
		if (str.empty()) return default_val;
		try { return std::stof(str); }
		catch (...) { return default_val; }
	}

	bool config::get_bool(const char* section, const char* key, bool default_val) const
	{
		return get_int(section, key, default_val ? 1 : 0) != 0;
	}

	void config::parse_all()
	{
		// [Launch] — editor dialog preselect (3DRad.exe only)
		shared::globals::editor_backend =
			shared::globals::parse_launch_backend(get_string("Launch", "Backend", "remix").c_str());

		// [Remix]
		remix.enabled = get_bool("Remix", "Enabled", true);
		remix.dll_name = get_string("Remix", "DLLName", "d3d9_remix.dll");

		// [Chain]
		chain.preload = get_string("Chain", "PreLoad", "");
		chain.postload = get_string("Chain", "PostLoad", "");

		// [FFP]
		ffp.enabled = get_bool("FFP", "Enabled", true);
		ffp.albedo_stage = get_int("FFP", "AlbedoStage", 0);
		if (ffp.albedo_stage < 0 || ffp.albedo_stage > 7)
			ffp.albedo_stage = 0;

		// [Camera]
		camera.world_space_position = get_bool("Camera", "WorldSpacePosition", true);

		// [Skinning]
		skinning.enabled = get_bool("Skinning", "Enabled", false);

		// [Diagnostics]
		diagnostics.enabled = get_bool("Diagnostics", "Enabled", true);
		diagnostics.auto_capture = get_bool("Diagnostics", "AutoCapture", true);
		diagnostics.delay_ms = get_int("Diagnostics", "DelayMs", 50000);
		diagnostics.log_frames = get_int("Diagnostics", "LogFrames", 3);
		diagnostics.log_draw_calls = get_bool("Diagnostics", "LogDrawCalls", true);
		diagnostics.log_vs_constants = get_bool("Diagnostics", "LogVSConstants", true);
		diagnostics.log_vertex_data = get_bool("Diagnostics", "LogVertexData", true);
		diagnostics.log_declarations = get_bool("Diagnostics", "LogDeclarations", true);
		diagnostics.log_textures = get_bool("Diagnostics", "LogTextures", true);
		diagnostics.log_present_info = get_bool("Diagnostics", "LogPresentInfo", true);

		// [Tracer]
		tracer.backtrace_depth = get_int("Tracer", "BacktraceDepth", 8);
		tracer.output_dir = get_string("Tracer", "OutputDir", "captures");

		// [Video] — editor SetViewport inject (3DRad.exe)
		// Width/Height 0 = follow ChildClass (with MatchWindow=1).
		video.match_window = get_bool("Video", "MatchWindow", true);
		video.width = get_int("Video", "Width", 0);
		video.height = get_int("Video", "Height", 0);
		video.scale = get_int("Video", "Scale", 100);

		// [UI] — MFC font + system\\ui / buttons.dds scale + object list
		ui.scale = get_int("UI", "Scale", 100);
		ui.hud_scale = get_int("UI", "HudScale", 100);
		ui.font_size = get_int("UI", "FontSize", 13);
		ui.font_name = get_string("UI", "FontName", "Segoe UI");
		ui.list_width = get_int("UI", "ListWidth", 0);
		ui.row_height = get_int("UI", "RowHeight", 0);
		ui.check_size = get_int("UI", "CheckSize", 0);
		ui.show_object_ids = get_bool("UI", "ShowObjectIds", true);

		log("Config", std::format("Loaded from: {}", ini_path_));
		log("Config", std::format(
			"Video MatchWindow={} {}x{} Scale={} | UI Scale={} HudScale={} FontSize={} FontName={} "
			"ListWidth={} RowHeight={} CheckSize={} ShowObjectIds={}",
			video.match_window ? 1 : 0, video.width, video.height, video.scale,
			ui.scale, ui.hud_scale, ui.font_size, ui.font_name,
			ui.list_width, ui.row_height, ui.check_size, ui.show_object_ids ? 1 : 0));
		log("Config", std::format("Launch.Backend={} FFP={} AlbedoStage={} WorldSpaceCamera={}",
			shared::globals::launch_backend_name(shared::globals::editor_backend),
			ffp.enabled ? 1 : 0, ffp.albedo_stage, camera.world_space_position ? 1 : 0));
		if (skinning.enabled)
			log("Config", "Skinning ENABLED", LOG_TYPE::LOG_TYPE_WARN);
	}
}
