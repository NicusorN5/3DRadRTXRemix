#pragma once

namespace comp::project_file
{
	// Loaded 3D Rad project. Stem is whatever .3dr was just opened — never
	// hardcoded. Editor: `{scene_folder}\{stem}.ini`. Autoload (editor host
	// only) uses the same bind as File-Open: command-line .3dr, lastProject.txt
	// / windowed.ini, caption stem under 3DRad_res\projects, then CreateFile
	// remember. Compiler skip Remix. Compiled player:
	// `{exe}\3DRad_res\projects\{stem}.ini` (same layout as the editor, local
	// to that compiled build). Stem comes from lastProject / CreateFile /
	// lone .3dr/.ini in that folder — never a hardcoded project name and
	// never C:\\3DRadRTX\\3DRad_res\\projects.
	const char* stem();
	const char* scene_folder();
	const char* ini_path();

	void note_path(const char* path);
	void note_path_w(const wchar_t* path);
	void poll();
	void install_hooks();

	// before: still on the previous project (save A.ini).
	// after: stem/folder/ini_path already point at B (drop A maps in memory,
	// load B.ini). Never write A or B during that drop.
	void set_on_before_switch(void (*cb)());
	void set_on_after_switch(void (*cb)());
}
