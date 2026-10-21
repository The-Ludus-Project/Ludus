#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/lighting.hpp>
#include <ludus/graphics/renderer/scene_shader_layout.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::renderer
{
/// Logical scene API revision checked by generated graph headers and runtime identities.
inline constexpr foundation::uint32 SCENE_SHADER_INTERFACE_VERSION = LUDUS_SCENE_SHADER_INTERFACE_VERSION;
/// Explicitly unassigned material slot: use neutral rough non-emissive material.
inline constexpr foundation::uint32 DEFAULT_SCENE_MATERIAL = ~foundation::uint32{0};
/// Instance override sentinel: retain the mesh slot's assignment.
inline constexpr foundation::uint32 INHERIT_SCENE_MATERIAL = DEFAULT_SCENE_MATERIAL - 1;
/// Borrowed fixed-mip RGB reflection material. Scene graph Update copies all values
/// and texture bytes before returning. Maps are RGBA8 unorm or sRGB color/emission;
/// surface data is unorm RG normal, B roughness and A metallic. Omitted maps are white.
struct SceneShaderMaterial final
{
    foundation::math::Vector3 BaseColor{0.5F, 0.5F, 0.5F}; ///< Linear reflectance in [0,1].
    foundation::float32 Metallic = 0;                      ///< Metallic fraction in [0,1].
    foundation::float32 Roughness = 1;                     ///< Perceptual roughness in [0.045,1].
    foundation::math::Vector3 Emission{};                  ///< Linear RGB emission in [0,10000].
    bool IsEmissionTwoSided = false;                       ///< Geometric emission sidedness, independent of reflection.
    foundation::math::Vector4 UvTransform{1, 1, 0, 0};     ///< UV scale XY and offset ZW.
    foundation::float32 NormalScale = 1;                   ///< Tangent normal XY strength in [0,8].
    rhi::SamplerDescription Sampler{};                     ///< Fixed-mip filter/address policy.
    rhi::TextureCpuContent ColorTexture{};                 ///< Optional retained source mip, copied synchronously.
    rhi::TextureCpuContent SurfaceTexture{};               ///< Optional linear data mip, copied synchronously.
    rhi::TextureCpuContent EmissionTexture{};              ///< Optional linear/sRGB emission multiplier.
    bool HasError = false; ///< Import diagnostic: error material in preview; reject strict admission.
};
/// Optional shared scene inputs. Empty material assignments use the neutral default;
/// malformed references use diagnostic magenta only under DiagnosticColor admission.
/// Analytic lights have no required geometry. Raster area proxy records are rejected:
/// submit real mesh emitters instead to avoid duplicate illumination.
struct SceneShaderInputs final
{
    foundation::BorrowedView<const SceneShaderMaterial> Materials{}; ///< Global material table.
    foundation::BorrowedView<const DirectLight> Lights{};            ///< Directional, point and spot sources.
    foundation::math::Vector3 Environment{};  ///< Constant RGB, or HDR pixel multiplier, [0,10000].
    foundation::uint32 EnvironmentWidth = 0;  ///< Equirectangular width, zero for a constant.
    foundation::uint32 EnvironmentHeight = 0; ///< Equirectangular height, zero for a constant.
    foundation::BorrowedView<const foundation::math::Vector3> EnvironmentPixels{}; ///< North-to-south HDR texels.
};
/// Ray preparation byte ceiling that rejected a candidate; independent of allocator failure.
enum class SceneMemoryBudget : foundation::uint8
{
    None,               ///< No measured byte ceiling was exceeded.
    CpuScene,           ///< Frozen triangles, BVH, surfaces, textures and environment setup.
    CpuScratch,         ///< Fingerprint, packed upload data or pixel initialization scratch.
    GpuPreview,         ///< Logical preview buffers/images, including retained preview jobs.
    RhiResources,       ///< Device-wide logical resource bytes, including other live/retained resources.
    DeviceStorageRange, ///< One buffer exceeds the device's storage binding byte range.
};
/// Candidate material/geometry diagnostics, separate from the last drawable scene.
struct SceneShaderDiagnostics final
{
    foundation::uint32 DefaultBindings = 0;  ///< Referenced unassigned instance/material slots.
    foundation::uint32 ErrorBindings = 0;    ///< Referenced broken/unsupported instance/material slots.
    foundation::uint32 SkippedTriangles = 0; ///< Exactly zero-area source triangles excluded from traversal.
    foundation::uint32 ErrorMaterial = DEFAULT_SCENE_MATERIAL; ///< First erroneous global material index.
    foundation::uint64 ErrorSourceId = 0; ///< First erroneous instance, if binding resolution reached it.
    SceneMemoryBudget ExceededBudget = SceneMemoryBudget::None; ///< First measured byte ceiling exceeded.
    /// Candidate bytes required by ExceededBudget; zero when no byte ceiling was diagnosed.
    foundation::uint64 RequiredBytes = 0;
    /// Bytes available at the failed stage, after retained/setup usage; not total physical memory.
    foundation::uint64 AvailableBytes = 0;
};
struct RenderGraphSceneDescription;
} // namespace ludus::graphics::renderer
