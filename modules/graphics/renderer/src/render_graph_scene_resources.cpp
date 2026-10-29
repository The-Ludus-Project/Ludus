#include "internal/render_graph_scene_resources.h"
#include "internal/raster_mesh_lights.h"
#include "internal/scene_material_validation.h"
#include <ludus/graphics/renderer/deferred_layout.h>

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
using Status = rhi::RasterStatus;
namespace
{
constexpr usize RETAINED_GENERATION_COUNT = 3;
constexpr usize CACHE_MESH_COUNT = format::MAX_ITEMS * RETAINED_GENERATION_COUNT;
constexpr usize MATERIAL_TEXTURE_COUNT = 3;
constexpr usize CACHE_TEXTURE_COUNT = format::MAX_ITEMS * MATERIAL_TEXTURE_COUNT * RETAINED_GENERATION_COUNT;
constexpr usize CHANNEL_COUNT = 4;
constexpr usize TRIANGLE_CORNER_COUNT = 3;
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
constexpr usize CACHE_IDENTITY_BYTES = usize{128} * 1024 * 1024;
constexpr usize ENVIRONMENT_CPU_BYTES = usize{64} * 1024 * 1024;
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
    bool IsClustered = false;
    bool HasMeshLights = false;
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
    void evictIdle() noexcept
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
    Status acquireMesh(const MeshDescription& mesh, usize& outIndex) noexcept
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
            evictIdle();
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
            evictIdle();
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
    Status acquireTexture(const rhi::TextureCpuContent& source, usize& outIndex) noexcept
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
            evictIdle();
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
            evictIdle();
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
    Array<math::Vector4> MeshLightWords;
    rhi::BufferHandle MeshLightBuffer{};
    uint32 MeshLightCount = 0;
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
                                         bool hasMeshLights) noexcept
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
        description.Clusters = owner.IsClustered == true ? &state.Clusters : nullptr;
        LightSelection selection;
        const Status status = owner.RendererOrNull->CreateLitMaterial(owner.Program,
                                                                      description,
                                                                      state.Materials[state.PreparedMaterials],
                                                                      selection);
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
    // Reserve pin arrays before any acquisition, so failed allocation cannot leak pins.
    if (candidate.MeshPins.TryEnsureCapacity(format::MAX_ITEMS) == false ||
        candidate.TexturePins.TryEnsureCapacity(format::MAX_ITEMS * MATERIAL_TEXTURE_COUNT) == false ||
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
        for (usize slot = 0; slot < slotCount; ++slot)
        {
            Array<uint32> indices;
            MeshDescription geometry = mesh.Geometry;
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
            Status status = owner.acquireMesh(geometry, meshIndex);
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
                status = AppendRasterMeshLights(geometry, item.MeshToWorld, material, candidate.MeshLightWords);
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
            description.Surface.Lod = 0;
            description.Metallic = material.Metallic;
            description.Roughness = material.Roughness;
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
                status = owner.acquireTexture(sources[texture], index);
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
            static_cast<void>(candidate.Items.TryAdd(drawing));
            static_cast<void>(candidate.Descriptions.TryAdd(description));
        }
    }
    if (owner.HasMeshLights == true)
    {
        candidate.MeshLightCount = static_cast<uint32>(candidate.MeshLightWords.GetSize() / MESH_LIGHT_TRIANGLE_WORDS);
        if (candidate.MeshLightWords.IsEmpty() == true &&
            candidate.MeshLightWords.TryResize(MESH_LIGHT_TRIANGLE_WORDS) == false)
        {
            return Status::OutOfMemory;
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
        owner.Report.MeshLightTriangles = candidate.MeshLightCount;
        owner.Report.MeshLightBytes = candidate.MeshLightWords.GetSize() * sizeof(math::Vector4);
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
