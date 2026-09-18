#pragma once

namespace comp::game::lights
{
	// 3Impact never calls D3D SetLight. PointLight / SunLight are plugins
	// (object.dll) on the HOST exe list — 3DRad.exe in the editor,
	// 3drad_player.exe (renamed scary.exe / client.exe) when compiled.
	// Same host+0 / +0x291A object, different BSS VAs. iLightLocal* BSS is
	// only the SkinMesh 3-slot upload (empty until a slot index is assigned).
	//
	// Once per D3D frame (first BeginScene, Present is the fence if no
	// scene ran): re-read plugins still in the host list, resolve WORLD pos
	// from local +0x494 via host parent (same cadence as camera C +0x50).
	// Cubemap / blit BeginScenes reuse that capture — do not walk the host
	// list or SetLight 6–7 times in one Present. Keep last-good color /
	// range / pos so scripted lights do not vanish when shown/RGB briefly
	// hits 0 — never store insane coords. SetLight every frame (legacy /
	// below-Ultra). Remix API
	// CreateLight is off: it AVed the bridge on Reset and dropped lights
	// when garbage gizmo pos reached DrawLightInstance. SetLight-only
	// matches the working overlay (Sphere from D3DLIGHT_POINT, Distant
	// from directional). Reconsider API shapes only after positions are
	// sane, remix_api_init_allowed(), and never on Reset teardown.
	//
	// Object title containing sphere / rectangle / disk / cylinder selects
	// a shape token for a future API path. Default is sphere. Ignore
	// two-letter blob false positives ('ht'). Emitter radius is constant;
	// range factor is radiance / attenuation, not sphere radius.
	//
	// Fallback directional in ffp_state runs only when this frame captured
	// zero shown lights.

	void on_frame();
	int captured_count();
	int captured_suns();
	int captured_points();
	void reset();
}
