#pragma once

namespace comp
{
	void on_begin_scene_cb();
	void main();
	bool start_game_hooks(); // MinHook + window thread. Remix mode only.
}
