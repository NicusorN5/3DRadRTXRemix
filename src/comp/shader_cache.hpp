#pragma once

namespace comp::shader_cache
{
	struct stats
	{
		int deleted = 0;
		int failed = 0;
		int dirs_removed = 0;
	};

	// rundll32.exe host — DllMain must not treat this as a compiled player.
	bool host_is_wipe_helper();

	// In-process pass. Does not spawn a helper. Leaves D3DSCache / .glslfx /
	// d3d9_dxvk.dll alone.
	stats wipe_now(const wchar_t* install_root);

	// Hidden rundll32 of THIS d3d9.dll. Waits for pid, retries wipe, relaunches
	// exe from cwd. No cmd.exe / findstr. No --rtx-comp-warmed.
	bool spawn_post_exit_helper(DWORD pid, const wchar_t* relaunch_exe,
		const wchar_t* cwd, const wchar_t* install_root);

	// rundll32 entry (d3d9.def). Not for game hosts.
	void run_exported_wipe(const char* cmd);
}
