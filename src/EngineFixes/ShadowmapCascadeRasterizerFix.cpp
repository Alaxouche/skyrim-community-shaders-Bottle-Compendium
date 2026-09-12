#include "ShadowmapCascadeRasterizerFix.h"

#include "State.h"

void ShadowmapRasterizerFix::Install()
{
	// This function is called once per cascade to begin the updating and rendering process
	stl::write_thunk_call<BSShadowDirectionalLight_RenderShadowmaps_RenderCascade>(REL::RelocationID(101495, 108489).address() + REL::Relocate(0xC6, 0xC6));

	gRasterStates = reinterpret_cast<RasterStateArray*>(REL::RelocationID(524748, 411363).address());

	numCascades = static_cast<uint>(Util::GetGameSettingValue<std::int32_t>("iNumSplits:Display", Settings.at("iNumSplits:Display")));
}

void ShadowmapRasterizerFix::BSShadowDirectionalLight_RenderShadowmaps_RenderCascade::thunk(RE::BSShadowDirectionalLight* light, void* arg1, void* arg2, uint32_t flags)
{
	// The engine only re-binds a rasterizer state when one of these dirty bits is set; swapping the
	// pointer table alone leaves whatever D3D object was last bound on the context.
	constexpr auto rasterDirtyFlags = static_cast<uint32_t>(RE::BSGraphics::ShaderFlags::DIRTY_RASTER_CULL_MODE) |
	                                  static_cast<uint32_t>(RE::BSGraphics::ShaderFlags::DIRTY_RASTER_DEPTH_BIAS);
	auto markRasterStateDirty = [] {
		if (auto* dirtyFlags = globals::game::stateUpdateFlags)
			dirtyFlags->set(static_cast<RE::BSGraphics::ShaderFlags>(rasterDirtyFlags));
	};

	static bool backupTaken = false;
	static bool cascadeCloned[maxCascades] = {};
	static uint32_t lastFrame = UINT32_MAX;
	static const RE::BSShadowDirectionalLight* lastLight = nullptr;
	static uint cascade = 0;

	// Derive the cascade index from the call order within the current frame instead of a counter that
	// wraps forever: if the engine renders a different number of cascades than iNumSplits reported at
	// install time, a free-running counter drifts and the per-cascade states end up on the wrong split.
	const uint32_t frame = globals::state ? globals::state->frameCount : 0;
	if (frame != lastFrame || light != lastLight) {
		lastFrame = frame;
		lastLight = light;
		cascade = 0;
	}
	const uint cascadeIndex = std::min(cascade, maxCascades - 1);
	++cascade;

	if (!backupTaken) {
		std::memcpy(backupGameRasterStates, *gRasterStates, sizeof(RasterStateArray));
		backupTaken = true;
	}

	if (!cascadeCloned[cascadeIndex]) {
		CloneRasterStates(&backupGameRasterStates, cascadeIndex);
		cascadeCloned[cascadeIndex] = true;
	}

	// Emplace the biased states for this cascade and force the first shadow draw to bind them.
	std::memcpy(*gRasterStates, shadowmapRasterStates[cascadeIndex], sizeof(RasterStateArray));
	markRasterStateDirty();

	func(light, arg1, arg2, flags);

	// Restore after every cascade and force a re-bind. Without the dirty bits the last biased state
	// (depth bias plus slope-scaled bias, applied to all 12 depth-bias modes) stayed bound into the
	// depth prepass whenever its first draw shared cull/bias indices with the last shadow draw. That
	// depends on draw order, so on random frames grazing surfaces such as terrain wrote pushed-back
	// depth, the shadow mask reconstructed wrong positions, and directional shadows vanished for a frame.
	std::memcpy(*gRasterStates, backupGameRasterStates, sizeof(RasterStateArray));
	markRasterStateDirty();
}

void ShadowmapRasterizerFix::GetUpdatedRasterDesc(D3D11_RASTERIZER_DESC& outputDesc, ShadowMapRasterizerDescriptor shadowmapDesc)
{
	outputDesc.DepthBias = shadowmapDesc.rasterDepthBias;
	outputDesc.DepthBiasClamp = shadowmapDesc.rasterDepthBiasClamp;
	outputDesc.SlopeScaledDepthBias = shadowmapDesc.rasterSlopeScaleBias;
}

// Since state objects are shared globally across the pipeline we make duplicate arrays that cover the same range of states the game does
void ShadowmapRasterizerFix::CloneRasterStates(RasterStateArray* inputArray, int cascade)
{
	for (int fill = 0; fill < 2; fill++) {
		for (int cull = 0; cull < 3; cull++) {
			for (int depth = 0; depth < 12; depth++) {
				for (int scissor = 0; scissor < 2; scissor++) {
					if (auto* gRasterizer = (*inputArray)[fill][cull][depth][scissor]) {
						D3D11_RASTERIZER_DESC desc{};
						gRasterizer->GetDesc(&desc);

						GetUpdatedRasterDesc(desc, cascadeDescriptors[cascade]);

						DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&desc, &shadowmapRasterStates[cascade][fill][cull][depth][scissor]));
					}
				}
			}
		}
	}
}