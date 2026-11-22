#pragma once

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/atmosphere.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/raster.h>

#include <span>

namespace ludus::graphics::renderer
{
struct ViewDescription;
namespace internal
{
[[nodiscard]] bool TryGetAtmosphereObserver(const AtmosphereDescription& atmosphere,
                                            foundation::math::Vector3 eyeWorld,
                                            foundation::float64& outAltitudeMetres,
                                            foundation::math::Vector3& outUpWorld) noexcept;

/// Rows of CloudLayerParameters (renderer_cloud_layer.slang), shared by the sky and deferred resolve uniforms. All
/// zero omits the layer.
struct CloudLayerUniform final
{
    foundation::math::Vector4 AxisXWorld{};
    foundation::math::Vector4 UpWorld{};
    foundation::math::Vector4 AxisZWorld{};
    foundation::math::Vector4 CameraLocal{};
    foundation::math::Vector4 Medium{};
    foundation::math::Vector4 Phase{};
    /// CloudLayerTestField: unit split normal in the layer frame and the plane's offset in metres.
    foundation::math::Vector4 TestSplit{};
    /// Per region: source radiance in xyz and extinction (density applied) per metre in w.
    foundation::StaticArray<foundation::math::Vector4, CloudLayerTestField::REGION_COUNT> TestRegions{};
};

/// Validate an Atmosphere's cloud layer (CloudLayerDescription) and pack it for one camera. A disabled layer, an
/// orthographic camera, or an eye below the ground or above the shell writes the all-zero uniform, which omits the
/// layer. The camera's position enters in binary64 relative to the surface anchor.
/// @return False for invalid cloud fields or anchor, preserving outUniform; the caller rejects the description.
[[nodiscard]] bool TryPackCloudLayer(const AtmosphereDescription& atmosphere,
                                     const ViewDescription& camera,
                                     CloudLayerUniform& outUniform) noexcept;

// Mirrors AuroraLayerParameters in the sky and deferred resolve shaders.
struct AuroraLayerUniform final
{
    foundation::math::Vector4 AxisXWorld{};
    foundation::math::Vector4 AxisZWorld{};
};

/// Validate the emission, frame and time. Write zero for disabled or unsupported views.
[[nodiscard]] bool TryPackAuroraLayer(const AtmosphereDescription& atmosphere,
                                      bool isOrthographic,
                                      bool isOrbital,
                                      AuroraLayerUniform& outUniform) noexcept;

struct AtmosphereLightingResources final
{
    rhi::BindingLayoutHandle Layout{};
    rhi::RasterPipelineHandle Pipeline{};
    std::span<const rhi::RasterBinding> Bindings;
    rhi::TextureViewHandle Transmittance{};
    rhi::TextureViewHandle Multiple{};
    rhi::TextureViewHandle Sky{};
};

struct AtmosphereSkyTable;
struct AtmosphereViewUniform;
class RenderGraphAtmosphereView;

class RenderGraphAtmosphereProgram final
{
public:
    RenderGraphAtmosphereProgram() noexcept = default;
    ~RenderGraphAtmosphereProgram() noexcept;
    RenderGraphAtmosphereProgram(const RenderGraphAtmosphereProgram&) = delete;
    RenderGraphAtmosphereProgram& operator=(const RenderGraphAtmosphereProgram&) = delete;

    [[nodiscard]] rhi::RasterStatus Prepare(rhi::DeviceHandle device) noexcept;

private:
    friend class RenderGraphAtmosphereView;
    enum class Stage : foundation::uint8
    {
        Transmittance,
        Multiple,
        Sky,
        Display,
        Surface,
        Count,
    };
    enum class Phase : foundation::uint8
    {
        Empty,
        Dependencies,
        Pipelines,
        Ready,
        Failed,
    };
    struct PassResources final
    {
        rhi::RasterShaderHandle Shader{};
        rhi::BindingLayoutHandle Layout{};
        rhi::RasterPipelineHandle Pipeline{};
        std::span<const rhi::RasterBinding> Bindings;
    };
    [[nodiscard]] rhi::RasterStatus pollDependencies() noexcept;
    [[nodiscard]] rhi::RasterStatus draw(Stage stage,
                                         rhi::BindingSetHandle bindings,
                                         const rhi::RasterVertexSlice& vertices,
                                         rhi::BufferHandle indices) const noexcept;
    [[nodiscard]] rhi::RasterStatus acquireSky(const AtmosphereViewUniform& uniform,
                                               AtmosphereSkyTable*& outTableOrNull) noexcept;
    void releaseSky(AtmosphereSkyTable& inoutTable) noexcept;

    foundation::StaticArray<PassResources, static_cast<foundation::usize>(Stage::Count)> mPasses{};
    rhi::DeviceHandle mDevice{};
    rhi::RasterShaderHandle mVertex{};
    rhi::TextureHandle mTransmittance{};
    rhi::TextureViewHandle mTransmittanceView{};
    rhi::BindingSetHandle mTransmittanceBindings{};
    rhi::TextureHandle mMultiple{};
    rhi::TextureViewHandle mMultipleView{};
    rhi::BindingSetHandle mMultipleBindings{};
    AtmosphereSkyTable* mSkyTablesOrNull = nullptr;
    Phase mPhase = Phase::Empty;
    rhi::RasterStatus mStatus = rhi::RasterStatus::NotReady;
};

// Tied to a prepared deferred view. The program owns the fixed Earth transmittance
// table and shares sky tables between live views of the same altitude and solar zenith.
class RenderGraphAtmosphereView final
{
public:
    RenderGraphAtmosphereView() noexcept = default;
    ~RenderGraphAtmosphereView() noexcept;
    RenderGraphAtmosphereView(const RenderGraphAtmosphereView&) = delete;
    RenderGraphAtmosphereView& operator=(const RenderGraphAtmosphereView&) = delete;

    [[nodiscard]] rhi::RasterStatus Prepare(RenderGraphAtmosphereProgram& inoutProgram,
                                            rhi::DeviceHandle device,
                                            const ViewDescription& camera,
                                            foundation::uint32 width,
                                            foundation::uint32 height,
                                            foundation::float32 exposure,
                                            foundation::uint32 curve) noexcept;
    [[nodiscard]] bool TryGetLightingResources(AtmosphereLightingResources& outResources) const noexcept;
    [[nodiscard]] rhi::RasterStatus RecordTables(const rhi::RasterVertexSlice& vertices,
                                                 rhi::BufferHandle indices) const noexcept;
    [[nodiscard]] rhi::RasterStatus RecordSky(const rhi::RasterVertexSlice& vertices,
                                              rhi::BufferHandle indices) const noexcept;

private:
    RenderGraphAtmosphereProgram* mProgramOrNull = nullptr;
    AtmosphereSkyTable* mSkyOrNull = nullptr;
    rhi::BufferHandle mUniform{};
    rhi::BindingSetHandle mSkyBindings{};
    rhi::BindingSetHandle mDisplayBindings{};
    bool mIsOrbital = false;
    bool mHasUniform = false;
    bool mHasSkyBindings = false;
    bool mHasDisplayBindings = false;
};
} // namespace internal
} // namespace ludus::graphics::renderer
