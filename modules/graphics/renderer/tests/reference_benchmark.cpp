// Reference capture timing on a cooked LUSCN scene (for example `ludus-cook cook`
// output for staircase2) or on a generated Cornell tile room. Each run begins a fresh capture, so setup
// includes the CPU BVH build, scene packing and upload; the first-sample and completion times are reported
// separately, with the device time of the transport dispatches, which other processes' CPU load does not
// inflate. The radiance checksum identifies the converged image, so traversal changes that must not alter
// results can be compared bit for bit.
// When LUDUS_REFERENCE_BENCHMARK_OUTPUT names a directory, the last run's linear
// radiance is published there as reference-{gpu,cpu}.rgb32f (float32 RGB rows). With a wavelength,
// LUDUS_REFERENCE_BENCHMARK_OPTICAL_CONNECTION=utd or fraunhofer selects UTD or Fraunhofer edge diffraction
// instead of the angular spectrum.
//
// usage: ludus_reference_benchmark SCENE [WIDTH HEIGHT SAMPLES DEPTH {gpu|cpu} RUNS [WAVELENGTH_NANOMETERS]]
//
// SCENE is a cooked scene directory or cornell:ARRANGEMENT[:TILES]. The Cornell room has fixed geometry, two
// diffuse blocks and one ceiling light. Its five walls are split into TILES x TILES quads (5 or 10, default 10);
// every quad is its own object with one of five reference materials: 0 diffuse, 1 metallic-roughness, 2
// conductor, 3 coated diffuse and 4 thin dielectric. ARRANGEMENT assigns them to the wall quads: `single` makes
// every wall diffuse; `striped`, `interleaved` and `shuffled` put each material on a fifth of every wall, in
// vertical stripes, so that neighbouring quads always differ, and in a fixed random order; `only-N` puts
// material N on every wall.
//
// LUDUS_REFERENCE_BENCHMARK_KERNELS selects GPU transport kernels from reference, repeat-closest,
// repeat-visibility, permuted and optical, separated by commas (default reference). Each kernel gets its own
// renderer on one device. Runs alternate the kernel order, so slow drift on a loaded machine affects every kernel
// alike. Every kernel must reproduce the first kernel's checksum and query counts; see the wavefront hit-shading
// qualification in docs/architecture/renderer-systems.md. `optical` traces geometric captures with
// renderer_reference_optical too, so `reference,optical` measures what the optical state costs them. A positive
// WAVELENGTH_NANOMETERS dispatches renderer_reference_optical and accepts only reference and optical; the
// diagnostic kernels are geometric.
#include "renderer_authored.h"
#include "renderer_authored_shadow.h"
#include "renderer_composite.h"
#include "renderer_flat.h"
#include "renderer_output.h"
#include "renderer_reference.h"
#include "renderer_reference_optical.h"
#include "renderer_reference_permuted.h"
#include "renderer_reference_reduce.h"
#include "renderer_reference_repeat_closest.h"
#include "renderer_reference_repeat_visibility.h"
#include "renderer_shadow.h"

#include <ludus/content/content.h>
#include <ludus/foundation/base/parse_number.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/runtime/resources/scene.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace resources = ludus::resources;
namespace shaders = ludus::shaders;
using Status = rhi::RasterStatus;

namespace
{
// Defaults and budgets match the editor capture this benchmark was written for.
constexpr uint32 DEFAULT_WIDTH = 512;
constexpr uint32 DEFAULT_HEIGHT = 225;
constexpr uint32 DEFAULT_SAMPLES = 64;
constexpr uint32 DEFAULT_DEPTH = 8;
constexpr uint32 DEFAULT_RUNS = 3;
constexpr uint32 MAX_EXTENT = 4096;
constexpr uint32 MAX_SAMPLES = 4096;
constexpr uint32 MAX_DEPTH = 64;
constexpr uint32 MAX_RUNS = 64;
constexpr uint32 ROULETTE_DEPTH = 3;
constexpr uint64 SEED = 871;
constexpr usize SCENE_BYTES = usize{384} * 1024 * 1024;
constexpr usize CPU_BUDGET = usize{4096} * 1024 * 1024;
constexpr usize GPU_BUDGET = usize{2048} * 1024 * 1024;
constexpr usize RHI_HOST_BUDGET = usize{1024} * 1024 * 1024;
constexpr uint64 RHI_RESOURCE_BUDGET = uint64{4096} * 1024 * 1024;
constexpr uint32 CPU_TILE_PIXELS = 2048; // The largest tile ReferenceRenderingOptions admits.
constexpr float32 NEAR_DISTANCE = .05F;
constexpr float32 FAR_DISTANCE = 100;
constexpr uint64 TIMEOUT = 3600 * time::NANOSECONDS_PER_SECOND;
constexpr float64 NANOSECONDS_PER_MILLISECOND = 1e6;
constexpr float64 RAYS_PER_MEGARAY_MILLISECOND = 1e3;
constexpr usize RGBA8_BYTES = 4;
constexpr usize TRIANGLE_CORNERS = 3;
// A lit material retains several texture views, in addition to its parameter buffer.
constexpr uint32 REFERENCE_RESOURCES_PER_KIND = 1536;
constexpr uint32 REFERENCE_BINDING_DESCRIPTORS = 8192;
constexpr uint64 FNV_OFFSET = 0xcbf29ce484222325;
constexpr uint64 FNV_PRIME = 0x100000001b3;
// Positional arguments after the scene directory.
constexpr int WIDTH_ARGUMENT = 2;
constexpr int HEIGHT_ARGUMENT = 3;
constexpr int SAMPLES_ARGUMENT = 4;
constexpr int DEPTH_ARGUMENT = 5;
constexpr int TRAVERSAL_ARGUMENT = 6;
constexpr int RUNS_ARGUMENT = 7;
constexpr int WAVELENGTH_ARGUMENT = 8;
constexpr int ARGUMENT_COUNT = 9;
constexpr uint32 MAX_WAVELENGTH_NANOMETERS = 1000000;
constexpr float32 METRES_PER_NANOMETRE = 1e-9F;
constexpr uint32 ENVIRONMENT_FILTER_WIDTH = 64;
constexpr uint32 ENVIRONMENT_FILTER_SAMPLES = 16;

template <typename Handle>
[[nodiscard]] Status Wait(rr::Renderer& inoutRenderer, const Handle& handle, Status status) noexcept
{
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        status = inoutRenderer.GetStatus(handle);
    }
    return status;
}

[[nodiscard]] bool ParseArgument(int argc, char** argv, int index, uint32 maximum, uint32& inoutValue) noexcept
{
    if (index >= argc)
    {
        return true;
    }
    uint64 value = 0;
    if (ParseUint64(argv[index], std::strlen(argv[index]), value) != NumberParseStatus::Success || value == 0 ||
        value > maximum)
    {
        return false;
    }
    inoutValue = static_cast<uint32>(value);
    return true;
}

// --- Generated Cornell tile room -----------------------------------------------------------------------
// The same quads, in the same object order, for every arrangement, so the BVH, the diffuse blocks and the light
// are identical and only the materials of the wall quads change. Every arrangement except only-N puts each
// material on the same number of quads of every wall.
constexpr std::string_view CORNELL_PREFIX = "cornell:";
constexpr uint32 CORNELL_MATERIALS = 5;
constexpr uint32 DEFAULT_CORNELL_TILES = 2 * CORNELL_MATERIALS;
// 5 walls x 10^2 + 40 block quads + the light fit the RHI default of 1024 draws per frame.
constexpr uint32 MAX_CORNELL_TILES = 2 * CORNELL_MATERIALS;
constexpr uint32 CORNELL_BLOCK_TILES = 2;
constexpr uint32 CORNELL_LIGHT_MATERIAL = CORNELL_MATERIALS;
constexpr uint32 CORNELL_INTERLEAVED_ROW_STEP = 2; // Coprime with the material count: every neighbour differs.
constexpr float32 CORNELL_REFLECTANCE = .7F;
constexpr float32 CORNELL_METALLIC_ROUGHNESS = .5F;
constexpr float32 CORNELL_GLOSSY_ALPHA = .09F; // Perceptual roughness 0.3.
constexpr float32 CORNELL_COATING_ETA = 1.5F;
constexpr float32 CORNELL_THIN_DIELECTRIC_ETA = 1.5F;
constexpr math::Vector3 CORNELL_GOLD_ETA{.143F, .374F, 1.442F};
constexpr math::Vector3 CORNELL_GOLD_K{3.983F, 2.385F, 1.603F};
constexpr math::Vector3 CORNELL_LIGHT_RADIANCE{12, 12, 12};
constexpr float32 CORNELL_LIGHT_HALF_WIDTH = .25F;
constexpr float32 CORNELL_LIGHT_HEIGHT = .999F;
constexpr math::Vector3 CORNELL_EYE{0, 0, 3.4F};
constexpr math::Vector3 CORNELL_TARGET{0, 0, -1};
constexpr float32 CORNELL_VERTICAL_FOV_DEGREES = 48.88791F; // As tests/scenes/cornell_box/cornell-box.luscene.json.
constexpr uint64 CORNELL_SHUFFLE_SEED = 0x9e3779b97f4a7c15;
// SplitMix64's finalizer (Guy L. Steele Jr., Doug Lea and Christine H. Flood, "Fast Splittable Pseudorandom
// Number Generators", OOPSLA 2014, https://doi.org/10.1145/2660193.2660195).
constexpr uint64 SPLITMIX_FIRST_MULTIPLIER = 0xbf58476d1ce4e5b9;
constexpr uint64 SPLITMIX_SECOND_MULTIPLIER = 0x94d049bb133111eb;
constexpr uint32 SPLITMIX_FIRST_SHIFT = 30;
constexpr uint32 SPLITMIX_SECOND_SHIFT = 27;
constexpr uint32 SPLITMIX_THIRD_SHIFT = 31;
constexpr usize QUAD_CORNERS = 4;
constexpr usize CORNELL_WALL_COUNT = 5;
constexpr usize CORNELL_BLOCK_COUNT = 2;
constexpr usize BLOCK_FACES = 5;

enum class CornellArrangement : uint8
{
    Single,      ///< Diffuse walls: the single-material control (the same as only-0).
    Striped,     ///< Each wall in vertical stripes, one per material.
    Interleaved, ///< Neighbouring quads of a wall always differ in material.
    Shuffled,    ///< Each wall's striped assignment in a fixed random order.
    Only,        ///< One chosen material on every wall.
};

/// The wall assignment and subdivision of one generated room.
struct CornellLayout final
{
    CornellArrangement Arrangement = CornellArrangement::Single;
    uint32 OnlyMaterial = 0; ///< Material of every wall quad for CornellArrangement::Only.
    uint32 Tiles = DEFAULT_CORNELL_TILES;
};
/// Row and column of one quad on its face.
struct CornellTile final
{
    uint32 Row = 0;
    uint32 Column = 0;
};

struct CornellFace final
{
    math::Vector3 OriginWorld;
    math::Vector3 EdgeUWorld;
    math::Vector3 EdgeVWorld;
    math::Vector3 NormalWorld; ///< Unit; Cross(EdgeU, EdgeV) points the same way.
};

// Five walls of [-1, 1]^3, open towards the camera on +z, each facing into the room.
constexpr StaticArray<CornellFace, CORNELL_WALL_COUNT> CORNELL_WALLS{{
    {{-1, -1, 1}, {2, 0, 0}, {0, 0, -2}, {0, 1, 0}}, // floor
    {{-1, 1, -1}, {2, 0, 0}, {0, 0, 2}, {0, -1, 0}}, // ceiling
    {{-1, -1, -1}, {2, 0, 0}, {0, 2, 0}, {0, 0, 1}}, // back
    {{-1, -1, 1}, {0, 0, -2}, {0, 2, 0}, {1, 0, 0}}, // left
    {{1, -1, -1}, {0, 0, 2}, {0, 2, 0}, {-1, 0, 0}}, // right
}};

struct CornellBlock final
{
    math::Vector3 CenterWorld;
    math::Vector3 HalfExtentWorld;
};
constexpr StaticArray<CornellBlock, CORNELL_BLOCK_COUNT> CORNELL_BLOCKS{{
    {{-.35F, -.4F, -.35F}, {.28F, .6F, .28F}},
    {{.4F, -.7F, .25F}, {.28F, .3F, .28F}},
}};

[[nodiscard]] uint64 MixSplit(uint64 value) noexcept
{
    value = (value ^ (value >> SPLITMIX_FIRST_SHIFT)) * SPLITMIX_FIRST_MULTIPLIER;
    value = (value ^ (value >> SPLITMIX_SECOND_SHIFT)) * SPLITMIX_SECOND_MULTIPLIER;
    return value ^ (value >> SPLITMIX_THIRD_SHIFT);
}

[[nodiscard]] bool
AddCornellQuad(const CornellFace& face, uint32 material, resources::SceneDefinition& inoutScene) noexcept
{
    resources::SceneObject object;
    object.Mesh = 0;
    object.Material = material;
    object.SourceId = inoutScene.Objects.GetSize() + 1;
    object.Transform.Columns[0] = face.EdgeUWorld;
    object.Transform.Columns[1] = face.EdgeVWorld;
    object.Transform.Columns[2] = face.NormalWorld;
    object.Transform.Translation = face.OriginWorld;
    return inoutScene.Objects.TryAdd(object);
}

// The material of one wall quad; Shuffled starts from Striped and is permuted per wall afterwards.
[[nodiscard]] uint32 GetCornellMaterial(const CornellLayout& layout, CornellTile tile) noexcept
{
    switch (layout.Arrangement)
    {
        case CornellArrangement::Single:
            return 0;
        case CornellArrangement::Only:
            return layout.OnlyMaterial;
        case CornellArrangement::Interleaved:
            return (tile.Column + CORNELL_INTERLEAVED_ROW_STEP * tile.Row) % CORNELL_MATERIALS;
        case CornellArrangement::Striped:
        case CornellArrangement::Shuffled:
            break;
    }
    return tile.Column * CORNELL_MATERIALS / layout.Tiles;
}

// Splits a face into tiles x tiles quads in row-major order.
[[nodiscard]] bool
AddCornellTiles(const CornellFace& face, const CornellLayout& layout, resources::SceneDefinition& inoutScene) noexcept
{
    const float32 scale = 1.0F / float32(layout.Tiles);
    for (uint32 row = 0; row < layout.Tiles; ++row)
    {
        for (uint32 column = 0; column < layout.Tiles; ++column)
        {
            const CornellFace tile{face.OriginWorld + face.EdgeUWorld * (float32(column) * scale) +
                                       face.EdgeVWorld * (float32(row) * scale),
                                   face.EdgeUWorld * scale,
                                   face.EdgeVWorld * scale,
                                   face.NormalWorld};
            if (AddCornellQuad(tile, GetCornellMaterial(layout, {row, column}), inoutScene) == false)
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool AddCornellBlock(const CornellBlock& block, resources::SceneDefinition& inoutScene) noexcept
{
    const math::Vector3 low = block.CenterWorld - block.HalfExtentWorld;
    const math::Vector3 high = block.CenterWorld + block.HalfExtentWorld;
    const math::Vector3 size = high - low;
    const StaticArray<CornellFace, BLOCK_FACES> faces{{
        {{low.X, high.Y, high.Z}, {size.X, 0, 0}, {0, 0, -size.Z}, {0, 1, 0}}, // top
        {{low.X, low.Y, high.Z}, {size.X, 0, 0}, {0, size.Y, 0}, {0, 0, 1}},   // front
        {{high.X, low.Y, low.Z}, {-size.X, 0, 0}, {0, size.Y, 0}, {0, 0, -1}}, // back
        {{high.X, low.Y, high.Z}, {0, 0, -size.Z}, {0, size.Y, 0}, {1, 0, 0}}, // right
        {{low.X, low.Y, low.Z}, {0, 0, size.Z}, {0, size.Y, 0}, {-1, 0, 0}},   // left
    }};
    for (const CornellFace& face : faces)
    {
        // The blocks stay diffuse in every arrangement.
        if (AddCornellTiles(face, {CornellArrangement::Single, 0, CORNELL_BLOCK_TILES}, inoutScene) == false)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] resources::SceneMaterial MakeCornellMaterial(uint32 index) noexcept
{
    namespace format = ludus::content::scene_format;
    resources::SceneMaterial material;
    material.HasSourceMaterial = true;
    material.Reflectance = {CORNELL_REFLECTANCE, CORNELL_REFLECTANCE, CORNELL_REFLECTANCE};
    material.Roughness = 1;
    material.Scattering = {};
    switch (index)
    {
        case 0:
            material.Scattering.Model = format::MaterialModel::Diffuse;
            break;
        case 1:
            material.Scattering.Model = format::MaterialModel::MetallicRoughness;
            material.Roughness = CORNELL_METALLIC_ROUGHNESS;
            break;
        case 2:
            material.Scattering.Model = format::MaterialModel::Conductor;
            material.Scattering.Eta = {{CORNELL_GOLD_ETA.X, CORNELL_GOLD_ETA.Y, CORNELL_GOLD_ETA.Z}};
            material.Scattering.K = {{CORNELL_GOLD_K.X, CORNELL_GOLD_K.Y, CORNELL_GOLD_K.Z}};
            material.Scattering.AlphaU = CORNELL_GLOSSY_ALPHA;
            material.Scattering.AlphaV = CORNELL_GLOSSY_ALPHA;
            break;
        case 3:
            material.Scattering.Model = format::MaterialModel::CoatedDiffuse;
            material.Scattering.AlphaU = CORNELL_GLOSSY_ALPHA;
            material.Scattering.AlphaV = CORNELL_GLOSSY_ALPHA;
            material.Scattering.CoatingEta = CORNELL_COATING_ETA;
            break;
        case 4:
            material.Scattering.Model = format::MaterialModel::ThinDielectric;
            material.Scattering.Eta = {
                {CORNELL_THIN_DIELECTRIC_ETA, CORNELL_THIN_DIELECTRIC_ETA, CORNELL_THIN_DIELECTRIC_ETA}};
            material.Scattering.AlphaU = format::MIN_MATERIAL_ALPHA;
            material.Scattering.AlphaV = format::MIN_MATERIAL_ALPHA;
            break;
        default:
            material.Scattering.Model = format::MaterialModel::Diffuse;
            material.Reflectance = {};
            material.Emission = CORNELL_LIGHT_RADIANCE;
            material.IsEmissionTwoSided = false;
            break;
    }
    return material;
}

// Parses ARRANGEMENT[:TILES] after the cornell: prefix; TILES is a multiple of the material count.
[[nodiscard]] bool ParseCornell(std::string_view text, CornellLayout& outLayout) noexcept
{
    const usize separator = text.find(':');
    const std::string_view name = text.substr(0, separator);
    uint32 tiles = DEFAULT_CORNELL_TILES;
    if (separator != std::string_view::npos)
    {
        const std::string_view count = text.substr(separator + 1);
        uint64 value = 0;
        if (ParseUint64(count.data(), count.size(), value) != NumberParseStatus::Success || value == 0 ||
            value > MAX_CORNELL_TILES || value % CORNELL_MATERIALS != 0)
        {
            return false;
        }
        tiles = static_cast<uint32>(value);
    }
    constexpr std::string_view ONLY_PREFIX = "only-";
    uint32 only = 0;
    CornellArrangement arrangement = CornellArrangement::Single;
    if (name == "single")
    {
        arrangement = CornellArrangement::Single;
    }
    else if (name == "striped")
    {
        arrangement = CornellArrangement::Striped;
    }
    else if (name == "interleaved")
    {
        arrangement = CornellArrangement::Interleaved;
    }
    else if (name == "shuffled")
    {
        arrangement = CornellArrangement::Shuffled;
    }
    else if (name.size() == ONLY_PREFIX.size() + 1 && name.starts_with(ONLY_PREFIX) == true && name.back() >= '0' &&
             name.back() < char('0' + CORNELL_MATERIALS))
    {
        arrangement = CornellArrangement::Only;
        only = static_cast<uint32>(name.back() - '0');
    }
    else
    {
        return false;
    }
    outLayout = {arrangement, only, tiles};
    return true;
}

[[nodiscard]] bool MakeCornellScene(const CornellLayout& layout, resources::SceneDefinition& outScene) noexcept
{
    resources::SceneDefinition scene;
    resources::SceneMesh quad;
    constexpr StaticArray<math::Vector3, QUAD_CORNERS> QUAD_POSITIONS{{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}};
    constexpr StaticArray<uint32, 2 * TRIANGLE_CORNERS> QUAD_INDICES{{0, 1, 2, 0, 2, 3}};
    for (const math::Vector3& corner : QUAD_POSITIONS)
    {
        if (quad.Positions.TryAdd(corner) == false || quad.Normals.TryAdd({0, 0, 1}) == false ||
            quad.Texcoords.TryAdd({corner.X, corner.Y}) == false)
        {
            return false;
        }
    }
    for (const uint32 index : QUAD_INDICES)
    {
        if (quad.Indices.TryAdd(index) == false)
        {
            return false;
        }
    }
    if (scene.Meshes.TryAdd(Move(quad)) == false)
    {
        return false;
    }
    for (uint32 i = 0; i <= CORNELL_LIGHT_MATERIAL; ++i)
    {
        if (scene.Materials.TryAdd(MakeCornellMaterial(i)) == false)
        {
            return false;
        }
    }
    const usize wallQuads = usize(layout.Tiles) * layout.Tiles;
    uint64 state = CORNELL_SHUFFLE_SEED;
    for (const CornellFace& wall : CORNELL_WALLS)
    {
        const usize first = scene.Objects.GetSize();
        if (AddCornellTiles(wall, layout, scene) == false)
        {
            return false;
        }
        if (layout.Arrangement == CornellArrangement::Shuffled)
        {
            // Fisher-Yates within the wall keeps each material's quad count on every wall.
            for (usize i = wallQuads - 1; i > 0; --i)
            {
                state = MixSplit(state + CORNELL_SHUFFLE_SEED);
                const usize other = static_cast<usize>(state % (i + 1));
                const uint32 swapped = scene.Objects[first + i].Material;
                scene.Objects[first + i].Material = scene.Objects[first + other].Material;
                scene.Objects[first + other].Material = swapped;
            }
        }
    }
    for (const CornellBlock& block : CORNELL_BLOCKS)
    {
        if (AddCornellBlock(block, scene) == false)
        {
            return false;
        }
    }
    const CornellFace light{{-CORNELL_LIGHT_HALF_WIDTH, CORNELL_LIGHT_HEIGHT, -CORNELL_LIGHT_HALF_WIDTH},
                            {2 * CORNELL_LIGHT_HALF_WIDTH, 0, 0},
                            {0, 0, 2 * CORNELL_LIGHT_HALF_WIDTH},
                            {0, -1, 0}};
    if (AddCornellQuad(light, CORNELL_LIGHT_MATERIAL, scene) == false)
    {
        return false;
    }
    scene.Eye = CORNELL_EYE;
    scene.Target = CORNELL_TARGET;
    scene.Up = {0, 1, 0};
    scene.VerticalFov = math::DegreesToRadians(CORNELL_VERTICAL_FOV_DEGREES);
    outScene = Move(scene);
    return true;
}

// --- Transport kernels ----------------------------------------------------------------------------------
enum class TransportKernel : uint8
{
    Reference,        ///< The production renderer_reference recipe.
    RepeatClosest,    ///< Every closest-hit query traced twice.
    RepeatVisibility, ///< Every visibility query traced twice.
    Permuted,         ///< Threads take pixels in a fixed pseudorandom order.
    /// renderer_reference_optical for every capture: what geometric captures cost with the optical state compiled in.
    Optical,
};
constexpr usize TRANSPORT_KERNELS = 5;
constexpr StaticArray<const char*, TRANSPORT_KERNELS> TRANSPORT_KERNEL_NAMES{
    {"reference", "repeat-closest", "repeat-visibility", "permuted", "optical"}};

[[nodiscard]] rhi::RasterShaderDescription GetTraceKernel(TransportKernel kernel) noexcept
{
    switch (kernel)
    {
        case TransportKernel::RepeatClosest:
            return shaders::renderer_reference_repeat_closest::Compute();
        case TransportKernel::RepeatVisibility:
            return shaders::renderer_reference_repeat_visibility::Compute();
        case TransportKernel::Permuted:
            return shaders::renderer_reference_permuted::Compute();
        case TransportKernel::Optical:
            return shaders::renderer_reference_optical::Compute();
        case TransportKernel::Reference:
            break;
    }
    return shaders::renderer_reference::Compute();
}

// Parses a comma-separated list of distinct kernel names.
[[nodiscard]] bool ParseKernels(std::string_view text, Array<TransportKernel>& outKernels) noexcept
{
    Array<TransportKernel> kernels;
    while (text.empty() == false)
    {
        const usize separator = text.find(',');
        const std::string_view name = text.substr(0, separator);
        bool isKnown = false;
        for (usize i = 0; i < TRANSPORT_KERNELS; ++i)
        {
            if (name == TRANSPORT_KERNEL_NAMES[i])
            {
                const TransportKernel kernel = static_cast<TransportKernel>(i);
                for (const TransportKernel existing : kernels)
                {
                    if (existing == kernel)
                    {
                        return false;
                    }
                }
                if (kernels.TryAdd(kernel) == false)
                {
                    return false;
                }
                isKnown = true;
            }
        }
        if (isKnown == false)
        {
            return false;
        }
        text = separator == std::string_view::npos ? std::string_view{} : text.substr(separator + 1);
    }
    if (kernels.IsEmpty() == true)
    {
        return false;
    }
    outKernels = Move(kernels);
    return true;
}

[[nodiscard]] bool LoadScene(const resources::SceneDefinition& scene,
                             rr::Renderer& inoutRenderer,
                             rhi::DeviceHandle device,
                             rr::MaterialProgram program,
                             rr::Snapshot& outSnapshot,
                             usize& outTriangles) noexcept
{
    Array<rhi::TextureHandle> textures;
    Array<rr::MaterialVersion> materials;
    Array<rr::Mesh> meshes;
    Array<rr::SceneItem> items;
    if (textures.TryResize(scene.Textures.GetSize()) == false ||
        materials.TryResize(scene.Materials.GetSize()) == false || meshes.TryResize(scene.Meshes.GetSize()) == false ||
        items.TryResize(scene.Objects.GetSize()) == false)
    {
        return false;
    }
    for (usize i = 0; i < scene.Textures.GetSize(); ++i)
    {
        const resources::SceneTexture& source = scene.Textures[i];
        rhi::TextureDescription description{source.Width,
                                            source.Height,
                                            source.IsLinear == true ? rhi::RasterFormat::Rgba8Unorm
                                                                    : rhi::RasterFormat::Rgba8Srgb};
        description.RetainCpuContent = true;
        if (rhi::CreateTexture(device,
                               description,
                               {{source.Pixels.GetData(), source.Pixels.GetSize()}, usize(source.Width) * RGBA8_BYTES},
                               textures[i]) != Status::Ready)
        {
            std::fprintf(stderr, "texture installation failed at %zu\n", i);
            return false;
        }
    }
    for (usize i = 0; i < scene.Materials.GetSize(); ++i)
    {
        const resources::SceneMaterial& source = scene.Materials[i];
        rr::LitMaterialDescription description;
        description.AreAuthoredInputsEnabled = true;
        description.Metallic = source.Metallic;
        description.Roughness = source.Roughness;
        description.Scattering = source.Scattering;
        description.Program = source.Program;
        if (source.HasSourceMaterial == true)
        {
            description.Surface.Tint = {source.Reflectance.X, source.Reflectance.Y, source.Reflectance.Z, 1};
        }
        description.Emission = source.Emission;
        description.IsEmissionTwoSided = source.IsEmissionTwoSided;
        if (source.Texture != ~uint32{0})
        {
            description.Surface.Texture = textures[source.Texture];
            description.Surface.Sampler.Filter = rhi::RasterFilter::Linear;
            description.Surface.Sampler.U = description.Surface.Sampler.V =
                source.Repeat == true ? rhi::RasterAddress::Repeat : rhi::RasterAddress::Clamp;
            if (source.IsAlphaMasked == true)
            {
                description.Surface.Alpha = rr::MaterialAlpha::Masked;
                description.Surface.AlphaCutoff = source.AlphaCutoff;
            }
        }
        else
        {
            if (source.HasSourceMaterial == false)
            {
                description.Surface.Tint = {0, 0, 0, 1};
            }
        }
        if (source.SurfaceTexture != ~uint32{0})
        {
            description.SurfaceMap = textures[source.SurfaceTexture];
            description.NormalScale = source.NormalScale;
        }
        if (source.EmissionTexture != ~uint32{0})
        {
            description.EmissionMap = textures[source.EmissionTexture];
        }
        rr::LightSelection selection;
        const Status materialStatus =
            Wait(inoutRenderer,
                 materials[i],
                 inoutRenderer.CreateLitMaterial(program, description, materials[i], selection));
        if (materialStatus != Status::Ready)
        {
            std::fprintf(stderr,
                         "material installation failed at %zu (%.*s): status %u\n",
                         i,
                         static_cast<int>(source.Name.View().size()),
                         source.Name.View().data(),
                         static_cast<uint32>(materialStatus));
            return false;
        }
    }
    for (usize i = 0; i < scene.Meshes.GetSize(); ++i)
    {
        const resources::SceneMesh& source = scene.Meshes[i];
        rr::MeshDescription description{source.Positions.GetData(),
                                        source.Positions.GetSize(),
                                        source.Indices.GetData(),
                                        source.Indices.GetSize()};
        description.Normals = source.Normals.GetData();
        description.Texcoords = source.Texcoords.GetData();
        if (source.Tangents.GetSize() == source.Positions.GetSize())
        {
            description.Tangents = {source.Tangents.GetData(), source.Tangents.GetSize()};
        }
        if (Wait(inoutRenderer, meshes[i], inoutRenderer.CreateMesh(description, meshes[i])) != Status::Ready)
        {
            std::fprintf(stderr, "mesh installation failed at %zu\n", i);
            return false;
        }
    }
    outTriangles = 0;
    for (usize i = 0; i < scene.Objects.GetSize(); ++i)
    {
        const resources::SceneObject& source = scene.Objects[i];
        items[i].Geometry = meshes[source.Mesh];
        items[i].Surface = materials[source.Material];
        items[i].Transform = source.Transform;
        items[i].SourceId = source.SourceId != 0 ? source.SourceId : i + 1;
        outTriangles += scene.Meshes[source.Mesh].Indices.GetSize() / TRIANGLE_CORNERS;
    }
    const bool isCreated = inoutRenderer.CreateSnapshot(items.GetData(), items.GetSize(), outSnapshot) == Status::Ready;
    for (rr::Mesh& mesh : meshes)
    {
        static_cast<void>(inoutRenderer.Release(mesh));
    }
    for (rr::MaterialVersion& material : materials)
    {
        static_cast<void>(inoutRenderer.Release(material));
    }
    for (rhi::TextureHandle texture : textures)
    {
        static_cast<void>(rhi::Destroy(device, texture));
    }
    return isCreated;
}

struct RunResult final
{
    float64 FirstSampleMilliseconds = 0;
    float64 TotalMilliseconds = 0;
    rr::ReferenceCaptureInfo Info;
    uint64 Checksum = FNV_OFFSET;
    float64 MeanLuminance = 0;
    Array<math::Vector3> Radiance;
    float64 TransportMilliseconds = 0;
    uint32 TimedDispatches = 0;
    float64 ReductionMilliseconds = 0;
    uint32 TimedReductions = 0;
    uint64 DroppedPasses = 0;
};
// The ordered graph scopes each pass by name; see rhi::GpuPassTimings.
constexpr const char* TRANSPORT_PASS_NAME = "Reference compute transport";
constexpr const char* REDUCTION_PASS_NAME = "Reference compute reduction";
// Adds a completed timing batch's reference passes, then arms the next batch.
void CollectTransportTimings(rhi::DeviceHandle device, RunResult& inoutResult) noexcept
{
    rhi::GpuPassTimings timings;
    if (rhi::GetGpuPassTimings(device, timings) == Status::Ready && timings.State == rhi::GpuTimingState::Available)
    {
        inoutResult.DroppedPasses += timings.DroppedPasses;
        for (usize i = 0; i < timings.Count; ++i)
        {
            const rhi::GpuPassTiming& pass = timings.Passes[i];
            if (pass.Timing.State != rhi::GpuTimingState::Available)
            {
                continue;
            }
            const float64 milliseconds = pass.Timing.DurationNanoseconds / NANOSECONDS_PER_MILLISECOND;
            if (std::strcmp(pass.Scope.Name.GetData(), TRANSPORT_PASS_NAME) == 0)
            {
                inoutResult.TransportMilliseconds += milliseconds;
                ++inoutResult.TimedDispatches;
            }
            else if (std::strcmp(pass.Scope.Name.GetData(), REDUCTION_PASS_NAME) == 0)
            {
                inoutResult.ReductionMilliseconds += milliseconds;
                ++inoutResult.TimedReductions;
            }
        }
    }
    if (timings.State != rhi::GpuTimingState::Armed && timings.State != rhi::GpuTimingState::Pending)
    {
        static_cast<void>(rhi::ArmGpuPassTimings(device));
    }
}
// Waits for a batch still pending from the previous capture and discards it, so its passes are not counted
// against the next capture, which may use another kernel.
void DiscardPendingTimings(rhi::DeviceHandle device) noexcept
{
    const uint64 start = time::NowTicks();
    rhi::GpuPassTimings timings;
    while (rhi::GetGpuPassTimings(device, timings) == Status::Ready && timings.State == rhi::GpuTimingState::Pending &&
           time::NowTicks() - start < TIMEOUT)
    {
    }
    if (timings.State != rhi::GpuTimingState::Armed)
    {
        static_cast<void>(rhi::ArmGpuPassTimings(device));
    }
}

[[nodiscard]] bool RunCapture(rr::Renderer& inoutRenderer,
                              rhi::DeviceHandle device,
                              rr::Snapshot snapshot,
                              const rr::ReferenceCaptureDescription& description,
                              rhi::SurfaceHandle surface,
                              RunResult& outResult) noexcept
{
    DiscardPendingTimings(device);
    const uint64 start = time::NowTicks();
    rr::ReferenceCapture capture;
    Status status = inoutRenderer.BeginReferenceCapture(snapshot, description, capture);
    if (status != Status::Ready && status != Status::Pending)
    {
        std::fprintf(stderr, "capture preparation rejected: status %u\n", static_cast<uint32>(status));
        return false;
    }
    // A Ready setup still has zero samples; only Advance reports completion.
    status = Status::Pending;
    bool hasFirstSample = false;
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        CollectTransportTimings(device, outResult);
        status = inoutRenderer.AdvanceReferenceCapture(capture, surface);
        if (hasFirstSample == false &&
            inoutRenderer.GetReferenceCaptureInfo(capture, outResult.Info) == Status::Ready &&
            outResult.Info.MinimumSamples != 0)
        {
            outResult.FirstSampleMilliseconds = float64(time::NowTicks() - start) / NANOSECONDS_PER_MILLISECOND;
            hasFirstSample = true;
        }
    }
    outResult.TotalMilliseconds = float64(time::NowTicks() - start) / NANOSECONDS_PER_MILLISECOND;
    Array<rr::ReferencePixel> pixels;
    const Status informationStatus = inoutRenderer.GetReferenceCaptureInfo(capture, outResult.Info);
    if (status != Status::Ready)
    {
        std::fprintf(stderr, "capture advancement returned status %u\n", static_cast<uint32>(status));
    }
    if (status != Status::Ready || informationStatus != Status::Ready ||
        pixels.TryResize(usize(description.Width) * description.Height) == false ||
        inoutRenderer.CopyReferencePixels(capture, pixels.GetData(), pixels.GetSize()) != Status::Ready)
    {
        static_cast<void>(inoutRenderer.Release(capture));
        return false;
    }
    float64 luminance = 0;
    if (outResult.Radiance.TryResize(pixels.GetSize()) == false)
    {
        static_cast<void>(inoutRenderer.Release(capture));
        return false;
    }
    for (usize i = 0; i < pixels.GetSize(); ++i)
    {
        const rr::ReferencePixel& pixel = pixels[i];
        const math::Vector3 radiance = pixel.Emission + pixel.Direct + pixel.Indirect;
        outResult.Radiance[i] = radiance;
        luminance += .2126 * float64(radiance.X) + .7152 * float64(radiance.Y) + .0722 * float64(radiance.Z);
        const math::Vector3 parts[]{pixel.Emission, pixel.Direct, pixel.Indirect};
        for (const math::Vector3& part : parts)
        {
            for (const float32 component : {part.X, part.Y, part.Z})
            {
                uint32 bits = 0;
                std::memcpy(&bits, &component, sizeof(bits));
                outResult.Checksum = (outResult.Checksum ^ bits) * FNV_PRIME;
            }
        }
    }
    outResult.MeanLuminance = luminance / float64(pixels.GetSize());
    return inoutRenderer.Release(capture) == Status::Ready;
}
// Prepares one renderer with the given trace kernel, plus the optical kernel for optical captures, and installs the
// scene into it.
[[nodiscard]] const char* PrepareRenderer(rr::Renderer& inoutRenderer,
                                          rhi::DeviceHandle device,
                                          const resources::SceneDefinition& scene,
                                          bool isCpu,
                                          bool isOptical,
                                          TransportKernel kernel,
                                          rr::MaterialProgram& outProgram,
                                          rr::Snapshot& outSnapshot,
                                          usize& outTriangles) noexcept
{
    const rr::ReferenceRenderingOptions options{isCpu == true ? rr::ReferenceTraversal::CpuOracle
                                                              : rr::ReferenceTraversal::GpuCompute,
                                                CPU_BUDGET,
                                                0,
                                                CPU_TILE_PIXELS,
                                                isCpu == true ? usize{0} : GPU_BUDGET};
    rr::Limits limits;
    limits.Meshes = static_cast<uint32>(scene.Meshes.GetSize());
    limits.Materials = static_cast<uint32>(scene.Materials.GetSize());
    limits.Objects = static_cast<uint32>(scene.Objects.GetSize());
    if (inoutRenderer.Initialize(device,
                                 shaders::renderer_flat::Vertex(),
                                 shaders::renderer_flat::Fragment(),
                                 limits) != Status::Ready)
    {
        return "renderer initialization";
    }
    Status enabled = Status::Ready;
    if (isCpu == true)
    {
        enabled = inoutRenderer.EnableReferenceRendering({}, options);
    }
    else if (isOptical == true)
    {
        enabled = inoutRenderer.EnableReferenceRendering(GetTraceKernel(kernel),
                                                         shaders::renderer_reference_optical::Compute(),
                                                         shaders::renderer_reference_reduce::Compute(),
                                                         options);
    }
    else
    {
        enabled = inoutRenderer.EnableReferenceRendering(GetTraceKernel(kernel),
                                                         shaders::renderer_reference_reduce::Compute(),
                                                         options);
    }
    if (enabled != Status::Ready)
    {
        return "reference rendering";
    }
    if (inoutRenderer.InitializeViews(shaders::renderer_composite::Vertex(), shaders::renderer_composite::Fragment()) !=
            Status::Ready ||
        inoutRenderer.InitializeLighting(shaders::renderer_output::Vertex(),
                                         shaders::renderer_output::Fragment(),
                                         shaders::renderer_shadow::Fragment()) != Status::Ready ||
        Wait(inoutRenderer,
             outProgram,
             inoutRenderer.PrewarmLitProgram(shaders::renderer_authored::Vertex(),
                                             shaders::renderer_authored::Fragment(),
                                             shaders::renderer_authored_shadow::Fragment(),
                                             outProgram)) != Status::Ready)
    {
        return "lighting programs";
    }
    if (LoadScene(scene, inoutRenderer, device, outProgram, outSnapshot, outTriangles) == false)
    {
        return "scene installation";
    }
    return nullptr;
}

[[nodiscard]] float64 GetMedian(Array<float64>& inoutValues) noexcept
{
    if (inoutValues.IsEmpty() == true)
    {
        return 0;
    }
    Sort(inoutValues.GetData(), inoutValues.GetSize(), [](float64 left, float64 right) noexcept {
        return left < right;
    });
    return inoutValues[inoutValues.GetSize() / 2];
}

// Per-kernel measurements over every run, in run order.
struct KernelMeasurements final
{
    Array<float64> FirstSampleMilliseconds;
    Array<float64> TotalMilliseconds;
    Array<float64> TransportPerDispatchMilliseconds;
    Array<float64> ReductionPerDispatchMilliseconds;
    uint64 TimedDispatches = 0;
    uint64 TimedReductions = 0;
    uint64 DroppedPasses = 0;
};

} // namespace

int main(int argc, char** argv)
{
    rr::ReferenceCaptureDescription description;
    description.Width = DEFAULT_WIDTH;
    description.Height = DEFAULT_HEIGHT;
    description.TargetSamples = DEFAULT_SAMPLES;
    description.MaxDepth = DEFAULT_DEPTH;
    description.RouletteDepth = ROULETTE_DEPTH;
    description.Seed = SEED;
    uint32 runs = DEFAULT_RUNS;
    uint32 wavelengthNanometers = 0;
    const bool isCpu = argc > TRAVERSAL_ARGUMENT && std::strcmp(argv[TRAVERSAL_ARGUMENT], "cpu") == 0;
    Array<TransportKernel> kernels;
    const char* kernelsOrNull = std::getenv("LUDUS_REFERENCE_BENCHMARK_KERNELS");
    const bool hasKernels = ParseKernels(kernelsOrNull != nullptr ? kernelsOrNull : TRANSPORT_KERNEL_NAMES[0], kernels);
    if (argc < 2 || argc > ARGUMENT_COUNT ||
        ParseArgument(argc, argv, WIDTH_ARGUMENT, MAX_EXTENT, description.Width) == false ||
        ParseArgument(argc, argv, HEIGHT_ARGUMENT, MAX_EXTENT, description.Height) == false ||
        ParseArgument(argc, argv, SAMPLES_ARGUMENT, MAX_SAMPLES, description.TargetSamples) == false ||
        ParseArgument(argc, argv, DEPTH_ARGUMENT, MAX_DEPTH, description.MaxDepth) == false ||
        ParseArgument(argc, argv, RUNS_ARGUMENT, MAX_RUNS, runs) == false ||
        ParseArgument(argc, argv, WAVELENGTH_ARGUMENT, MAX_WAVELENGTH_NANOMETERS, wavelengthNanometers) == false ||
        (argc > TRAVERSAL_ARGUMENT && isCpu == false && std::strcmp(argv[TRAVERSAL_ARGUMENT], "gpu") != 0) ||
        hasKernels == false || (isCpu == true && kernels.GetSize() != 1))
    {
        std::fprintf(
            stderr,
            "usage: %s {SCENE_DIRECTORY|cornell:ARRANGEMENT[:TILES]} [WIDTH HEIGHT SAMPLES DEPTH {gpu|cpu} RUNS "
            "[WAVELENGTH_NANOMETERS]]\n"
            "LUDUS_REFERENCE_BENCHMARK_KERNELS: distinct comma-separated GPU kernels from reference, "
            "repeat-closest, repeat-visibility, permuted and optical\n",
            argc > 0 ? argv[0] : "ludus_reference_benchmark");
        return 1;
    }
    description.OpticalWavelengthWorld = static_cast<float32>(wavelengthNanometers) * METRES_PER_NANOMETRE;
    for (const TransportKernel kernel : kernels)
    {
        if (wavelengthNanometers > 0 && kernel != TransportKernel::Reference && kernel != TransportKernel::Optical)
        {
            std::fprintf(stderr, "WAVELENGTH_NANOMETERS needs the reference or optical kernel\n");
            return 1;
        }
    }
    // LUDUS_REFERENCE_BENCHMARK_OPTICAL_CONNECTION=utd or fraunhofer selects an edge-diffraction operator for a
    // wavelength's connections.
    const char* connectionOrNull = std::getenv("LUDUS_REFERENCE_BENCHMARK_OPTICAL_CONNECTION");
    if (connectionOrNull != nullptr && std::strcmp(connectionOrNull, "utd") == 0)
    {
        description.OpticalConnection = rr::ReferenceOpticalConnection::UtdEdgeDiffraction;
    }
    else if (connectionOrNull != nullptr && std::strcmp(connectionOrNull, "fraunhofer") == 0)
    {
        description.OpticalConnection = rr::ReferenceOpticalConnection::FraunhoferEdgeDiffraction;
    }
    else if (connectionOrNull != nullptr && std::strcmp(connectionOrNull, "angular-spectrum") != 0)
    {
        std::fprintf(stderr,
                     "LUDUS_REFERENCE_BENCHMARK_OPTICAL_CONNECTION must be utd, fraunhofer or angular-spectrum\n");
        return 1;
    }
    resources::SceneDefinition scene;
    const std::string_view sceneName = argv[1];
    if (sceneName.starts_with(CORNELL_PREFIX) == true)
    {
        CornellLayout layout;
        if (ParseCornell(sceneName.substr(CORNELL_PREFIX.size()), layout) == false ||
            MakeCornellScene(layout, scene) == false)
        {
            std::fprintf(stderr, "could not generate %s\n", argv[1]);
            return 2;
        }
    }
    else
    {
        ludus::content::Bytes bundle;
        if (ludus::content::ReadFile(argv[1], "scene.luscn", SCENE_BYTES, bundle) != ludus::content::Status::Ok ||
            resources::SceneDefinition::Decode(bundle.Data().data(), bundle.Data().size(), scene) !=
                resources::Status::Ok)
        {
            std::fprintf(stderr, "could not read %s/scene.luscn\n", argv[1]);
            return 2;
        }
    }
    math::Matrix4 view;
    math::Matrix4 projection;
    if (math::TryLookAt(scene.Eye, scene.Target, scene.Up, view) != math::MathStatus::Success ||
        math::TryPerspectiveReverseZ(scene.VerticalFov,
                                     float32(description.Width) / float32(description.Height),
                                     NEAR_DISTANCE,
                                     FAR_DISTANCE,
                                     projection) != math::MathStatus::Success)
    {
        return 3;
    }
    description.WorldToClip = projection * view;
    description.CameraPosition = scene.Eye;
    description.ReverseZ = true;
    description.Environment = scene.Environment.Width == 0 ? scene.Environment.Rgb : math::Vector3{};
    Array<rr::DirectLight> authoredLights;
    for (const resources::SceneLight& source : scene.Lights)
    {
        if (source.IsEnabled == false)
        {
            continue;
        }
        rr::DirectLight light;
        light.SourceId = source.SourceId;
        light.Kind =
            source.Kind == resources::SceneLightKind::Directional
                ? rr::LightKind::Directional
                : (source.Kind == resources::SceneLightKind::Spot ? rr::LightKind::Spot : rr::LightKind::Point);
        light.Color = source.IntensityRgb;
        light.Intensity = 1;
        light.Position = source.PositionWorld;
        light.Direction = source.DirectionWorld;
        light.Range = source.Range;
        light.InnerCos = source.InnerCos;
        light.OuterCos = source.OuterCos;
        if (authoredLights.TryAdd(light) == false)
        {
            return 3;
        }
    }
    description.Lights = authoredLights.GetData();
    description.LightCount = authoredLights.GetSize();

    rhi::DeviceDescription request;
    request.Storage.ResourcesPerKind = REFERENCE_RESOURCES_PER_KIND;
    request.Storage.BindingDescriptors = REFERENCE_BINDING_DESCRIPTORS;
    request.Storage.HostBytes = RHI_HOST_BUDGET;
    request.Storage.ResourceBytes = RHI_RESOURCE_BUDGET;
    request.Required.PortableRaster = true;
    request.Required.Compute = isCpu == false;
    request.EnableGpuTiming = isCpu == false;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    if (rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, request, device, surface) != rhi::DeviceStatus::Ready)
    {
        std::fprintf(stderr, "no device with the requested capabilities\n");
        return 4;
    }
    struct Shutdown final
    {
        ~Shutdown() noexcept
        {
            rhi::Shutdown();
        }
    } shutdown;
    int exitCode = 0;
    {
        StaticArray<rr::Renderer, TRANSPORT_KERNELS> renderers;
        StaticArray<rr::MaterialProgram, TRANSPORT_KERNELS> programs;
        StaticArray<rr::Snapshot, TRANSPORT_KERNELS> snapshots;
        usize triangles = 0;
        for (usize k = 0; k < kernels.GetSize(); ++k)
        {
            const char* failureOrNull = PrepareRenderer(renderers[k],
                                                        device,
                                                        scene,
                                                        isCpu,
                                                        wavelengthNanometers > 0,
                                                        kernels[k],
                                                        programs[k],
                                                        snapshots[k],
                                                        triangles);
            if (failureOrNull != nullptr)
            {
                std::fprintf(stderr,
                             "setup failed for %s: %s\n",
                             TRANSPORT_KERNEL_NAMES[usize(kernels[k])],
                             failureOrNull);
                return 5;
            }
        }
        std::printf("scene %s: %zu objects, %zu triangles; %ux%u, %u spp, depth %u, %s\n",
                    argv[1],
                    scene.Objects.GetSize(),
                    triangles,
                    description.Width,
                    description.Height,
                    description.TargetSamples,
                    description.MaxDepth,
                    isCpu == true ? "CPU oracle" : "GPU compute");
        rr::EnvironmentMap environment;
        if (scene.Environment.Width != 0)
        {
            Array<math::Vector4> pixels;
            if (pixels.TryResize(scene.Environment.Pixels.GetSize()) == false)
            {
                return 5;
            }
            for (usize index = 0; index < pixels.GetSize(); ++index)
            {
                const math::Vector3 value = math::Hadamard(scene.Environment.Pixels[index], scene.Environment.Rgb);
                pixels[index] = {value.X, value.Y, value.Z, 1};
            }
            rr::EnvironmentDescription environmentDescription;
            environmentDescription.PixelsOrNull = pixels.GetData();
            environmentDescription.Width = scene.Environment.Width;
            environmentDescription.Height = scene.Environment.Height;
            environmentDescription.FilterWidth = ENVIRONMENT_FILTER_WIDTH;
            environmentDescription.FilterSamples = ENVIRONMENT_FILTER_SAMPLES;
            environmentDescription.CpuByteBudget = CPU_BUDGET;
            Status environmentStatus = environment.Initialize(device, environmentDescription);
            const uint64 environmentStart = time::NowTicks();
            while (environmentStatus == Status::Pending && time::NowTicks() - environmentStart < TIMEOUT)
            {
                environmentStatus = environment.GetStatus();
            }
            if (environmentStatus != Status::Ready)
            {
                std::fprintf(stderr, "authored environment preparation failed\n");
                return 5;
            }
            description.EnvironmentTextureOrNull = &environment;
        }
        std::printf("optical wavelength %u nm; %zu authored analytic lights; environment %ux%u\n",
                    wavelengthNanometers,
                    authoredLights.GetSize(),
                    scene.Environment.Width,
                    scene.Environment.Height);
        StaticArray<KernelMeasurements, TRANSPORT_KERNELS> measurements;
        RunResult baseline; // The first kernel's first run; every later capture must reproduce it.
        bool hasBaseline = false;
        RunResult result; // The last run's radiance is published below.
        for (uint32 run = 0; run < runs && exitCode == 0; ++run)
        {
            for (usize step = 0; step < kernels.GetSize() && exitCode == 0; ++step)
            {
                // Reverse the order on odd runs: A B C, C B A, A B C, ...
                const usize k = run % 2 == 0 ? step : kernels.GetSize() - 1 - step;
                const char* name = TRANSPORT_KERNEL_NAMES[usize(kernels[k])];
                result = RunResult{};
                if (RunCapture(renderers[k], device, snapshots[k], description, surface, result) == false ||
                    result.Info.InvalidContributions != 0)
                {
                    std::fprintf(stderr,
                                 "%s capture %u failed: pixel %u depth %u sample %u reason %u, invalid %llu\n",
                                 name,
                                 run,
                                 result.Info.FailurePixel,
                                 result.Info.FailureDepth,
                                 result.Info.FailureSample,
                                 static_cast<uint32>(result.Info.Failure),
                                 static_cast<unsigned long long>(result.Info.InvalidContributions));
                    exitCode = 6;
                    break;
                }
                if (hasBaseline == false && k == 0)
                {
                    baseline.Checksum = result.Checksum;
                    baseline.Info = result.Info;
                    hasBaseline = true;
                }
                if (hasBaseline == true && (result.Checksum != baseline.Checksum ||
                                            result.Info.ClosestQueries != baseline.Info.ClosestQueries ||
                                            result.Info.VisibilityQueries != baseline.Info.VisibilityQueries ||
                                            result.Info.MinimumSamples != baseline.Info.MinimumSamples))
                {
                    std::fprintf(stderr,
                                 "%s run %u differs from %s: checksum %016llx, %llu closest, %llu visibility\n",
                                 name,
                                 run,
                                 TRANSPORT_KERNEL_NAMES[usize(kernels[0])],
                                 static_cast<unsigned long long>(result.Checksum),
                                 static_cast<unsigned long long>(result.Info.ClosestQueries),
                                 static_cast<unsigned long long>(result.Info.VisibilityQueries));
                    exitCode = 8;
                    break;
                }
                KernelMeasurements& kernel = measurements[k];
                const float64 transportPerDispatch =
                    result.TimedDispatches == 0 ? 0 : result.TransportMilliseconds / result.TimedDispatches;
                const float64 reductionPerDispatch =
                    result.TimedReductions == 0 ? 0 : result.ReductionMilliseconds / result.TimedReductions;
                if (kernel.FirstSampleMilliseconds.TryAdd(result.FirstSampleMilliseconds) == false ||
                    kernel.TotalMilliseconds.TryAdd(result.TotalMilliseconds) == false ||
                    kernel.TransportPerDispatchMilliseconds.TryAdd(transportPerDispatch) == false ||
                    kernel.ReductionPerDispatchMilliseconds.TryAdd(reductionPerDispatch) == false)
                {
                    exitCode = 6;
                    break;
                }
                kernel.TimedDispatches += result.TimedDispatches;
                kernel.TimedReductions += result.TimedReductions;
                kernel.DroppedPasses += result.DroppedPasses;
                const float64 queries = float64(result.Info.ClosestQueries + result.Info.VisibilityQueries);
                std::printf("%s run %u: %u-%u spp, first sample %.1f ms, total %.1f ms, GPU transport %.1f ms in %u "
                            "timed dispatches (%.3f ms each; reduction %.3f ms), %.2f Mqueries/s (%llu closest, %llu "
                            "visibility), mean luminance %.6f, checksum %016llx\n",
                            name,
                            run,
                            result.Info.MinimumSamples,
                            result.Info.MaximumSamples,
                            result.FirstSampleMilliseconds,
                            result.TotalMilliseconds,
                            result.TransportMilliseconds,
                            result.TimedDispatches,
                            transportPerDispatch,
                            reductionPerDispatch,
                            queries / (result.TotalMilliseconds * RAYS_PER_MEGARAY_MILLISECOND),
                            static_cast<unsigned long long>(result.Info.ClosestQueries),
                            static_cast<unsigned long long>(result.Info.VisibilityQueries),
                            result.MeanLuminance,
                            static_cast<unsigned long long>(result.Checksum));
            }
        }
        const char* outputOrNull = std::getenv("LUDUS_REFERENCE_BENCHMARK_OUTPUT");
        if (exitCode == 0 && outputOrNull != nullptr)
        {
            filesystem::WriteDirectory directory;
            if (directory.Open(outputOrNull).Succeeded() == false ||
                directory
                        .Publish(isCpu == true ? "reference-cpu.rgb32f" : "reference-gpu.rgb32f",
                                 {reinterpret_cast<const uint8*>(result.Radiance.GetData()),
                                  result.Radiance.GetSize() * sizeof(math::Vector3)})
                        .Outcome.Succeeded() == false)
            {
                std::fprintf(stderr, "could not publish radiance to %s\n", outputOrNull);
                exitCode = 7;
            }
        }
        // Each sample is one transport dispatch, so the per-dispatch median scales to the whole capture even
        // when the timing batches cover only some dispatches.
        const float64 samples = float64(description.TargetSamples);
        Array<float64> referencePerDispatch;
        for (const float64 value : measurements[0].TransportPerDispatchMilliseconds)
        {
            static_cast<void>(referencePerDispatch.TryAdd(value));
        }
        for (usize k = 0; k < kernels.GetSize(); ++k)
        {
            KernelMeasurements& kernel = measurements[k];
            const usize count = kernel.TotalMilliseconds.GetSize();
            if (count == 0)
            {
                continue;
            }
            // Paired per-run difference from the first kernel, whose runs sit next to this kernel's.
            Array<float64> differences;
            for (usize i = 0; i < count && i < referencePerDispatch.GetSize(); ++i)
            {
                static_cast<void>(
                    differences.TryAdd(kernel.TransportPerDispatchMilliseconds[i] - referencePerDispatch[i]));
            }
            const float64 transport = GetMedian(kernel.TransportPerDispatchMilliseconds);
            std::printf("%s median over %zu runs: total %.1f ms, first sample %.1f ms, transport %.3f ms per dispatch "
                        "(%.1f ms per capture), paired difference %+.3f ms per dispatch, reduction %.3f ms; timed "
                        "%llu of %llu dispatches, %llu dropped passes\n",
                        TRANSPORT_KERNEL_NAMES[usize(kernels[k])],
                        count,
                        GetMedian(kernel.TotalMilliseconds),
                        GetMedian(kernel.FirstSampleMilliseconds),
                        transport,
                        transport * samples,
                        GetMedian(differences),
                        GetMedian(kernel.ReductionPerDispatchMilliseconds),
                        static_cast<unsigned long long>(kernel.TimedDispatches),
                        static_cast<unsigned long long>(count) * description.TargetSamples,
                        static_cast<unsigned long long>(kernel.DroppedPasses));
        }
        for (usize k = 0; k < kernels.GetSize(); ++k)
        {
            static_cast<void>(renderers[k].Release(snapshots[k]));
        }
    }
    return exitCode;
}
