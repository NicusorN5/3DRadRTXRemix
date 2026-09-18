#pragma once

namespace comp::game::fog
{
	// 3D Rad Fog v1.03 plugin → Remix legacy fog remap + D3D9 FFP fog.
	// Host list same as PointLight / Particles. Identity is the plugin
	// pointer (not host slot). Compiler skips. No rtx.conf rewrite.

	void on_frame();
	int captured_count();
	void reset();
	void on_project_before();
	void on_project_after();
}
