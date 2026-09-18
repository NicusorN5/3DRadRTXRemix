#include "std_include.hpp"
#include "renderer.hpp"

#include "imgui.hpp"
#include "diagnostics.hpp"
#include "comp/game/camera.hpp"
#include "comp/editor_settings.hpp"
#include "shared/common/ffp_state.hpp"

namespace comp
{
	namespace tex_addons
	{
		bool initialized = false;
		LPDIRECT3DTEXTURE9 icon = nullptr;

		void init_texture_addons(bool release)
		{
			if (release)
			{
				if (tex_addons::icon) tex_addons::icon->Release();
				return;
			}

			const auto dev = shared::globals::d3d_device;
			const char* icon_path = "rtx_comp\\textures\\icon.png";

			// Only load if the file exists — no icon is shipped by default
			if (GetFileAttributesA(icon_path) != INVALID_FILE_ATTRIBUTES)
			{
				HRESULT hr = D3DXCreateTextureFromFileA(dev, icon_path, &tex_addons::icon);
				if (FAILED(hr))
					shared::common::log("Renderer", std::format("Failed to load {}", icon_path), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			}

			tex_addons::initialized = true;
		}
	}


	// ----

	drawcall_mod_context& setup_context(IDirect3DDevice9* dev)
	{
		auto& ctx = renderer::dc_ctx;
		ctx.info.device_ptr = dev;
		return ctx;
	}

	/*
	 * True when the draw is going to the scene (Remix MAIN), not a cubemap /
	 * preview target.
	 *
	 * Editor: 3Impact composites through an offscreen RT that is often a
	 * *different* size than the CreateDevice back buffer (log: 1332×614 3D
	 * pane vs 827×620 swapchain). Pointer equality with GetBackBuffer would
	 * reject the whole scene; matching only the backbuffer would too.
	 *
	 * Compiled player: the scene is the swapchain (or a client-sized RT) after
	 * Reset (often 1920×1080). Leftover windowed/client rects (1519×824 on
	 * 1920×1080) are not scene sizes.
	 * Reject: other sizes, especially square cubemap faces.
	 *
	 * Size is cached from SetRenderTarget(0). Draws only GetRenderTarget when
	 * that hook has not run yet for this device (implicit CreateDevice RT).
	 */
	struct rt0_cache
	{
		IDirect3DDevice9* dev = nullptr;
		IDirect3DSurface9* rt = nullptr;
		UINT w = 0;
		UINT h = 0;
		bool valid = false;
	};

	constexpr int rt0_slots = 4;
	rt0_cache rt0[rt0_slots]{};
	bool rt0_reported = false;

	rt0_cache* rt0_for(IDirect3DDevice9* dev, const bool allocate)
	{
		for (int i = 0; i < rt0_slots; i++)
		{
			if (rt0[i].valid && rt0[i].dev == dev) {
				return &rt0[i];
			}
		}

		if (!allocate) {
			return nullptr;
		}

		for (int i = 0; i < rt0_slots; i++)
		{
			if (!rt0[i].valid)
			{
				rt0[i] = {};
				rt0[i].dev = dev;
				return &rt0[i];
			}
		}

		rt0[0] = {};
		rt0[0].dev = dev;
		return &rt0[0];
	}

	void remember_rt0(rt0_cache* slot, IDirect3DDevice9* dev, IDirect3DSurface9* rt,
		const UINT w, const UINT h)
	{
		slot->dev = dev;
		slot->rt = rt;
		slot->w = w;
		slot->h = h;
		slot->valid = true;
	}

	void renderer::note_render_target(IDirect3DDevice9* dev, DWORD index, IDirect3DSurface9* rt)
	{
		if (!dev || index != 0) {
			return;
		}

		auto* slot = rt0_for(dev, true);
		if (!rt)
		{
			remember_rt0(slot, dev, nullptr, 0, 0);
			return;
		}

		if (slot->valid && slot->rt == rt && slot->w != 0) {
			return;
		}

		D3DSURFACE_DESC desc{};
		if (FAILED(rt->GetDesc(&desc))) {
			return;
		}

		remember_rt0(slot, dev, rt, desc.Width, desc.Height);
		game::camera::note_editor_scene_rt(desc.Width, desc.Height);

		if (!rt0_reported)
		{
			rt0_reported = true;
			const UINT scene_w = game::camera::viewport_width();
			const UINT scene_h = game::camera::viewport_height();
			const bool is_scene = game::camera::scene_conversion_allowed() &&
				game::camera::size_is_scene_viewport(desc.Width, desc.Height);
			shared::common::log("Renderer", std::format(
				"Scene render target: 0x{:X} {}x{} fmt={} usage=0x{:X} | viewport {}x{} -> {}",
				reinterpret_cast<std::uintptr_t>(rt), desc.Width, desc.Height,
				static_cast<int>(desc.Format), desc.Usage,
				scene_w, scene_h,
				is_scene ? "is the scene viewport" : "not scene (pregame or secondary)"),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
	}

	struct backbuffer_ident
	{
		IDirect3DDevice9* dev = nullptr;
		IDirect3DSurface9* ptr = nullptr;
		UINT w = 0;
		UINT h = 0;
		bool valid = false;
	};

	backbuffer_ident bb{};

	void cache_backbuffer(IDirect3DDevice9* dev)
	{
		bb = {};
		if (!dev) {
			return;
		}

		IDirect3DSurface9* back = nullptr;
		if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) {
			return;
		}

		D3DSURFACE_DESC desc{};
		back->GetDesc(&desc);
		bb.dev = dev;
		bb.ptr = back;
		bb.w = desc.Width;
		bb.h = desc.Height;
		bb.valid = true;
		back->Release();
		game::camera::note_surface_size(desc.Width, desc.Height);
	}

	void renderer::invalidate_render_target_cache()
	{
		for (auto& slot : rt0) {
			slot = {};
		}
		bb = {};
		rt0_reported = false;
	}

	bool targets_scene(IDirect3DDevice9* dev)
	{
		if (!game::camera::scene_conversion_allowed()) {
			return false;
		}
		if (!bb.valid || bb.dev != dev) {
			cache_backbuffer(dev);
		}

		IDirect3DSurface9* rt = nullptr;
		UINT tw = 0;
		UINT th = 0;
		if (const auto* slot = rt0_for(dev, false); slot && slot->valid)
		{
			if (!slot->rt)
			{
				// Implicit backbuffer. Editor 3D is a separate RT — the 827
				// swapchain blit is not MAIN once that RT is known.
				if (bb.valid && game::camera::size_is_scene_viewport(bb.w, bb.h)) {
					return true;
				}
				return game::camera::viewport_width() == 0;
			}
			rt = slot->rt;
			tw = slot->w;
			th = slot->h;
		}
		else
		{
			IDirect3DSurface9* target = nullptr;
			if (FAILED(dev->GetRenderTarget(0, &target)) || !target) {
				return true;
			}

			D3DSURFACE_DESC desc{};
			target->GetDesc(&desc);
			tw = desc.Width;
			th = desc.Height;
			rt = target;
			remember_rt0(rt0_for(dev, true), dev, target, tw, th);
			game::camera::note_editor_scene_rt(tw, th);
			target->Release();
		}

		if (game::camera::size_is_scene_viewport(tw, th)) {
			return true;
		}

		// Compiled player: swapchain pointer / same size as backbuffer.
		// Editor: once the 3D RT is known, size_is_scene_viewport already
		// answered; the 827 swapchain is not the scene.
		if (bb.valid && game::camera::size_is_scene_viewport(bb.w, bb.h))
		{
			if (rt && rt == bb.ptr) {
				return true;
			}
			if (tw == bb.w && th == bb.h) {
				return true;
			}
		}

		// Cubemap faces are square and not the scene size.
		if (tw == th && tw >= 32) {
			return false;
		}

		if (game::camera::viewport_width() == 0 || game::camera::viewport_height() == 0) {
			return true;
		}

		return false;
	}


	// ----

	HRESULT renderer::on_draw_primitive(IDirect3DDevice9* dev, const D3DPRIMITIVETYPE& PrimitiveType, const UINT& StartVertex, const UINT& PrimitiveCount)
	{
		if (!is_initialized() || shared::globals::imgui_is_rendering) {
			return dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		}

		static auto im = imgui::get();
		im->m_stats._drawcall_prim_incl_ignored.track_single();

		auto& ctx = setup_context(dev);
		auto& ffp = shared::common::ffp_state::get();
		ffp.increment_draw_count();
		const bool ui_scaled = editor_settings::begin_ui_draw(dev);

		auto hr = S_OK;

		/*
		 * FFP draw routing for non-indexed draws.
		 *
		 * GAME-SPECIFIC: Adjust conditions if your game's non-indexed draws
		 * include world geometry that should be converted to FFP.
		 */
		const bool scene = targets_scene(dev);
		using route = shared::common::ffp_state::draw_route;

		if (!scene && ffp.last_decl() && !ffp.cur_decl_has_pos_t() &&
			game::camera::scene_conversion_allowed())
		{
			game::camera::note_non_scene_skip();
		}

		// Narrow HUD guard: screen-space POSITIONT / XYZRHW only. Scene meshes
		// use XYZ (+ NORMAL). Never isolate those with identity View/P.
		if (!game::camera::scene_conversion_allowed())
		{
			ffp.record_route(route::pre_game);
			game::camera::note_pregame_draw(dev);
			ffp.disengage(dev);
			hr = dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
			im->m_stats._drawcall_prim.track_single();
			im->m_stats._drawcall_using_vs.track_single();
		}
		else if (ffp.cur_decl_has_pos_t())
		{
			ffp.record_route(route::hud);
			ffp.disengage(dev);
			hr = dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
			im->m_stats._drawcall_prim.track_single();
			im->m_stats._drawcall_using_vs.track_single();
		}
		else if (D3DMATRIX view, proj, world; ffp.is_enabled() && scene &&
			ffp.last_decl() && !ffp.cur_decl_is_skinned() &&
			game::camera::resolve_draw(ffp.last_vs(), ffp.vs_const_data(), view, proj, world))
		{
			// View/Proj are published on the first scene VP this frame (and
			// again only if C/View changed). Per-draw camera SetTransform
			// would still be a Remix cut.
			(void)view;
			(void)proj;
			ffp.set_world(world);
			ffp.engage(dev);
			ffp.setup_albedo_texture(dev);

			hr = dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
			im->m_stats._drawcall_prim.track_single();

			ffp.restore_textures(dev);
		}
		else
		{
			// Passthrough: no decl / pre-viewProj / skinned / FFP disabled
			if (!scene) {
				ffp.record_route(route::secondary_target);
			}
			ffp.disengage(dev);
			hr = dev->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
			im->m_stats._drawcall_prim.track_single();
			im->m_stats._drawcall_using_vs.track_single();
		}

		if (auto* d = diagnostics::get()) d->on_draw_primitive(ffp.draw_call_count(), PrimitiveType, StartVertex, PrimitiveCount);

		editor_settings::end_ui_draw(dev, ui_scaled);
		ctx.restore_all(dev);
		ctx.reset_context();

		return hr;
	}


	// ----

	HRESULT renderer::on_draw_indexed_prim(IDirect3DDevice9* dev, const D3DPRIMITIVETYPE& PrimitiveType, const INT& BaseVertexIndex, const UINT& MinVertexIndex, const UINT& NumVertices, const UINT& startIndex, const UINT& primCount)
	{
		if (!is_initialized() || shared::globals::imgui_is_rendering) {
			return dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		}

		auto& ctx = setup_context(dev);
		const auto im = imgui::get();
		auto& ffp = shared::common::ffp_state::get();
		ffp.increment_draw_count();
		const bool ui_scaled = editor_settings::begin_ui_draw(dev);

		im->m_stats._drawcall_indexed_prim_incl_ignored.track_single();

		if (ctx.modifiers.do_not_render)
		{
			editor_settings::end_ui_draw(dev, ui_scaled);
			ctx.restore_all(dev);
			ctx.reset_context();
			return S_OK;
		}

		auto hr = S_OK;

		/*
		 * FFP draw routing for indexed draws — the main conversion path.
		 *
		 * Decision tree (port of d3d9_device.c WD_DrawIndexedPrimitive):
		 *   viewProjValid?
		 *   +-- NO  -> passthrough with shaders
		 *   +-- YES
		 *       +-- curDeclIsSkinned?
		 *       |   +-- YES + skinning module -> skinning::draw_skinned_dip()
		 *       |   +-- YES + no skinning     -> passthrough with shaders
		 *       +-- !curDeclHasNormal?
		 *       |   +-- passthrough (HUD/UI)
		 *       |   GAME-SPECIFIC: remove this filter if world geometry lacks NORMAL
		 *       +-- else (rigid 3D mesh)
		 *           +-- FFP engage + draw + restore
		 */
		using route = shared::common::ffp_state::draw_route;

		// Size match against recorded scene viewports (CreateDevice / Reset /
		// client / swapchain), not pointer equality with GetBackBuffer and not
		// a hard-coded 723×542 editor size. Cubemap/preview are other sizes.
		// Picker / windowed-boot / loading: swapchain is not scene.
		const bool scene = targets_scene(dev);

		if (!ffp.is_enabled())
		{
			ffp.record_route(route::ffp_disabled);
			ffp.disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (!game::camera::scene_conversion_allowed())
		{
			// Compiled picker / windowed-boot / loading: shader path, no FFP.
			ffp.record_route(route::pre_game);
			game::camera::note_pregame_draw(dev);
			ffp.disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (!scene)
		{
			// Cubemap / preview — a different size than the viewport.
			// Must not write D3DTS_VIEW/PROJECTION; Remix would treat that as a cut.
			game::camera::note_non_scene_skip();
			ffp.record_route(route::secondary_target);
			ffp.disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (ffp.cur_decl_has_pos_t())
		{
			// Screen-space POSITIONT / XYZRHW. Cannot be a scene mesh.
			ffp.record_route(route::hud);
			ffp.disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (ffp.cur_decl_is_skinned())
		{
			// Skinned mesh: passthrough with shaders.
			// GAME-SPECIFIC: wire up skinning::get()->draw_skinned_dip() here when enabling skinning for this game.
			ffp.record_route(route::skinned);
			ffp.disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (!ffp.cur_decl_has_normal())
		{
			// Shader passthrough when the decl has no NORMAL. Not HUD isolation
			// (POSITIONT is the only HUD skip). Do not SetTransform identity.
			ffp.record_route(route::no_normal);
			ffp.disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}
		else if (D3DMATRIX view, proj, world;
			game::camera::resolve_draw(ffp.last_vs(), ffp.vs_const_data(), view, proj, world))
		{
			// Rigid 3D mesh with NORMAL. Its vertices are in model space, so the FFP
			// needs this object's world matrix on top of its own camera.
			ffp.record_route(route::ffp);
			(void)view;
			(void)proj;
			ffp.set_world(world);
			ffp.engage(dev);
			ffp.setup_albedo_texture(dev);

			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();

			ffp.restore_textures(dev);
		}
		else
		{
			// The world transform could not be recovered, so FFP transforms would place
			// this mesh wrongly. Leave it to the game's own shaders.
			ffp.record_route(route::world_unresolved);
			ffp.disengage(dev);
			hr = dev->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			im->m_stats._drawcall_indexed_prim.track_single();
			im->m_stats._drawcall_indexed_prim_using_vs.track_single();
		}

		if (auto* d = diagnostics::get()) d->on_draw_indexed_prim(ffp.draw_call_count(), dev, PrimitiveType, BaseVertexIndex, NumVertices, primCount);

		editor_settings::end_ui_draw(dev, ui_scaled);
		ctx.restore_all(dev);
		ctx.reset_context();

		return hr;
	}

	// ---

	void renderer::manually_trigger_remix_injection(IDirect3DDevice9* dev)
	{
		if (!m_triggered_remix_injection)
		{
			auto& ctx = dc_ctx;

			dev->SetRenderState(D3DRS_FOGENABLE, FALSE);

			ctx.save_vs(dev);
			dev->SetVertexShader(nullptr);
			ctx.save_ps(dev);
			dev->SetPixelShader(nullptr);

			ctx.save_rs(dev, D3DRS_ZWRITEENABLE);
			dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

			IDirect3DVertexDeclaration9* saved_decl = nullptr;
			dev->GetVertexDeclaration(&saved_decl);
			dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

			struct CUSTOMVERTEX
			{
				float x, y, z, rhw;
				D3DCOLOR color;
			};

			const auto color = D3DCOLOR_COLORVALUE(0, 0, 0, 0);
			const auto w = -0.49f;
			const auto h = -0.495f;

			CUSTOMVERTEX vertices[] =
			{
				{ -0.5f, -0.5f, 0.0f, 1.0f, color },
				{     w, -0.5f, 0.0f, 1.0f, color },
				{ -0.5f,     h, 0.0f, 1.0f, color },
				{     w,     h, 0.0f, 1.0f, color }
			};

			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(CUSTOMVERTEX));

			if (saved_decl)
			{
				dev->SetVertexDeclaration(saved_decl);
				saved_decl->Release();
			}

			ctx.restore_vs(dev);
			ctx.restore_ps(dev);
			ctx.restore_render_state(dev, D3DRS_ZWRITEENABLE);
			m_triggered_remix_injection = true;
		}
	}


	renderer::renderer()
	{
		p_this = this;

		// Initialize FFP state tracker
		shared::common::ffp_state::get().init(shared::globals::d3d_device);

		// Capture View and Projection before 3Impact concatenates them into the single
		// matrix its shaders receive. Safe here: dll3impact.dll imported d3d9, so
		// its IAT is resolved. init() is idempotent (CreateDevice runs twice).
		game::camera::init();

		m_initialized = true;
		shared::common::log("Renderer", "Module initialized.", shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, false);
	}

	renderer::~renderer()
	{
		tex_addons::init_texture_addons(true);
		p_this = nullptr;
	}
}
