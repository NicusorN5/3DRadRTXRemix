#include "std_include.hpp"
#include "ffp_state.hpp"

namespace shared::common
{
	ffp_state& ffp_state::get()
	{
		static ffp_state instance;
		return instance;
	}

	void ffp_state::init(IDirect3DDevice9* /*real_device*/)
	{
		cfg_ = &config::get().ffp;
		enabled_ = cfg_->enabled;
		create_tick_ = GetTickCount();

		log("FFP", std::format("State tracker initialized, FFP={}", enabled_ ? "ON" : "OFF"));
		log("FFP", "Transforms come from the engine via comp::game::camera, not from a fixed register layout.");
	}

	// ---- State mutators ----

	void ffp_state::on_set_vs_const_f(UINT start_reg, const float* data, UINT count)
	{
		if (!data || start_reg + count > 256) return;

		std::memcpy(&vs_const_[start_reg * 4], data, count * 4 * sizeof(float));

		// No View/Proj tracking by register here: 3Impact packs World, View and
		// Projection into a single matrix whose register moves between shader
		// variants. comp::game::camera supplies the transforms via set_camera() and
		// set_world(), and reads this constant array to recover the per-object world.

		for (UINT i = 0; i < count; i++)
			vs_const_write_log_[start_reg + i] = 1;

		// Bone palette detection (for skinning module)
		if (config::get().skinning.enabled &&
			start_reg >= static_cast<UINT>(vs_reg_bone_threshold_) &&
			count >= static_cast<UINT>(vs_bone_min_regs_) &&
			(count % static_cast<UINT>(vs_regs_per_bone_)) == 0)
		{
			bone_start_reg_ = static_cast<int>(start_reg);
			num_bones_ = static_cast<int>(count) / vs_regs_per_bone_;
		}
	}

	void ffp_state::on_set_ps_const_f(UINT start_reg, const float* data, UINT count)
	{
		if (!data || start_reg + count > 32) return;

		std::memcpy(&ps_const_[start_reg * 4], data, count * 4 * sizeof(float));
	}

	void ffp_state::on_set_vertex_shader(IDirect3DVertexShader9* shader)
	{
		if (shader) shader->AddRef();
		if (last_vs_) last_vs_->Release();
		last_vs_ = shader;
		ffp_active_ = false;
	}

	void ffp_state::on_set_pixel_shader(IDirect3DPixelShader9* shader)
	{
		if (shader) shader->AddRef();
		if (last_ps_) last_ps_->Release();
		last_ps_ = shader;
	}

	void ffp_state::on_set_texture(UINT stage, IDirect3DBaseTexture9* texture)
	{
		if (stage < 8)
			cur_texture_[stage] = texture;
	}

	void ffp_state::on_set_stream_source(UINT stream, IDirect3DVertexBuffer9* vb, UINT offset, UINT stride)
	{
		if (stream < 4)
		{
			stream_vb_[stream] = vb;
			stream_offset_[stream] = offset;
			stream_stride_[stream] = stride;
		}
	}

	void ffp_state::on_set_render_state(D3DRENDERSTATETYPE state, DWORD value)
	{
		switch (state)
		{
		case D3DRS_ZWRITEENABLE: rs_zwrite_ = value; break;
		case D3DRS_ZENABLE: rs_zenable_ = value; break;
		case D3DRS_ALPHABLENDENABLE: rs_blend_ = value; break;
		default: break;
		}
	}

	void ffp_state::on_set_vertex_declaration(IDirect3DVertexDeclaration9* decl)
	{
		last_decl_ = decl;
		cur_decl_is_skinned_ = false;
		cur_decl_has_texcoord_ = false;
		cur_decl_has_normal_ = false;
		cur_decl_has_color_ = false;
		cur_decl_has_pos_t_ = false;
		cur_decl_texcoord_type_ = -1;
		cur_decl_texcoord_off_ = 0;
		cur_decl_num_weights_ = 0;
		cur_decl_blend_weight_off_ = 0;
		cur_decl_blend_weight_type_ = 0;
		cur_decl_blend_indices_off_ = 0;
		cur_decl_pos_off_ = 0;
		cur_decl_normal_off_ = 0;
		cur_decl_normal_type_ = -1;

		if (!decl) return;

		UINT num_elems = 0;
		if (FAILED(decl->GetDeclaration(nullptr, &num_elems))) return;
		if (num_elems == 0 || num_elems > 32) return;

		D3DVERTEXELEMENT9 elems[32];
		if (FAILED(decl->GetDeclaration(elems, &num_elems))) return;

		bool has_blend_weight = false;
		bool has_blend_indices = false;

		for (UINT e = 0; e < num_elems; e++)
		{
			const auto& el = elems[e];
			if (el.Stream == 0xFF) break;

			switch (el.Usage)
			{
			case D3DDECLUSAGE_POSITIONT:
				cur_decl_has_pos_t_ = true;
				break;

			case D3DDECLUSAGE_BLENDWEIGHT:
				has_blend_weight = true;
				cur_decl_blend_weight_off_ = el.Offset;
				cur_decl_blend_weight_type_ = el.Type;
				break;

			case D3DDECLUSAGE_BLENDINDICES:
				has_blend_indices = true;
				cur_decl_blend_indices_off_ = el.Offset;
				break;

			case D3DDECLUSAGE_POSITION:
				if (el.Stream == 0)
					cur_decl_pos_off_ = el.Offset;
				break;

			case D3DDECLUSAGE_NORMAL:
				if (el.Stream == 0)
				{
					cur_decl_has_normal_ = true;
					cur_decl_normal_off_ = el.Offset;
					cur_decl_normal_type_ = el.Type;
				}
				break;

			case D3DDECLUSAGE_TEXCOORD:
				if (el.UsageIndex == 0 && el.Stream == 0)
				{
					cur_decl_has_texcoord_ = true;
					cur_decl_texcoord_type_ = el.Type;
					cur_decl_texcoord_off_ = el.Offset;
				}
				break;

			case D3DDECLUSAGE_COLOR:
				cur_decl_has_color_ = true;
				break;
			}
		}

		if (has_blend_weight && has_blend_indices)
		{
			cur_decl_is_skinned_ = true;

			switch (cur_decl_blend_weight_type_)
			{
			case D3DDECLTYPE_FLOAT1:  cur_decl_num_weights_ = 1; break;
			case D3DDECLTYPE_FLOAT2:  cur_decl_num_weights_ = 2; break;
			case D3DDECLTYPE_FLOAT3:  cur_decl_num_weights_ = 3; break;
			case D3DDECLTYPE_FLOAT4:  cur_decl_num_weights_ = 3; break;
			case D3DDECLTYPE_UBYTE4N: cur_decl_num_weights_ = 3; break;
			default:                  cur_decl_num_weights_ = 3; break;
			}
		}
	}

	void ffp_state::on_present(IDirect3DDevice9* dev)
	{
		frame_count_++;
		ffp_setup_ = false;
		draw_call_count_ = 0;
		scene_count_ = 0;
		// Safety net: restore shaders if a draw path exited without calling disengage.
		// No-op when already disengaged (the normal case). Uses the presenting device
		// rather than the global one: 3D Rad creates two devices, and the shaders being
		// restored belong to whichever one is drawing.
		disengage(dev);
		std::memset(vs_const_write_log_, 0, sizeof(vs_const_write_log_));
		std::memset(route_counts_, 0, sizeof(route_counts_));
	}

	void ffp_state::on_begin_scene()
	{
		ffp_setup_ = false;
		scene_count_++;
	}

	void ffp_state::on_reset()
	{
		if (last_vs_) { last_vs_->Release(); last_vs_ = nullptr; }
		if (last_ps_) { last_ps_->Release(); last_ps_ = nullptr; }
		last_decl_ = nullptr;

		// Default-pool resources (textures, VBs) are released by the game before Reset
		std::memset(cur_texture_, 0, sizeof(cur_texture_));
		std::memset(stream_vb_, 0, sizeof(stream_vb_));
		std::memset(stream_offset_, 0, sizeof(stream_offset_));
		std::memset(stream_stride_, 0, sizeof(stream_stride_));

		view_proj_valid_ = false;
		ffp_setup_ = false;
		world_dirty_ = false;
		camera_dirty_ = false;
		scene_light_count_ = 0;
		last_enabled_lights_ = 0;
		ffp_active_ = false;
		bone_start_reg_ = 0;
		num_bones_ = 0;

		cur_decl_is_skinned_ = false;
		cur_decl_has_texcoord_ = false;
		cur_decl_has_normal_ = false;
		cur_decl_has_color_ = false;
		cur_decl_has_pos_t_ = false;
		cur_decl_texcoord_type_ = -1;
		cur_decl_texcoord_off_ = 0;
		cur_decl_num_weights_ = 0;
		cur_decl_blend_weight_off_ = 0;
		cur_decl_blend_weight_type_ = 0;
		cur_decl_blend_indices_off_ = 0;
		cur_decl_pos_off_ = 0;
		cur_decl_normal_off_ = 0;
		cur_decl_normal_type_ = -1;

		std::memset(vs_const_write_log_, 0, sizeof(vs_const_write_log_));
		std::memset(route_counts_, 0, sizeof(route_counts_));

		rs_zwrite_ = TRUE;
		rs_zenable_ = TRUE;
		rs_blend_ = FALSE;

		log("FFP", "State reset");
	}

	// ---- Transform input ----

	void ffp_state::set_camera(const D3DMATRIX& view, const D3DMATRIX& proj)
	{
		if (view_proj_valid_ &&
			std::memcmp(&view_, &view, sizeof(view)) == 0 &&
			std::memcmp(&proj_, &proj, sizeof(proj)) == 0)
		{
			return;
		}

		view_ = view;
		proj_ = proj;
		camera_dirty_ = true;
		view_proj_valid_ = true;
	}

	void ffp_state::set_world(const D3DMATRIX& world)
	{
		if (std::memcmp(&world_, &world, sizeof(world)) == 0) {
			return;
		}

		world_ = world;
		world_dirty_ = true;
	}

	void ffp_state::set_scene_lights(const D3DLIGHT9* lights, const int count)
	{
		if (!lights || count <= 0)
		{
			scene_light_count_ = 0;
			return;
		}

		scene_light_count_ = std::min(count, k_max_d3d_lights);
		std::memcpy(scene_lights_, lights, static_cast<size_t>(scene_light_count_) * sizeof(D3DLIGHT9));
	}

	void ffp_state::apply_pending_camera(IDirect3DDevice9* dev)
	{
		if (!dev || !view_proj_valid_) {
			return;
		}

		// Early VP and Present publish via set_camera only when View/C actually
		// changed (FNV hash) or the first time a scene camera is seen.
		apply_transforms(dev);
	}

	void ffp_state::record_route(draw_route route)
	{
		if (route < draw_route::count)
			route_counts_[static_cast<int>(route)]++;
	}

	UINT ffp_state::route_count(draw_route route) const
	{
		return route < draw_route::count ? route_counts_[static_cast<int>(route)] : 0;
	}

	// ---- State consumers ----

	void ffp_state::engage(IDirect3DDevice9* dev)
	{
		if (!cfg_ || !enabled_ || !dev) return;

		const bool just_engaged = !ffp_active_;
		if (just_engaged)
		{
			dev->SetVertexShader(nullptr);
			dev->SetPixelShader(nullptr);
			ffp_active_ = true;
		}

		apply_transforms(dev);
		if (just_engaged) {
			setup_texture_stages(dev);
			// Re-submit every FFP streak so Remix DirtyLights fires after HUD
			// disengage. Lights are otherwise garbage-collected as stale.
			setup_lighting(dev);
			ffp_setup_ = true;
		}
	}

	void ffp_state::disengage(IDirect3DDevice9* dev)
	{
		if (!ffp_active_ || !dev) return;

		dev->SetVertexShader(last_vs_);
		dev->SetPixelShader(last_ps_);
		ffp_active_ = false;
	}

	void ffp_state::setup_albedo_texture(IDirect3DDevice9* dev)
	{
		if (!cfg_ || !dev) return;

		int as = cfg_->albedo_stage;
		auto* albedo = (as >= 0 && as < 8) ? cur_texture_[as] : cur_texture_[0];

		dev->SetTexture(0, albedo);
		for (DWORD ts = 1; ts < 8; ts++)
			dev->SetTexture(ts, nullptr);
	}

	void ffp_state::restore_textures(IDirect3DDevice9* dev)
	{
		if (!dev) return;

		for (DWORD ts = 0; ts < 8; ts++)
			dev->SetTexture(ts, cur_texture_[ts]);
	}

	// ---- Internal helpers ----

	void ffp_state::apply_transforms(IDirect3DDevice9* dev)
	{
		// Both matrices arrive row-major from D3DX, which is what SetTransform expects,
		// so unlike shader constants they need no transpose.
		if (camera_dirty_)
		{
			dev->SetTransform(D3DTS_VIEW, &view_);
			dev->SetTransform(D3DTS_PROJECTION, &proj_);

			camera_dirty_ = false;
		}

		if (world_dirty_)
		{
			dev->SetTransform(D3DTS_WORLD, &world_);

			world_dirty_ = false;
		}
	}

	void ffp_state::setup_lighting(IDirect3DDevice9* dev)
	{
		// 3Impact never SetLight/LightEnable — lighting lived in the shaders we
		// null. Captured PointLight / SunLight (iLight* buffers) replace the
		// fallback sun when any are shown this frame. Do not SetConfigVariable.
		D3DMATERIAL9 mat = {};
		mat.Diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
		mat.Ambient = { 1.0f, 1.0f, 1.0f, 1.0f };
		mat.Specular = { 0.0f, 0.0f, 0.0f, 1.0f };
		mat.Emissive = { 0.0f, 0.0f, 0.0f, 1.0f };
		mat.Power = 0.0f;
		dev->SetMaterial(&mat);

		int enabled = 0;
		if (scene_light_count_ > 0)
		{
			enabled = std::min(scene_light_count_, k_max_d3d_lights);
			for (int i = 0; i < enabled; i++)
			{
				dev->SetLight(static_cast<DWORD>(i), &scene_lights_[i]);
				dev->LightEnable(static_cast<DWORD>(i), TRUE);
			}
		}
		else
		{
			D3DLIGHT9 light = {};
			light.Type = D3DLIGHT_DIRECTIONAL;
			light.Diffuse = { 1.00f, 0.95f, 0.85f, 1.0f };
			light.Specular = { 0.0f, 0.0f, 0.0f, 1.0f };
			light.Ambient = { 0.25f, 0.28f, 0.35f, 1.0f };
			light.Direction = { -0.35f, -0.85f, 0.40f };
			light.Position = { 0.0f, 0.0f, 0.0f };
			light.Range = 1.0f;
			light.Falloff = 1.0f;
			light.Attenuation0 = 1.0f;
			light.Attenuation1 = 0.0f;
			light.Attenuation2 = 0.0f;
			light.Theta = 0.0f;
			light.Phi = 0.0f;
			dev->SetLight(0, &light);
			dev->LightEnable(0, TRUE);
			enabled = 1;
		}

		for (int i = enabled; i < last_enabled_lights_ && i < k_max_d3d_lights; i++) {
			dev->LightEnable(static_cast<DWORD>(i), FALSE);
		}
		last_enabled_lights_ = enabled;

		dev->SetRenderState(D3DRS_LIGHTING, TRUE);
		dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
		dev->SetRenderState(D3DRS_COLORVERTEX, FALSE);
		dev->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
		dev->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
		dev->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
		dev->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
		dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
		dev->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_ARGB(255, 64, 72, 88));

		static int logged_mode = -1;
		if (logged_mode != (scene_light_count_ > 0 ? 1 : 0))
		{
			logged_mode = scene_light_count_ > 0 ? 1 : 0;
			if (scene_light_count_ > 0)
			{
				log("FFP", std::format("Scene lights {} (SetLight, no fallback sun)",
					scene_light_count_));
			}
			else
			{
				log("FFP", "Fallback directional light 0 + ambient (no captured Point/Sun this frame)");
			}
		}
	}

	void ffp_state::setup_texture_stages(IDirect3DDevice9* dev)
	{
		// Stage 0: modulate texture color with vertex/material diffuse
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

		// Disable stages 1-7: the game binds shadow maps, LUTs, normal maps etc.
		// on higher stages for its pixel shaders. In FFP mode those become active
		// and Remix may consume the wrong textures.
		for (DWORD s = 1; s <= 7; s++)
		{
			dev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_DISABLE);
			dev->SetTextureStageState(s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		}
	}

	// ---- Utility ----

	void ffp_state::mat4_transpose(float* dst, const float* src)
	{
		dst[0]  = src[0];  dst[1]  = src[4];  dst[2]  = src[8];  dst[3]  = src[12];
		dst[4]  = src[1];  dst[5]  = src[5];  dst[6]  = src[9];  dst[7]  = src[13];
		dst[8]  = src[2];  dst[9]  = src[6];  dst[10] = src[10]; dst[11] = src[14];
		dst[12] = src[3];  dst[13] = src[7];  dst[14] = src[11]; dst[15] = src[15];
	}

	bool ffp_state::mat4_is_interesting(const float* m)
	{
		bool all_zero = true;
		for (int i = 0; i < 16; i++)
		{
			if (m[i] != 0.0f) { all_zero = false; break; }
		}
		if (all_zero) return false;

		// Check for identity
		if (m[0] == 1.0f && m[1] == 0.0f && m[2] == 0.0f  && m[3] == 0.0f &&
			m[4] == 0.0f && m[5] == 1.0f && m[6] == 0.0f  && m[7] == 0.0f &&
			m[8] == 0.0f && m[9] == 0.0f && m[10] == 1.0f && m[11] == 0.0f &&
			m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f && m[15] == 1.0f)
			return false;

		return true;
	}
}
