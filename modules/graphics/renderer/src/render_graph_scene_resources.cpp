#include "internal/render_graph_scene_resources.h"
#include "internal/raster_mesh_light_grid.h"
#include "internal/raster_mesh_light_shadows.h"
#include "internal/raster_mesh_lights.h"
#include "internal/scene_material_validation.h"
#include <ludus/graphics/renderer/deferred_layout.h>

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/queries.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/graphics/renderer/environment.hpp>
#include <ludus/graphics/renderer/lighting.hpp>

#include <cstring>
#include <new>

// Thanks to Alegruz, "Efficient Rendering Study Notes" (19 July 2022),
// sections "Forward Rendering" and "Single Pass Lighting": scene material/light inputs
// feed one geometry shading pass. The portable clustered extension reuses L5's
// conservative spatial grid and full-list overflow fallback. Design and limits:
// docs/architecture/shared-scene-shader-contract.md#authored-forward-material-execution
// https://alegruz.github.io/graphics/2022/07/19/efficient-rendering-study-notes.html
namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace format = content::render_graph_format;
namespace scene_format = content::scene_format;
using Status = rhi::RasterStatus;
namespace
{
constexpr usize RETAINED_GENERATION_COUNT = 3;
constexpr usize CACHE_MESH_COUNT = format::MAX_ITEMS * RETAINED_GENERATION_COUNT;
constexpr usize MATERIAL_TEXTURE_COUNT = 3;
constexpr usize DRAW_TEXTURE_COUNT = MATERIAL_TEXTURE_COUNT + 1;
constexpr usize CACHE_TEXTURE_COUNT = format::MAX_ITEMS * DRAW_TEXTURE_COUNT * RETAINED_GENERATION_COUNT;
constexpr usize CHANNEL_COUNT = 4;
constexpr usize TRIANGLE_CORNER_COUNT = 3;
constexpr uint32 DIRECTIONAL_SHADOW_GUTTER_TEXELS = 1;
constexpr uint32 DIRECTIONAL_SHADOW_INTERIOR_TEXELS = DIRECTIONAL_SHADOW_TEXELS - 2 * DIRECTIONAL_SHADOW_GUTTER_TEXELS;
/// Padding keeps degenerate scene bounds projectable even with the light parallel to the plane.
constexpr math::Vector3 DIRECTIONAL_SHADOW_PADDING_METERS{0.5F, 0.5F, 0.5F};

Status BuildDirectionalShadowRegion(BorrowedView<const DirectLight> lights,
                                    const math::Aabb3& boundsWorld,
                                    ShadowRegion& outRegion) noexcept
{
    if (math::IsEmpty(boundsWorld) == true)
    {
        return Status::Ready;
    }
    const DirectLight* selectedOrNull = nullptr;
    for (usize index = 0; index < lights.GetSize(); ++index)
    {
        const DirectLight& light = lights[index];
        if (light.Kind == LightKind::Directional && light.ShouldCastRasterShadows == true && light.Intensity > 0 &&
            light.Color != math::Vector3{} && (selectedOrNull == nullptr || light.SourceId < selectedOrNull->SourceId))
        {
            selectedOrNull = &light;
        }
    }
    if (selectedOrNull == nullptr)
    {
        return Status::Ready;
    }
    ShadowRegion region;
    region.Light = *selectedOrNull;
    region.Interior = {DIRECTIONAL_SHADOW_GUTTER_TEXELS,
                       DIRECTIONAL_SHADOW_GUTTER_TEXELS,
                       DIRECTIONAL_SHADOW_INTERIOR_TEXELS,
                       DIRECTIONAL_SHADOW_INTERIOR_TEXELS};
    const Status status = BuildDirectionalShadowMatrix(
        {boundsWorld.Min - DIRECTIONAL_SHADOW_PADDING_METERS, boundsWorld.Max + DIRECTIONAL_SHADOW_PADDING_METERS},
        region.Light.Direction,
        DIRECTIONAL_SHADOW_INTERIOR_TEXELS,
        region.WorldToClip);
    if (status == Status::Ready)
    {
        outRegion = region;
    }
    return status;
}
enum MeshKeyPart : uint8
{
    POSITIONS,
    INDICES,
    TEXCOORDS,
    NORMALS,
    BAKE_TEXCOORDS,
    TANGENTS,
    COLORS,
    MATERIAL_TEXCOORDS,
    MESH_KEY_PART_COUNT,
};
constexpr usize NO_RESOURCE = ~usize{0};
// Separate CPU identity-copy budget, covering current/pending/idle assets together.
// RHI independently admits GPU allocations and upload staging against its device budgets.
constexpr usize CACHE_IDENTITY_BYTES = usize{128} * 1024 * 1024 * (sizeof(usize) > sizeof(uint32) ? 8 : 1);
// A 4096x2048 source retains RGBA32F and its sampling distribution, with a
// temporary RGB32F encoding during preparation. Keep this peak separate from texel upload bytes.
constexpr usize ENVIRONMENT_CPU_BYTES = usize{384} * 1024 * 1024;
constexpr uint32 ENVIRONMENT_FILTER_WIDTH = 32;
constexpr uint32 ENVIRONMENT_FILTER_SAMPLES = 16;
bool IsAccepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
bool AreBytesEqual(const Array<uint8>& left, const Array<uint8>& right) noexcept
{
    return left.GetSize() == right.GetSize() &&
           (left.IsEmpty() == true || std::memcmp(left.GetData(), right.GetData(), left.GetSize()) == 0);
}
template <typename T>
bool Append(Array<uint8>& inoutBytes, const T* valuesOrNull, usize count) noexcept
{
    usize bytes = 0;
    if (TryMultiply(count, sizeof(T), bytes) == false || bytes > CACHE_IDENTITY_BYTES ||
        inoutBytes.GetSize() > CACHE_IDENTITY_BYTES - bytes || (count != 0 && valuesOrNull == nullptr))
    {
        return false;
    }
    return bytes == 0 || inoutBytes.TryAddRange({reinterpret_cast<const uint8*>(valuesOrNull), bytes}) == true;
}
bool MakeMeshKey(const MeshDescription& mesh, Array<uint8>& outBytes) noexcept
{
    if (mesh.VertexCount == 0 || mesh.IndexCount == 0 || mesh.IndexCount % TRIANGLE_CORNER_COUNT != 0 ||
        mesh.Normals == nullptr || mesh.Tangents.IsValid() == false || mesh.Colors.IsValid() == false ||
        mesh.MaterialTexcoords1.IsValid() == false ||
        (mesh.Tangents.IsEmpty() == false && mesh.Tangents.GetSize() != mesh.VertexCount) ||
        (mesh.Colors.IsEmpty() == false && mesh.Colors.GetSize() != mesh.VertexCount) ||
        (mesh.MaterialTexcoords1.IsEmpty() == false && mesh.MaterialTexcoords1.GetSize() != mesh.VertexCount))
    {
        return false;
    }
    const StaticArray<usize, MESH_KEY_PART_COUNT> counts{{mesh.VertexCount,
                                                          mesh.IndexCount,
                                                          mesh.Texcoords == nullptr ? 0 : mesh.VertexCount,
                                                          mesh.VertexCount,
                                                          mesh.BakeTexcoords == nullptr ? 0 : mesh.VertexCount,
                                                          mesh.Tangents.GetSize(),
                                                          mesh.Colors.GetSize(),
                                                          mesh.MaterialTexcoords1.GetSize()}};
    return Append(outBytes, counts.GetData(), counts.GetSize()) == true &&
           Append(outBytes, mesh.Positions, counts[POSITIONS]) == true &&
           Append(outBytes, mesh.Indices, counts[INDICES]) == true &&
           Append(outBytes, mesh.Texcoords, counts[TEXCOORDS]) == true &&
           Append(outBytes, mesh.Normals, counts[NORMALS]) == true &&
           Append(outBytes, mesh.BakeTexcoords, counts[BAKE_TEXCOORDS]) == true &&
           Append(outBytes, mesh.Tangents.GetDataOrNull(), counts[TANGENTS]) == true &&
           Append(outBytes, mesh.Colors.GetDataOrNull(), counts[COLORS]) == true &&
           Append(outBytes, mesh.MaterialTexcoords1.GetDataOrNull(), counts[MATERIAL_TEXCOORDS]) == true;
}
struct SurfaceChartMesh final
{
    Array<math::Vector3> PositionsMesh;
    Array<math::Vector3> NormalsMesh;
    Array<math::Vector2> MaterialTexcoords;
    Array<math::Vector2> SecondaryTexcoords;
    Array<math::Vector2> BakeTexcoords;
    Array<math::Vector4> TangentsMesh;
    Array<math::Vector4> Colors;
    Array<uint32> Indices;
};

template <typename T>
bool ExpandChartAttribute(const MeshDescription& mesh, const T* valuesOrNull, Array<T>& outValues) noexcept
{
    if (valuesOrNull == nullptr)
    {
        return true;
    }
    if (outValues.TryResize(mesh.IndexCount) == false)
    {
        return false;
    }
    for (usize index = 0; index < mesh.IndexCount; ++index)
    {
        outValues[index] = valuesOrNull[mesh.Indices[index]];
    }
    return true;
}

bool BuildSurfaceChartMesh(const MeshDescription& source,
                           const scene_format::BakeChartLayout& layout,
                           SurfaceChartMesh& outMesh,
                           MeshDescription& outGeometry) noexcept
{
    if (source.Positions == nullptr || source.Normals == nullptr || source.Indices == nullptr ||
        source.IndexCount > scene_format::MAX_VERTICES || source.Tangents.IsValid() == false ||
        source.Colors.IsValid() == false || source.MaterialTexcoords1.IsValid() == false ||
        (source.Tangents.IsEmpty() == false && source.Tangents.GetSize() != source.VertexCount) ||
        (source.Colors.IsEmpty() == false && source.Colors.GetSize() != source.VertexCount) ||
        (source.MaterialTexcoords1.IsEmpty() == false && source.MaterialTexcoords1.GetSize() != source.VertexCount))
    {
        return false;
    }
    for (usize index = 0; index < source.IndexCount; ++index)
    {
        if (source.Indices[index] >= source.VertexCount)
        {
            return false;
        }
    }
    if (ExpandChartAttribute(source, source.Positions, outMesh.PositionsMesh) == false ||
        ExpandChartAttribute(source, source.Normals, outMesh.NormalsMesh) == false ||
        ExpandChartAttribute(source, source.Texcoords, outMesh.MaterialTexcoords) == false ||
        ExpandChartAttribute(source, source.MaterialTexcoords1.GetDataOrNull(), outMesh.SecondaryTexcoords) == false ||
        ExpandChartAttribute(source, source.Tangents.GetDataOrNull(), outMesh.TangentsMesh) == false ||
        ExpandChartAttribute(source, source.Colors.GetDataOrNull(), outMesh.Colors) == false ||
        outMesh.BakeTexcoords.TryResize(source.IndexCount) == false ||
        outMesh.Indices.TryResize(source.IndexCount) == false)
    {
        return false;
    }
    for (usize index = 0; index < source.IndexCount; ++index)
    {
        if (scene_format::TryGetBakeChartUv(layout,
                                            static_cast<uint32>(index / TRIANGLE_CORNER_COUNT),
                                            static_cast<uint32>(index % TRIANGLE_CORNER_COUNT),
                                            outMesh.BakeTexcoords[index].X,
                                            outMesh.BakeTexcoords[index].Y) == false)
        {
            return false;
        }
        outMesh.Indices[index] = static_cast<uint32>(index);
    }
    outGeometry = source;
    outGeometry.Positions = outMesh.PositionsMesh.GetData();
    outGeometry.Normals = outMesh.NormalsMesh.GetData();
    outGeometry.VertexCount = source.IndexCount;
    outGeometry.Indices = outMesh.Indices.GetData();
    outGeometry.Texcoords = outMesh.MaterialTexcoords.GetData();
    outGeometry.BakeTexcoords = outMesh.BakeTexcoords.GetData();
    outGeometry.Tangents = {outMesh.TangentsMesh.GetData(), outMesh.TangentsMesh.GetSize()};
    outGeometry.Colors = {outMesh.Colors.GetData(), outMesh.Colors.GetSize()};
    outGeometry.MaterialTexcoords1 = {outMesh.SecondaryTexcoords.GetData(), outMesh.SecondaryTexcoords.GetSize()};
    return true;
}
} // namespace
struct ForwardSceneResources::State final
{
    struct MeshEntry final
    {
        Mesh Handle{};
        Array<uint8> Key;
        usize Pins = 0;
        bool IsUsed = false;
    };
    struct TextureEntry final
    {
        rhi::TextureHandle Handle{};
        Array<uint8> Key;
        uint32 Width = 0;
        uint32 Height = 0;
        rhi::RasterFormat Format = rhi::RasterFormat::Rgba8Unorm;
        usize Pins = 0;
        bool IsUsed = false;
    };
    rhi::DeviceHandle Device{};
    Renderer* RendererOrNull = nullptr;
    MaterialProgram Program{};
    MaterialProgram SurfaceChartProgram{};
    bool IsClustered = false;
    bool HasMeshLights = false;
    bool HasSurfaceCharts = false;
    StaticArray<MeshEntry, CACHE_MESH_COUNT> Meshes{};
    StaticArray<TextureEntry, CACHE_TEXTURE_COUNT> Textures{};
    usize IdentityBytes = 0;
    RenderGraphSceneResourceReport Report{};
    ~State() noexcept
    {
        for (MeshEntry& entry : Meshes)
        {
            static_cast<void>(RendererOrNull->Release(entry.Handle));
        }
        for (TextureEntry& entry : Textures)
        {
            static_cast<void>(rhi::Destroy(Device, entry.Handle));
        }
    }
    void EvictIdle() noexcept
    {
        for (MeshEntry& entry : Meshes)
        {
            if (entry.IsUsed == true && entry.Pins == 0)
            {
                IdentityBytes -= entry.Key.GetSize();
                static_cast<void>(RendererOrNull->Release(entry.Handle));
                entry = {};
            }
        }
        for (TextureEntry& entry : Textures)
        {
            if (entry.IsUsed == true && entry.Pins == 0)
            {
                IdentityBytes -= entry.Key.GetSize();
                static_cast<void>(rhi::Destroy(Device, entry.Handle));
                entry = {};
            }
        }
    }
    Status AcquireMesh(const MeshDescription& mesh, usize& outIndex) noexcept
    {
        Array<uint8> key;
        if (MakeMeshKey(mesh, key) == false)
        {
            return Status::InvalidDescription;
        }
        for (usize i = 0; i < Meshes.GetSize(); ++i)
        {
            MeshEntry& entry = Meshes[i];
            if (entry.IsUsed == true && AreBytesEqual(entry.Key, key) == true)
            {
                const Status status = RendererOrNull->GetStatus(entry.Handle);
                if (IsAccepted(status) == false)
                {
                    if (entry.Pins != 0)
                    {
                        return status;
                    }
                    IdentityBytes -= entry.Key.GetSize();
                    static_cast<void>(RendererOrNull->Release(entry.Handle));
                    entry = {};
                    continue;
                }
                ++entry.Pins;
                ++Report.ReusedMeshes;
                outIndex = i;
                return Status::Ready;
            }
        }
        if (key.GetSize() > CACHE_IDENTITY_BYTES - IdentityBytes)
        {
            EvictIdle();
        }
        if (key.GetSize() > CACHE_IDENTITY_BYTES - IdentityBytes)
        {
            return Status::CapacityExceeded;
        }
        bool hasFreeSlot = false;
        for (const MeshEntry& entry : Meshes)
        {
            hasFreeSlot = hasFreeSlot == true || entry.IsUsed == false;
        }
        if (hasFreeSlot == false)
        {
            EvictIdle();
        }
        for (usize i = 0; i < Meshes.GetSize(); ++i)
        {
            MeshEntry& entry = Meshes[i];
            if (entry.IsUsed == false)
            {
                const Status status = RendererOrNull->CreateMesh(mesh, entry.Handle);
                if (IsAccepted(status) == true)
                {
                    IdentityBytes += key.GetSize();
                    entry.Key = Move(key);
                    entry.IsUsed = true;
                    entry.Pins = 1;
                    ++Report.CreatedMeshes;
                    outIndex = i;
                }
                return status;
            }
        }
        return Status::CapacityExceeded;
    }
    Status AcquireTexture(const rhi::TextureCpuContent& source, usize& outIndex) noexcept
    {
        if (source.Bytes == nullptr && source.Size == 0 && source.Width == 0 && source.Height == 0)
        {
            outIndex = NO_RESOURCE;
            return Status::Ready;
        }
        usize rowBytes = 0;
        usize size = 0;
        if (source.Bytes == nullptr || source.Width == 0 || source.Height == 0 ||
            (source.Format != rhi::RasterFormat::Rgba8Unorm && source.Format != rhi::RasterFormat::Rgba8Srgb) ||
            TryMultiply(usize(source.Width), CHANNEL_COUNT, rowBytes) == false || source.RowPitch < rowBytes ||
            TryMultiply(source.RowPitch, usize(source.Height), size) == false || source.Size < size ||
            TryMultiply(rowBytes, usize(source.Height), size) == false || size > CACHE_IDENTITY_BYTES)
        {
            return Status::InvalidDescription;
        }
        Array<uint8> key;
        if (key.TryResize(size) == false)
        {
            return Status::OutOfMemory;
        }
        for (usize row = 0; row < source.Height; ++row)
        {
            std::memcpy(key.GetData() + row * rowBytes, source.Bytes + row * source.RowPitch, rowBytes);
        }
        for (usize i = 0; i < Textures.GetSize(); ++i)
        {
            TextureEntry& entry = Textures[i];
            if (entry.IsUsed == true && entry.Width == source.Width && entry.Height == source.Height &&
                entry.Format == source.Format && AreBytesEqual(entry.Key, key) == true)
            {
                const Status status = rhi::GetStatus(Device, entry.Handle);
                if (IsAccepted(status) == false)
                {
                    if (entry.Pins != 0)
                    {
                        return status;
                    }
                    IdentityBytes -= entry.Key.GetSize();
                    static_cast<void>(rhi::Destroy(Device, entry.Handle));
                    entry = {};
                    continue;
                }
                ++entry.Pins;
                ++Report.ReusedTextures;
                outIndex = i;
                return Status::Ready;
            }
        }
        if (size > CACHE_IDENTITY_BYTES - IdentityBytes)
        {
            EvictIdle();
        }
        if (size > CACHE_IDENTITY_BYTES - IdentityBytes)
        {
            return Status::CapacityExceeded;
        }
        bool hasFreeSlot = false;
        for (const TextureEntry& entry : Textures)
        {
            hasFreeSlot = hasFreeSlot == true || entry.IsUsed == false;
        }
        if (hasFreeSlot == false)
        {
            EvictIdle();
        }
        for (usize i = 0; i < Textures.GetSize(); ++i)
        {
            TextureEntry& entry = Textures[i];
            if (entry.IsUsed == false)
            {
                rhi::TextureDescription description{source.Width, source.Height, source.Format};
                description.AsyncUpload = true;
                const Status status = rhi::CreateTexture(Device, description, {key.AsSpan(), rowBytes}, entry.Handle);
                if (IsAccepted(status) == true)
                {
                    IdentityBytes += size;
                    entry.Key = Move(key);
                    entry.Width = source.Width;
                    entry.Height = source.Height;
                    entry.Format = source.Format;
                    entry.IsUsed = true;
                    entry.Pins = 1;
                    ++Report.CreatedTextures;
                    outIndex = i;
                }
                return status;
            }
        }
        return Status::CapacityExceeded;
    }
};
struct ForwardSceneGeneration::State final
{
    ForwardSceneResources::State* OwnerOrNull = nullptr;
    Array<usize> MeshPins;
    Array<usize> TexturePins;
    Array<SceneItem> Items;
    Array<LitMaterialDescription> Descriptions;
    Array<MaterialVersion> Materials;
    Array<DirectLight> Lights;
    Array<uint64> BakedLightIds;
    usize SurfaceChartDraws = 0;
    Array<math::Vector4> MeshLightWords;
    rhi::BufferHandle MeshLightBuffer{};
    uint32 MeshLightCount = 0;
    uint32 MeshLightGridWord = 0;
    /// Shadow casters with allocated atlas faces; empty without a culling grid.
    Array<MeshLightShadowCaster> MeshLightShadowCasters;
    /// Shared directional and mesh-light atlas. It exists only when at least one projection is needed.
    DeferredShadowAtlas Shadows;
    LightClusterDescription Clusters;
    math::Aabb3 BoundsWorld = math::EmptyAabb3();
    EnvironmentMap Environment;
    usize PreparedMaterials = 0;
    bool HasEnvironment = false;
    ~State() noexcept
    {
        if (OwnerOrNull == nullptr)
        {
            return;
        }
        static_cast<void>(rhi::Destroy(OwnerOrNull->Device, MeshLightBuffer));
        static_cast<void>(rhi::Destroy(OwnerOrNull->Device, Shadows.Sampled));
        static_cast<void>(rhi::Destroy(OwnerOrNull->Device, Shadows.Texture));
        if (Shadows.Packets.IsEmpty() == false)
        {
            static_cast<void>(rhi::Destroy(OwnerOrNull->Device, Shadows.Instances));
        }
        for (MaterialVersion& material : Materials)
        {
            static_cast<void>(OwnerOrNull->RendererOrNull->Release(material));
        }
        for (const usize index : MeshPins)
        {
            --OwnerOrNull->Meshes[index].Pins;
        }
        for (const usize index : TexturePins)
        {
            if (index != NO_RESOURCE)
            {
                --OwnerOrNull->Textures[index].Pins;
            }
        }
    }
};
ForwardSceneGeneration::ForwardSceneGeneration() noexcept = default;
ForwardSceneGeneration::~ForwardSceneGeneration() noexcept = default;
ForwardSceneResources::ForwardSceneResources() noexcept = default;
ForwardSceneResources::~ForwardSceneResources() noexcept = default;
void ForwardSceneResources::Reset() noexcept
{
    mState.Reset();
}
Status ForwardSceneResources::Initialize(rhi::DeviceHandle device,
                                         Renderer& inoutRenderer,
                                         MaterialProgram program,
                                         bool isClustered,
                                         bool hasMeshLights,
                                         MaterialProgram surfaceChartProgram) noexcept
{
    if (mState.Get() != nullptr)
    {
        return Status::InvalidState;
    }
    State* allocationOrNull = new (std::nothrow) State;
    if (allocationOrNull == nullptr)
    {
        return Status::OutOfMemory;
    }
    allocationOrNull->Device = device;
    allocationOrNull->RendererOrNull = &inoutRenderer;
    allocationOrNull->Program = program;
    allocationOrNull->SurfaceChartProgram = surfaceChartProgram;
    allocationOrNull->HasSurfaceCharts = inoutRenderer.GetStatus(surfaceChartProgram) == Status::Ready;
    allocationOrNull->IsClustered = isClustered;
    allocationOrNull->HasMeshLights = hasMeshLights;
    mState.Reset(allocationOrNull);
    return Status::Ready;
}
RenderGraphSceneResourceReport ForwardSceneResources::GetReport() const noexcept
{
    return mState.Get() == nullptr ? RenderGraphSceneResourceReport{} : mState->Report;
}
BorrowedView<const SceneItem> ForwardSceneGeneration::GetItems() const noexcept
{
    return mState.Get() == nullptr ? BorrowedView<const SceneItem>{}
                                   : BorrowedView<const SceneItem>{mState->Items.GetData(), mState->Items.GetSize()};
}
rhi::RasterBindingResource ForwardSceneGeneration::GetMeshLights() const noexcept
{
    if (mState.Get() == nullptr)
    {
        return {};
    }
    return
    {
        .Binding = DEFERRED_MESH_LIGHT_BINDING,
        .Buffer = mState->MeshLightBuffer,
        .Size = mState->MeshLightWords.GetSize() * sizeof(math::Vector4),
    };
}
uint32 ForwardSceneGeneration::GetMeshLightCount() const noexcept
{
    return mState.Get() == nullptr ? 0 : mState->MeshLightCount;
}
uint32 ForwardSceneGeneration::GetMeshLightGridWord() const noexcept
{
    return mState.Get() == nullptr ? 0 : mState->MeshLightGridWord;
}
BorrowedView<const MeshLightShadowCaster> ForwardSceneGeneration::GetMeshLightShadowCasters() const noexcept
{
    if (mState.Get() == nullptr)
    {
        return {};
    }
    return {mState->MeshLightShadowCasters.GetData(), mState->MeshLightShadowCasters.GetSize()};
}
usize ForwardSceneGeneration::GetSurfaceChartDrawCount() const noexcept
{
    return mState.Get() == nullptr ? 0 : mState->SurfaceChartDraws;
}
rhi::TextureViewHandle ForwardSceneGeneration::GetShadowAtlasView() const noexcept
{
    return mState.Get() == nullptr ? rhi::TextureViewHandle{} : mState->Shadows.Sampled;
}
bool ForwardSceneGeneration::HasShadowAtlas() const noexcept
{
    return mState.Get() != nullptr && rhi::IsNull(mState->Shadows.Texture) == false;
}
DeferredShadowAtlas* ForwardSceneGeneration::GetShadowAtlasOrNull() noexcept
{
    return HasShadowAtlas() == true ? &mState->Shadows : nullptr;
}
uint32 ForwardSceneGeneration::GetPendingMeshLightShadowFaces() const noexcept
{
    if (mState.Get() == nullptr)
    {
        return 0;
    }
    const DeferredShadowAtlas& atlas = mState->Shadows;
    if (atlas.HasPackets == false)
    {
        uint32 faces = 0;
        for (const MeshLightShadowCaster& caster : mState->MeshLightShadowCasters)
        {
            for (uint32 face = 0; face < MESH_LIGHT_SHADOW_FACE_COUNT; ++face)
            {
                faces += (caster.FaceMask >> face) & 1U;
            }
        }
        return faces;
    }
    return static_cast<uint32>(atlas.Faces.GetSize()) - atlas.Submitted.NextFace;
}
Status ForwardSceneGeneration::Poll() noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    State& state = *mState;
    ForwardSceneResources::State& owner = *state.OwnerOrNull;
    if (owner.HasMeshLights == true)
    {
        const Status status = rhi::GetStatus(owner.Device, state.MeshLightBuffer);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    if (rhi::IsNull(state.Shadows.Texture) == false)
    {
        Status status = rhi::GetStatus(owner.Device, state.Shadows.Texture);
        if (status == Status::Ready && state.Shadows.HasSampled == false)
        {
            status = rhi::CreateTextureView(owner.Device, state.Shadows.Texture, state.Shadows.Sampled);
            state.Shadows.HasSampled = IsAccepted(status);
        }
        if (status == Status::Ready)
        {
            status = rhi::GetStatus(owner.Device, state.Shadows.Sampled);
        }
        if (status != Status::Ready)
        {
            return status;
        }
    }
    for (const usize index : state.MeshPins)
    {
        const Status status = owner.RendererOrNull->GetStatus(owner.Meshes[index].Handle);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    for (const usize index : state.TexturePins)
    {
        if (index != NO_RESOURCE)
        {
            const Status status = rhi::GetStatus(owner.Device, owner.Textures[index].Handle);
            if (status != Status::Ready)
            {
                return status;
            }
        }
    }
    if (state.HasEnvironment == true)
    {
        const Status status = state.Environment.GetStatus();
        if (status != Status::Ready)
        {
            return status;
        }
    }
    for (; state.PreparedMaterials < state.Descriptions.GetSize(); ++state.PreparedMaterials)
    {
        LitMaterialDescription& description = state.Descriptions[state.PreparedMaterials];
        description.Lights = state.Lights.GetData();
        description.LightCount = state.Lights.GetSize();
        if (rhi::IsNull(description.Bake.Texture) == false)
        {
            description.BakedDirectLightIds = {state.BakedLightIds.GetData(), state.BakedLightIds.GetSize()};
        }
        description.Clusters = owner.IsClustered == true ? &state.Clusters : nullptr;
        LightSelection selection;
        const MaterialProgram program =
            rhi::IsNull(description.Bake.Texture) == false ? owner.SurfaceChartProgram : owner.Program;
        const ShadowRegion& directional = state.Shadows.Directional;
        const bool hasDirectionalShadow = directional.Light.SourceId != 0;
        const Status status =
            owner.RendererOrNull->createLitMaterial(program,
                                                    description,
                                                    state.Materials[state.PreparedMaterials],
                                                    selection,
                                                    hasDirectionalShadow == true ? &directional : nullptr,
                                                    state.Shadows.ExtentTexels);
        if (IsAccepted(status) == false)
        {
            return status;
        }
        owner.Report.DroppedLights = selection.Dropped;
        owner.Report.OverflowClusters = selection.OverflowCells;
        owner.Report.ClusterAssignments = selection.Assignments;
    }
    for (usize i = 0; i < state.Materials.GetSize(); ++i)
    {
        const Status status = owner.RendererOrNull->GetStatus(state.Materials[i]);
        if (status != Status::Ready)
        {
            return status;
        }
        state.Items[i].Surface = state.Materials[i];
    }
    return Status::Ready;
}
Status ForwardSceneResources::Prepare(const RenderGraphSceneDescription& scene,
                                      core::UniquePtr<ForwardSceneGeneration>& outGeneration) noexcept
{
    if (mState.Get() == nullptr || outGeneration.Get() != nullptr)
    {
        return Status::InvalidState;
    }
    State& owner = *mState;
    ForwardSceneGeneration* allocationOrNull = new (std::nothrow) ForwardSceneGeneration;
    core::UniquePtr<ForwardSceneGeneration> generation{allocationOrNull};
    if (generation.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    generation->mState.Reset(new (std::nothrow) ForwardSceneGeneration::State);
    if (generation->mState.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    ForwardSceneGeneration::State& candidate = *generation->mState;
    candidate.OwnerOrNull = &owner;
    if (scene.HasMaterialInterface == false || scene.Inputs.Materials.IsValid() == false ||
        scene.Inputs.Lights.IsValid() == false || scene.Inputs.EnvironmentPixels.IsValid() == false ||
        scene.Inputs.Materials.GetSize() > format::MAX_ITEMS || IsSceneRadianceValid(scene.Inputs.Environment) == false)
    {
        return Status::InvalidDescription;
    }
    for (usize material = 0; material < scene.Inputs.Materials.GetSize(); ++material)
    {
        if (IsSceneShaderMaterialValid(scene.Inputs.Materials[material]) == false)
        {
            return Status::InvalidDescription;
        }
    }
    LightSelection normalized;
    const Status normalizedStatus =
        NormalizeDirectLights(scene.Inputs.Lights.GetDataOrNull(), scene.Inputs.Lights.GetSize(), normalized);
    if (normalizedStatus != Status::Ready)
    {
        return normalizedStatus;
    }
    for (usize light = 0; light < normalized.Count; ++light)
    {
        if (IsSceneShaderLightKindSupported(normalized.Lights[light].Kind) == false)
        {
            return Status::Unsupported;
        }
    }
    if (owner.HasSurfaceCharts == true &&
        (scene.Inputs.SurfaceCharts.IsValid() == false || scene.Inputs.BakedLightIds.IsValid() == false ||
         (scene.Inputs.SurfaceCharts.IsEmpty() == false &&
          scene.Inputs.SurfaceCharts.GetSize() != scene.Items.GetSize()) ||
         scene.Inputs.BakedLightIds.GetSize() > MAX_DIRECT_LIGHTS))
    {
        return Status::InvalidDescription;
    }
    if (owner.HasSurfaceCharts == true &&
        candidate.BakedLightIds.TryAddRange(
            {scene.Inputs.BakedLightIds.GetDataOrNull(), scene.Inputs.BakedLightIds.GetSize()}) == false)
    {
        return Status::OutOfMemory;
    }
    // Reserve pin arrays before any acquisition, so failed allocation cannot leak pins.
    if (candidate.MeshPins.TryEnsureCapacity(format::MAX_ITEMS) == false ||
        candidate.TexturePins.TryEnsureCapacity(format::MAX_ITEMS * DRAW_TEXTURE_COUNT) == false ||
        candidate.Items.TryEnsureCapacity(format::MAX_ITEMS) == false ||
        candidate.Descriptions.TryEnsureCapacity(format::MAX_ITEMS) == false ||
        candidate.Lights.TryAddRange({normalized.Lights, normalized.Count}) == false)
    {
        return Status::OutOfMemory;
    }
    if (scene.Inputs.EnvironmentWidth != 0 || scene.Inputs.EnvironmentHeight != 0 ||
        scene.Inputs.EnvironmentPixels.IsEmpty() == false)
    {
        usize count = 0;
        if (TryMultiply(usize(scene.Inputs.EnvironmentWidth), usize(scene.Inputs.EnvironmentHeight), count) == false ||
            count != scene.Inputs.EnvironmentPixels.GetSize() || count > ENVIRONMENT_CPU_BYTES / sizeof(math::Vector4))
        {
            return Status::InvalidDescription;
        }
        Array<math::Vector4> pixels;
        if (pixels.TryResize(count) == false)
        {
            return Status::OutOfMemory;
        }
        for (usize i = 0; i < count; ++i)
        {
            const math::Vector3 value = scene.Inputs.EnvironmentPixels[i];
            pixels[i] = {value.X * scene.Inputs.Environment.X,
                         value.Y * scene.Inputs.Environment.Y,
                         value.Z * scene.Inputs.Environment.Z,
                         1};
        }
        EnvironmentDescription environment;
        environment.PixelsOrNull = pixels.GetData();
        environment.Width = scene.Inputs.EnvironmentWidth;
        environment.Height = scene.Inputs.EnvironmentHeight;
        environment.FilterWidth = ENVIRONMENT_FILTER_WIDTH;
        environment.FilterSamples = ENVIRONMENT_FILTER_SAMPLES;
        environment.CpuByteBudget = ENVIRONMENT_CPU_BYTES;
        const Status status = candidate.Environment.Initialize(owner.Device, environment);
        if (IsAccepted(status) == false)
        {
            return status;
        }
        candidate.HasEnvironment = true;
    }
    for (usize itemIndex = 0; itemIndex < scene.Items.GetSize(); ++itemIndex)
    {
        const RenderGraphItem& item = scene.Items[itemIndex];
        if (item.IsIntentionallyExcluded == true)
        {
            continue;
        }
        if (item.MeshIndex >= scene.Meshes.GetSize() || item.Coverage != RenderGraphMaterial::Opaque ||
            scene.Meshes[item.MeshIndex].Layout != RenderGraphLayout::Static || item.SurfaceTint.W != 1)
        {
            return Status::Unsupported;
        }
        const RenderGraphMesh& mesh = scene.Meshes[item.MeshIndex];
        if (mesh.Geometry.IndexCount == 0 || mesh.Geometry.IndexCount % TRIANGLE_CORNER_COUNT != 0)
        {
            return Status::InvalidDescription;
        }
        const usize triangleCount = mesh.Geometry.IndexCount / TRIANGLE_CORNER_COUNT;
        if (mesh.MaterialSlots.IsValid() == false || mesh.MaterialBindings.IsValid() == false ||
            item.MaterialOverrides.IsValid() == false ||
            (mesh.MaterialSlots.IsEmpty() == false && mesh.MaterialSlots.GetSize() != triangleCount))
        {
            return Status::InvalidDescription;
        }
        const usize slotCount =
            math::Max(usize{1}, math::Max(mesh.MaterialBindings.GetSize(), item.MaterialOverrides.GetSize()));
        if (slotCount > format::MAX_ITEMS)
        {
            return Status::CapacityExceeded;
        }
        MeshDescription placementGeometry = mesh.Geometry;
        SurfaceChartMesh expandedMesh;
        const SceneSurfaceChart* chartOrNull = owner.HasSurfaceCharts == true &&
                                                       scene.Inputs.SurfaceCharts.IsEmpty() == false &&
                                                       candidate.BakedLightIds.IsEmpty() == false
                                                   ? &scene.Inputs.SurfaceCharts[itemIndex]
                                                   : nullptr;
        usize chartTexture = NO_RESOURCE;
        if (chartOrNull != nullptr && chartOrNull->SourceId == item.SourceId && item.SourceId != 0 &&
            chartOrNull->MeshToWorld == item.MeshToWorld)
        {
            scene_format::BakeChartLayout layout;
            const rhi::TextureCpuContent& texture = chartOrNull->Irradiance;
            if (triangleCount <= scene_format::MAX_VERTICES &&
                scene_format::TryMakeBakeChartLayout(static_cast<uint32>(triangleCount), layout) == true &&
                texture.Format == rhi::RasterFormat::Rgba8Unorm && texture.Width == layout.Width &&
                texture.Height == layout.Height && math::IsFinite(chartOrNull->Scale) == true &&
                chartOrNull->Scale.X >= 0 && chartOrNull->Scale.Y >= 0 && chartOrNull->Scale.Z >= 0 &&
                BuildSurfaceChartMesh(mesh.Geometry, layout, expandedMesh, placementGeometry) == true)
            {
                const Status status = owner.AcquireTexture(texture, chartTexture);
                if (IsAccepted(status) == true && chartTexture != NO_RESOURCE)
                {
                    static_cast<void>(candidate.TexturePins.TryAdd(chartTexture));
                }
                else
                {
                    chartTexture = NO_RESOURCE;
                    placementGeometry = mesh.Geometry;
                }
            }
        }
        for (usize slot = 0; slot < slotCount; ++slot)
        {
            Array<uint32> indices;
            MeshDescription geometry = placementGeometry;
            if (mesh.MaterialSlots.IsEmpty() == false)
            {
                if (indices.TryEnsureCapacity(geometry.IndexCount) == false)
                {
                    return Status::OutOfMemory;
                }
                for (usize triangle = 0; triangle < triangleCount; ++triangle)
                {
                    if (mesh.MaterialSlots[triangle] >= slotCount)
                    {
                        return Status::InvalidDescription;
                    }
                    if (mesh.MaterialSlots[triangle] == slot)
                    {
                        if (geometry.Indices == nullptr)
                        {
                            return Status::InvalidDescription;
                        }
                        if (indices.TryAddRange(
                                {geometry.Indices + triangle * TRIANGLE_CORNER_COUNT, TRIANGLE_CORNER_COUNT}) == false)
                        {
                            return Status::OutOfMemory;
                        }
                    }
                }
                if (indices.IsEmpty() == true)
                {
                    continue;
                }
                geometry.Indices = indices.GetData();
                geometry.IndexCount = indices.GetSize();
            }
            else if (slot != 0)
            {
                continue;
            }
            if (candidate.Items.GetSize() >= format::MAX_ITEMS)
            {
                return Status::CapacityExceeded;
            }
            uint32 materialIndex =
                slot < mesh.MaterialBindings.GetSize() ? mesh.MaterialBindings[slot] : DEFAULT_SCENE_MATERIAL;
            if (slot < item.MaterialOverrides.GetSize() && item.MaterialOverrides[slot] != INHERIT_SCENE_MATERIAL)
            {
                materialIndex = item.MaterialOverrides[slot];
            }
            SceneShaderMaterial material;
            if (materialIndex != DEFAULT_SCENE_MATERIAL)
            {
                if (materialIndex >= scene.Inputs.Materials.GetSize())
                {
                    return Status::InvalidDescription;
                }
                material = scene.Inputs.Materials[materialIndex];
            }
            if (material.HasError == true)
            {
                return Status::Unsupported;
            }
            usize meshIndex = NO_RESOURCE;
            Status status = owner.AcquireMesh(geometry, meshIndex);
            if (IsAccepted(status) == false)
            {
                return status;
            }
            static_cast<void>(candidate.MeshPins.TryAdd(meshIndex));
            if (owner.IsClustered == true)
            {
                math::Aabb3 boundsMesh = math::EmptyAabb3();
                for (usize vertex = 0; vertex < geometry.VertexCount; ++vertex)
                {
                    const math::Vector3 positionMesh = geometry.Positions[vertex];
                    boundsMesh = math::Merge(boundsMesh, {positionMesh, positionMesh});
                }
                math::Aabb3 boundsWorld;
                if (math::TryTransformAabb(item.MeshToWorld, boundsMesh, boundsWorld) != math::MathStatus::Success)
                {
                    return Status::InvalidDescription;
                }
                candidate.BoundsWorld = math::Merge(candidate.BoundsWorld, boundsWorld);
            }
            if (owner.HasMeshLights == true)
            {
                status = AppendRasterMeshLights(geometry,
                                                item.MeshToWorld,
                                                material,
                                                candidate.MeshLightWords,
                                                candidate.MeshLightCount);
                if (status != Status::Ready)
                {
                    return status;
                }
            }
            LitMaterialDescription description;
            description.AreAuthoredInputsEnabled = true;
            description.Surface.Tint = {material.BaseColor.X * item.SurfaceTint.X,
                                        material.BaseColor.Y * item.SurfaceTint.Y,
                                        material.BaseColor.Z * item.SurfaceTint.Z,
                                        1};
            description.Surface.UvTransform = material.UvTransform;
            description.Surface.Sampler = material.Sampler;
            description.Surface.Alpha = material.IsAlphaMasked == true ? MaterialAlpha::Masked : MaterialAlpha::Opaque;
            description.Surface.AlphaCutoff = material.AlphaCutoff;
            description.Surface.Lod = 0;
            description.Metallic = material.Metallic;
            description.Roughness = material.Roughness;
            description.Scattering = material.Scattering;
            description.NormalScale = material.NormalScale;
            description.Emission = material.Emission;
            description.IsEmissionTwoSided = material.IsEmissionTwoSided;
            description.EnvironmentIrradiance = scene.Inputs.Environment;
            description.EnvironmentRadiance = scene.Inputs.Environment;
            description.EnvironmentOrNull = candidate.HasEnvironment == true ? &candidate.Environment : nullptr;
            const StaticArray<rhi::TextureCpuContent, MATERIAL_TEXTURE_COUNT> sources{
                {material.ColorTexture, material.SurfaceTexture, material.EmissionTexture}};
            StaticArray<rhi::TextureHandle*, MATERIAL_TEXTURE_COUNT> destinations{
                {&description.Surface.Texture, &description.SurfaceMap, &description.EmissionMap}};
            for (usize texture = 0; texture < MATERIAL_TEXTURE_COUNT; ++texture)
            {
                usize index = NO_RESOURCE;
                status = owner.AcquireTexture(sources[texture], index);
                if (IsAccepted(status) == false)
                {
                    return status;
                }
                static_cast<void>(candidate.TexturePins.TryAdd(index));
                if (index != NO_RESOURCE)
                {
                    *destinations[texture] = owner.Textures[index].Handle;
                }
            }
            SceneItem drawing;
            drawing.Geometry = owner.Meshes[meshIndex].Handle;
            drawing.Transform = item.MeshToWorld;
            drawing.SourceId = candidate.Items.GetSize() + 1;
            drawing.InstanceIndex = static_cast<uint32>(candidate.Items.GetSize());
            drawing.DrawIndex = static_cast<uint32>(item.MeshIndex);
            if (chartTexture != NO_RESOURCE && chartOrNull != nullptr)
            {
                drawing.BakeSceneRevision = scene.SourceGeneration;
                drawing.BakeDependencyRevision = scene.SourceGeneration;
                description.Bake.Texture = owner.Textures[chartTexture].Handle;
                description.Bake.Scale = chartOrNull->Scale;
                description.Bake.UvTransform = {1, 1, 0, 0};
                description.Bake.SourceId = drawing.SourceId;
                description.Bake.SceneRevision = drawing.BakeSceneRevision;
                description.Bake.DependencyRevision = drawing.BakeDependencyRevision;
                description.Bake.Transform = drawing.Transform;
                description.Bake.AreTriangleChartsEnabled = true;
                description.Bake.ShouldAddToEnvironment = true;
                ++candidate.SurfaceChartDraws;
            }
            static_cast<void>(candidate.Items.TryAdd(drawing));
            static_cast<void>(candidate.Descriptions.TryAdd(description));
        }
    }
    MeshLightShadowReport shadows;
    if (owner.HasMeshLights == true)
    {
        // An empty light set uploads one zero header, which the resolve never reads.
        if (candidate.MeshLightWords.IsEmpty() == true &&
            candidate.MeshLightWords.TryResize(MESH_LIGHT_POLYGON_HEADER_WORDS) == false)
        {
            return Status::OutOfMemory;
        }
        // Clustered profiles bound every shaded surface; the resolve then reads only its cell's emitters.
        if (owner.IsClustered == true)
        {
            const Status directional =
                BuildDirectionalShadowRegion({candidate.Lights.GetData(), candidate.Lights.GetSize()},
                                             candidate.BoundsWorld,
                                             candidate.Shadows.Directional);
            if (directional != Status::Ready)
            {
                return directional;
            }
            const bool hasDirectionalShadow = candidate.Shadows.Directional.Light.SourceId != 0;
            // Casters come first: they tag each record, and the grid lists each caster's records together.
            if (candidate.MeshLightCount != 0)
            {
                const Status casters = BuildRasterMeshLightShadowCasters(candidate.MeshLightWords,
                                                                         candidate.MeshLightCount,
                                                                         candidate.MeshLightShadowCasters,
                                                                         shadows);
                if (casters != Status::Ready)
                {
                    return casters;
                }
            }
            const Status grid = AppendRasterMeshLightGrid(candidate.MeshLightWords,
                                                          candidate.MeshLightCount,
                                                          candidate.BoundsWorld,
                                                          candidate.MeshLightGridWord);
            if (grid != Status::Ready)
            {
                return grid;
            }
            // The resolve finds the caster table through the grid, so a scene without a grid is unshadowed.
            if (candidate.MeshLightGridWord == 0)
            {
                shadows = { .UnshadowedRecords = shadows.ShadowedRecords + shadows.UnshadowedRecords };
                candidate.MeshLightShadowCasters.Clear();
            }
            else if (candidate.MeshLightShadowCasters.IsEmpty() == false)
            {
                if (AllocateRasterMeshLightShadowFaces(candidate.MeshLightShadowCasters,
                                                       MESH_LIGHT_SHADOW_ATLAS_TEXELS,
                                                       hasDirectionalShadow) == false)
                {
                    return Status::OutOfMemory;
                }
                uint32 tableWord = 0;
                const Status table = AppendRasterMeshLightShadowTable(
                    candidate.MeshLightWords,
                    {candidate.MeshLightShadowCasters.GetData(), candidate.MeshLightShadowCasters.GetSize()},
                    MESH_LIGHT_SHADOW_ATLAS_TEXELS,
                    tableWord);
                if (table != Status::Ready)
                {
                    return table;
                }
                candidate.MeshLightWords[candidate.MeshLightGridWord + MESH_LIGHT_GRID_CULL_WORD].Y =
                    static_cast<float32>(tableWord);
            }
            if (hasDirectionalShadow == true || candidate.MeshLightShadowCasters.IsEmpty() == false)
            {
                // One packed-depth colour attachment and its private depth. Pure directional scenes need one square.
                const uint32 atlasTexels = candidate.MeshLightShadowCasters.IsEmpty() == true
                                               ? DIRECTIONAL_SHADOW_TEXELS
                                               : MESH_LIGHT_SHADOW_ATLAS_TEXELS;
                candidate.Shadows.ExtentTexels = atlasTexels;
                const Status atlas = rhi::CreateTexture(owner.Device,
                                                        {atlasTexels, atlasTexels, rhi::RasterFormat::Rgba8Unorm, true},
                                                        {},
                                                        candidate.Shadows.Texture);
                if (IsAccepted(atlas) == false)
                {
                    return atlas;
                }
            }
        }
        const usize byteCount = candidate.MeshLightWords.GetSize() * sizeof(math::Vector4);
        const Status status =
            rhi::CreateBuffer(owner.Device,
                              {rhi::BufferRole::Storage, byteCount},
                              {reinterpret_cast<const uint8*>(candidate.MeshLightWords.GetData()), byteCount},
                              candidate.MeshLightBuffer);
        if (IsAccepted(status) == false)
        {
            return status;
        }
    }
    if (owner.HasMeshLights == true)
    {
        owner.Report.MeshLightPolygons = candidate.MeshLightCount;
        owner.Report.MeshLightBytes = uint64{candidate.MeshLightWords.GetSize()} * sizeof(math::Vector4);
        owner.Report.MeshLightShadowCasters = static_cast<uint32>(candidate.MeshLightShadowCasters.GetSize());
        owner.Report.UnshadowedMeshLights = shadows.UnshadowedRecords;
        owner.Report.MeshLightShadowFaces = shadows.Faces;
        owner.Report.DirectionalShadowLightSourceId = candidate.Shadows.Directional.Light.SourceId;
    }
    if (owner.IsClustered == true && math::IsEmpty(candidate.BoundsWorld) == false)
    {
        // A half-metre margin gives planar scenes nonzero cells. The shader evaluates
        // every light outside the representable grid, so clamping never drops illumination.
        const math::Vector3 minimumWorld = candidate.BoundsWorld.Min;
        const math::Vector3 maximumWorld = candidate.BoundsWorld.Max;
        candidate.Clusters.Bounds = {
            {math::Clamp(minimumWorld.X - 0.5F, -MAX_CLUSTER_COORDINATE, MAX_CLUSTER_COORDINATE - 1),
             math::Clamp(minimumWorld.Y - 0.5F, -MAX_CLUSTER_COORDINATE, MAX_CLUSTER_COORDINATE - 1),
             math::Clamp(minimumWorld.Z - 0.5F, -MAX_CLUSTER_COORDINATE, MAX_CLUSTER_COORDINATE - 1)},
            {math::Clamp(maximumWorld.X + 0.5F, -MAX_CLUSTER_COORDINATE + 1, MAX_CLUSTER_COORDINATE),
             math::Clamp(maximumWorld.Y + 0.5F, -MAX_CLUSTER_COORDINATE + 1, MAX_CLUSTER_COORDINATE),
             math::Clamp(maximumWorld.Z + 0.5F, -MAX_CLUSTER_COORDINATE + 1, MAX_CLUSTER_COORDINATE)},
        };
    }
    if (candidate.Materials.TryResize(candidate.Descriptions.GetSize()) == false)
    {
        return Status::OutOfMemory;
    }
    const Status status = generation->Poll();
    if (IsAccepted(status) == true)
    {
        outGeneration = Move(generation);
    }
    return status;
}
} // namespace ludus::graphics::renderer::internal
