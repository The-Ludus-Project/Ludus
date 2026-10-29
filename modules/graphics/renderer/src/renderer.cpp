#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/views.hpp>

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/stable_sort.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/queries.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>
#include <ludus/foundation/math/transform.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

#include <atomic>
#include <new>

#include "internal/environment_data.h"
#include "internal/gpu_preview_data.h"
#include "internal/ltc_table_data.h"
#include "internal/reference_data.h"
#include "internal/render_graph_deferred.h"
#include "internal/render_graph_ray.h"
#include "internal/uniform_set_table.h"
#include "internal/visibility_buffer.h"

namespace ludus::graphics::renderer
{
using namespace foundation;
namespace math = foundation::math;
using Status = rhi::RasterStatus;
namespace internal
{
struct Access final
{
    static EnvironmentData* GetEnvironment(const EnvironmentMap* mapOrNull) noexcept
    {
        return mapOrNull != nullptr ? mapOrNull->mDataOrNull : nullptr;
    }
    template <typename T>
    static uint64 Owner(T value) noexcept
    {
        return value.Owner;
    }
    template <typename T>
    static uint64 Generation(T value) noexcept
    {
        return value.Generation;
    }
    template <typename T>
    static usize Slot(T value) noexcept
    {
        return value.Slot;
    }
    struct Identity final
    {
        uint64 Owner, Generation;
        usize Slot;
    };
    template <typename T>
    static T Make(Identity identity) noexcept
    {
        T value;
        value.Owner = identity.Owner;
        value.Generation = identity.Generation;
        value.Slot = static_cast<uint32>(identity.Slot);
        return value;
    }
};
} // namespace internal
namespace
{
using internal::Access;
constexpr usize NO_MATERIAL = ~usize{0}, NO_SHADOW = ~usize{0};
/// Packet/item marker for a flat draw using the bound program's zero-filled default uniforms.
constexpr usize NO_UNIFORM = ~usize{0};
constexpr usize LIT_SHADOW_OFFSET = FORWARD_SHADOW_OFFSET;
constexpr usize CLUSTER_SHADOW_OFFSET = CLUSTERED_SHADOW_OFFSET;
constexpr usize LIT_BYTES = FORWARD_SURFACE_OFFSET;
constexpr usize CLUSTER_GRID_OFFSET = CLUSTERED_GRID_OFFSET;
constexpr usize CLUSTER_BYTES = CLUSTERED_SURFACE_OFFSET;
constexpr usize AUTHORED_PARAMETER_BYTES = LUDUS_AUTHORED_PARAMETER_WORDS * sizeof(math::Vector4);
constexpr usize CLUSTER_LIGHT_WORD_COUNT = static_cast<usize>(CLUSTERED_LIGHT_COUNT) * FORWARD_LIGHT_WORDS;
constexpr usize LIGHT_GRID_ROW_COUNT = CLUSTERED_GRID_ROWS;
constexpr usize LEGACY_LIGHT_PARAMETER_BYTES = LIT_BYTES + sizeof(math::Vector4) + AUTHORED_PARAMETER_BYTES;
// Owner-thread scratch is charged to Limits::HostBytes. Keeping large reflected
// layouts here avoids overflowing the browser's default 64 KiB call stack.
struct LitMaterialParameters final
{
    math::Vector4 Tint;
    math::Vector4 UvTransform;
    math::Vector4 Options;
    math::Vector4 Pbr;
    math::Vector4 Emission;
    math::Vector4 EnvironmentIrradiance;
    math::Vector4 EnvironmentRadiance;
    math::Vector4 BakeScale;
    math::Vector4 BakeUv;
    StaticArray<math::Vector4, CLUSTER_LIGHT_WORD_COUNT> Lights{};
    StaticArray<math::Vector4, SHADOW_DATA_WORDS> ShadowData{};
    StaticArray<math::Vector4, LIGHT_GRID_ROW_COUNT> GridRows{};
    math::Vector4 GridMinimum{};
    math::Vector4 GridScale{};
    math::Vector4 SurfaceMap{};
    math::Vector4 Authored{};
    StaticArray<math::Vector4, internal::ENVIRONMENT_SH_COUNT> IrradianceSh{};
};
static_assert(sizeof(LitMaterialParameters) == CLUSTER_BYTES + sizeof(math::Vector4) + AUTHORED_PARAMETER_BYTES);
struct LitMaterialScratch final
{
    LitMaterialParameters Parameters{};
    StaticArray<uint8, LEGACY_LIGHT_PARAMETER_BYTES> Legacy{};
    LightSelection Selection{};
};
using ShadowRegionScratch = StaticArray<ShadowRegion, SHADOW_REGION_CAPACITY>;
constexpr uint64 LAST_ID = ~uint64{0};
// An explicit Initialize reserves identity; no resource registration at static initialization.
std::atomic<uint64> gNextOwner{1};
uint64 ReserveOwner() noexcept
{
    auto next = gNextOwner.load(std::memory_order_relaxed);
    while (next != LAST_ID)
    {
        if (gNextOwner.compare_exchange_weak(next, next + 1, std::memory_order_relaxed))
        {
            return next;
        }
    }
    return 0;
}
bool ValidCapacities(const Limits& limits) noexcept
{
    constexpr uint32 invalid = ~uint32{0};
    const uint32 counts[]{limits.Meshes,
                          limits.Snapshots,
                          limits.Views,
                          limits.Objects,
                          limits.Presentations,
                          limits.MaterialPrograms,
                          limits.Materials,
                          limits.ShadowAtlases,
                          limits.UniformSets};
    for (const auto count : counts)
    {
        if (count == 0 || count == invalid)
        {
            return false;
        }
    }
    // Mesh reference counters cover primary and LOD references in every snapshot.
    return limits.HostBytes != 0 && uint64(limits.Objects) * limits.Snapshots <= invalid / 2 &&
           // Snapshot references include prepared views, atlases and four RT1 captures.
           uint64(limits.Views) + limits.ShadowAtlases + 4 <= invalid;
}
bool Accepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
Status RendererAdmission(rhi::DeviceHandle device, bool mutation = true) noexcept
{
    rhi::DeviceInfo info;
    switch (rhi::GetDeviceInfo(device, info))
    {
        case rhi::DeviceStatus::Ready:
            return mutation && info.FrameOpen ? Status::InvalidState : Status::Ready;
        case rhi::DeviceStatus::InvalidHandle:
            return Status::InvalidHandle;
        case rhi::DeviceStatus::DeviceLost:
            return Status::DeviceLost;
        case rhi::DeviceStatus::Pending:
            return Status::NotReady;
        default:
            return Status::Failed;
    }
}
/// Largest world-unit distance a debug depth range may name (matches the camera-position bound
/// plus a full scene span), keeping the packed per-instance floats finite and ordered.
constexpr float32 MAX_DEBUG_DEPTH_DISTANCE = 10000000.0F;
/// Accepted scene lighting exposure multiplier.
constexpr float32 MIN_EXPOSURE = 0.000001F;
constexpr float32 MAX_EXPOSURE = 1000;
/// Smallest perceptual roughness a lit surface accepts; lower values alias badly.
constexpr float32 MIN_ROUGHNESS = 0.045F;
/// Pipeline variants a scene program compiles: lit programs without HDR need fewer.
constexpr uint32 LDR_LIT_PROGRAM_VARIANT_COUNT = 3;
constexpr uint32 PROGRAM_VARIANT_COUNT = 6;
/// Snapshot items per frustum-culling group.
constexpr usize CULL_GROUP_ITEM_COUNT = 16;
/// Output composite fragment bindings: uniforms, source texture and sampler.
constexpr usize OUTPUT_FRAGMENT_BINDING_COUNT = 3;
bool IsDebugDepthRangeValid(math::Vector2 range) noexcept
{
    return math::IsFinite(range) == true && ((range.X == 0 && range.Y == 0) || (range.X >= 0 && range.X < range.Y &&
                                                                                range.Y <= MAX_DEBUG_DEPTH_DISTANCE));
}
static_assert(static_cast<uint32>(MaterialDebugView::LightCount) == MATERIAL_DEBUG_VIEW_LIGHT_COUNT);
bool ValidColor(math::Vector4 color) noexcept
{
    return math::IsFinite(color) && color.X >= 0 && color.X <= 1 && color.Y >= 0 && color.Y <= 1 && color.Z >= 0 &&
           color.Z <= 1 && color.W >= 0 && color.W <= 1;
}
struct SlotIdentity
{
    uint64 Generation = 1;
    bool Used = false;
    bool Owned = false;
};
template <typename T>
Status FreeSlot(const T* slots, usize count, usize& output) noexcept
{
    bool exhausted = false;
    for (usize i = 0; i < count; ++i)
    {
        if (slots[i].Used == false)
        {
            if (slots[i].Generation != LAST_ID)
            {
                output = i;
                return Status::Ready;
            }
            exhausted = true;
        }
    }
    return exhausted ? Status::IdentityExhausted : Status::CapacityExceeded;
}
template <typename T, typename Handle>
T* Resolve(T* slots, usize count, uint64 owner, Handle handle) noexcept
{
    const auto index = Access::Slot(handle);
    if (Access::Owner(handle) != owner || index >= count)
    {
        return nullptr;
    }
    auto& slot = slots[index];
    return slot.Used && slot.Owned && slot.Generation == Access::Generation(handle) ? &slot : nullptr;
}
template <typename T>
Status FreeSlot(const Array<T>& slots, usize& output) noexcept
{
    return FreeSlot(slots.GetData(), slots.GetSize(), output);
}
template <typename T, usize N>
Status FreeSlot(const T (&slots)[N], usize& output) noexcept
{
    return FreeSlot(slots, N, output);
}
template <typename T, typename Handle>
T* Resolve(Array<T>& slots, uint64 owner, Handle handle) noexcept
{
    return Resolve(slots.GetData(), slots.GetSize(), owner, handle);
}
template <typename T, usize N, typename Handle>
T* Resolve(T (&slots)[N], uint64 owner, Handle handle) noexcept
{
    return Resolve(slots, N, owner, handle);
}
void Retire(SlotIdentity& slot) noexcept
{
    slot.Used = slot.Owned = false;
    ++slot.Generation; // LAST_ID slots are never admitted, so retirement cannot wrap.
}
math::Matrix4 Matrix(const math::Affine3& transform) noexcept
{
    return {{math::Vector4{transform.Columns[0].X, transform.Columns[0].Y, transform.Columns[0].Z, 0},
             math::Vector4{transform.Columns[1].X, transform.Columns[1].Y, transform.Columns[1].Z, 0},
             math::Vector4{transform.Columns[2].X, transform.Columns[2].Y, transform.Columns[2].Z, 0},
             math::Vector4{transform.Translation.X, transform.Translation.Y, transform.Translation.Z, 1}}};
}
struct Vertex final
{
    math::Vector3 Position;
    math::Vector2 Uv;
    math::Vector3 Normal;
    math::Vector2 BakeUv;
    math::Vector4 Tangent;
    math::Vector4 Color;
    math::Vector2 MaterialUv1;
};
using internal::VisibilityVertex;
struct Instance final
{
    math::Vector4 Rows[4];
    math::Vector4 Color;
    math::Vector4 WorldRows[3], NormalRows[3], Eye;
};
constexpr usize FLAT_STREAM_COUNT = 2;
constexpr usize FLAT_ATTRIBUTE_COUNT = 6;
constexpr StaticArray<rhi::RasterVertexStream, FLAT_STREAM_COUNT> FLAT_STREAMS{{
    {sizeof(Vertex), false},
    {sizeof(Instance), true},
}};
constexpr StaticArray<rhi::RasterVertexAttribute, FLAT_ATTRIBUTE_COUNT> FLAT_ATTRIBUTES{{
    {0, 0, 0, rhi::RasterVertexFormat::Float3},
    {1, 1, 0, rhi::RasterVertexFormat::Float4},
    {2, 1, sizeof(math::Vector4), rhi::RasterVertexFormat::Float4},
    {3, 1, 2 * sizeof(math::Vector4), rhi::RasterVertexFormat::Float4},
    {4, 1, 3 * sizeof(math::Vector4), rhi::RasterVertexFormat::Float4},
    {5, 1, 4 * sizeof(math::Vector4), rhi::RasterVertexFormat::Float4},
}};
constexpr usize VISIBILITY_ATTRIBUTE_COUNT = 8;
constexpr usize VISIBILITY_PRIMITIVE_LOCATION = 6;
constexpr usize VISIBILITY_INSTANCE_LOCATION = 7;
constexpr StaticArray<rhi::RasterVertexStream, FLAT_STREAM_COUNT> VISIBILITY_STREAMS{{
    {sizeof(VisibilityVertex), false},
    {sizeof(Instance), true},
}};
constexpr StaticArray<rhi::RasterVertexAttribute, VISIBILITY_ATTRIBUTE_COUNT> VISIBILITY_ATTRIBUTES{{
    {0, 0, 0, rhi::RasterVertexFormat::Float3},
    {1, 1, 0, rhi::RasterVertexFormat::Float4},
    {2, 1, sizeof(math::Vector4), rhi::RasterVertexFormat::Float4},
    {3, 1, 2 * sizeof(math::Vector4), rhi::RasterVertexFormat::Float4},
    {4, 1, 3 * sizeof(math::Vector4), rhi::RasterVertexFormat::Float4},
    {5, 1, 4 * sizeof(math::Vector4), rhi::RasterVertexFormat::Float4},
    {VISIBILITY_PRIMITIVE_LOCATION, 0, sizeof(math::Vector3), rhi::RasterVertexFormat::Float2},
    {VISIBILITY_INSTANCE_LOCATION, 1, sizeof(Instance) - sizeof(math::Vector4), rhi::RasterVertexFormat::Float2},
}};
/// Grid resources are private to one immutable prepared view; RHI retires submitted uses.
struct WorldGridBuffers final
{
    rhi::DeviceHandle Device{};
    rhi::BufferHandle Vertices{};
    rhi::BufferHandle Indices{};
    rhi::BufferHandle Instances{};
    uint32 Count = 0;
    usize PipelineCount = 0;
    ~WorldGridBuffers() noexcept
    {
        static_cast<void>(rhi::Destroy(Device, Vertices));
        static_cast<void>(rhi::Destroy(Device, Indices));
        static_cast<void>(rhi::Destroy(Device, Instances));
    }
};
constexpr usize WORLD_GRID_VERTEX_COUNT = 4;
constexpr usize WORLD_GRID_INDEX_COUNT = 6;
constexpr usize WORLD_GRID_PIPELINE_COUNT = 6;
constexpr usize WORLD_GRID_TARGET_VARIANTS = 2;
constexpr usize WORLD_GRID_UNORM_PIPELINE = 2;
constexpr usize WORLD_GRID_HDR_PIPELINE = 4;
constexpr usize WORLD_GRID_MATRIX_ROWS = 4;
/// Owner-thread scratch, charged to the renderer's host budget instead of the browser stack.
struct WorldGridScratch final
{
    StaticArray<WorldGridLine, WORLD_GRID_LINE_CAPACITY> Lines{};
    StaticArray<Instance, WORLD_GRID_LINE_CAPACITY> Instances{};
};
Status PrepareWorldGridBuffers(rhi::DeviceHandle device,
                               const ViewDescription& description,
                               WorldGridScratch& inoutScratch,
                               core::UniquePtr<WorldGridBuffers>& outBuffers) noexcept
{
    usize count = 0;
    float32 spacing = 0;
    if (BuildWorldGridLines(description, {inoutScratch.Lines.GetData(), WORLD_GRID_LINE_CAPACITY}, count, spacing) ==
        false)
    {
        return Status::InvalidDescription;
    }
    if (count == 0)
    {
        return Status::Ready;
    }
    core::UniquePtr<WorldGridBuffers> candidate;
    candidate.Reset(new (std::nothrow) WorldGridBuffers);
    if (candidate.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    candidate->Device = device;
    StaticArray<Vertex, WORLD_GRID_VERTEX_COUNT> vertices{};
    vertices[0].Position = {-1, -1, 0};
    vertices[1].Position = {1, -1, 0};
    vertices[2].Position = {1, 1, 0};
    vertices[WORLD_GRID_VERTEX_COUNT - 1].Position = {-1, 1, 0};
    constexpr StaticArray<uint32, WORLD_GRID_INDEX_COUNT> indices{0, 1, 2, 2, 3, 0};
    for (usize i = 0; i < count; ++i)
    {
        const WorldGridLine& line = inoutScratch.Lines[i];
        const math::Vector3 alongWorld = (line.EndWorld - line.StartWorld) * 0.5F;
        const math::Vector3 centerWorld = (line.StartWorld + line.EndWorld) * 0.5F;
        const math::Affine3 transformWorld{{alongWorld, line.HalfWidthWorld, {}}, centerWorld};
        const math::Matrix4 meshToClip = description.WorldToClip * math::ToMatrix4(transformWorld);
        Instance& instance = inoutScratch.Instances[i];
        instance = {};
        for (usize row = 0; row < WORLD_GRID_MATRIX_ROWS; ++row)
        {
            instance.Rows[row] = {meshToClip.At(row, 0),
                                  meshToClip.At(row, 1),
                                  meshToClip.At(row, 2),
                                  meshToClip.At(row, WORLD_GRID_MATRIX_ROWS - 1)};
        }
        instance.Color = line.Color;
    }
    Status status = rhi::CreateBuffer(device,
                                      {rhi::BufferRole::Vertex, sizeof(vertices)},
                                      {reinterpret_cast<const uint8*>(vertices.GetData()), sizeof(vertices)},
                                      candidate->Vertices);
    if (Accepted(status) == false)
    {
        return status;
    }
    bool isPending = status == Status::Pending;
    status = rhi::CreateBuffer(device,
                               {rhi::BufferRole::Index32, sizeof(indices)},
                               {reinterpret_cast<const uint8*>(indices.GetData()), sizeof(indices)},
                               candidate->Indices);
    if (Accepted(status) == false)
    {
        return status;
    }
    isPending = isPending == true || status == Status::Pending;
    const usize bytes = count * sizeof(Instance);
    status = rhi::CreateBuffer(device,
                               {rhi::BufferRole::Vertex, bytes},
                               {reinterpret_cast<const uint8*>(inoutScratch.Instances.GetData()), bytes},
                               candidate->Instances);
    if (Accepted(status) == false)
    {
        return status;
    }
    candidate->Count = static_cast<uint32>(count);
    outBuffers = Move(candidate);
    return isPending == true || status == Status::Pending ? Status::Pending : Status::Ready;
}
static_assert(sizeof(math::Vector3) == 12 && sizeof(Vertex) == LUDUS_AUTHORED_VERTEX_BYTES && sizeof(Instance) == 192);
struct Rect final
{
    float64 Left = -1, Bottom = -1, Right = 1, Top = 1;
};
Rect ProjectBounds(const math::Aabb3& bounds, const math::Matrix4& matrix, bool reverse = false) noexcept
{
    Rect result{1e300, 1e300, -1e300, -1e300};
    for (usize corner = 0; corner < 8; ++corner)
    {
        const float64 point[4]{static_cast<float64>((corner & 1) != 0 ? bounds.Max.X : bounds.Min.X),
                               static_cast<float64>((corner & 2) != 0 ? bounds.Max.Y : bounds.Min.Y),
                               static_cast<float64>((corner & 4) != 0 ? bounds.Max.Z : bounds.Min.Z),
                               1};
        float64 clip[4]{};
        for (usize row = 0; row < 4; ++row)
        {
            for (usize column = 0; column < 4; ++column)
            {
                clip[row] += static_cast<float64>(matrix.At(row, column)) * point[column];
            }
        }
        // Near-plane crossings conservatively block ordering. Projection division is setup-only.
        if (clip[3] <= 1e-6 || (reverse ? clip[3] - clip[2] <= 0 : clip[2] <= 0))
        {
            return {};
        }
        const auto x = clip[0] / clip[3], y = clip[1] / clip[3];
        if (x < result.Left)
        {
            result.Left = x;
        }
        if (x > result.Right)
        {
            result.Right = x;
        }
        if (y < result.Bottom)
        {
            result.Bottom = y;
        }
        if (y > result.Top)
        {
            result.Top = y;
        }
    }
    return result;
}
bool Overlap(const Rect& left, const Rect& right) noexcept
{
    constexpr float64 slack = 1e-5;
    return left.Right + slack >= right.Left && right.Right + slack >= left.Left && left.Top + slack >= right.Bottom &&
           right.Top + slack >= left.Bottom;
}
struct Candidate final
{
    usize Item = 0, MeshSlot = 0, MaterialSlot = NO_MATERIAL;
    usize UniformSlot = NO_UNIFORM;
    bool Coarse = false;
    Rect Bounds;
    Instance Data;
};
struct MeshLess final
{
    bool operator()(const Candidate& left, const Candidate& right) const noexcept
    {
        if (left.MaterialSlot != right.MaterialSlot)
        {
            return left.MaterialSlot < right.MaterialSlot;
        }
        return left.UniformSlot != right.UniformSlot ? left.UniformSlot < right.UniformSlot
                                                     : left.MeshSlot < right.MeshSlot;
    }
};
} // namespace
struct Renderer::State final
{
    struct MeshRecord final : SlotIdentity
    {
        core::UniquePtr<internal::ReferenceMeshData> Reference;
        rhi::BufferHandle Vertices{}, Indices{};
        uint32 VertexCount = 0, IndexCount = 0, References = 0;
        bool HasNormals = false;
        math::Aabb3 Bounds;
    };
    struct Item final
    {
        SceneItem Input;
        usize MeshSlot = 0, MaterialSlot = NO_MATERIAL;
        usize UniformSlot = NO_UNIFORM;
        math::Aabb3 Bounds;
    };
    struct SnapshotRecord final : SlotIdentity
    {
        Array<Item> Items;
        Array<math::Aabb3> Groups;
        Array<bool> HasGroup;
        usize Count = 0;
        uint32 References = 0;
    };
    struct Packet final
    {
        usize MeshSlot = 0, First = 0;
        uint32 Count = 0;
        Layer Pass = Layer::Opaque;
        usize MaterialSlot = NO_MATERIAL;
        usize UniformSlot = NO_UNIFORM;
    };
    struct ViewRecord final : SlotIdentity
    {
        rhi::BufferHandle Instances{};
        core::UniquePtr<WorldGridBuffers> Grid;
        rhi::UploadTicket Upload{};
        bool UploadOwned = false;
        rhi::CommandBatch Retry{};
        bool HasRetry = false;
        usize SnapshotSlot = 0, Visible = 0, DrawCount = 0;
        DepthConvention Depth = DepthConvention::Conventional;
        Array<Packet> Packets;
    };
    struct PresentationRecord final : SlotIdentity
    {
        rhi::BufferHandle Vertices{};
        rhi::TextureViewHandle Texture{};
        rhi::BindingSetHandle Bindings{};
        ViewMapping Mapping;
        uint32 Phase = 0;
        Status Setup = Status::Pending;
        bool Lighting = false, Encode = false;
        rhi::BufferHandle OutputParameters[2]{};
        rhi::BindingSetHandle OutputBindings[2]{};
    };
    struct ProgramRecord final : SlotIdentity
    {
        rhi::RasterShaderHandle Vertex{};
        rhi::RasterShaderHandle Fragment{};
        rhi::RasterShaderHandle ShadowFragment{};
        rhi::RasterShaderHandle DepthFragment{};
        rhi::RasterShaderHandle GBufferFragment{};
        static constexpr usize GBUFFER_PIPELINE_COUNT = 2;
        StaticArray<rhi::RasterPipelineHandle, GBUFFER_PIPELINE_COUNT> GBufferPipelines{};
        bool HasGBuffer = false;
        rhi::BindingLayoutHandle Layout{};
        rhi::TextureHandle White{};
        rhi::RasterPipelineHandle ShadowPipeline{};
        rhi::RasterPipelineHandle DepthPipeline{};
        bool Lit = false;
        bool Hdr = false;
        bool Clustered = false;
        bool HasSurfaceMapProfile = false;
        bool HasAuthoredProfile = false;
        uint32 VariantCount = 6;
        static constexpr usize REQUEST_CAPACITY = PROGRAM_VARIANT_COUNT + 2 + GBUFFER_PIPELINE_COUNT;
        StaticArray<rhi::PipelineRequest, REQUEST_CAPACITY> Requests{};
        bool HasDepthPrepass = false;
        rhi::RasterPipelineHandle Pipelines[6]{};
        uint32 References = 0;
        uint32 Phase = 0;
        Status Setup = Status::Pending;
    };
    struct MaterialRecord final : SlotIdentity
    {
        core::UniquePtr<internal::ReferenceSurfaceData> Reference;
        rhi::BufferHandle Parameters{};
        rhi::SamplerHandle Sampler{};
        rhi::TextureViewHandle Texture{};
        rhi::BindingSetHandle Bindings{};
        usize ProgramSlot = 0;
        uint32 References = 0;
        MaterialAlpha Alpha = MaterialAlpha::Opaque;
        bool BindingCreated = false;
        Status Setup = Status::Pending;
        bool Lit = false;
        bool HasBake = false;
        bool Clustered = false;
        bool ClusterViewCreated = false;
        rhi::TextureHandle ClusterTexture{};
        rhi::TextureViewHandle ClusterView{};
        usize ParameterSize = 48;
        usize ShadowSlot = NO_SHADOW;
        rhi::TextureViewHandle BakeTexture{};
        rhi::TextureViewHandle ShadowTexture{};
        rhi::TextureViewHandle SurfaceTexture{};
        internal::EnvironmentData* EnvironmentOrNull = nullptr;
        rhi::TextureViewHandle EnvironmentTexture{};
        rhi::TextureViewHandle EmissionTexture{};
        rhi::SamplerHandle ShadowSampler{};
        BakedLighting Bake;
    };
    struct ShadowRecord final : SlotIdentity
    {
        rhi::TextureHandle Texture{};
        StaticArray<PreparedView, SHADOW_REGION_CAPACITY> Views{};
        StaticArray<ShadowRegion, SHADOW_REGION_CAPACITY> Regions{};
        usize Count = 0, SnapshotSlot = 0;
        uint32 Width = 0, Height = 0, References = 0;
        bool Rendered = false, Failed = false;
    };
    Array<ShadowRecord> Shadows;
    Status LightingSetup = Status::NotReady;
    uint32 LightingPhase = 0;
    bool Hdr = false;
    rhi::RasterShaderHandle OutputVertex{}, OutputFragment{}, FlatShadowFragment{};
    rhi::BindingLayoutHandle OutputLayout{};
    rhi::RasterPipelineHandle OutputPipelines[2]{}, HdrFlat[3]{}, FlatShadow{};
    Array<ProgramRecord> Programs;
    Array<MaterialRecord> Materials;
    Array<PresentationRecord> Presentations;
    rhi::RasterShaderHandle CompositeVertex{}, CompositeFragment{};
    rhi::BindingLayoutHandle CompositeLayout{};
    rhi::SamplerHandle CompositeSampler{};
    rhi::BufferHandle CompositeIndices{};
    rhi::RasterPipelineHandle SurfaceReverse{}, TargetOpaque{}, TargetReverse{}, TargetOverlay{}, Composite{};
    Status ViewsSetup = Status::NotReady;
    uint32 ViewsPhase = 0;
    uint64 Owner = 0;
    rhi::DeviceHandle Device;
    rhi::RasterShaderHandle Vertex{}, Fragment{};
    rhi::BindingLayoutHandle Layout{};
    rhi::BindingSetHandle Bindings{};
    /// Bound flat program layout (empty when unbound), its zero-filled defaults and per-draw sets.
    internal::FlatUniformLayout FlatUniforms;
    bool HasVisibilityInputs = false;
    rhi::BufferHandle DefaultUniforms{};
    internal::UniformSetTable UniformSets;
    rhi::RasterPipelineHandle Opaque{}, Overlay{};
    uint32 Phase = 0;
    Status Setup = Status::Pending;
    Array<MeshRecord> MeshRecords;
    Array<SnapshotRecord> SnapshotRecords;
    Array<ViewRecord> ViewRecords;
    // Reserved once: PrepareView must fit the browser's default 64 KiB stack.
    Array<Candidate> Candidates, Scratch;
    Array<Instance> Instances;
    Array<math::FrustumRelation> GroupRelations;
    core::UniquePtr<WorldGridScratch> GridScratch;
    StaticArray<rhi::RasterPipelineHandle, WORLD_GRID_PIPELINE_COUNT> GridPipelines{};
    StaticArray<bool, WORLD_GRID_PIPELINE_COUNT> HasGridPipelines{};
    Status PrepareGridPipelines() noexcept;
    Limits Capacities;
    usize HostBytes = 0;
    Status ReserveStorage(const Limits&) noexcept;
    core::UniquePtr<LightClusters> ClusterScratch;
    core::UniquePtr<LitMaterialScratch> LitScratch;
    core::UniquePtr<ShadowRegionScratch> ShadowScratch;
    core::UniquePtr<internal::ReferenceState> Reference;
    core::UniquePtr<internal::GpuPreviewState> GpuPreview;
    void DropGpuPreview(usize) noexcept;
    void RetireGpuPreviews() noexcept;
    void ClearGpuPreview() noexcept;
    Status PrepareReferenceMesh(const MeshDescription&,
                                const struct Vertex*,
                                core::UniquePtr<internal::ReferenceMeshData>&) noexcept;
    Status PrepareReferenceSurface(const MaterialDescription&,
                                   const LitMaterialDescription*,
                                   core::UniquePtr<internal::ReferenceSurfaceData>&) noexcept;
    Status BuildReferenceJob(usize,
                             const ReferenceCaptureDescription&,
                             core::UniquePtr<internal::ReferenceJob>&,
                             bool gpuSetup = false) noexcept;
    usize RemainingReferenceRayBytes(Status&) noexcept;
    void DropReferenceCapture(usize) noexcept;
    void ClearReference() noexcept;
    ~State()
    {
        ClearGpuPreview();
        ClearReference();
        for (rhi::RasterPipelineHandle pipeline : GridPipelines)
        {
            static_cast<void>(rhi::Destroy(Device, pipeline));
        }
        for (usize i = 0; i < ViewRecords.GetSize(); ++i)
        {
            if (ViewRecords[i].Used)
            {
                DropView(i);
            }
        }
        for (usize i = 0; i < SnapshotRecords.GetSize(); ++i)
        {
            auto& scene = SnapshotRecords[i];
            if (scene.Used)
            {
                scene.Owned = false;
                DropSnapshot(i);
            }
        }
        for (auto& mesh : MeshRecords)
        {
            if (mesh.Used)
            {
                static_cast<void>(rhi::Destroy(Device, mesh.Vertices));
                static_cast<void>(rhi::Destroy(Device, mesh.Indices));
            }
        }
        for (usize i = 0; i < Materials.GetSize(); ++i)
        {
            if (Materials[i].Used)
            {
                Materials[i].Owned = false;
                DropMaterial(i);
            }
        }
        for (usize i = 0; i < Programs.GetSize(); ++i)
        {
            if (Programs[i].Used)
            {
                Programs[i].Owned = false;
                DropProgram(i);
            }
        }
        for (usize i = 0; i < Shadows.GetSize(); ++i)
        {
            if (Shadows[i].Used)
            {
                Shadows[i].Owned = false;
                DropShadow(i);
            }
        }
        for (auto pipeline : OutputPipelines)
        {
            static_cast<void>(rhi::Destroy(Device, pipeline));
        }
        for (auto pipeline : HdrFlat)
        {
            static_cast<void>(rhi::Destroy(Device, pipeline));
        }
        static_cast<void>(rhi::Destroy(Device, FlatShadow));
        static_cast<void>(rhi::Destroy(Device, OutputLayout));
        static_cast<void>(rhi::Destroy(Device, OutputVertex));
        static_cast<void>(rhi::Destroy(Device, OutputFragment));
        static_cast<void>(rhi::Destroy(Device, FlatShadowFragment));
        for (auto& presentation : Presentations)
        {
            if (presentation.Used)
            {
                DropPresentation(presentation);
            }
        }
        const rhi::RasterPipelineHandle extra[]{SurfaceReverse, TargetOpaque, TargetReverse, TargetOverlay, Composite};
        for (auto pipeline : extra)
        {
            static_cast<void>(rhi::Destroy(Device, pipeline));
        }
        static_cast<void>(rhi::Destroy(Device, CompositeIndices));
        static_cast<void>(rhi::Destroy(Device, CompositeSampler));
        static_cast<void>(rhi::Destroy(Device, CompositeLayout));
        static_cast<void>(rhi::Destroy(Device, CompositeFragment));
        static_cast<void>(rhi::Destroy(Device, CompositeVertex));
        static_cast<void>(rhi::Destroy(Device, Overlay));
        static_cast<void>(rhi::Destroy(Device, Opaque));
        UniformSets.Clear(Device);
        static_cast<void>(rhi::Destroy(Device, DefaultUniforms));
        static_cast<void>(rhi::Destroy(Device, Bindings));
        static_cast<void>(rhi::Destroy(Device, Layout));
        static_cast<void>(rhi::Destroy(Device, Fragment));
        static_cast<void>(rhi::Destroy(Device, Vertex));
    }
    void DropProgram(usize index) noexcept
    {
        auto& p = Programs[index];
        if (p.Owned == false && p.References == 0)
        {
            for (auto& request : p.Requests)
            {
                static_cast<void>(rhi::Release(Device, request));
            }
            static_cast<void>(rhi::Destroy(Device, p.ShadowFragment));
            static_cast<void>(rhi::Destroy(Device, p.DepthFragment));
            static_cast<void>(rhi::Destroy(Device, p.GBufferFragment));
            static_cast<void>(rhi::Destroy(Device, p.White));
            static_cast<void>(rhi::Destroy(Device, p.Layout));
            static_cast<void>(rhi::Destroy(Device, p.Fragment));
            static_cast<void>(rhi::Destroy(Device, p.Vertex));
            const auto generation = p.Generation + 1;
            p = {};
            p.Generation = generation;
        }
    }
    void DropMaterial(usize index) noexcept
    {
        auto& m = Materials[index];
        if (m.Owned == false && m.References == 0)
        {
            static_cast<void>(rhi::Destroy(Device, m.Bindings));
            static_cast<void>(rhi::Destroy(Device, m.Texture));
            static_cast<void>(rhi::Destroy(Device, m.Sampler));
            static_cast<void>(rhi::Destroy(Device, m.Parameters));
            static_cast<void>(rhi::Destroy(Device, m.BakeTexture));
            static_cast<void>(rhi::Destroy(Device, m.SurfaceTexture));
            static_cast<void>(rhi::Destroy(Device, m.EnvironmentTexture));
            internal::ReleaseEnvironment(m.EnvironmentOrNull);
            static_cast<void>(rhi::Destroy(Device, m.EmissionTexture));
            static_cast<void>(rhi::Destroy(Device, m.ShadowTexture));
            static_cast<void>(rhi::Destroy(Device, m.ShadowSampler));
            static_cast<void>(rhi::Destroy(Device, m.ClusterView));
            static_cast<void>(rhi::Destroy(Device, m.ClusterTexture));
            if (m.ShadowSlot != NO_SHADOW)
            {
                --Shadows[m.ShadowSlot].References;
                DropShadow(m.ShadowSlot);
            }
            --Programs[m.ProgramSlot].References;
            DropProgram(m.ProgramSlot);
            if (m.Reference && Reference)
            {
                Reference->CpuBytes -= sizeof(internal::ReferenceSurfaceData);
            }
            m.Reference.Reset();
            const auto generation = m.Generation + 1;
            m = {};
            m.Generation = generation;
        }
    }
    void DropShadow(usize index) noexcept
    {
        auto& shadow = Shadows[index];
        if (shadow.Owned == false && shadow.References == 0)
        {
            for (usize i = 0; i < shadow.Count; ++i)
            {
                if (Resolve(ViewRecords, Owner, shadow.Views[i]) != nullptr)
                {
                    DropView(Access::Slot(shadow.Views[i]));
                }
            }
            static_cast<void>(rhi::Destroy(Device, shadow.Texture));
            --SnapshotRecords[shadow.SnapshotSlot].References;
            DropSnapshot(shadow.SnapshotSlot);
            const auto generation = shadow.Generation + 1;
            shadow = {};
            shadow.Generation = generation;
        }
    }
    [[nodiscard]] bool SameCasters(const SnapshotRecord& a, usize right) const noexcept
    {
        const auto& b = SnapshotRecords[right];
        usize ca = 0, cb = 0;
        for (usize i = 0; i < a.Count; ++i)
        {
            ca += a.Items[i].Input.Pass == Layer::Opaque ? 1U : 0U;
        }
        for (usize i = 0; i < b.Count; ++i)
        {
            cb += b.Items[i].Input.Pass == Layer::Opaque ? 1U : 0U;
        }
        if (ca != cb)
        {
            return false;
        }
        for (usize i = 0; i < a.Count; ++i)
        {
            const auto& x = a.Items[i];
            if (x.Input.Pass != Layer::Opaque)
            {
                continue;
            }
            bool found = false;
            for (usize j = 0; j < b.Count; ++j)
            {
                const auto& y = b.Items[j];
                if (y.Input.Pass != Layer::Opaque || x.Input.SourceId != y.Input.SourceId)
                {
                    continue;
                }
                if (x.MeshSlot != y.MeshSlot || x.Input.Transform != y.Input.Transform)
                {
                    return false;
                }
                const auto ax = x.MaterialSlot == NO_MATERIAL ? MaterialAlpha::Opaque : Materials[x.MaterialSlot].Alpha;
                const auto ay = y.MaterialSlot == NO_MATERIAL ? MaterialAlpha::Opaque : Materials[y.MaterialSlot].Alpha;
                if (ax != ay || (ax == MaterialAlpha::Masked && x.MaterialSlot != y.MaterialSlot))
                {
                    return false;
                }
                found = true;
                break;
            }
            if (found == false)
            {
                return false;
            }
        }
        return true;
    }
    void DropPresentation(PresentationRecord& presentation) noexcept
    {
        for (auto& set : presentation.OutputBindings)
        {
            static_cast<void>(rhi::Destroy(Device, set));
        }
        for (auto& buffer : presentation.OutputParameters)
        {
            static_cast<void>(rhi::Destroy(Device, buffer));
        }
        static_cast<void>(rhi::Destroy(Device, presentation.Bindings));
        static_cast<void>(rhi::Destroy(Device, presentation.Texture));
        static_cast<void>(rhi::Destroy(Device, presentation.Vertices));
        Retire(presentation);
    }
    void DropMesh(usize index) noexcept
    {
        auto& mesh = MeshRecords[index];
        if (mesh.Owned == false && mesh.References == 0)
        {
            static_cast<void>(rhi::Destroy(Device, mesh.Vertices));
            static_cast<void>(rhi::Destroy(Device, mesh.Indices));
            if (mesh.Reference && Reference)
            {
                Reference->CpuBytes -= mesh.Reference->CpuBytes;
            }
            mesh.Reference.Reset();
            Retire(mesh);
        }
    }
    void DropSnapshot(usize index) noexcept
    {
        auto& scene = SnapshotRecords[index];
        if (scene.Owned == false && scene.References == 0)
        {
            for (usize i = 0; i < scene.Count; ++i)
            {
                const auto coarse = scene.Items[i].Input.LodGeometry;
                if (Access::Owner(coarse) != 0)
                {
                    --MeshRecords[Access::Slot(coarse)].References;
                    DropMesh(Access::Slot(coarse));
                }
                const auto mesh = scene.Items[i].MeshSlot;
                --MeshRecords[mesh].References;
                DropMesh(mesh);
                const auto material = scene.Items[i].MaterialSlot;
                if (material != NO_MATERIAL)
                {
                    --Materials[material].References;
                    DropMaterial(material);
                }
                if (scene.Items[i].UniformSlot != NO_UNIFORM)
                {
                    UniformSets.Unretain(Device, scene.Items[i].UniformSlot);
                }
            }
            Retire(scene);
        }
    }
    void DropView(usize index) noexcept
    {
        auto& view = ViewRecords[index];
        if (view.HasRetry)
        {
            static_cast<void>(rhi::DiscardCommands(Device, view.Retry));
        }
        if (view.UploadOwned)
        {
            static_cast<void>(rhi::Release(Device, view.Upload));
            view.UploadOwned = false;
        }
        if (view.Visible != 0)
        {
            static_cast<void>(rhi::Destroy(Device, view.Instances));
        }
        if (view.Grid.Get() != nullptr)
        {
            HostBytes -= sizeof(WorldGridBuffers);
            view.Grid.Reset();
        }
        auto& scene = SnapshotRecords[view.SnapshotSlot];
        --scene.References;
        DropSnapshot(view.SnapshotSlot);
        view.HasRetry = false;
        Retire(view);
    }
};
Status Renderer::State::ReserveStorage(const Limits& limits) noexcept
{
    usize bytes = sizeof(State);
    const usize groups = usize(limits.Objects) / 16 + (limits.Objects % 16 != 0 ? 1U : 0U);
    const auto charge = [&bytes](usize count, usize width) noexcept {
        usize requested = 0;
        return TryMultiply(count, width, requested) && TryAdd(bytes, requested, bytes);
    };
    usize snapshotItems = 0, snapshotGroups = 0, viewPackets = 0;
    if (TryMultiply(usize(limits.Snapshots), usize(limits.Objects), snapshotItems) == false ||
        TryMultiply(usize(limits.Snapshots), groups, snapshotGroups) == false ||
        TryMultiply(usize(limits.Views), usize(limits.Objects), viewPackets) == false ||
        charge(limits.Meshes, sizeof(MeshRecord)) == false ||
        charge(limits.Snapshots, sizeof(SnapshotRecord)) == false ||
        charge(limits.Views, sizeof(ViewRecord)) == false ||
        charge(limits.Materials, sizeof(MaterialRecord)) == false ||
        charge(limits.MaterialPrograms, sizeof(ProgramRecord)) == false ||
        charge(limits.ShadowAtlases, sizeof(ShadowRecord)) == false ||
        charge(limits.Presentations, sizeof(PresentationRecord)) == false ||
        charge(snapshotItems, sizeof(Item)) == false ||
        charge(snapshotGroups, sizeof(math::Aabb3) + sizeof(bool)) == false ||
        charge(viewPackets, sizeof(Packet)) == false ||
        charge(limits.Objects, 2 * sizeof(Candidate) + sizeof(Instance)) == false ||
        charge(groups, sizeof(math::FrustumRelation)) == false ||
        TryAdd(bytes, internal::UniformSetTable::GetHostBytes(limits.UniformSets, FlatUniforms.BufferBytes), bytes) ==
            false ||
        bytes > limits.HostBytes)
    {
        return Status::CapacityExceeded;
    }
    // Exact reservations avoid geometric slack and growth overlap; addresses stay
    // stable for the renderer lifetime. PrepareView reuses these CPU workspaces.
    const auto reserve = []<typename T>(Array<T>& values, usize count) noexcept {
        return values.TryEnsureCapacity(count) && values.TryResize(count);
    };
    if (reserve(MeshRecords, limits.Meshes) == false || reserve(SnapshotRecords, limits.Snapshots) == false ||
        reserve(ViewRecords, limits.Views) == false || reserve(Materials, limits.Materials) == false ||
        reserve(Programs, limits.MaterialPrograms) == false || reserve(Shadows, limits.ShadowAtlases) == false ||
        reserve(Presentations, limits.Presentations) == false || reserve(Candidates, limits.Objects) == false ||
        reserve(Scratch, limits.Objects) == false || reserve(Instances, limits.Objects) == false ||
        reserve(GroupRelations, groups) == false ||
        UniformSets.Reserve(limits.UniformSets, FlatUniforms.BufferBytes) == false)
    {
        return Status::OutOfMemory;
    }
    for (auto& snapshot : SnapshotRecords)
    {
        if (reserve(snapshot.Items, limits.Objects) == false || reserve(snapshot.Groups, groups) == false ||
            reserve(snapshot.HasGroup, groups) == false)
        {
            return Status::OutOfMemory;
        }
    }
    for (auto& view : ViewRecords)
    {
        if (reserve(view.Packets, limits.Objects) == false)
        {
            return Status::OutOfMemory;
        }
    }
    Capacities = limits;
    HostBytes = bytes;
    return Status::Ready;
}
Renderer::Renderer() noexcept = default;
Renderer::~Renderer() = default;
Limits Renderer::GetLimits() noexcept
{
    return {};
}
Limits Renderer::GetConfiguredLimits() const noexcept
{
    return mState ? mState->Capacities : Limits{};
}
usize Renderer::GetHostBytes() const noexcept
{
    return mState ? mState->HostBytes : 0;
}
void Renderer::Reset() noexcept
{
    mState.Reset();
}
Status Renderer::Initialize(rhi::DeviceHandle device,
                            const rhi::RasterShaderDescription& vertex,
                            const rhi::RasterShaderDescription& fragment,
                            const Limits& limits) noexcept
{
    if (mState)
    {
        return Status::InvalidState;
    }
    if (ValidCapacities(limits) == false)
    {
        return Status::InvalidDescription;
    }
    rhi::RasterCapabilities capabilities;
    auto status = rhi::GetRasterCapabilities(device, capabilities);
    if (status != Status::Ready)
    {
        return status;
    }
    if (capabilities.DrawsPerFrame < limits.Objects || capabilities.ResourcesPerKind < 16)
    {
        return Status::Unsupported;
    }
    internal::FlatUniformLayout uniforms;
    if (vertex.Artifact.Stage != rhi::ShaderStage::Vertex || fragment.Artifact.Stage != rhi::ShaderStage::Fragment ||
        (vertex.Inputs.size() != FLAT_ATTRIBUTE_COUNT && vertex.Inputs.size() != VISIBILITY_ATTRIBUTE_COUNT) ||
        fragment.Inputs.empty() == false ||
        internal::MergeFlatUniformLayout(vertex.Bindings,
                                         fragment.Bindings,
                                         capabilities.UniformOffsetAlignment,
                                         capabilities.MaxUniformRange,
                                         uniforms) == false)
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < vertex.Inputs.size(); ++i)
    {
        if (vertex.Inputs[i].Location != i ||
            vertex.Inputs[i].Format != (i == 0                     ? rhi::RasterVertexFormat::Float3
                                        : i < FLAT_ATTRIBUTE_COUNT ? rhi::RasterVertexFormat::Float4
                                                                   : rhi::RasterVertexFormat::Float2))
        {
            return Status::InvalidDescription;
        }
    }
    auto* state = new (std::nothrow) State;
    if (state == nullptr)
    {
        return Status::OutOfMemory;
    }
    mState.Reset(state);
    state->Device = device;
    state->FlatUniforms = uniforms;
    state->HasVisibilityInputs = vertex.Inputs.size() == VISIBILITY_ATTRIBUTE_COUNT;
    status = state->ReserveStorage(limits);
    if (status != Status::Ready)
    {
        Reset();
        return status;
    }
    state->Owner = ReserveOwner();
    if (state->Owner == 0)
    {
        Reset();
        return Status::IdentityExhausted;
    }
    status = rhi::CreateRasterShader(device, vertex, state->Vertex);
    if (Accepted(status))
    {
        status = rhi::CreateRasterShader(device, fragment, state->Fragment);
    }
    if (Accepted(status) == false)
    {
        Reset();
        return status;
    }
    status = Poll();
    if (Accepted(status) == false)
    {
        Reset();
    }
    return status;
}
Status Renderer::Poll() noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    auto& state = *mState;
    if (state.Setup != Status::Pending)
    {
        return state.Setup;
    }
    const StaticArray<rhi::RasterVertexStream, FLAT_STREAM_COUNT>& streams =
        state.HasVisibilityInputs == true ? VISIBILITY_STREAMS : FLAT_STREAMS;
    const rhi::RasterVertexAttribute* attributes =
        state.HasVisibilityInputs == true ? VISIBILITY_ATTRIBUTES.GetData() : FLAT_ATTRIBUTES.GetData();
    const usize attributeCount = state.HasVisibilityInputs == true ? VISIBILITY_ATTRIBUTE_COUNT : FLAT_ATTRIBUTE_COUNT;
    for (;;)
    {
        Status status = Status::Ready;
        switch (state.Phase)
        {
            case 0:
                status = rhi::GetStatus(state.Device, state.Vertex);
                if (status == Status::Ready)
                {
                    status = rhi::GetStatus(state.Device, state.Fragment);
                }
                if (status == Status::Ready)
                {
                    status = rhi::CreateBindingLayout(state.Device,
                                                      {state.FlatUniforms.Bindings.GetData(), state.FlatUniforms.Count},
                                                      state.Layout);
                    if (Accepted(status) && state.FlatUniforms.Count != 0)
                    {
                        status =
                            state.UniformSets.CreateDefault(state.Device, state.FlatUniforms, state.DefaultUniforms);
                    }
                    ++state.Phase;
                }
                break;
            case 1:
                status = rhi::GetStatus(state.Device, state.Layout);
                if (status == Status::Ready && state.FlatUniforms.Count != 0)
                {
                    status = rhi::GetStatus(state.Device, state.DefaultUniforms);
                }
                if (status == Status::Ready)
                {
                    state.FlatUniforms.Handle = state.Layout;
                    status = state.FlatUniforms.Count == 0
                                 ? rhi::CreateBindingSet(state.Device, state.Layout, {}, state.Bindings)
                                 : internal::CreateFlatUniformBindings(state.Device,
                                                                       state.FlatUniforms,
                                                                       state.DefaultUniforms,
                                                                       state.Bindings);
                    ++state.Phase;
                }
                break;
            case 2:
                status = rhi::GetStatus(state.Device, state.Bindings);
                if (status == Status::Ready)
                {
                    status = rhi::CreateRasterPipeline(
                        state.Device,
                        {state.Vertex, state.Fragment, state.Layout, streams, {attributes, attributeCount}, true},
                        state.Opaque);
                    ++state.Phase;
                }
                break;
            case 3:
                status = rhi::GetStatus(state.Device, state.Opaque);
                if (status == Status::Ready)
                {
                    status = rhi::CreateRasterPipeline(state.Device,
                                                       {state.Vertex,
                                                        state.Fragment,
                                                        state.Layout,
                                                        streams,
                                                        {attributes, attributeCount},
                                                        false,
                                                        true},
                                                       state.Overlay);
                    ++state.Phase;
                }
                break;
            default:
                status = rhi::GetStatus(state.Device, state.Overlay);
                if (status == Status::Ready)
                {
                    state.Setup = Status::Ready;
                }
                break;
        }
        if (status == Status::Pending)
        {
            return status;
        }
        if (status != Status::Ready)
        {
            state.Setup = status;
            return status;
        }
        if (state.Setup == Status::Ready)
        {
            return Status::Ready;
        }
    }
}
Status Renderer::InitializeViews(const rhi::RasterShaderDescription& vertex,
                                 const rhi::RasterShaderDescription& fragment) noexcept
{
    if (mState.Get() == nullptr || mState->Setup != Status::Ready)
    {
        return Status::NotReady;
    }
    auto& s = *mState;
    if (s.ViewsSetup != Status::NotReady)
    {
        return Status::InvalidState;
    }
    if (vertex.Artifact.Stage != rhi::ShaderStage::Vertex || fragment.Artifact.Stage != rhi::ShaderStage::Fragment ||
        vertex.Inputs.size() != 1 || vertex.Inputs[0].Location != 0 ||
        vertex.Inputs[0].Format != rhi::RasterVertexFormat::Float4 || vertex.Bindings.empty() == false ||
        fragment.Inputs.empty() == false || fragment.Bindings.size() != 2)
    {
        return Status::InvalidDescription;
    }
    const auto& texture = fragment.Bindings[0];
    const auto& sampler = fragment.Bindings[1];
    if (texture.Binding != 0 || texture.Kind != rhi::RasterBindingKind::Texture2D ||
        texture.Visibility != rhi::RasterVisibility::Fragment || sampler.Binding != 1 ||
        sampler.Kind != rhi::RasterBindingKind::Sampler || sampler.Visibility != rhi::RasterVisibility::Fragment ||
        texture.MinSize != 0 || sampler.MinSize != 0)
    {
        return Status::InvalidDescription;
    }
    auto status = rhi::CreateRasterShader(s.Device, vertex, s.CompositeVertex);
    if (Accepted(status))
    {
        status = rhi::CreateRasterShader(s.Device, fragment, s.CompositeFragment);
    }
    if (Accepted(status) == false)
    {
        static_cast<void>(rhi::Destroy(s.Device, s.CompositeVertex));
        static_cast<void>(rhi::Destroy(s.Device, s.CompositeFragment));
        s.CompositeVertex = {};
        s.CompositeFragment = {};
        return status;
    }
    s.ViewsSetup = Status::Pending;
    return PollViews();
}
Status Renderer::PollViews() noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidState;
    }
    auto& s = *mState;
    if (s.ViewsSetup != Status::Pending)
    {
        return s.ViewsSetup;
    }
    const StaticArray<rhi::RasterVertexStream, FLAT_STREAM_COUNT>& streams =
        mState->HasVisibilityInputs == true ? VISIBILITY_STREAMS : FLAT_STREAMS;
    const rhi::RasterVertexAttribute* attributes =
        mState->HasVisibilityInputs == true ? VISIBILITY_ATTRIBUTES.GetData() : FLAT_ATTRIBUTES.GetData();
    const usize attributeCount =
        mState->HasVisibilityInputs == true ? VISIBILITY_ATTRIBUTE_COUNT : FLAT_ATTRIBUTE_COUNT;
    const rhi::RasterVertexStream quadStreams[]{{16, false}};
    const rhi::RasterVertexAttribute quadAttributes[]{{0, 0, 0, rhi::RasterVertexFormat::Float4}};
    for (;;)
    {
        auto status = Status::Ready;
        if (s.ViewsPhase == 0)
        {
            status = rhi::GetStatus(s.Device, s.CompositeVertex);
            if (status == Status::Ready)
            {
                status = rhi::GetStatus(s.Device, s.CompositeFragment);
            }
            if (status == Status::Ready)
            {
                const rhi::RasterBinding entries[]{
                    {0, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
                    {1, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}};
                status = rhi::CreateBindingLayout(s.Device, entries, s.CompositeLayout);
                if (Accepted(status))
                {
                    status = rhi::CreateSampler(s.Device, {}, s.CompositeSampler);
                }
                const uint32 indices[]{0, 1, 2, 2, 3, 0};
                if (Accepted(status))
                {
                    status = rhi::CreateBuffer(s.Device,
                                               {rhi::BufferRole::Index32, sizeof(indices)},
                                               {reinterpret_cast<const uint8*>(indices), sizeof(indices)},
                                               s.CompositeIndices);
                }
                ++s.ViewsPhase;
            }
        }
        else if (s.ViewsPhase == 1)
        {
            status = rhi::GetStatus(s.Device, s.CompositeLayout);
            if (status == Status::Ready)
            {
                status = rhi::GetStatus(s.Device, s.CompositeSampler);
            }
            if (status == Status::Ready)
            {
                status = rhi::GetStatus(s.Device, s.CompositeIndices);
            }
            if (status == Status::Ready)
            {
                rhi::RasterPipelineDescription description{s.Vertex,
                                                           s.Fragment,
                                                           s.Layout,
                                                           streams,
                                                           {attributes, attributeCount},
                                                           true};
                description.DepthCompare = rhi::RasterDepthCompare::Greater;
                status = rhi::CreateRasterPipeline(s.Device, description, s.SurfaceReverse);
                ++s.ViewsPhase;
            }
        }
        else if (s.ViewsPhase >= 2 && s.ViewsPhase <= 4)
        {
            const rhi::RasterPipelineHandle previous[]{s.SurfaceReverse, s.TargetOpaque, s.TargetReverse};
            status = rhi::GetStatus(s.Device, previous[s.ViewsPhase - 2]);
            if (status == Status::Ready)
            {
                rhi::RasterPipelineDescription description{s.Vertex,
                                                           s.Fragment,
                                                           s.Layout,
                                                           streams,
                                                           {attributes, attributeCount},
                                                           s.ViewsPhase != 4,
                                                           s.ViewsPhase == 4};
                description.Target = rhi::RasterTarget::Rgba8Unorm;
                if (s.ViewsPhase == 3)
                {
                    description.DepthCompare = rhi::RasterDepthCompare::Greater;
                }
                rhi::RasterPipelineHandle* targets[]{&s.TargetOpaque, &s.TargetReverse, &s.TargetOverlay};
                status = rhi::CreateRasterPipeline(s.Device, description, *targets[s.ViewsPhase - 2]);
                ++s.ViewsPhase;
            }
        }
        else if (s.ViewsPhase == 5)
        {
            status = rhi::GetStatus(s.Device, s.TargetOverlay);
            if (status == Status::Ready)
            {
                status = rhi::CreateRasterPipeline(
                    s.Device,
                    {s.CompositeVertex, s.CompositeFragment, s.CompositeLayout, quadStreams, quadAttributes},
                    s.Composite);
                ++s.ViewsPhase;
            }
        }
        else
        {
            status = rhi::GetStatus(s.Device, s.Composite);
            if (status == Status::Ready)
            {
                s.ViewsSetup = Status::Ready;
            }
        }
        if (status == Status::Pending)
        {
            return status;
        }
        if (status != Status::Ready)
        {
            s.ViewsSetup = status;
            return status;
        }
        if (s.ViewsSetup == Status::Ready)
        {
            return Status::Ready;
        }
    }
}
Status Renderer::State::PrepareGridPipelines() noexcept
{
    if (HasVisibilityInputs == true)
    {
        return Status::Unsupported;
    }
    const StaticArray<rhi::RasterVertexStream, FLAT_STREAM_COUNT>& streams = FLAT_STREAMS;
    const StaticArray<rhi::RasterVertexAttribute, FLAT_ATTRIBUTE_COUNT>& attributes = FLAT_ATTRIBUTES;
    bool isPending = false;
    const usize count = Hdr == true ? WORLD_GRID_PIPELINE_COUNT : WORLD_GRID_HDR_PIPELINE;
    for (usize i = 0; i < count; ++i)
    {
        Status status;
        if (HasGridPipelines[i] == false)
        {
            rhi::RasterPipelineDescription pipeline{Vertex, Fragment, Layout, streams, attributes, true};
            pipeline.Target = i < WORLD_GRID_UNORM_PIPELINE ? rhi::RasterTarget::Surface
                              : i < WORLD_GRID_HDR_PIPELINE ? rhi::RasterTarget::Rgba8Unorm
                                                            : rhi::RasterTarget::Rgba16Float;
            pipeline.DepthWrite = false;
            pipeline.DepthCompare =
                i % WORLD_GRID_TARGET_VARIANTS == 0 ? rhi::RasterDepthCompare::Less : rhi::RasterDepthCompare::Greater;
            status = rhi::CreateRasterPipeline(Device, pipeline, GridPipelines[i]);
            HasGridPipelines[i] = Accepted(status);
        }
        else
        {
            status = rhi::GetStatus(Device, GridPipelines[i]);
        }
        if (Accepted(status) == false)
        {
            return status;
        }
        isPending = isPending == true || status == Status::Pending;
    }
    return isPending == true ? Status::Pending : Status::Ready;
}
Status Renderer::AppendVisibilityView(PreparedView handle,
                                      rhi::OrderedGraph& inoutGraph,
                                      const rhi::RasterPassDescription& attachment,
                                      rhi::GraphVersion previous,
                                      rhi::GraphVersion& outWritten) noexcept
{
    const Status status = GetStatus(handle);
    if (status != Status::Ready)
    {
        return status;
    }
    State& state = *mState;
    const State::ViewRecord& view = state.ViewRecords[Access::Slot(handle)];
    if (state.HasVisibilityInputs == false || state.FlatUniforms.Count != 0 || view.Grid.Get() != nullptr ||
        view.Depth != DepthConvention::Conventional || attachment.ColorLoad != rhi::RasterLoad::Clear ||
        attachment.DepthLoad != rhi::RasterLoad::Clear || attachment.ColorStore != rhi::RasterStore::Store ||
        attachment.DepthStore != rhi::RasterStore::Store)
    {
        return Status::Unsupported;
    }
    Array<internal::VisibilityDraw> draws;
    if (draws.TryResize(view.DrawCount) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize index = 0; index < view.DrawCount; ++index)
    {
        const State::Packet& packet = view.Packets[index];
        if (packet.Pass != Layer::Opaque || packet.MaterialSlot != NO_MATERIAL || packet.UniformSlot != NO_UNIFORM)
        {
            return Status::Unsupported;
        }
        const State::MeshRecord& mesh = state.MeshRecords[packet.MeshSlot];
        draws[index] = {mesh.Vertices,
                        mesh.Indices,
                        mesh.VertexCount,
                        mesh.IndexCount,
                        packet.First,
                        packet.Count,
                        packet.MeshSlot};
    }
    const internal::VisibilityCommands commands{{draws.GetData(), draws.GetSize()},
                                                view.Instances,
                                                view.Visible,
                                                sizeof(Instance),
                                                state.TargetOpaque,
                                                state.Bindings,
                                                attachment};
    return internal::AppendVisibilityCommands(state.Device, inoutGraph, commands, previous, outWritten);
}
Status Renderer::getDeferredMaterial(PreparedView handle, internal::DeferredMaterialBindings& outBindings) noexcept
{
    const Status ready = GetStatus(handle);
    if (ready != Status::Ready)
    {
        return ready;
    }
    State& state = *mState;
    const State::ViewRecord& view = state.ViewRecords[Access::Slot(handle)];
    outBindings = {};
    for (usize index = 0; index < view.DrawCount; ++index)
    {
        const State::Packet& packet = view.Packets[index];
        if (packet.Pass != Layer::Opaque)
        {
            continue;
        }
        if (packet.MaterialSlot == NO_MATERIAL)
        {
            return Status::Unsupported;
        }
        const State::MaterialRecord& material = state.Materials[packet.MaterialSlot];
        if (material.Clustered == false || state.Programs[material.ProgramSlot].HasGBuffer == false)
        {
            return Status::Unsupported;
        }
        const StaticArray<rhi::TextureViewHandle, CLUSTERED_AUTHORED_BINDING_COUNT> textures{
            {{},
             material.Texture,
             {},
             material.BakeTexture,
             material.ShadowTexture,
             {},
             material.ClusterView,
             material.SurfaceTexture,
             material.EnvironmentTexture,
             {},
             material.EmissionTexture}};
        const StaticArray<rhi::SamplerHandle, CLUSTERED_AUTHORED_BINDING_COUNT> samplers{
            {{},
             {},
             material.Sampler,
             {},
             {},
             material.ShadowSampler,
             {},
             {},
             {},
             material.EnvironmentOrNull != nullptr ? material.EnvironmentOrNull->Sampler : material.Sampler,
             {}}};
        for (usize slot = 0; slot < CLUSTERED_AUTHORED_BINDING_COUNT; ++slot)
        {
            outBindings.Resources[slot].Binding = static_cast<uint32>(slot);
            outBindings.Resources[slot].Texture = textures[slot];
            outBindings.Resources[slot].Sampler = samplers[slot];
        }
        outBindings.Resources[0].Buffer = material.Parameters;
        outBindings.Resources[0].Size = material.ParameterSize;
        outBindings.HasMaterial = true;
        break;
    }
    return Status::Ready;
}
Status Renderer::DrawView(PreparedView handle, const rhi::RasterPassDescription& description) noexcept
{
    return drawView(handle, description, DrawMode::Color);
}
Status Renderer::DrawDepthView(PreparedView handle, const rhi::RasterPassDescription& description) noexcept
{
    return drawView(handle, description, DrawMode::Depth);
}
Status Renderer::drawView(PreparedView handle, const rhi::RasterPassDescription& description, DrawMode mode) noexcept
{
    const bool isDepthOnly = mode == DrawMode::Depth;
    const bool isGBuffer = mode == DrawMode::GBuffer;
    const bool areOverlaysOnly = mode == DrawMode::Overlays;
    auto status = GetStatus(handle);
    if (status != Status::Ready)
    {
        return status;
    }
    if (mState->ViewsSetup != Status::Ready)
    {
        return Status::NotReady;
    }
    auto& s = *mState;
    auto& view = s.ViewRecords[Access::Slot(handle)];
    if (areOverlaysOnly == true)
    {
        bool hasOverlays = view.Grid.Get() != nullptr;
        for (usize index = 0; index < view.DrawCount; ++index)
        {
            hasOverlays = hasOverlays == true || view.Packets[index].Pass != Layer::Opaque;
        }
        if (hasOverlays == false)
        {
            return Status::Ready;
        }
    }
    auto pass = description;
    pass.ClearDepth = view.Depth == DepthConvention::ReverseZ ? 0 : 1;
    const bool isSurface = rhi::IsNull(description.Color) == true && description.ColorCount == 0;
    bool isHdr = isGBuffer;
    if (isGBuffer == true && (description.ColorCount != DEFERRED_TARGET_COUNT || description.Colors == nullptr))
    {
        return Status::InvalidDescription;
    }
    if (isSurface == false && isGBuffer == false)
    {
        rhi::TextureDescription texture;
        const auto targetStatus = rhi::GetTextureDescription(s.Device, description.Color, texture);
        if (targetStatus != Status::Ready)
        {
            return targetStatus;
        }
        isHdr = texture.Format == rhi::RasterFormat::Rgba16Float;
        if (texture.Attachment == false || (texture.Format != rhi::RasterFormat::Rgba8Unorm && isHdr == false) ||
            (isHdr == true && mState->LightingSetup != Status::Ready))
        {
            return Status::Unsupported;
        }
    }
    if (isDepthOnly == true && (isSurface == true || isHdr == true || view.Depth != DepthConvention::Conventional))
    {
        return Status::Unsupported;
    }
    for (usize i = 0; i < view.DrawCount; ++i)
    {
        const auto slot = view.Packets[i].MaterialSlot;
        if (isGBuffer == true && view.Packets[i].Pass == Layer::Opaque &&
            (slot == NO_MATERIAL || s.Materials[slot].Alpha != MaterialAlpha::Opaque ||
             s.Programs[s.Materials[slot].ProgramSlot].HasGBuffer == false))
        {
            return Status::Unsupported;
        }
        if (isDepthOnly == true && view.Packets[i].Pass == Layer::Opaque &&
            (slot == NO_MATERIAL || s.Materials[slot].Alpha != MaterialAlpha::Opaque ||
             s.Programs[s.Materials[slot].ProgramSlot].HasDepthPrepass == false))
        {
            return Status::Unsupported;
        }
        if (slot != NO_MATERIAL &&
            ((s.Materials[slot].Lit && isSurface == true) || (s.Materials[slot].Lit == false && isHdr == true)))
        {
            return Status::Unsupported;
        }
    }
    status = rhi::BeginRasterPass(s.Device, pass);
    if (status != Status::Ready)
    {
        return status;
    }
    bool hasDrawnGrid = false;
    const auto drawGrid = [&]() noexcept {
        hasDrawnGrid = true;
        if (view.Grid.Get() == nullptr)
        {
            return Status::Ready;
        }
        const WorldGridBuffers& grid = *view.Grid;
        const rhi::RasterVertexSlice slices[]{{grid.Vertices, 0, WORLD_GRID_VERTEX_COUNT * sizeof(Vertex)},
                                              {grid.Instances, 0, grid.Count * sizeof(Instance)}};
        const usize target = isHdr == true       ? WORLD_GRID_HDR_PIPELINE
                             : isSurface == true ? 0
                                                 : WORLD_GRID_UNORM_PIPELINE;
        const usize reverse = view.Depth == DepthConvention::ReverseZ ? 1 : 0;
        return rhi::DrawIndexed(s.Device,
                                {s.GridPipelines[target + reverse],
                                 s.Bindings,
                                 slices,
                                 grid.Indices,
                                 0,
                                 WORLD_GRID_INDEX_COUNT,
                                 WORLD_GRID_VERTEX_COUNT,
                                 grid.Count});
    };
    for (usize i = 0; i < view.DrawCount; ++i)
    {
        const auto& packet = view.Packets[i];
        if (((isDepthOnly == true || isGBuffer == true) && packet.Pass != Layer::Opaque) ||
            (areOverlaysOnly == true && packet.Pass == Layer::Opaque))
        {
            continue;
        }
        if (packet.Pass == Layer::Overlay && hasDrawnGrid == false)
        {
            status = drawGrid();
            if (status != Status::Ready)
            {
                return status;
            }
        }
        const auto& mesh = s.MeshRecords[packet.MeshSlot];
        const rhi::RasterVertexSlice slices[]{
            {mesh.Vertices,
             0,
             mesh.VertexCount * (s.HasVisibilityInputs == true ? sizeof(VisibilityVertex) : sizeof(Vertex))},
            {view.Instances, packet.First * sizeof(Instance), packet.Count * sizeof(Instance)}};
        const rhi::RasterPipelineHandle opaque =
            isHdr == true       ? (view.Depth == DepthConvention::ReverseZ ? s.HdrFlat[1] : s.HdrFlat[0])
            : isSurface == true ? (view.Depth == DepthConvention::ReverseZ ? s.SurfaceReverse : s.Opaque)
                                : (view.Depth == DepthConvention::ReverseZ ? s.TargetReverse : s.TargetOpaque);
        const rhi::RasterPipelineHandle overlay =
            isHdr == true ? s.HdrFlat[2] : (isSurface == true ? s.Overlay : s.TargetOverlay);
        auto pipeline = packet.Pass == Layer::Opaque ? opaque : overlay;
        rhi::BindingSetHandle bindings =
            packet.UniformSlot != NO_UNIFORM ? s.UniformSets.GetBindings(packet.UniformSlot) : s.Bindings;
        if (packet.MaterialSlot != NO_MATERIAL)
        {
            const auto& material = s.Materials[packet.MaterialSlot];
            const auto& program = s.Programs[material.ProgramSlot];
            pipeline =
                program.Pipelines[(program.Lit == true ? (isHdr == true ? LDR_LIT_PROGRAM_VARIANT_COUNT : 0)
                                                       : (isSurface == true ? 0 : LDR_LIT_PROGRAM_VARIANT_COUNT)) +
                                  (packet.Pass == Layer::Overlay             ? 2
                                   : view.Depth == DepthConvention::ReverseZ ? 1
                                                                             : 0)];
            if (isDepthOnly == true)
            {
                pipeline = program.DepthPipeline;
            }
            if (isGBuffer == true)
            {
                pipeline = program.GBufferPipelines[view.Depth == DepthConvention::ReverseZ ? 1 : 0];
            }
            bindings = material.Bindings;
        }
        status = rhi::DrawIndexed(
            s.Device,
            {pipeline, bindings, slices, mesh.Indices, 0, mesh.IndexCount, mesh.VertexCount, packet.Count});
        if (status != Status::Ready)
        {
            return status;
        }
    }
    return isDepthOnly == true || isGBuffer == true || hasDrawnGrid == true ? Status::Ready : drawGrid();
}
Status
Renderer::PreparePresentation(rhi::TextureHandle source, const ViewMapping& mapping, Presentation& output) noexcept
{
    return PreparePresentationData(source, mapping, nullptr, output);
}
Status Renderer::PrepareLightingPresentation(rhi::TextureHandle source,
                                             const ViewMapping& mapping,
                                             const LightingOutput& lighting,
                                             Presentation& output) noexcept
{
    return PreparePresentationData(source, mapping, &lighting, output);
}
Status Renderer::PreparePresentationData(rhi::TextureHandle source,
                                         const ViewMapping& mapping,
                                         const LightingOutput* lighting,
                                         Presentation& output) noexcept
{
    if (mState.Get() == nullptr || mState->ViewsSetup != Status::Ready)
    {
        return Status::NotReady;
    }
    if (Access::Owner(output) != 0)
    {
        return Status::InvalidState;
    }
    auto& s = *mState;
    if (lighting != nullptr && s.LightingSetup != Status::Ready)
    {
        return Status::NotReady;
    }
    if (lighting != nullptr && (math::IsFinite(lighting->Exposure) == false || lighting->Exposure < MIN_EXPOSURE ||
                                lighting->Exposure > MAX_EXPOSURE))
    {
        return Status::InvalidDescription;
    }
    ViewMapping resolved;
    auto status =
        ResolveViewMapping(mapping.Screen, mapping.Region, mapping.DrawableWidth, mapping.DrawableHeight, resolved);
    if (status != Status::Ready)
    {
        return status;
    }
    rhi::TextureDescription description;
    status = rhi::GetTextureDescription(s.Device, source, description);
    if (status != Status::Ready)
    {
        return status;
    }
    if (description.Attachment == false ||
        (description.Format != rhi::RasterFormat::Rgba8Unorm &&
         (lighting == nullptr || description.Format != rhi::RasterFormat::Rgba16Float)))
    {
        return Status::Unsupported;
    }
    // Logical dimensions define the sampled image's aspect. Rendering resolution is independent.
    if (static_cast<float64>(description.Width) * static_cast<float64>(resolved.Screen.Height) !=
        static_cast<float64>(description.Height) * static_cast<float64>(resolved.Screen.Width))
    {
        return Status::InvalidDescription;
    }
    usize index = 0;
    status = FreeSlot(s.Presentations, index);
    if (status != Status::Ready)
    {
        return status;
    }
    auto& p = s.Presentations[index];
    p.Mapping = resolved;
    p.Lighting = lighting != nullptr;
    p.Encode = lighting != nullptr && lighting->Encode;
    p.Phase = 0;
    p.Setup = Status::Pending;
    const auto sx = resolved.ClipTransform.At(0, 0), sy = resolved.ClipTransform.At(1, 1);
    const math::Vector4 vertices[]{{-sx, -sy, 0, 1}, {sx, -sy, 1, 1}, {sx, sy, 1, 0}, {-sx, sy, 0, 0}};
    status = rhi::CreateBuffer(s.Device,
                               {rhi::BufferRole::Vertex, sizeof(vertices)},
                               {reinterpret_cast<const uint8*>(vertices), sizeof(vertices)},
                               p.Vertices);
    if (Accepted(status))
    {
        status = rhi::CreateTextureView(s.Device, source, p.Texture);
    }
    if (Accepted(status) && lighting != nullptr)
    {
        for (usize mode = 0; mode < 2 && Accepted(status); ++mode)
        {
            const math::Vector4 parameters{lighting->Exposure,
                                           lighting->ToneMap == true ? static_cast<float32>(lighting->Curve) : 0.0F,
                                           mode != 0 && lighting->Encode ? 1.0F : 0.0F,
                                           lighting->HasBackdrop == true ? 1.0F : 0.0F};
            status = rhi::CreateBuffer(s.Device,
                                       {rhi::BufferRole::Uniform, sizeof(parameters)},
                                       {reinterpret_cast<const uint8*>(&parameters), sizeof(parameters)},
                                       p.OutputParameters[mode]);
        }
    }
    if (Accepted(status) == false)
    {
        for (auto& buffer : p.OutputParameters)
        {
            static_cast<void>(rhi::Destroy(s.Device, buffer));
        }
        static_cast<void>(rhi::Destroy(s.Device, p.Texture));
        static_cast<void>(rhi::Destroy(s.Device, p.Vertices));
        p.Texture = {};
        p.Vertices = {};
        return status;
    }
    p.Used = p.Owned = true;
    output = Access::Make<Presentation>({s.Owner, p.Generation, index});
    status = GetStatus(output);
    if (Accepted(status) == false)
    {
        static_cast<void>(Release(output));
    }
    return status;
}
Status Renderer::GetStatus(Presentation handle) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto& s = *mState;
    auto* p = Resolve(s.Presentations, s.Owner, handle);
    if (p == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (p->Setup != Status::Pending)
    {
        return p->Setup;
    }
    auto status = rhi::GetStatus(s.Device, p->Vertices);
    if (status == Status::Ready)
    {
        status = rhi::GetStatus(s.Device, p->Texture);
    }
    if (status != Status::Ready)
    {
        if (status != Status::Pending)
        {
            p->Setup = status;
        }
        return status;
    }
    if (p->Lighting)
    {
        for (usize mode = 0; mode < 2; ++mode)
        {
            status = rhi::GetStatus(s.Device, p->OutputParameters[mode]);
            if (status != Status::Ready)
            {
                if (status != Status::Pending)
                {
                    p->Setup = status;
                }
                return status;
            }
        }
        while (p->Phase < 2)
        {
            const usize mode = p->Phase;
            const rhi::RasterBindingResource resources[]{{0, p->OutputParameters[mode], 0, 16, {}, {}},
                                                         {1, {}, 0, 0, p->Texture, {}},
                                                         {2, {}, 0, 0, {}, s.CompositeSampler}};
            status = rhi::CreateBindingSet(s.Device, s.OutputLayout, resources, p->OutputBindings[mode]);
            if (Accepted(status) == false)
            {
                return p->Setup = status;
            }
            ++p->Phase;
            if (status == Status::Pending)
            {
                return status;
            }
        }
        for (auto set : p->OutputBindings)
        {
            status = rhi::GetStatus(s.Device, set);
            if (status != Status::Ready)
            {
                if (status != Status::Pending)
                {
                    p->Setup = status;
                }
                return status;
            }
        }
    }
    else
    {
        if (p->Phase == 0)
        {
            const rhi::RasterBindingResource resources[]{{ .Binding = 0, .Texture = p->Texture },
                                                         { .Binding = 1, .Sampler = s.CompositeSampler }};
            status = rhi::CreateBindingSet(s.Device, s.CompositeLayout, resources, p->Bindings);
            if (Accepted(status) == false)
            {
                return p->Setup = status;
            }
            p->Phase = 1;
        }
        status = rhi::GetStatus(s.Device, p->Bindings);
    }
    if (status != Status::Pending)
    {
        p->Setup = status;
    }
    return status;
}
Status Renderer::DrawPresentation(Presentation handle) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto& s = *mState;
    const auto* p = Resolve(s.Presentations, s.Owner, handle);
    if (p == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (p->Lighting)
    {
        return Status::InvalidDescription;
    }
    if (p->Setup != Status::Ready)
    {
        return p->Setup == Status::Pending ? Status::NotReady : p->Setup;
    }
    const auto extent = rhi::GetFrameInfo();
    if (extent.Width == 0 || extent.Height == 0)
    {
        return Status::InvalidState;
    }
    if (extent.Width != p->Mapping.DrawableWidth || extent.Height != p->Mapping.DrawableHeight)
    {
        return Status::NotReady;
    }
    rhi::RasterPassDescription pass;
    pass.ColorLoad = rhi::RasterLoad::Load;
    pass.Viewport = p->Mapping.Viewport;
    pass.Scissor = p->Mapping.Region;
    pass.UseViewport = pass.UseScissor = true;
    auto status = rhi::BeginRasterPass(s.Device, pass);
    if (status != Status::Ready)
    {
        return status;
    }
    const rhi::RasterVertexSlice vertices[]{{p->Vertices, 0, 4 * sizeof(math::Vector4)}};
    return rhi::DrawIndexed(s.Device, {s.Composite, p->Bindings, vertices, s.CompositeIndices, 0, 6, 4});
}
Status Renderer::DrawLightingPresentation(Presentation handle, const rhi::RasterPassDescription& description) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto& s = *mState;
    const auto* p = Resolve(s.Presentations, s.Owner, handle);
    if (p == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (p->Lighting == false)
    {
        return Status::InvalidDescription;
    }
    if (p->Setup != Status::Ready)
    {
        return p->Setup == Status::Pending ? Status::NotReady : p->Setup;
    }
    const auto frame = rhi::GetFrameInfo();
    if (frame.Width == 0 || frame.Height == 0)
    {
        return Status::InvalidState;
    }
    const bool surface = rhi::IsNull(description.Color);
    uint32 width = frame.Width, height = frame.Height;
    if (surface == false)
    {
        rhi::TextureDescription target;
        const auto status = rhi::GetTextureDescription(s.Device, description.Color, target);
        if (status != Status::Ready)
        {
            return status;
        }
        if (target.Attachment == false || target.Format != rhi::RasterFormat::Rgba8Unorm)
        {
            return Status::Unsupported;
        }
        width = target.Width;
        height = target.Height;
    }
    if (width != p->Mapping.DrawableWidth || height != p->Mapping.DrawableHeight)
    {
        return Status::NotReady;
    }
    auto pass = description;
    pass.UseViewport = pass.UseScissor = true;
    pass.Viewport = p->Mapping.Viewport;
    pass.Scissor = p->Mapping.Region;
    const auto status = rhi::BeginRasterPass(s.Device, pass);
    if (status != Status::Ready)
    {
        return status;
    }
    const usize mode = p->Encode && (surface == false || frame.Encoding == rhi::SurfaceEncoding::Unorm) ? 1U : 0U;
    const rhi::RasterVertexSlice vertices[]{{p->Vertices, 0, 4 * sizeof(math::Vector4)}};
    return rhi::DrawIndexed(
        s.Device,
        {s.OutputPipelines[surface ? 0 : 1], p->OutputBindings[mode], vertices, s.CompositeIndices, 0, 6, 4});
}
Status Renderer::Release(Presentation& handle) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto* p = Resolve(mState->Presentations, mState->Owner, handle);
    if (p == nullptr)
    {
        return Status::InvalidHandle;
    }
    mState->DropPresentation(*p);
    handle = {};
    return Status::Ready;
}

Status Renderer::PrewarmMaterialProgram(const rhi::RasterShaderDescription& vertex,
                                        const rhi::RasterShaderDescription& fragment,
                                        MaterialProgram& output) noexcept
{
    return prewarmProgram(vertex, fragment, nullptr, output);
}
Status Renderer::PrewarmLitProgram(const rhi::RasterShaderDescription& vertex,
                                   const rhi::RasterShaderDescription& fragment,
                                   const rhi::RasterShaderDescription& shadowFragment,
                                   MaterialProgram& outProgram,
                                   const rhi::RasterShaderDescription* depthOrNull) noexcept
{
    return prewarmProgram(vertex, fragment, &shadowFragment, outProgram, depthOrNull);
}
Status Renderer::prewarmProgram(const rhi::RasterShaderDescription& vertex,
                                const rhi::RasterShaderDescription& fragment,
                                const rhi::RasterShaderDescription* shadowOrNull,
                                MaterialProgram& outProgram,
                                const rhi::RasterShaderDescription* depthOrNull,
                                const rhi::RasterShaderDescription* gBufferOrNull) noexcept
{
    constexpr usize PLAIN_BINDING_COUNT = 3;
    constexpr usize CLUSTER_BINDING_COUNT = 8;
    constexpr usize AUTHORED_BINDING_COUNT = 3;
    constexpr usize LIT_BINDING_COUNT = 6;
    constexpr usize PLAIN_MEMBER_COUNT = 3;
    constexpr usize LIT_MEMBER_COUNT = FORWARD_HEADER_WORDS + 2;
    constexpr usize CLUSTER_MEMBER_COUNT = LIT_MEMBER_COUNT + CLUSTERED_GRID_ROWS;
    constexpr usize PLAIN_VERTEX_ATTRIBUTE_COUNT = 7;
    constexpr usize LIT_VERTEX_ATTRIBUTE_COUNT = 16;
    constexpr usize WORD_BYTES = sizeof(math::Vector4);
    const bool isLit = shadowOrNull != nullptr;
    if (depthOrNull != nullptr &&
        (shadowOrNull == nullptr || depthOrNull->Artifact.Stage != rhi::ShaderStage::Fragment ||
         depthOrNull->Inputs.empty() == false || depthOrNull->Bindings.empty() == false ||
         depthOrNull->UniformMembers.empty() == false))
    {
        return Status::InvalidDescription;
    }
    bool hasSurfaceMapProfile = false;
    bool hasAuthoredProfile = false;
    for (const rhi::RasterUniformMember& member : fragment.UniformMembers)
    {
        hasSurfaceMapProfile = hasSurfaceMapProfile || member.Name == "SurfaceMap";
        hasAuthoredProfile = hasAuthoredProfile || member.Name == "Authored";
    }
    if (hasSurfaceMapProfile == true && isLit == false)
    {
        return Status::InvalidDescription;
    }
    const bool isClustered =
        isLit == true &&
        fragment.Bindings.size() ==
            (hasAuthoredProfile == true ? CLUSTER_BINDING_COUNT + AUTHORED_BINDING_COUNT : CLUSTER_BINDING_COUNT);
    if (gBufferOrNull != nullptr && (isClustered == false || hasAuthoredProfile == false || depthOrNull == nullptr ||
                                     gBufferOrNull->Artifact.Stage != rhi::ShaderStage::Fragment))
    {
        return Status::InvalidDescription;
    }
    const usize bindingCount = (isClustered == true ? CLUSTER_BINDING_COUNT
                                : isLit == true     ? LIT_BINDING_COUNT + (hasSurfaceMapProfile == true ? 1 : 0)
                                                    : PLAIN_BINDING_COUNT) +
                               (hasAuthoredProfile == true ? AUTHORED_BINDING_COUNT : 0);
    const usize memberCount = (isClustered == true ? CLUSTER_MEMBER_COUNT
                               : isLit == true     ? LIT_MEMBER_COUNT
                                                   : PLAIN_MEMBER_COUNT) +
                              (hasSurfaceMapProfile ? 1U : 0U) + (hasAuthoredProfile ? 2U : 0U);
    if (mState.Get() == nullptr || mState->Setup != Status::Ready ||
        (isLit == true && mState->LightingSetup != Status::Ready))
    {
        return Status::NotReady;
    }
    if (Access::Owner(outProgram) != 0)
    {
        return Status::InvalidState;
    }
    if (vertex.Artifact.Stage != rhi::ShaderStage::Vertex || fragment.Artifact.Stage != rhi::ShaderStage::Fragment ||
        vertex.Inputs.size() != (isLit == true ? LIT_VERTEX_ATTRIBUTE_COUNT : PLAIN_VERTEX_ATTRIBUTE_COUNT) ||
        vertex.Bindings.empty() == false || vertex.UniformMembers.empty() == false ||
        fragment.Inputs.empty() == false || fragment.Bindings.size() != bindingCount ||
        fragment.UniformMembers.size() != memberCount)
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < (isLit == true ? LIT_VERTEX_ATTRIBUTE_COUNT : PLAIN_VERTEX_ATTRIBUTE_COUNT); ++i)
    {
        const rhi::RasterVertexFormat format = (hasAuthoredProfile && i == 13) ? rhi::RasterVertexFormat::Float2
                                               : (i == 0 || i == 7)            ? rhi::RasterVertexFormat::Float3
                                               : (i == 6 || i == 15)           ? rhi::RasterVertexFormat::Float2
                                                                               : rhi::RasterVertexFormat::Float4;
        if (vertex.Inputs[i].Location != i || vertex.Inputs[i].Format != format)
        {
            return Status::InvalidDescription;
        }
    }
    const usize byteSize = (isClustered == true ? CLUSTER_BYTES
                            : isLit == true     ? LIT_BYTES
                                                : PLAIN_MEMBER_COUNT * WORD_BYTES) +
                           (hasSurfaceMapProfile == true ? WORD_BYTES : 0) +
                           (hasAuthoredProfile ? AUTHORED_PARAMETER_BYTES : 0U);
    rhi::RasterBinding expected[]{
        {0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Fragment, byteSize},
        {1, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
        {2, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0},
        {3, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
        {4, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
        {5, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0},
        {6, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
        {7,
         hasSurfaceMapProfile ? rhi::RasterBindingKind::Texture2D : rhi::RasterBindingKind::Sampler,
         rhi::RasterVisibility::Fragment,
         0},
        {8, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
        {9, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0},
        {10, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0}};
    for (usize i = 0; i < bindingCount; ++i)
    {
        bool found = false;
        rhi::RasterBinding& required = expected[i];
        if (hasAuthoredProfile && i >= bindingCount - 3)
        {
            required.Kind = i == bindingCount - 2 ? rhi::RasterBindingKind::Sampler : rhi::RasterBindingKind::Texture2D;
        }
        for (const rhi::RasterBinding& binding : fragment.Bindings)
        {
            found |= binding.Binding == required.Binding && binding.Kind == required.Kind &&
                     binding.Visibility == required.Visibility && binding.MinSize == required.MinSize;
        }
        if (found == false)
        {
            return Status::InvalidDescription;
        }
    }
    const char* names[]{"Tint",
                        "UvTransform",
                        "Options",
                        "Pbr",
                        "Emission",
                        "EnvironmentIrradiance",
                        "EnvironmentRadiance",
                        "BakeScale",
                        "BakeUv",
                        "Lights",
                        "ShadowData",
                        "GridRows",
                        "GridMinimum",
                        "GridScale"};
    for (usize i = 0; i < (isClustered == true ? CLUSTER_MEMBER_COUNT
                           : isLit == true     ? LIT_MEMBER_COUNT
                                               : PLAIN_MEMBER_COUNT);
         ++i)
    {
        const usize offset = i == FORWARD_HEADER_WORDS + 1
                                 ? (isClustered == true ? CLUSTER_SHADOW_OFFSET : LIT_SHADOW_OFFSET)
                             : i == 11 ? CLUSTER_GRID_OFFSET
                             : i == 12 ? CLUSTER_GRID_OFFSET + 3 * sizeof(math::Vector4)
                             : i == 13 ? CLUSTER_GRID_OFFSET + 4 * sizeof(math::Vector4)
                                       : i * 16;
        const auto shape =
            i == 9 || i == 10 || i == 11 ? rhi::RasterUniformShape::Float4Array : rhi::RasterUniformShape::Float4;
        bool found = false;
        for (const auto& member : fragment.UniformMembers)
        {
            found |= member.Binding == 0 && member.Name == names[i] && member.Offset == offset && member.Shape == shape;
        }
        if (found == false)
        {
            return Status::InvalidDescription;
        }
    }
    if (hasSurfaceMapProfile)
    {
        bool hasSurfaceMap = false;
        for (const rhi::RasterUniformMember& member : fragment.UniformMembers)
        {
            hasSurfaceMap = hasSurfaceMap || (member.Binding == 0 && member.Name == "SurfaceMap" &&
                                              member.Offset == (isClustered == true ? CLUSTER_BYTES : LIT_BYTES) &&
                                              member.Shape == rhi::RasterUniformShape::Float4);
        }
        if (hasSurfaceMap == false)
        {
            return Status::InvalidDescription;
        }
    }
    if (hasAuthoredProfile == true)
    {
        const usize authoredOffset = (isClustered == true ? CLUSTER_BYTES : LIT_BYTES) + 16;
        for (usize index = 0; index < 2; ++index)
        {
            bool found = false;
            for (const rhi::RasterUniformMember& member : fragment.UniformMembers)
            {
                found = found || (member.Binding == 0 && member.Name == (index == 0 ? "Authored" : "IrradianceSh") &&
                                  member.Offset == authoredOffset + index * 16 &&
                                  member.Shape == (index == 0 ? rhi::RasterUniformShape::Float4
                                                              : rhi::RasterUniformShape::Float4Array));
            }
            if (found == false)
            {
                return Status::InvalidDescription;
            }
        }
    }
    if (isLit == true)
    {
        if (shadowOrNull->Artifact.Stage != rhi::ShaderStage::Fragment || shadowOrNull->Inputs.empty() == false ||
            shadowOrNull->Bindings.size() != PLAIN_BINDING_COUNT ||
            shadowOrNull->UniformMembers.size() != fragment.UniformMembers.size())
        {
            return Status::InvalidDescription;
        }
        for (usize i = 0; i < 3; ++i)
        {
            bool found = false;
            for (const rhi::RasterBinding& binding : shadowOrNull->Bindings)
            {
                found |= binding.Binding == expected[i].Binding && binding.Kind == expected[i].Kind &&
                         binding.MinSize == expected[i].MinSize && binding.Visibility == expected[i].Visibility;
            }
            if (found == false)
            {
                return Status::InvalidDescription;
            }
        }
        for (const auto& expectedMember : fragment.UniformMembers)
        {
            bool found = false;
            for (const rhi::RasterUniformMember& member : shadowOrNull->UniformMembers)
            {
                found |= member.Binding == expectedMember.Binding && member.Name == expectedMember.Name &&
                         member.Offset == expectedMember.Offset && member.Shape == expectedMember.Shape;
            }
            if (found == false)
            {
                return Status::InvalidDescription;
            }
        }
        rhi::RasterCapabilities caps;
        const auto ready = rhi::GetRasterCapabilities(mState->Device, caps);
        if (ready != Status::Ready)
        {
            return ready;
        }
        if (caps.MaxVertexAttributes < 16 || caps.MaxUniformRange < byteSize)
        {
            return Status::Unsupported;
        }
    }
    auto& s = *mState;
    usize slot = 0;
    auto status = FreeSlot(s.Programs, slot);
    if (status != Status::Ready)
    {
        return status;
    }
    auto& p = s.Programs[slot];
    p.Used = p.Owned = true;
    p.Lit = isLit;
    p.Clustered = isClustered;
    p.HasDepthPrepass = depthOrNull != nullptr;
    p.HasGBuffer = gBufferOrNull != nullptr;
    p.HasSurfaceMapProfile = hasSurfaceMapProfile;
    p.HasAuthoredProfile = hasAuthoredProfile;
    p.Hdr = isLit == true && s.Hdr;
    p.VariantCount = isLit == true && p.Hdr == false ? LDR_LIT_PROGRAM_VARIANT_COUNT : PROGRAM_VARIANT_COUNT;
    status = rhi::CreateRasterShader(s.Device, vertex, p.Vertex);
    if (Accepted(status))
    {
        status = rhi::CreateRasterShader(s.Device, fragment, p.Fragment);
    }
    if (Accepted(status) == true && isLit == true)
    {
        status = rhi::CreateRasterShader(s.Device, *shadowOrNull, p.ShadowFragment);
    }
    if (Accepted(status))
    {
        status = rhi::CreateBindingLayout(s.Device, {expected, bindingCount}, p.Layout);
    }
    if (Accepted(status) == true && depthOrNull != nullptr)
    {
        status = rhi::CreateRasterShader(s.Device, *depthOrNull, p.DepthFragment);
    }
    if (Accepted(status) == true && gBufferOrNull != nullptr)
    {
        status = rhi::CreateRasterShader(s.Device, *gBufferOrNull, p.GBufferFragment);
    }
    const uint8 white[]{255, 255, 255, 255};
    rhi::TextureDescription texture{1, 1, rhi::RasterFormat::Rgba8Unorm};
    texture.AsyncUpload = true;
    if (Accepted(status))
    {
        status = rhi::CreateTexture(s.Device, texture, {white, 4}, p.White);
    }
    if (Accepted(status) == false)
    {
        p.Owned = false;
        s.DropProgram(slot);
        return status;
    }
    const auto handle = Access::Make<MaterialProgram>({s.Owner, p.Generation, slot});
    status = GetStatus(handle);
    if (Accepted(status) == false)
    {
        p.Owned = false;
        s.DropProgram(slot);
        return status;
    }
    outProgram = handle;
    return status;
}
Status Renderer::GetStatus(MaterialProgram handle) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto& s = *mState;
    auto* p = Resolve(s.Programs, s.Owner, handle);
    if (p == nullptr)
    {
        return Status::InvalidHandle;
    }
    rhi::RasterCapabilities caps;
    const auto device = rhi::GetRasterCapabilities(s.Device, caps);
    if (device != Status::Ready)
    {
        return device;
    }
    if (p->Setup != Status::Pending)
    {
        return p->Setup;
    }
    const Status prerequisites[]{rhi::GetStatus(s.Device, p->Vertex),
                                 rhi::GetStatus(s.Device, p->Fragment),
                                 rhi::GetStatus(s.Device, p->Layout),
                                 rhi::GetStatus(s.Device, p->White)};
    for (auto ready : prerequisites)
    {
        if (ready != Status::Ready)
        {
            if (ready != Status::Pending)
            {
                p->Setup = ready;
            }
            return ready;
        }
    }
    if (p->Lit)
    {
        const auto shadow = rhi::GetStatus(s.Device, p->ShadowFragment);
        if (shadow != Status::Ready)
        {
            return shadow;
        }
    }
    // Thanks to Khronos, "Pipeline Cache", Vulkan Guide (Pipeline Cache chapter),
    // https://docs.vulkan.org/guide/latest/pipeline_cache.html: make expensive
    // driver creation a prewarm obligation. We reuse R2's field-comparing cache;
    // no persisted driver blob or unbounded permutation cache is introduced.
    const StaticArray<rhi::RasterVertexStream, FLAT_STREAM_COUNT>& streams = FLAT_STREAMS;
    rhi::RasterVertexAttribute attributes[]{{0, 0, 0, rhi::RasterVertexFormat::Float3},
                                            {1, 1, 0, rhi::RasterVertexFormat::Float4},
                                            {2, 1, 16, rhi::RasterVertexFormat::Float4},
                                            {3, 1, 32, rhi::RasterVertexFormat::Float4},
                                            {4, 1, 48, rhi::RasterVertexFormat::Float4},
                                            {5, 1, 64, rhi::RasterVertexFormat::Float4},
                                            {6, 0, 12, rhi::RasterVertexFormat::Float2},
                                            {7, 0, 20, rhi::RasterVertexFormat::Float3},
                                            {8, 1, 80, rhi::RasterVertexFormat::Float4},
                                            {9, 1, 96, rhi::RasterVertexFormat::Float4},
                                            {10, 1, 112, rhi::RasterVertexFormat::Float4},
                                            {11, 1, 128, rhi::RasterVertexFormat::Float4},
                                            {12, 1, 144, rhi::RasterVertexFormat::Float4},
                                            {13, 1, 160, rhi::RasterVertexFormat::Float4},
                                            {14, 1, 176, rhi::RasterVertexFormat::Float4},
                                            {15, 0, 32, rhi::RasterVertexFormat::Float2}};
    if (p->HasAuthoredProfile == true)
    {
        attributes[11] = {11, 0, LUDUS_AUTHORED_TANGENT_OFFSET, rhi::RasterVertexFormat::Float4};
        attributes[12] = {12, 0, LUDUS_AUTHORED_COLOR_OFFSET, rhi::RasterVertexFormat::Float4};
        attributes[13] = {13, 0, LUDUS_AUTHORED_UV1_OFFSET, rhi::RasterVertexFormat::Float2};
    }
    const uint32 total =
        p->VariantCount + (p->Lit == true ? 1U : 0U) + (p->HasDepthPrepass == true ? 1U : 0U) +
        (p->HasGBuffer == true ? static_cast<uint32>(State::ProgramRecord::GBUFFER_PIPELINE_COUNT) : 0U);
    while (p->Phase <= total)
    {
        if (p->Phase != 0)
        {
            const auto ready = rhi::GetStatus(s.Device, p->Requests[p->Phase - 1]);
            if (ready != Status::Ready)
            {
                if (ready != Status::Pending)
                {
                    p->Setup = ready;
                }
                return ready;
            }
            const Status acquired = rhi::GetRequestedPipeline(
                s.Device,
                p->Requests[p->Phase - 1],
                p->Phase <= p->VariantCount       ? p->Pipelines[p->Phase - 1]
                : p->Phase == p->VariantCount + 1 ? p->ShadowPipeline
                : p->Phase == p->VariantCount + 2 ? p->DepthPipeline
                                                  : p->GBufferPipelines[p->Phase - p->VariantCount - 3]);
            if (acquired != Status::Ready)
            {
                return p->Setup = acquired;
            }
        }
        if (p->Phase == total)
        {
            return p->Setup = Status::Ready;
        }
        const auto variant = p->Phase % 3;
        rhi::RasterPipelineDescription description{p->Vertex,
                                                   p->Fragment,
                                                   p->Layout,
                                                   streams,
                                                   {attributes, p->Lit ? 16U : 7U},
                                                   variant != 2,
                                                   variant == 2};
        description.Target = p->Lit ? (p->Phase < 3 ? rhi::RasterTarget::Rgba8Unorm : rhi::RasterTarget::Rgba16Float)
                                    : (p->Phase < 3 ? rhi::RasterTarget::Surface : rhi::RasterTarget::Rgba8Unorm);
        description.DepthWrite = variant != 2;
        description.DepthCompare = variant == 1 ? rhi::RasterDepthCompare::Greater : rhi::RasterDepthCompare::Less;
        if (p->HasDepthPrepass == true && variant == 0)
        {
            description.DepthCompare = rhi::RasterDepthCompare::LessEqual;
        }
        if (p->Lit && p->Phase == p->VariantCount)
        {
            description.Fragment = p->ShadowFragment;
            description.Target = rhi::RasterTarget::Rgba8Unorm;
            description.Depth = description.DepthWrite = true;
            description.Blend = false;
            description.DepthCompare = rhi::RasterDepthCompare::Less;
        }
        if (p->HasDepthPrepass == true && p->Phase == p->VariantCount + 1)
        {
            const Status ready = rhi::GetStatus(s.Device, p->DepthFragment);
            if (ready != Status::Ready)
            {
                return ready;
            }
            description.Fragment = p->DepthFragment;
            description.ShouldWriteColor = false;
            description.Target = rhi::RasterTarget::Rgba8Unorm;
            description.Depth = true;
            description.DepthWrite = true;
            description.Blend = false;
            description.DepthCompare = rhi::RasterDepthCompare::Less;
        }
        if (p->HasGBuffer == true && p->Phase >= p->VariantCount + 2)
        {
            const Status ready = rhi::GetStatus(s.Device, p->GBufferFragment);
            if (ready != Status::Ready)
            {
                return ready;
            }
            static constexpr StaticArray<rhi::RasterFormat, DEFERRED_TARGET_COUNT> formats{
                {rhi::RasterFormat::Rgba16Float, rhi::RasterFormat::Rgba16Float, rhi::RasterFormat::Rgba16Float}};
            description.Fragment = p->GBufferFragment;
            description.Target = rhi::RasterTarget::Rgba16Float;
            description.ColorFormats = formats.AsSpan();
            description.Depth = true;
            description.DepthWrite = true;
            description.Blend = false;
            description.DepthCompare = p->Phase == p->VariantCount + 2 ? rhi::RasterDepthCompare::LessEqual
                                                                       : rhi::RasterDepthCompare::GreaterEqual;
        }
        const auto requested = rhi::RequestPipeline(s.Device, description, p->Requests[p->Phase]);
        if (Accepted(requested) == false)
        {
            return p->Setup = requested;
        }
        ++p->Phase;
        if (requested == Status::Pending)
        {
            return requested;
        }
    }
    return p->Setup;
}
Status Renderer::Release(MaterialProgram& handle) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto* p = Resolve(mState->Programs, mState->Owner, handle);
    if (p == nullptr)
    {
        return Status::InvalidHandle;
    }
    p->Owned = false;
    mState->DropProgram(Access::Slot(handle));
    handle = {};
    return Status::Ready;
}
Status Renderer::CreateMaterial(MaterialProgram program,
                                const MaterialDescription& description,
                                MaterialVersion& output) noexcept
{
    const math::Vector4 parameters[]{
        description.Tint,
        description.UvTransform,
        {description.AlphaCutoff, description.Lod, static_cast<float32>(description.Alpha), 0}};
    return CreateMaterialData(program,
                              description,
                              reinterpret_cast<const uint8*>(parameters),
                              sizeof(parameters),
                              nullptr,
                              output);
}
Status Renderer::CreateMaterialData(MaterialProgram program,
                                    const MaterialDescription& description,
                                    const uint8* bytes,
                                    usize size,
                                    const LitMaterialDescription* lighting,
                                    MaterialVersion& output,
                                    const LightClusters* clusters) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::NotReady;
    }
    if (Access::Owner(output) != 0)
    {
        return Status::InvalidState;
    }
    auto status = GetStatus(program);
    if (status != Status::Ready)
    {
        return status == Status::Pending ? Status::NotReady : status;
    }
    if (ValidColor(description.Tint) == false || math::IsFinite(description.UvTransform) == false ||
        static_cast<uint8>(description.Alpha) > 2 || math::IsFinite(description.AlphaCutoff) == false ||
        description.AlphaCutoff < 0 || description.AlphaCutoff > 1 ||
        (description.Alpha == MaterialAlpha::Opaque && description.Tint.W != 1))
    {
        return Status::InvalidDescription;
    }
    auto& s = *mState;
    if (s.Programs[Access::Slot(program)].Lit != (lighting != nullptr))
    {
        return Status::InvalidDescription;
    }
    const auto source =
        rhi::IsNull(description.Texture) ? s.Programs[Access::Slot(program)].White : description.Texture;
    rhi::TextureDescription texture;
    status = rhi::GetTextureDescription(s.Device, source, texture);
    if (status != Status::Ready)
    {
        return status;
    }
    if (texture.Format != rhi::RasterFormat::Rgba8Unorm && texture.Format != rhi::RasterFormat::Rgba8Srgb)
    {
        return Status::InvalidDescription;
    }
    if (math::IsFinite(description.Lod) == false ||
        (description.Lod != -1 &&
         (description.Lod < 0 || description.Lod > static_cast<float32>(texture.MipLevels - 1))))
    {
        return Status::InvalidDescription;
    }
    usize slot = 0;
    status = FreeSlot(s.Materials, slot);
    if (status != Status::Ready)
    {
        return status;
    }
    core::UniquePtr<internal::ReferenceSurfaceData> referenceSurface;
    if (s.Reference)
    {
        status = s.PrepareReferenceSurface(description, lighting, referenceSurface);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    auto& m = s.Materials[slot];
    if (referenceSurface)
    {
        s.Reference->CpuBytes += sizeof(internal::ReferenceSurfaceData);
    }
    m.Reference = core::Move(referenceSurface);
    m.Used = m.Owned = true;
    m.ProgramSlot = Access::Slot(program);
    m.Alpha = description.Alpha;
    ++s.Programs[m.ProgramSlot].References;
    m.Lit = lighting != nullptr;
    m.Clustered = clusters != nullptr;
    m.ParameterSize = size;
    if (lighting != nullptr)
    {
        m.EnvironmentOrNull = Access::GetEnvironment(lighting->EnvironmentOrNull);
        internal::RetainEnvironment(m.EnvironmentOrNull);
        m.Bake = lighting->Bake;
        m.HasBake = rhi::IsNull(lighting->Bake.Texture) == false;
        if (Access::Owner(lighting->Shadows) != 0)
        {
            m.ShadowSlot = Access::Slot(lighting->Shadows);
            ++s.Shadows[m.ShadowSlot].References;
        }
    }
    status = rhi::CreateBuffer(s.Device, {rhi::BufferRole::Uniform, size}, {bytes, size}, m.Parameters);
    if (Accepted(status))
    {
        status = rhi::CreateSampler(s.Device, description.Sampler, m.Sampler);
    }
    if (Accepted(status))
    {
        status = rhi::CreateTextureView(s.Device, source, m.Texture);
    }
    if (Accepted(status) && lighting != nullptr)
    {
        const auto bakeSource = m.HasBake ? lighting->Bake.Texture : s.Programs[m.ProgramSlot].White;
        const auto shadowSource =
            m.ShadowSlot != NO_SHADOW ? s.Shadows[m.ShadowSlot].Texture : s.Programs[m.ProgramSlot].White;
        status = rhi::CreateTextureView(s.Device, bakeSource, m.BakeTexture);
        if (Accepted(status))
        {
            status = rhi::CreateTextureView(s.Device, shadowSource, m.ShadowTexture);
        }
        if (Accepted(status))
        {
            status = rhi::CreateSampler(s.Device, {}, m.ShadowSampler);
        }
    }
    if (Accepted(status) && lighting != nullptr && s.Programs[m.ProgramSlot].HasSurfaceMapProfile)
    {
        const rhi::TextureHandle mapSource =
            rhi::IsNull(lighting->SurfaceMap) ? s.Programs[m.ProgramSlot].White : lighting->SurfaceMap;
        status = rhi::CreateTextureView(s.Device, mapSource, m.SurfaceTexture);
    }
    if (Accepted(status) && lighting != nullptr && s.Programs[m.ProgramSlot].HasAuthoredProfile)
    {
        status = rhi::CreateTextureView(s.Device,
                                        m.EnvironmentOrNull != nullptr ? m.EnvironmentOrNull->Texture
                                                                       : s.Programs[m.ProgramSlot].White,
                                        m.EnvironmentTexture);
        if (Accepted(status))
        {
            status = rhi::CreateTextureView(s.Device,
                                            rhi::IsNull(lighting->EmissionMap) ? s.Programs[m.ProgramSlot].White
                                                                               : lighting->EmissionMap,
                                            m.EmissionTexture);
        }
    }
    if (Accepted(status) && clusters != nullptr)
    {
        status = rhi::CreateTexture(s.Device,
                                    {64, 64, rhi::RasterFormat::Rgba8Unorm},
                                    {{clusters->Texels, sizeof(clusters->Texels)}, 256},
                                    m.ClusterTexture);
    }
    if (Accepted(status) == false)
    {
        m.Owned = false;
        s.DropMaterial(slot);
        return status;
    }
    const auto handle = Access::Make<MaterialVersion>({s.Owner, m.Generation, slot});
    status = GetStatus(handle);
    if (Accepted(status) == false)
    {
        m.Owned = false;
        s.DropMaterial(slot);
        return status;
    }
    output = handle;
    return status;
}
Status Renderer::GetStatus(MaterialVersion handle) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto& s = *mState;
    auto* m = Resolve(s.Materials, s.Owner, handle);
    if (m == nullptr)
    {
        return Status::InvalidHandle;
    }
    rhi::RasterCapabilities caps;
    const auto device = rhi::GetRasterCapabilities(s.Device, caps);
    if (device != Status::Ready)
    {
        return device;
    }
    if (m->Setup != Status::Pending)
    {
        return m->Setup;
    }
    const Status prerequisites[]{rhi::GetStatus(s.Device, m->Parameters),
                                 rhi::GetStatus(s.Device, m->Sampler),
                                 rhi::GetStatus(s.Device, m->Texture)};
    for (auto ready : prerequisites)
    {
        if (ready != Status::Ready)
        {
            if (ready != Status::Pending)
            {
                m->Setup = ready;
            }
            return ready;
        }
    }
    if (m->Lit)
    {
        const Status extras[]{rhi::GetStatus(s.Device, m->BakeTexture),
                              rhi::GetStatus(s.Device, m->ShadowTexture),
                              rhi::GetStatus(s.Device, m->ShadowSampler)};
        for (auto ready : extras)
        {
            if (ready != Status::Ready)
            {
                if (ready != Status::Pending)
                {
                    m->Setup = ready;
                }
                return ready;
            }
        }
    }
    if (s.Programs[m->ProgramSlot].HasSurfaceMapProfile)
    {
        const Status ready = rhi::GetStatus(s.Device, m->SurfaceTexture);
        if (ready != Status::Ready)
        {
            if (ready != Status::Pending)
            {
                m->Setup = ready;
            }
            return ready;
        }
    }
    if (s.Programs[m->ProgramSlot].HasAuthoredProfile == true)
    {
        const Status ready = rhi::GetStatus(s.Device, m->EnvironmentTexture);
        if (ready != Status::Ready)
        {
            return ready;
        }
        const Status emissionReady = rhi::GetStatus(s.Device, m->EmissionTexture);
        if (emissionReady != Status::Ready)
        {
            return emissionReady;
        }
    }
    if (m->Clustered)
    {
        const auto texture = rhi::GetStatus(s.Device, m->ClusterTexture);
        if (texture != Status::Ready)
        {
            if (texture != Status::Pending)
            {
                m->Setup = texture;
            }
            return texture;
        }
        if (m->ClusterViewCreated == false)
        {
            const auto created = rhi::CreateTextureView(s.Device, m->ClusterTexture, m->ClusterView);
            m->ClusterViewCreated = Accepted(created);
            if (Accepted(created) == false)
            {
                return m->Setup = created;
            }
        }
        const auto ready = rhi::GetStatus(s.Device, m->ClusterView);
        if (ready != Status::Ready)
        {
            if (ready != Status::Pending)
            {
                m->Setup = ready;
            }
            return ready;
        }
    }
    if (m->BindingCreated == false)
    {
        const bool hasSurfaceMapProfile = s.Programs[m->ProgramSlot].HasSurfaceMapProfile;
        rhi::RasterBindingResource values[]{{0, m->Parameters, 0, m->ParameterSize, {}, {}},
                                            {1, {}, 0, 0, m->Texture, {}},
                                            {2, {}, 0, 0, {}, m->Sampler},
                                            {3, {}, 0, 0, m->BakeTexture, {}},
                                            {4, {}, 0, 0, m->ShadowTexture, {}},
                                            {5, {}, 0, 0, {}, m->ShadowSampler},
                                            {6, {}, 0, 0, m->Clustered ? m->ClusterView : m->SurfaceTexture, {}},
                                            {7,
                                             {},
                                             0,
                                             0,
                                             hasSurfaceMapProfile ? m->SurfaceTexture : rhi::TextureViewHandle{},
                                             hasSurfaceMapProfile ? rhi::SamplerHandle{} : m->ShadowSampler},
                                            {8, {}, 0, 0, {}, {}},
                                            {9, {}, 0, 0, {}, {}},
                                            {10, {}, 0, 0, {}, {}}};
        const bool hasAuthoredProfile = s.Programs[m->ProgramSlot].HasAuthoredProfile;
        const usize bindingCount = (m->Clustered ? 8U
                                    : m->Lit     ? (hasSurfaceMapProfile ? 7U : 6U)
                                                 : 3U) +
                                   (hasAuthoredProfile ? 3U : 0U);
        if (hasAuthoredProfile == true)
        {
            values[bindingCount - 3] = {static_cast<uint32>(bindingCount - 3), {}, 0, 0, m->EnvironmentTexture, {}};
            values[bindingCount - 2] = {static_cast<uint32>(bindingCount - 2),
                                        {},
                                        0,
                                        0,
                                        {},
                                        m->EnvironmentOrNull != nullptr ? m->EnvironmentOrNull->Sampler : m->Sampler};
            values[bindingCount - 1] = {static_cast<uint32>(bindingCount - 1), {}, 0, 0, m->EmissionTexture, {}};
        }
        const Status created =
            rhi::CreateBindingSet(s.Device, s.Programs[m->ProgramSlot].Layout, {values, bindingCount}, m->Bindings);
        if (Accepted(created) == false)
        {
            return m->Setup = created;
        }
        m->BindingCreated = true;
    }
    const auto ready = rhi::GetStatus(s.Device, m->Bindings);
    if (ready != Status::Pending)
    {
        m->Setup = ready;
    }
    return ready;
}
Status Renderer::PublishMaterial(MaterialVersion& candidate, MaterialVersion& current) noexcept
{
    if (mState.Get() == nullptr ||
        (Access::Owner(candidate) == Access::Owner(current) && Access::Slot(candidate) == Access::Slot(current) &&
         Access::Generation(candidate) == Access::Generation(current)))
    {
        return Status::InvalidHandle;
    }
    auto ready = GetStatus(candidate);
    if (ready != Status::Ready)
    {
        return ready == Status::Pending ? Status::NotReady : ready;
    }
    if (Access::Owner(current) != 0)
    {
        ready = Release(current);
        if (ready != Status::Ready)
        {
            return ready;
        }
    }
    current = candidate;
    candidate = {};
    return Status::Ready;
}
Status Renderer::Release(MaterialVersion& handle) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto* m = Resolve(mState->Materials, mState->Owner, handle);
    if (m == nullptr)
    {
        return Status::InvalidHandle;
    }
    m->Owned = false;
    mState->DropMaterial(Access::Slot(handle));
    handle = {};
    return Status::Ready;
}

usize Renderer::GetUniformBytes() const noexcept
{
    return mState.Get() != nullptr ? mState->FlatUniforms.CallerBytes : 0;
}
Status Renderer::CreateUniformSet(BorrowedView<const uint8> bytes, UniformSet& output) noexcept
{
    if (mState.Get() == nullptr || mState->Setup != Status::Ready)
    {
        return Status::NotReady;
    }
    if (Access::Owner(output) != 0)
    {
        return Status::InvalidState;
    }
    const Status admitted = RendererAdmission(mState->Device);
    if (admitted != Status::Ready)
    {
        return admitted;
    }
    internal::UniformSetIdentity identity;
    const Status created = mState->UniformSets.Create(mState->Device, mState->FlatUniforms, bytes, identity);
    if (Accepted(created) == false)
    {
        return created;
    }
    output = Access::Make<UniformSet>({mState->Owner, identity.Generation, identity.Slot});
    return created;
}
Status Renderer::GetStatus(UniformSet handle) noexcept
{
    if (mState.Get() == nullptr || Access::Owner(handle) != mState->Owner)
    {
        return Status::InvalidHandle;
    }
    return mState->UniformSets.GetStatus(mState->Device,
                                         mState->FlatUniforms,
                                         Access::Slot(handle),
                                         Access::Generation(handle));
}
Status Renderer::Release(UniformSet& handle) noexcept
{
    if (mState.Get() == nullptr || Access::Owner(handle) != mState->Owner ||
        mState->UniformSets.IsOwned(Access::Slot(handle), Access::Generation(handle)) == false)
    {
        return Status::InvalidHandle;
    }
    mState->UniformSets.Detach(mState->Device, Access::Slot(handle));
    handle = {};
    return Status::Ready;
}

Status Renderer::CreateMesh(const MeshDescription& description, Mesh& output) noexcept
{
    if (mState.Get() == nullptr || mState->Setup != Status::Ready)
    {
        return Status::NotReady;
    }
    if (Access::Owner(output) != 0)
    {
        return Status::InvalidState;
    }
    const auto& input = description;
    rhi::RasterCapabilities caps;
    auto status = rhi::GetRasterCapabilities(mState->Device, caps);
    if (status != Status::Ready)
    {
        return status;
    }
    if (input.Positions == nullptr || input.Indices == nullptr || input.VertexCount == 0 || input.IndexCount == 0 ||
        input.IndexCount % 3 != 0 || input.VertexCount > ~uint32{0} || input.IndexCount > ~uint32{0} ||
        input.VertexCount > caps.MaxBufferSize / sizeof(Vertex) ||
        input.IndexCount > caps.MaxBufferSize / sizeof(uint32))
    {
        return Status::InvalidDescription;
    }
    if (input.Tangents.IsValid() == false || input.Colors.IsValid() == false ||
        input.MaterialTexcoords1.IsValid() == false ||
        (input.Tangents.IsEmpty() == false && input.Tangents.GetSize() != input.VertexCount) ||
        (input.Colors.IsEmpty() == false && input.Colors.GetSize() != input.VertexCount) ||
        (input.MaterialTexcoords1.IsEmpty() == false && input.MaterialTexcoords1.GetSize() != input.VertexCount))
    {
        return Status::InvalidDescription;
    }
    if (input.Tangents.IsEmpty() == false && input.Normals == nullptr)
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < input.VertexCount; ++i)
    {
        if (input.MaterialTexcoords1.IsEmpty() == false && math::IsFinite(input.MaterialTexcoords1[i]) == false)
        {
            return Status::InvalidDescription;
        }
        if (input.Colors.IsEmpty() == false)
        {
            const math::Vector4 color = input.Colors[i];
            if (math::IsFinite(color) == false || color.X < 0 || color.Y < 0 || color.Z < 0 || color.W < 0 ||
                color.X > 1 || color.Y > 1 || color.Z > 1 || color.W > 1)
            {
                return Status::InvalidDescription;
            }
        }
        if (input.Tangents.IsEmpty() == false)
        {
            const math::Vector4 tangent = input.Tangents[i];
            math::Vector3 normal;
            if (math::IsFinite(tangent) == false || (tangent.W != 0 && tangent.W != 1 && tangent.W != -1) ||
                math::TryNormalize(input.Normals[i], normal) != math::MathStatus::Success)
            {
                return Status::InvalidDescription;
            }
            const math::Vector3 axis{tangent.X, tangent.Y, tangent.Z};
            if (tangent.W != 0 && math::Length(axis - normal * math::Dot(normal, axis)) <= 1e-8F)
            {
                return Status::InvalidDescription;
            }
        }
    }
    auto bounds = math::EmptyAabb3();
    for (usize i = 0; i < input.VertexCount; ++i)
    {
        const auto position = input.Positions[i];
        if (math::IsFinite(position) == false ||
            (input.Texcoords != nullptr && math::IsFinite(input.Texcoords[i]) == false) ||
            (input.BakeTexcoords != nullptr && math::IsFinite(input.BakeTexcoords[i]) == false) ||
            (input.Normals != nullptr &&
             (math::IsFinite(input.Normals[i]) == false || math::Length(input.Normals[i]) == 0)))
        {
            return Status::InvalidDescription;
        }
        bounds = math::Merge(bounds, {position, position});
    }
    for (usize i = 0; i < input.IndexCount; ++i)
    {
        if (input.Indices[i] >= input.VertexCount)
        {
            return Status::InvalidDescription;
        }
    }
    usize slot = 0;
    status = FreeSlot(mState->MeshRecords, slot);
    if (status != Status::Ready)
    {
        return status;
    }
    auto& mesh = mState->MeshRecords[slot];
    foundation::Array<Vertex> vertices;
    if (vertices.TryResize(input.VertexCount) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < input.VertexCount; ++i)
    {
        const auto uv = input.Texcoords != nullptr ? input.Texcoords[i] : math::Vector2{};
        math::Vector3 normal{0, 0, 1};
        if (input.Normals != nullptr)
        {
            static_cast<void>(math::TryNormalize(input.Normals[i], normal));
        }
        math::Vector4 tangent{};
        if (input.Tangents.IsEmpty() == false)
        {
            const math::Vector4 source = input.Tangents[i];
            math::Vector3 axis{source.X, source.Y, source.Z};
            if (source.W != 0)
            {
                static_cast<void>(math::TryNormalize(axis - normal * math::Dot(normal, axis), axis));
            }
            else
            {
                axis = {};
            }
            tangent = {axis.X, axis.Y, axis.Z, source.W};
        }
        vertices[i] = {input.Positions[i],
                       uv,
                       normal,
                       input.BakeTexcoords != nullptr ? input.BakeTexcoords[i] : uv,
                       tangent,
                       input.Colors.IsEmpty() == false ? input.Colors[i] : math::Vector4{1, 1, 1, 1},
                       input.MaterialTexcoords1.IsEmpty() == false ? input.MaterialTexcoords1[i] : uv};
    }
    core::UniquePtr<internal::ReferenceMeshData> referenceMesh;
    if (mState->Reference)
    {
        status = mState->PrepareReferenceMesh(input, vertices.GetData(), referenceMesh);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    Array<VisibilityVertex> visibilityVertices;
    Array<uint32> visibilityIndices;
    const bool hasVisibilityInputs = mState->HasVisibilityInputs;
    if (hasVisibilityInputs == true)
    {
        status = internal::ExpandVisibilityGeometry({input.Positions, input.VertexCount},
                                                    {input.Indices, input.IndexCount},
                                                    caps.MaxBufferSize,
                                                    visibilityVertices,
                                                    visibilityIndices);
        if (status != Status::Ready)
        {
            return status;
        }
    }
    const usize vertexBytes =
        hasVisibilityInputs == true ? input.IndexCount * sizeof(VisibilityVertex) : input.VertexCount * sizeof(Vertex);
    const usize indexBytes = input.IndexCount * sizeof(uint32);
    const uint8* vertexData = hasVisibilityInputs == true ? reinterpret_cast<const uint8*>(visibilityVertices.GetData())
                                                          : reinterpret_cast<const uint8*>(vertices.GetData());
    const uint32* indexData = hasVisibilityInputs == true ? visibilityIndices.GetData() : input.Indices;
    status = rhi::CreateBuffer(mState->Device,
                               {rhi::BufferRole::Vertex, vertexBytes},
                               {vertexData, vertexBytes},
                               mesh.Vertices);
    if (Accepted(status) == false)
    {
        return status;
    }
    auto second = rhi::CreateBuffer(mState->Device,
                                    {rhi::BufferRole::Index32, indexBytes},
                                    {reinterpret_cast<const uint8*>(indexData), indexBytes},
                                    mesh.Indices);
    if (Accepted(second) == false)
    {
        static_cast<void>(rhi::Destroy(mState->Device, mesh.Vertices));
        return second;
    }
    if (referenceMesh)
    {
        mState->Reference->CpuBytes += referenceMesh->CpuBytes;
    }
    mesh.Reference = core::Move(referenceMesh);
    mesh.Used = mesh.Owned = true;
    mesh.References = 0;
    mesh.VertexCount = static_cast<uint32>(hasVisibilityInputs == true ? input.IndexCount : input.VertexCount);
    mesh.IndexCount = static_cast<uint32>(input.IndexCount);
    mesh.Bounds = bounds;
    mesh.HasNormals = input.Normals != nullptr;
    output = Access::Make<Mesh>({mState->Owner, mesh.Generation, slot});
    return status == Status::Pending || second == Status::Pending ||
                   (mesh.Reference && mesh.Reference->NativeStatus == Status::Pending)
               ? Status::Pending
               : Status::Ready;
}
Status Renderer::GetStatus(Mesh handle) const noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    const auto* mesh = Resolve(mState->MeshRecords, mState->Owner, handle);
    if (mesh == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto status = rhi::GetStatus(mState->Device, mesh->Vertices);
    if (status == Status::Ready)
    {
        status = rhi::GetStatus(mState->Device, mesh->Indices);
    }
    if (status == Status::Ready && mesh->Reference && mesh->Reference->NativeStatus == Status::Pending)
    {
        status = rhi::GetStatus(mState->Device, mesh->Reference->Geometry);
    }
    return status;
}
Status Renderer::Release(Mesh& handle) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto* mesh = Resolve(mState->MeshRecords, mState->Owner, handle);
    if (mesh == nullptr)
    {
        return Status::InvalidHandle;
    }
    mesh->Owned = false;
    mState->DropMesh(Access::Slot(handle));
    handle = {};
    return Status::Ready;
}
Status Renderer::CreateSnapshot(const SceneItem* items, usize count, Snapshot& output) noexcept
{
    if (mState.Get() == nullptr || mState->Setup != Status::Ready)
    {
        return Status::NotReady;
    }
    if (Access::Owner(output) != 0)
    {
        return Status::InvalidState;
    }
    if (count > mState->Capacities.Objects)
    {
        return Status::CapacityExceeded;
    }
    if (count != 0 && items == nullptr)
    {
        return Status::InvalidDescription;
    }
    usize slot = 0;
    auto status = FreeSlot(mState->SnapshotRecords, slot);
    if (status != Status::Ready)
    {
        return status;
    }
    auto& snapshot = mState->SnapshotRecords[slot];
    for (usize i = 0; i < count; ++i)
    {
        const auto& item = items[i];
        constexpr uint32 MAX_VISIBILITY_REFERENCE = 0xffff;
        if (mState->HasVisibilityInputs == true &&
            (item.DrawIndex > MAX_VISIBILITY_REFERENCE || item.InstanceIndex > MAX_VISIBILITY_REFERENCE ||
             Access::Owner(item.Surface) != 0 || Access::Owner(item.LodGeometry) != 0))
        {
            return Status::Unsupported;
        }
        if (item.SourceId == 0 || math::IsFinite(item.Transform) == false ||
            (item.Pass != Layer::Opaque && item.Pass != Layer::Overlay) ||
            (Access::Owner(item.Surface) == 0 &&
             (ValidColor(item.Material.Color) == false || (item.Pass == Layer::Opaque && item.Material.Color.W != 1))))
        {
            return Status::InvalidDescription;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (items[j].SourceId == item.SourceId)
            {
                return Status::InvalidDescription;
            }
        }
        status = GetStatus(item.Geometry);
        if (status != Status::Ready)
        {
            return status == Status::Pending ? Status::NotReady : status;
        }
        if (Access::Owner(item.LodGeometry) != 0)
        {
            status = GetStatus(item.LodGeometry);
            if (status != Status::Ready)
            {
                return status == Status::Pending ? Status::NotReady : status;
            }
            if (item.Pass != Layer::Opaque || math::IsFinite(item.LodPixels) == false || item.LodPixels <= 0)
            {
                return Status::InvalidDescription;
            }
        }
        else if (item.LodPixels != 0 || item.PreviousLod)
        {
            return Status::InvalidDescription;
        }
        usize uniformSlot = NO_UNIFORM;
        if (Access::Owner(item.Uniforms) != 0)
        {
            if (Access::Owner(item.Uniforms) != mState->Owner || Access::Owner(item.Surface) != 0 ||
                mState->FlatUniforms.Count == 0)
            {
                return Status::InvalidDescription;
            }
            status = GetStatus(item.Uniforms);
            if (status != Status::Ready)
            {
                return status == Status::Pending ? Status::NotReady : status;
            }
            uniformSlot = Access::Slot(item.Uniforms);
        }
        usize materialSlot = NO_MATERIAL;
        if (Access::Owner(item.Surface) != 0)
        {
            status = GetStatus(item.Surface);
            if (status != Status::Ready)
            {
                return status == Status::Pending ? Status::NotReady : status;
            }
            materialSlot = Access::Slot(item.Surface);
            const auto& material = mState->Materials[materialSlot];
            if (material.Lit && (mState->MeshRecords[Access::Slot(item.Geometry)].HasNormals == false ||
                                 (Access::Owner(item.LodGeometry) != 0 &&
                                  mState->MeshRecords[Access::Slot(item.LodGeometry)].HasNormals == false)))
            {
                return Status::InvalidDescription;
            }
            math::Matrix3 normals;
            if (material.Lit && math::TryNormalMatrix(item.Transform, {}, normals) != math::MathStatus::Success)
            {
                return Status::InvalidDescription;
            }
            if (material.Lit)
            {
                for (const auto& column : normals.Columns)
                {
                    if (math::Abs(column.X) > 1e12F || math::Abs(column.Y) > 1e12F || math::Abs(column.Z) > 1e12F)
                    {
                        return Status::InvalidDescription;
                    }
                }
            }
            const auto alpha = material.Alpha;
            if ((alpha == MaterialAlpha::Painter) != (item.Pass == Layer::Overlay))
            {
                return Status::InvalidDescription;
            }
        }
        auto& destination = snapshot.Items[i];
        destination.Input = item;
        destination.MeshSlot = Access::Slot(item.Geometry);
        destination.MaterialSlot = materialSlot;
        destination.UniformSlot = uniformSlot;
        if (math::TryTransformAabb(item.Transform,
                                   mState->MeshRecords[destination.MeshSlot].Bounds,
                                   destination.Bounds) != math::MathStatus::Success)
        {
            return Status::InvalidDescription;
        }
        if (Access::Owner(item.LodGeometry) != 0)
        {
            math::Aabb3 coarse;
            if (math::TryTransformAabb(item.Transform,
                                       mState->MeshRecords[Access::Slot(item.LodGeometry)].Bounds,
                                       coarse) != math::MathStatus::Success)
            {
                return Status::InvalidDescription;
            }
            auto& b = destination.Bounds;
            b.Min = {math::Min(b.Min.X, coarse.Min.X),
                     math::Min(b.Min.Y, coarse.Min.Y),
                     math::Min(b.Min.Z, coarse.Min.Z)};
            b.Max = {math::Max(b.Max.X, coarse.Max.X),
                     math::Max(b.Max.Y, coarse.Max.Y),
                     math::Max(b.Max.Z, coarse.Max.Z)};
        }
        if (materialSlot != NO_MATERIAL && mState->Materials[materialSlot].Lit)
        {
            const auto& bounds = destination.Bounds;
            if (math::Abs(bounds.Min.X) > 1000000 || math::Abs(bounds.Min.Y) > 1000000 ||
                math::Abs(bounds.Min.Z) > 1000000 || math::Abs(bounds.Max.X) > 1000000 ||
                math::Abs(bounds.Max.Y) > 1000000 || math::Abs(bounds.Max.Z) > 1000000)
            {
                return Status::InvalidDescription;
            }
        }
    }
    for (auto& used : snapshot.HasGroup)
    {
        used = false;
    }
    for (usize i = 0; i < count; ++i)
    {
        const auto& item = snapshot.Items[i];
        if (item.Input.Pass == Layer::Opaque)
        {
            const auto group = i / 16;
            auto& b = snapshot.Groups[group];
            if (snapshot.HasGroup[group] == false)
            {
                b = item.Bounds;
            }
            else
            {
                b.Min = {math::Min(b.Min.X, item.Bounds.Min.X),
                         math::Min(b.Min.Y, item.Bounds.Min.Y),
                         math::Min(b.Min.Z, item.Bounds.Min.Z)};
                b.Max = {math::Max(b.Max.X, item.Bounds.Max.X),
                         math::Max(b.Max.Y, item.Bounds.Max.Y),
                         math::Max(b.Max.Z, item.Bounds.Max.Z)};
            }
            snapshot.HasGroup[group] = true;
        }
        if (Access::Owner(item.Input.LodGeometry) != 0)
        {
            ++mState->MeshRecords[Access::Slot(item.Input.LodGeometry)].References;
        }
        ++mState->MeshRecords[snapshot.Items[i].MeshSlot].References;
        if (snapshot.Items[i].MaterialSlot != NO_MATERIAL)
        {
            ++mState->Materials[snapshot.Items[i].MaterialSlot].References;
        }
        if (snapshot.Items[i].UniformSlot != NO_UNIFORM)
        {
            mState->UniformSets.Retain(snapshot.Items[i].UniformSlot);
        }
    }
    snapshot.Count = count;
    snapshot.References = 0;
    snapshot.Used = snapshot.Owned = true;
    output = Access::Make<Snapshot>({mState->Owner, snapshot.Generation, slot});
    return Status::Ready;
}
Status Renderer::Release(Snapshot& handle) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto* snapshot = Resolve(mState->SnapshotRecords, mState->Owner, handle);
    if (snapshot == nullptr)
    {
        return Status::InvalidHandle;
    }
    snapshot->Owned = false;
    mState->DropSnapshot(Access::Slot(handle));
    handle = {};
    return Status::Ready;
}
Status Renderer::ReplaceSnapshot(const SceneItem* items, usize count, Snapshot& current) noexcept
{
    if (mState.Get() == nullptr || Resolve(mState->SnapshotRecords, mState->Owner, current) == nullptr)
    {
        return Status::InvalidHandle;
    }
    Snapshot replacement;
    const auto status = CreateSnapshot(items, count, replacement);
    if (status != Status::Ready)
    {
        return status;
    }
    const auto released = Release(current);
    if (released != Status::Ready)
    {
        static_cast<void>(Release(replacement));
        return released;
    }
    current = replacement;
    return Status::Ready;
}
Status Renderer::PrepareView(Snapshot handle,
                             const ViewDescription& description,
                             PreparedView& output,
                             ViewReport& report) noexcept
{
    if (mState.Get() == nullptr || mState->Setup != Status::Ready)
    {
        return Status::NotReady;
    }
    if (Access::Owner(output) != 0)
    {
        return Status::InvalidState;
    }
    const auto* snapshot = Resolve(mState->SnapshotRecords, mState->Owner, handle);
    if (snapshot == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (math::IsFinite(description.WorldToClip) == false || math::IsFinite(description.CullMargin) == false ||
        description.CullMargin < 0 || math::IsFinite(description.OverlayToClip) == false ||
        math::IsFinite(description.CameraPosition) == false ||
        math::Abs(description.CameraPosition.X) > internal::MAX_WORLD_COORDINATE ||
        math::Abs(description.CameraPosition.Y) > internal::MAX_WORLD_COORDINATE ||
        math::Abs(description.CameraPosition.Z) > internal::MAX_WORLD_COORDINATE ||
        static_cast<uint8>(description.Depth) > static_cast<uint8>(DepthConvention::ReverseZ) ||
        (description.InfiniteFar && description.Depth != DepthConvention::ReverseZ) ||
        IsDebugDepthRangeValid(description.DebugDepthRange) == false)
    {
        return Status::InvalidDescription;
    }
    if (math::IsFinite(description.LodHysteresis) == false || description.LodHysteresis < 0 ||
        description.LodHysteresis > 0.5F ||
        (description.EnableLod && (description.LodWidth == 0 || description.LodHeight == 0)))
    {
        return Status::InvalidDescription;
    }
    if (description.Depth == DepthConvention::ReverseZ && mState->ViewsSetup != Status::Ready)
    {
        return Status::NotReady;
    }
    math::Vector3 cameraForward;
    if (math::TryNormalize(description.CameraForward, cameraForward) != math::MathStatus::Success)
    {
        return Status::InvalidDescription;
    }
    // Orthographic debug depth is measured in shaders as dot(position, forward); shift the
    // camera-relative range into that frame so LinearDepth keeps its camera-distance contract.
    const float32 debugDepthOffset = description.OrthographicCamera == true &&
                                             (description.DebugDepthRange.X != 0 || description.DebugDepthRange.Y != 0)
                                         ? math::Dot(description.CameraPosition, cameraForward)
                                         : 0.0F;
    math::Frustum frustum, overlay;
    if (math::TryExtractFrustum(description.WorldToClip,
                                description.InfiniteFar ? math::FrustumMode::InfiniteReverseZPerspective
                                                        : math::FrustumMode::FinitePerspectiveOrOrthographic,
                                frustum) != math::MathStatus::Success ||
        math::TryExtractFrustum(description.OverlayToClip,
                                math::FrustumMode::FinitePerspectiveOrOrthographic,
                                overlay) != math::MathStatus::Success)
    {
        return Status::InvalidDescription;
    }
    core::UniquePtr<WorldGridBuffers> gridBuffers;
    Status gridStatus = Status::Ready;
    if (description.WorldGrid.IsVisible == true && description.ShadowCasters == false)
    {
        gridStatus = mState->PrepareGridPipelines();
        if (Accepted(gridStatus) == false)
        {
            return gridStatus;
        }
        usize gridHostBytes = sizeof(WorldGridBuffers);
        if (mState->GridScratch.Get() == nullptr)
        {
            gridHostBytes += sizeof(WorldGridScratch);
        }
        usize requiredHostBytes = 0;
        if (TryAdd(mState->HostBytes, gridHostBytes, requiredHostBytes) == false ||
            requiredHostBytes > mState->Capacities.HostBytes)
        {
            return Status::CapacityExceeded;
        }
        if (mState->GridScratch.Get() == nullptr)
        {
            mState->GridScratch.Reset(new (std::nothrow) WorldGridScratch);
            if (mState->GridScratch.Get() == nullptr)
            {
                return Status::OutOfMemory;
            }
            mState->HostBytes += sizeof(WorldGridScratch);
        }
        const Status buffers = PrepareWorldGridBuffers(mState->Device, description, *mState->GridScratch, gridBuffers);
        if (Accepted(buffers) == false)
        {
            return buffers;
        }
        if (buffers == Status::Pending)
        {
            gridStatus = Status::Pending;
        }
    }
    usize slot = 0;
    auto status = FreeSlot(mState->ViewRecords, slot);
    if (status != Status::Ready)
    {
        return status;
    }
    auto& view = mState->ViewRecords[slot];
    auto& candidates = mState->Candidates;
    auto& scratch = mState->Scratch;
    usize count = 0, opaqueCount = 0;
    uint32 boundsTests = 0, culledGroups = 0;
    auto& groups = mState->GroupRelations;
    if (description.Reference == false)
    {
        for (usize i = 0; i < (snapshot->Count + 15) / 16; ++i)
        {
            if (snapshot->HasGroup[i] == false)
            {
                continue;
            }
            if (math::TryClassifyAabb(frustum, snapshot->Groups[i], description.CullMargin, groups[i]) !=
                math::MathStatus::Success)
            {
                return Status::InvalidDescription;
            }
            culledGroups += groups[i] == math::FrustumRelation::Outside ? 1U : 0U;
        }
    }
    const Layer passes[]{Layer::Opaque, Layer::Overlay};
    for (const auto pass : passes)
    {
        for (usize i = 0; i < snapshot->Count; ++i)
        {
            const auto& item = snapshot->Items[i];
            if (item.Input.Pass != pass || (description.ShadowCasters && pass == Layer::Overlay))
            {
                continue;
            }
            math::FrustumRelation relation;
            if (description.Reference == false && pass == Layer::Opaque &&
                groups[i / CULL_GROUP_ITEM_COUNT] == math::FrustumRelation::Outside)
            {
                continue;
            }
            if (description.Reference == false &&
                (pass != Layer::Opaque || groups[i / CULL_GROUP_ITEM_COUNT] != math::FrustumRelation::Inside))
            {
                ++boundsTests;
                if (math::TryClassifyAabb(pass == Layer::Opaque ? frustum : overlay,
                                          item.Bounds,
                                          pass == Layer::Opaque ? description.CullMargin : 0,
                                          relation) != math::MathStatus::Success)
                {
                    return Status::InvalidDescription;
                }
                if (relation == math::FrustumRelation::Outside)
                {
                    continue;
                }
            }
            auto& candidate = candidates[count++];
            candidate.Data = {};
            candidate.Item = i;
            candidate.MeshSlot = item.MeshSlot;
            candidate.MaterialSlot = item.MaterialSlot;
            candidate.UniformSlot = item.UniformSlot;
            candidate.Coarse = false;
            const auto clip = (pass == Layer::Opaque ? description.WorldToClip : description.OverlayToClip) *
                              Matrix(item.Input.Transform);
            if (math::IsFinite(clip) == false)
            {
                return Status::InvalidDescription;
            }
            const auto rows = math::Transpose(clip);
            for (usize row = 0; row < 4; ++row)
            {
                candidate.Data.Rows[row] = rows.Columns[row];
            }
            candidate.Data.Color =
                item.MaterialSlot == NO_MATERIAL ? item.Input.Material.Color : math::Vector4{1, 1, 1, 1};
            if (item.MaterialSlot != NO_MATERIAL && mState->Materials[item.MaterialSlot].Clustered)
            {
                candidate.Data.Color.Y = description.Reference || description.DirectLighting ? 1.0F : 0.0F;
            }
            const auto worldRows = math::Transpose(Matrix(item.Input.Transform));
            for (usize row = 0; row < 3; ++row)
            {
                candidate.Data.WorldRows[row] = worldRows.Columns[row];
            }
            candidate.Data.Eye = {description.CameraPosition.X,
                                  description.CameraPosition.Y,
                                  description.CameraPosition.Z,
                                  1};
            if (item.MaterialSlot != NO_MATERIAL && mState->Materials[item.MaterialSlot].Lit)
            {
                const auto& material = mState->Materials[item.MaterialSlot];
                math::Matrix3 normal;
                if (math::TryNormalMatrix(item.Input.Transform, {}, normal) != math::MathStatus::Success)
                {
                    return Status::InvalidDescription;
                }
                for (usize row = 0; row < 3; ++row)
                {
                    candidate.Data.NormalRows[row] = {normal.At(row, 0), normal.At(row, 1), normal.At(row, 2), 0};
                }
                // Lit programs never read Color.ZW; debug-view programs map camera distance with it.
                candidate.Data.Color.Z = description.DebugDepthRange.X + debugDepthOffset;
                candidate.Data.Color.W = description.DebugDepthRange.Y + debugDepthOffset;
                const auto& bake = material.Bake;
                candidate.Data.Color.X = material.HasBake && bake.SourceId == item.Input.SourceId &&
                                                 bake.SceneRevision == item.Input.BakeSceneRevision &&
                                                 bake.DependencyRevision == item.Input.BakeDependencyRevision &&
                                                 bake.Transform == item.Input.Transform
                                             ? 1.0F
                                             : 0.0F;
                if (material.ShadowSlot != NO_SHADOW)
                {
                    candidate.Data.Eye.W =
                        mState->SameCasters(mState->SnapshotRecords[mState->Shadows[material.ShadowSlot].SnapshotSlot],
                                            Access::Slot(handle))
                            ? 1.0F
                            : 0.0F;
                }
            }
            if (description.OrthographicCamera)
            {
                candidate.Data.Eye.X = -cameraForward.X;
                candidate.Data.Eye.Y = -cameraForward.Y;
                candidate.Data.Eye.Z = -cameraForward.Z;
                candidate.Data.Eye.W += 2;
            }
            if (pass == Layer::Overlay)
            {
                candidate.Data.Color.X *= candidate.Data.Color.W;
                candidate.Data.Color.Y *= candidate.Data.Color.W;
                candidate.Data.Color.Z *= candidate.Data.Color.W;
            }
            if (mState->HasVisibilityInputs == true)
            {
                candidate.Data.Eye = {static_cast<float32>(item.Input.DrawIndex),
                                      static_cast<float32>(item.Input.InstanceIndex),
                                      0,
                                      0};
            }
            candidate.Bounds =
                ProjectBounds(item.Bounds,
                              pass == Layer::Opaque ? description.WorldToClip : description.OverlayToClip,
                              pass == Layer::Opaque && description.Depth == DepthConvention::ReverseZ);
            if (pass == Layer::Opaque && description.Reference == false && description.ShadowCasters == false &&
                description.EnableLod && Access::Owner(item.Input.LodGeometry) != 0)
            {
                const auto pixels =
                    math::Max((candidate.Bounds.Right - candidate.Bounds.Left) * description.LodWidth,
                              (candidate.Bounds.Top - candidate.Bounds.Bottom) * description.LodHeight) *
                    0.5;
                const auto threshold = static_cast<float64>(item.Input.LodPixels) *
                                       (item.Input.PreviousLod ? 1 + static_cast<float64>(description.LodHysteresis)
                                                               : 1 - static_cast<float64>(description.LodHysteresis));
                candidate.Coarse = pixels < static_cast<float64>(threshold);
                if (candidate.Coarse)
                {
                    candidate.MeshSlot = Access::Slot(item.Input.LodGeometry);
                }
            }
        }
        if (pass == Layer::Opaque)
        {
            opaqueCount = count;
        }
    }
    // Representation/context separation follows Donald Revie, "Designing a Data-Driven Renderer",
    // GPU Pro 3, V.4, sections 4.4-4.8. Thanks for the explicit pass-context distinction;
    // Ludus uses copied concrete packets rather than the chapter's general object/XML pipeline.
    // See docs/architecture/renderer-systems-gems-review.md for consulted source details.
    bool homogeneous = true;
    for (usize i = 1; i < opaqueCount; ++i)
    {
        homogeneous &= candidates[i].MeshSlot == candidates[0].MeshSlot &&
                       candidates[i].MaterialSlot == candidates[0].MaterialSlot &&
                       candidates[i].UniformSlot == candidates[0].UniformSlot;
    }
    // Already compatible objects need no reordering or pairwise overlap search;
    // preserving their source order also preserves equal-depth ownership.
    if (description.Reference == false && homogeneous == false)
    {
        for (usize first = 0; first < opaqueCount;)
        {
            usize last = first + 1;
            for (; last < opaqueCount; ++last)
            {
                bool blocked = false;
                for (usize previous = first; previous < last; ++previous)
                {
                    if (Overlap(candidates[previous].Bounds, candidates[last].Bounds))
                    {
                        blocked = true;
                        break;
                    }
                }
                if (blocked)
                {
                    break;
                }
            }
            static_cast<void>(StableSort(candidates.GetData() + first, scratch.GetData(), last - first, MeshLess{}));
            first = last;
        }
    }
    if ((report.SourceIdOutput != nullptr || report.SelectedLodOutput != nullptr) && report.DetailCapacity < count)
    {
        return Status::CapacityExceeded;
    }
    ViewReport result;
    result.SourceIdOutput = report.SourceIdOutput;
    result.SelectedLodOutput = report.SelectedLodOutput;
    result.DetailCapacity = report.DetailCapacity;
    result.Reported = static_cast<uint32>(count < 256 ? count : 256);
    result.BoundsTests = boundsTests;
    result.CulledGroups = culledGroups;
    result.Submitted = static_cast<uint32>(snapshot->Count);
    result.Visible = static_cast<uint32>(count);
    result.Culled = result.Submitted - result.Visible;
    auto& instances = mState->Instances;
    view.DrawCount = 0;
    for (usize i = 0; i < count; ++i)
    {
        const auto& candidate = candidates[i];
        const auto& item = snapshot->Items[candidate.Item].Input;
        instances[i] = candidate.Data;
        if (i < result.Reported)
        {
            result.SourceIds[i] = item.SourceId;
            result.SelectedLods[i] = candidate.Coarse;
        }
        result.LodObjects += candidate.Coarse ? 1U : 0U;
        if (candidate.MaterialSlot != NO_MATERIAL && mState->Materials[candidate.MaterialSlot].Lit)
        {
            const auto& material = mState->Materials[candidate.MaterialSlot];
            result.BakeFallbacks += material.HasBake && candidate.Data.Color.X == 0 ? 1U : 0U;
            result.ShadowFallbacks +=
                material.ShadowSlot != NO_SHADOW && (static_cast<uint32>(candidate.Data.Eye.W) & 1U) == 0 ? 1U : 0U;
        }
        if (description.Reference == false && view.DrawCount != 0 &&
            view.Packets[view.DrawCount - 1].MeshSlot == candidate.MeshSlot &&
            view.Packets[view.DrawCount - 1].Pass == item.Pass &&
            view.Packets[view.DrawCount - 1].MaterialSlot == candidate.MaterialSlot &&
            view.Packets[view.DrawCount - 1].UniformSlot == candidate.UniformSlot)
        {
            ++view.Packets[view.DrawCount - 1].Count;
        }
        else
        {
            view.Packets[view.DrawCount++] =
                {candidate.MeshSlot, i, 1, item.Pass, candidate.MaterialSlot, candidate.UniformSlot};
        }
    }
    status = Status::Ready;
    if (count != 0)
    {
        const auto bytes = count * sizeof(Instance);
        if (description.ScheduledUpload)
        {
            status = rhi::RequestBufferUpload(mState->Device,
                                              {rhi::BufferRole::Vertex, bytes},
                                              reinterpret_cast<const uint8*>(instances.GetData()),
                                              bytes,
                                              view.Upload);
            view.UploadOwned = status == Status::Pending;
        }
        else
        {
            status = rhi::CreateBuffer(mState->Device,
                                       {rhi::BufferRole::Vertex, bytes},
                                       {reinterpret_cast<const uint8*>(instances.GetData()), bytes},
                                       view.Instances);
        }
        if (Accepted(status) == false)
        {
            return status;
        }
    }
    view.Grid = Move(gridBuffers);
    if (view.Grid.Get() != nullptr)
    {
        mState->HostBytes += sizeof(WorldGridBuffers);
        view.Grid->PipelineCount = mState->Hdr == true ? WORLD_GRID_PIPELINE_COUNT : WORLD_GRID_HDR_PIPELINE;
    }
    view.Depth = description.Depth;
    view.Visible = count;
    view.SnapshotSlot = Access::Slot(handle);
    ++mState->SnapshotRecords[view.SnapshotSlot].References;
    view.Used = view.Owned = true;
    view.HasRetry = false;
    result.Draws = static_cast<uint32>(view.DrawCount + (view.Grid.Get() != nullptr ? 1 : 0));
    for (usize i = 0; i < count; ++i)
    {
        if (result.SourceIdOutput != nullptr)
        {
            result.SourceIdOutput[i] = snapshot->Items[candidates[i].Item].Input.SourceId;
        }
        if (result.SelectedLodOutput != nullptr)
        {
            result.SelectedLodOutput[i] = candidates[i].Coarse;
        }
    }
    report = result;
    output = Access::Make<PreparedView>({mState->Owner, view.Generation, slot});
    return gridStatus == Status::Pending ? gridStatus : status;
}
Status Renderer::GetStatus(PreparedView handle) const noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto* view = Resolve(mState->ViewRecords, mState->Owner, handle);
    if (view == nullptr)
    {
        return Status::InvalidHandle;
    }
    rhi::RasterCapabilities caps;
    const auto status = rhi::GetRasterCapabilities(mState->Device, caps);
    if (status != Status::Ready)
    {
        return status;
    }
    if (view->UploadOwned)
    {
        const auto upload = rhi::GetStatus(mState->Device, view->Upload);
        if (upload != Status::Ready)
        {
            return upload;
        }
        const auto taken = rhi::TakeUploadedBuffer(mState->Device, view->Upload, view->Instances);
        if (taken != Status::Ready)
        {
            return taken;
        }
        view->UploadOwned = false;
    }
    if (view->Grid.Get() != nullptr)
    {
        const usize pipelineCount = view->Grid->PipelineCount;
        for (usize i = 0; i < pipelineCount; ++i)
        {
            const Status ready = rhi::GetStatus(mState->Device, mState->GridPipelines[i]);
            if (ready != Status::Ready)
            {
                return ready;
            }
        }
        const WorldGridBuffers& grid = *view->Grid;
        const rhi::BufferHandle buffers[]{grid.Vertices, grid.Indices, grid.Instances};
        for (rhi::BufferHandle buffer : buffers)
        {
            const Status ready = rhi::GetStatus(mState->Device, buffer);
            if (ready != Status::Ready)
            {
                return ready;
            }
        }
    }
    return view->Visible == 0 ? Status::Ready : rhi::GetStatus(mState->Device, view->Instances);
}
Status Renderer::Submit(PreparedView handle, rhi::SurfaceHandle surface, rhi::SubmissionToken& completion) noexcept
{
    auto status = GetStatus(handle);
    if (status != Status::Ready)
    {
        return status;
    }
    auto& view = mState->ViewRecords[Access::Slot(handle)];
    for (usize i = 0; i < view.DrawCount; ++i)
    {
        const auto material = view.Packets[i].MaterialSlot;
        if (material != NO_MATERIAL && mState->Materials[material].Lit)
        {
            return Status::Unsupported;
        }
    }
    if (view.Depth != DepthConvention::Conventional)
    {
        return Status::InvalidDescription;
    }
    if (view.HasRetry == false)
    {
        status = rhi::BeginCommands(mState->Device, view.Retry);
        if (status != Status::Ready)
        {
            return status;
        }
        view.HasRetry = true;
        bool hasRecordedGrid = false;
        const auto recordGrid = [&]() noexcept {
            hasRecordedGrid = true;
            if (view.Grid.Get() == nullptr)
            {
                return Status::Ready;
            }
            const WorldGridBuffers& grid = *view.Grid;
            const rhi::RasterVertexSlice slices[]{{grid.Vertices, 0, WORLD_GRID_VERTEX_COUNT * sizeof(Vertex)},
                                                  {grid.Instances, 0, grid.Count * sizeof(Instance)}};
            return rhi::RecordDraw(mState->Device,
                                   view.Retry,
                                   {mState->GridPipelines[0],
                                    mState->Bindings,
                                    slices,
                                    grid.Indices,
                                    0,
                                    WORLD_GRID_INDEX_COUNT,
                                    WORLD_GRID_VERTEX_COUNT,
                                    grid.Count});
        };
        for (usize i = 0; i < view.DrawCount; ++i)
        {
            const auto& packet = view.Packets[i];
            if (packet.Pass == Layer::Overlay && hasRecordedGrid == false)
            {
                status = recordGrid();
                if (status != Status::Ready)
                {
                    break;
                }
            }
            const auto& mesh = mState->MeshRecords[packet.MeshSlot];
            const rhi::RasterVertexSlice slices[]{
                {mesh.Vertices, 0, mesh.VertexCount * sizeof(Vertex)},
                {view.Instances, packet.First * sizeof(Instance), packet.Count * sizeof(Instance)}};
            auto pipeline = packet.Pass == Layer::Opaque ? mState->Opaque : mState->Overlay;
            rhi::BindingSetHandle bindings = packet.UniformSlot != NO_UNIFORM
                                                 ? mState->UniformSets.GetBindings(packet.UniformSlot)
                                                 : mState->Bindings;
            if (packet.MaterialSlot != NO_MATERIAL)
            {
                const auto& material = mState->Materials[packet.MaterialSlot];
                pipeline = mState->Programs[material.ProgramSlot].Pipelines[packet.Pass == Layer::Opaque ? 0 : 2];
                bindings = material.Bindings;
            }
            status = rhi::RecordDraw(
                mState->Device,
                view.Retry,
                {pipeline, bindings, slices, mesh.Indices, 0, mesh.IndexCount, mesh.VertexCount, packet.Count});
            if (status != Status::Ready)
            {
                break;
            }
        }
        if (status == Status::Ready && hasRecordedGrid == false)
        {
            status = recordGrid();
        }
        if (status == Status::Ready)
        {
            status = rhi::FinishCommands(mState->Device, view.Retry);
        }
        if (status != Status::Ready)
        {
            static_cast<void>(rhi::DiscardCommands(mState->Device, view.Retry));
            view.HasRetry = false;
            return status;
        }
    }
    status = rhi::SubmitCommands(mState->Device, surface, view.Retry, completion);
    if (status == Status::Ready)
    {
        view.HasRetry = false;
    }
    else if (status != Status::NotReady)
    {
        static_cast<void>(rhi::DiscardCommands(mState->Device, view.Retry));
        view.HasRetry = false;
    }
    return status;
}
Status Renderer::Release(PreparedView& handle) noexcept
{
    if (mState.Get() == nullptr || Resolve(mState->ViewRecords, mState->Owner, handle) == nullptr)
    {
        return Status::InvalidHandle;
    }
    mState->DropView(Access::Slot(handle));
    handle = {};
    return Status::Ready;
}

Status Renderer::InitializeLighting(const rhi::RasterShaderDescription& outputVertex,
                                    const rhi::RasterShaderDescription& outputFragment,
                                    const rhi::RasterShaderDescription& shadowFragment) noexcept
{
    if (mState.Get() == nullptr || mState->ViewsSetup != Status::Ready)
    {
        return Status::NotReady;
    }
    auto& s = *mState;
    if (s.LightingSetup != Status::NotReady)
    {
        return Status::InvalidState;
    }
    if (outputVertex.Artifact.Stage != rhi::ShaderStage::Vertex || outputVertex.Inputs.size() != 1 ||
        outputVertex.Inputs[0].Location != 0 || outputVertex.Inputs[0].Format != rhi::RasterVertexFormat::Float4 ||
        outputVertex.Bindings.empty() == false || outputFragment.Artifact.Stage != rhi::ShaderStage::Fragment ||
        outputFragment.Inputs.empty() == false || outputFragment.Bindings.size() != OUTPUT_FRAGMENT_BINDING_COUNT ||
        outputFragment.UniformMembers.size() != 1 || outputFragment.UniformMembers[0].Binding != 0 ||
        outputFragment.UniformMembers[0].Name != "Output" || outputFragment.UniformMembers[0].Offset != 0 ||
        outputFragment.UniformMembers[0].Shape != rhi::RasterUniformShape::Float4 ||
        shadowFragment.Artifact.Stage != rhi::ShaderStage::Fragment || shadowFragment.Inputs.empty() == false ||
        shadowFragment.Bindings.empty() == false)
    {
        return Status::InvalidDescription;
    }
    const rhi::RasterBinding bindings[]{{0, rhi::RasterBindingKind::UniformBuffer, rhi::RasterVisibility::Fragment, 16},
                                        {1, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
                                        {2, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}};
    for (const auto& expected : bindings)
    {
        bool found = false;
        for (const auto& binding : outputFragment.Bindings)
        {
            found |= binding.Binding == expected.Binding && binding.Kind == expected.Kind &&
                     binding.Visibility == expected.Visibility && binding.MinSize == expected.MinSize;
        }
        if (found == false)
        {
            return Status::InvalidDescription;
        }
    }
    rhi::RasterCapabilities caps;
    auto status = rhi::GetRasterCapabilities(s.Device, caps);
    if (status != Status::Ready)
    {
        return status;
    }
    s.Hdr = caps.HdrColor;
    status = rhi::CreateRasterShader(s.Device, outputVertex, s.OutputVertex);
    if (Accepted(status))
    {
        status = rhi::CreateRasterShader(s.Device, outputFragment, s.OutputFragment);
    }
    if (Accepted(status))
    {
        status = rhi::CreateRasterShader(s.Device, shadowFragment, s.FlatShadowFragment);
    }
    if (Accepted(status))
    {
        status = rhi::CreateBindingLayout(s.Device, bindings, s.OutputLayout);
    }
    s.LightingSetup = Accepted(status) ? Status::Pending : status;
    return Accepted(status) ? PollLighting() : status;
}
Status Renderer::PollLighting() noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::NotReady;
    }
    auto& s = *mState;
    if (s.LightingSetup != Status::Pending)
    {
        return s.LightingSetup;
    }
    const Status prerequisites[]{rhi::GetStatus(s.Device, s.OutputVertex),
                                 rhi::GetStatus(s.Device, s.OutputFragment),
                                 rhi::GetStatus(s.Device, s.FlatShadowFragment),
                                 rhi::GetStatus(s.Device, s.OutputLayout)};
    for (auto ready : prerequisites)
    {
        if (ready != Status::Ready)
        {
            if (ready != Status::Pending)
            {
                s.LightingSetup = ready;
            }
            return ready;
        }
    }
    const rhi::RasterVertexStream quadStreams[]{{16, false}};
    const rhi::RasterVertexAttribute quadAttributes[]{{0, 0, 0, rhi::RasterVertexFormat::Float4}};
    const StaticArray<rhi::RasterVertexStream, FLAT_STREAM_COUNT>& streams =
        mState->HasVisibilityInputs == true ? VISIBILITY_STREAMS : FLAT_STREAMS;
    const rhi::RasterVertexAttribute* attributes =
        mState->HasVisibilityInputs == true ? VISIBILITY_ATTRIBUTES.GetData() : FLAT_ATTRIBUTES.GetData();
    const usize attributeCount =
        mState->HasVisibilityInputs == true ? VISIBILITY_ATTRIBUTE_COUNT : FLAT_ATTRIBUTE_COUNT;
    rhi::RasterPipelineHandle* targets[]{&s.OutputPipelines[0],
                                         &s.OutputPipelines[1],
                                         &s.FlatShadow,
                                         &s.HdrFlat[0],
                                         &s.HdrFlat[1],
                                         &s.HdrFlat[2]};
    const uint32 total = s.Hdr ? 6U : 3U;
    while (s.LightingPhase <= total)
    {
        if (s.LightingPhase != 0)
        {
            const auto ready = rhi::GetStatus(s.Device, *targets[s.LightingPhase - 1]);
            if (ready != Status::Ready)
            {
                if (ready != Status::Pending)
                {
                    s.LightingSetup = ready;
                }
                return ready;
            }
        }
        if (s.LightingPhase == total)
        {
            return s.LightingSetup = Status::Ready;
        }
        const auto phase = s.LightingPhase;
        rhi::RasterPipelineDescription description;
        if (phase < 2)
        {
            description = {s.OutputVertex, s.OutputFragment, s.OutputLayout, quadStreams, quadAttributes};
            description.Target = phase == 0 ? rhi::RasterTarget::Surface : rhi::RasterTarget::Rgba8Unorm;
        }
        else
        {
            description = {s.Vertex,
                           phase == 2 ? s.FlatShadowFragment : s.Fragment,
                           s.Layout,
                           streams,
                           {attributes, attributeCount},
                           phase != 5,
                           phase == 5};
            description.Target = phase == 2 ? rhi::RasterTarget::Rgba8Unorm : rhi::RasterTarget::Rgba16Float;
            description.DepthWrite = phase != 5;
            description.DepthCompare = phase == 4 ? rhi::RasterDepthCompare::Greater : rhi::RasterDepthCompare::Less;
        }
        const auto created = rhi::CreateRasterPipeline(s.Device, description, *targets[phase]);
        if (Accepted(created) == false)
        {
            return s.LightingSetup = created;
        }
        ++s.LightingPhase;
        if (created == Status::Pending)
        {
            return created;
        }
    }
    return s.LightingSetup;
}
Status Renderer::CreateLitMaterial(MaterialProgram program,
                                   const LitMaterialDescription& description,
                                   MaterialVersion& output,
                                   LightSelection& report) noexcept
{
    if (mState.Get() == nullptr || mState->LightingSetup != Status::Ready)
    {
        return Status::NotReady;
    }
    auto& s = *mState;
    const auto* p = Resolve(s.Programs, s.Owner, program);
    if (p == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (p->Lit == false)
    {
        return Status::InvalidDescription;
    }
    auto bounded = [](math::Vector3 value) noexcept {
        return math::IsFinite(value) && value.X >= 0 && value.Y >= 0 && value.Z >= 0 && value.X <= 10000 &&
               value.Y <= 10000 && value.Z <= 10000;
    };
    const auto& bake = description.Bake;
    if (math::IsFinite(description.Metallic) == false || description.Metallic < 0 || description.Metallic > 1 ||
        math::IsFinite(description.Roughness) == false || description.Roughness < MIN_ROUGHNESS ||
        description.Roughness > 1 || bounded(description.Emission) == false ||
        bounded(description.EnvironmentIrradiance) == false || bounded(description.EnvironmentRadiance) == false ||
        math::IsFinite(description.EnvironmentGroundScale) == false || description.EnvironmentGroundScale < 0 ||
        description.EnvironmentGroundScale > 1 || bounded(bake.Scale) == false ||
        math::IsFinite(bake.UvTransform) == false || math::IsFinite(bake.Transform) == false)
    {
        return Status::InvalidDescription;
    }
    if (math::IsFinite(description.NormalScale) == false || description.NormalScale < 0 || description.NormalScale > 8)
    {
        return Status::InvalidDescription;
    }
    if (static_cast<uint8>(description.DebugView) > static_cast<uint8>(MaterialDebugView::LightCount))
    {
        return Status::InvalidDescription;
    }
    if (description.MaterialUvSet > 1 ||
        (description.MaterialUvSet != 0 && description.AreAuthoredInputsEnabled == false) ||
        (description.AreAuthoredInputsEnabled == true && p->HasAuthoredProfile == false) ||
        (description.EnvironmentOrNull != nullptr && p->HasAuthoredProfile == false))
    {
        return Status::Unsupported;
    }
    if (description.EnvironmentOrNull != nullptr)
    {
        const internal::EnvironmentData* environmentOrNull = Access::GetEnvironment(description.EnvironmentOrNull);
        if (environmentOrNull == nullptr ||
            rhi::GetStatus(s.Device, environmentOrNull->Texture) == Status::InvalidHandle)
        {
            return Status::InvalidDescription;
        }
        const Status ready = description.EnvironmentOrNull->GetStatus();
        if (ready != Status::Ready)
        {
            return ready == Status::Pending ? Status::NotReady : ready;
        }
    }
    if (rhi::IsNull(description.EmissionMap) == false)
    {
        if (p->HasAuthoredProfile == false)
        {
            return Status::Unsupported;
        }
        rhi::TextureDescription texture;
        const Status ready = rhi::GetTextureDescription(s.Device, description.EmissionMap, texture);
        if (ready != Status::Ready)
        {
            return ready;
        }
        if (texture.Format != rhi::RasterFormat::Rgba8Unorm && texture.Format != rhi::RasterFormat::Rgba8Srgb)
        {
            return Status::Unsupported;
        }
        if (texture.Attachment == true || texture.Dimension != rhi::TextureDimension::D2)
        {
            return Status::InvalidDescription;
        }
        const Status upload = rhi::GetStatus(s.Device, description.EmissionMap);
        if (upload != Status::Ready)
        {
            return upload == Status::Pending ? Status::NotReady : upload;
        }
    }
    if (rhi::IsNull(description.SurfaceMap) == false)
    {
        if (p->HasSurfaceMapProfile == false)
        {
            return Status::Unsupported;
        }
        rhi::TextureDescription texture;
        Status ready = rhi::GetTextureDescription(s.Device, description.SurfaceMap, texture);
        if (ready != Status::Ready)
        {
            return ready;
        }
        if (texture.Format != rhi::RasterFormat::Rgba8Unorm || texture.Attachment == true ||
            (description.Surface.Lod != -1 &&
             (description.Surface.Lod < 0 || description.Surface.Lod > static_cast<float32>(texture.MipLevels - 1))))
        {
            return Status::InvalidDescription;
        }
        ready = rhi::GetStatus(s.Device, description.SurfaceMap);
        if (ready != Status::Ready)
        {
            return ready == Status::Pending ? Status::NotReady : ready;
        }
    }
    const bool clustered = description.Clusters != nullptr;
    if (p->Clustered != clustered)
    {
        return Status::InvalidDescription;
    }
    if (clustered == true && s.ClusterScratch.Get() == nullptr)
    {
        if (sizeof(LightClusters) > s.Capacities.HostBytes - s.HostBytes)
        {
            return Status::CapacityExceeded;
        }
        LightClusters* workspaceOrNull = new (std::nothrow) LightClusters;
        if (workspaceOrNull == nullptr)
        {
            return Status::OutOfMemory;
        }
        s.ClusterScratch.Reset(workspaceOrNull);
        s.HostBytes += sizeof(LightClusters);
    }
    auto* clusters = s.ClusterScratch.Get();
    if (s.LitScratch.Get() == nullptr)
    {
        if (sizeof(LitMaterialScratch) > s.Capacities.HostBytes - s.HostBytes)
        {
            return Status::CapacityExceeded;
        }
        LitMaterialScratch* workspaceOrNull = new (std::nothrow) LitMaterialScratch;
        if (workspaceOrNull == nullptr)
        {
            return Status::OutOfMemory;
        }
        s.LitScratch.Reset(workspaceOrNull);
        s.HostBytes += sizeof(LitMaterialScratch);
    }
    LightSelection& selected = s.LitScratch->Selection;
    selected.Count = 0;
    selected.Dropped = 0;
    selected.OverflowCells = 0;
    selected.Assignments = 0;
    auto status = clustered
                      ? BuildLightClusters(*description.Clusters, description.Lights, description.LightCount, *clusters)
                      : SelectDirectLights(description.Lights, description.LightCount, selected);
    if (clustered && status == Status::Ready)
    {
        selected.Count = clusters->Count;
        selected.OverflowCells = clusters->OverflowCells;
        selected.Assignments = clusters->Assignments;
        for (usize i = 0; i < clusters->Count; ++i)
        {
            selected.Lights[i] = clusters->Lights[i];
        }
    }
    if (status != Status::Ready)
    {
        return status;
    }
    if (rhi::IsNull(bake.Texture) == false)
    {
        rhi::TextureDescription texture;
        status = rhi::GetTextureDescription(s.Device, bake.Texture, texture);
        if (status != Status::Ready)
        {
            return status;
        }
        const float32 texelX = 1 / static_cast<float32>(texture.Width),
                      texelY = 1 / static_cast<float32>(texture.Height);
        if (bake.SourceId == 0 || bake.SceneRevision == 0 || bake.DependencyRevision == 0 ||
            texture.Format != rhi::RasterFormat::Rgba8Unorm || texture.MipLevels != 1 || texture.Attachment ||
            texture.Width < 4 || texture.Height < 4 ||
            (bake.AreTriangleChartsEnabled == true &&
             (bake.UvTransform != math::Vector4{1, 1, 0, 0} ||
              texture.Width > content::scene_format::MAX_BAKE_CHART_DIMENSION ||
              texture.Height > content::scene_format::MAX_BAKE_CHART_DIMENSION ||
              (texture.Width & (texture.Width - 1)) != 0 || (texture.Height & (texture.Height - 1)) != 0)) ||
            (bake.AreTriangleChartsEnabled == false &&
             (bake.UvTransform.X < texelX || bake.UvTransform.Y < texelY ||
              bake.UvTransform.Z + 1e-6F < 1.5F * texelX || bake.UvTransform.W + 1e-6F < 1.5F * texelY ||
              bake.UvTransform.X + bake.UvTransform.Z > 1 - 1.5F * texelX + 1e-6F ||
              bake.UvTransform.Y + bake.UvTransform.W > 1 - 1.5F * texelY + 1e-6F)))
        {
            return Status::InvalidDescription;
        }
        status = rhi::GetStatus(s.Device, bake.Texture);
        if (status != Status::Ready)
        {
            return status == Status::Pending ? Status::NotReady : status;
        }
    }
    State::ShadowRecord* atlas = nullptr;
    if (Access::Owner(description.Shadows) != 0)
    {
        atlas = Resolve(s.Shadows, s.Owner, description.Shadows);
        if (atlas == nullptr)
        {
            return Status::InvalidHandle;
        }
        status = GetStatus(description.Shadows);
        if (status != Status::Ready)
        {
            return status == Status::Pending ? Status::NotReady : status;
        }
        if (atlas->Rendered == false)
        {
            return Status::NotReady;
        }
        for (usize region = 0; region < atlas->Count; ++region)
        {
            const auto& expected = atlas->Regions[region].Light;
            bool match = false;
            for (usize light = 0; light < selected.Count; ++light)
            {
                const auto& actual = selected.Lights[light];
                match |= actual.SourceId == expected.SourceId && actual.Kind == expected.Kind &&
                         actual.Direction == expected.Direction && actual.Position == expected.Position &&
                         actual.Range == expected.Range && actual.InnerCos == expected.InnerCos &&
                         actual.OuterCos == expected.OuterCos;
            }
            if (match == false)
            {
                return Status::InvalidDescription;
            }
        }
    }
    // Explicit vec4 packing is independently verified for every cooked target.
    LitMaterialParameters& parameters = s.LitScratch->Parameters;
    // Every member defaults to zero and the size assertion above rules out padding,
    // so value-initialization yields the same bytes as clearing the storage.
    parameters = LitMaterialParameters{};
    parameters.Tint = description.Surface.Tint;
    parameters.UvTransform = description.Surface.UvTransform;
    // Options.W is reserved by ordinary lit programs; debug-view programs read the mode from it.
    parameters.Options = {description.Surface.AlphaCutoff,
                          description.Surface.Lod,
                          static_cast<float32>(description.Surface.Alpha),
                          static_cast<float32>(description.DebugView)};
    parameters.Pbr = {description.Metallic,
                      description.Roughness,
                      static_cast<float32>(selected.Count),
                      atlas != nullptr ? static_cast<float32>(atlas->Count) : 0};
    parameters.Emission = {description.Emission.X,
                           description.Emission.Y,
                           description.Emission.Z,
                           description.IsEmissionTwoSided == true ? 1.F : 0.F};
    parameters.EnvironmentIrradiance = {description.EnvironmentIrradiance.X,
                                        description.EnvironmentIrradiance.Y,
                                        description.EnvironmentIrradiance.Z,
                                        description.EnvironmentGroundScale};
    parameters.EnvironmentRadiance = {description.EnvironmentRadiance.X,
                                      description.EnvironmentRadiance.Y,
                                      description.EnvironmentRadiance.Z,
                                      0};
    parameters.BakeScale = {bake.Scale.X, bake.Scale.Y, bake.Scale.Z, 0};
    parameters.BakeUv = bake.UvTransform;
    parameters.SurfaceMap = {description.NormalScale,
                             rhi::IsNull(description.SurfaceMap) ? 0.0F : 1.0F,
                             rhi::IsNull(description.EmissionMap) ? 0.0F : 1.0F,
                             0};
    parameters.Authored = {description.AreAuthoredInputsEnabled ? 1.0F : 0.0F,
                           static_cast<float32>(description.MaterialUvSet),
                           description.EnvironmentOrNull != nullptr ? 1.0F : 0.0F,
                           0};
    if (description.EnvironmentOrNull != nullptr)
    {
        const internal::EnvironmentData& environment = *Access::GetEnvironment(description.EnvironmentOrNull);
        parameters.Authored.W = static_cast<float32>(environment.MipLevels - 1);
        for (usize coefficient = 0; coefficient < internal::ENVIRONMENT_SH_COUNT; ++coefficient)
        {
            parameters.IrradianceSh[coefficient] = environment.Irradiance[coefficient];
        }
    }
    for (usize i = 0; i < selected.Count; ++i)
    {
        const auto& light = selected.Lights[i];
        parameters.Lights[i * 4] = {light.Color.X * light.Intensity,
                                    light.Color.Y * light.Intensity,
                                    light.Color.Z * light.Intensity,
                                    static_cast<float32>(light.Kind)};
        parameters.Lights[i * 4 + 1] = {light.Position.X, light.Position.Y, light.Position.Z, light.Range};
        parameters.Lights[i * 4 + 2] = {light.Direction.X, light.Direction.Y, light.Direction.Z, light.OuterCos};
        const bool shouldReplaceDiffuse = light.SourceId == bake.DirectLightId ||
                                          (bake.HasStaticAreaDirect == true && (light.Kind == LightKind::AreaOneSided ||
                                                                                light.Kind == LightKind::AreaTwoSided));
        parameters.Lights[i * 4 + 3] = {light.InnerCos, shouldReplaceDiffuse == true ? 1.0F : 0.0F, 0, 0};
    }
    if (atlas != nullptr)
    {
        for (usize i = 0; i < atlas->Count; ++i)
        {
            const auto& region = atlas->Regions[i];
            const auto rows = math::Transpose(region.WorldToClip);
            for (usize row = 0; row < 4; ++row)
            {
                parameters.ShadowData[i * SHADOW_REGION_WORDS + row] = rows.Columns[row];
            }
            const auto w = static_cast<float32>(atlas->Width), h = static_cast<float32>(atlas->Height);
            const auto& rect = region.Interior;
            parameters.ShadowData[i * SHADOW_REGION_WORDS + 4] = {
                (static_cast<float32>(rect.X) + 0.5F) / w,
                (static_cast<float32>(rect.Y) + 0.5F) / h,
                (static_cast<float32>(rect.X + rect.Width) - 0.5F) / w,
                (static_cast<float32>(rect.Y + rect.Height) - 0.5F) / h};
            usize index = 0;
            while (selected.Lights[index].SourceId != region.Light.SourceId)
            {
                ++index;
            }
            parameters.ShadowData[i * SHADOW_REGION_WORDS + 5] = {region.Bias,
                                                                  static_cast<float32>(index),
                                                                  1 / w,
                                                                  1 / h};
        }
    }
    uint8* legacy = s.LitScratch->Legacy.GetData();
    std::memset(legacy, 0, s.LitScratch->Legacy.GetSize());
    if (clustered)
    {
        const auto& grid = *description.Clusters;
        const auto rows = math::Transpose(Matrix(grid.WorldToGrid));
        for (usize i = 0; i < 3; ++i)
        {
            parameters.GridRows[i] = rows.Columns[i];
        }
        parameters.GridMinimum = {grid.Bounds.Min.X, grid.Bounds.Min.Y, grid.Bounds.Min.Z, 0};
        const auto extent = grid.Bounds.Max - grid.Bounds.Min;
        parameters.GridScale = {8 / extent.X, 8 / extent.Y, 8 / extent.Z, 0};
    }
    else
    {
        std::memcpy(legacy, &parameters, 656);
        std::memcpy(legacy + LIT_SHADOW_OFFSET, parameters.ShadowData.GetData(), sizeof(parameters.ShadowData));
        std::memcpy(legacy + LIT_BYTES, &parameters.SurfaceMap, sizeof(math::Vector4) + AUTHORED_PARAMETER_BYTES);
    }
    status = CreateMaterialData(program,
                                description.Surface,
                                clustered ? reinterpret_cast<const uint8*>(&parameters) : legacy,
                                (clustered ? CLUSTER_BYTES : LIT_BYTES) + (p->HasSurfaceMapProfile ? 16U : 0U) +
                                    (p->HasAuthoredProfile ? AUTHORED_PARAMETER_BYTES : 0U),
                                &description,
                                output,
                                clustered ? clusters : nullptr);
    if (Accepted(status))
    {
        report = selected;
    }
    return status;
}
Status
Renderer::PrepareShadowAtlas(Snapshot handle, const ShadowAtlasDescription& description, ShadowAtlas& output) noexcept
{
    if (mState.Get() == nullptr || mState->LightingSetup != Status::Ready)
    {
        return Status::NotReady;
    }
    auto& s = *mState;
    if (Access::Owner(output) != 0)
    {
        return Status::InvalidState;
    }
    const auto* snapshot = Resolve(s.SnapshotRecords, s.Owner, handle);
    if (snapshot == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (description.Count == 0 || description.Count > SHADOW_REGION_CAPACITY || description.Regions == nullptr ||
        description.Width < 4 || description.Height < 4 || description.Width > 2048 || description.Height > 2048)
    {
        return Status::InvalidDescription;
    }
    if (s.ShadowScratch.Get() == nullptr)
    {
        if (sizeof(ShadowRegionScratch) > s.Capacities.HostBytes - s.HostBytes)
        {
            return Status::CapacityExceeded;
        }
        ShadowRegionScratch* workspaceOrNull = new (std::nothrow) ShadowRegionScratch;
        if (workspaceOrNull == nullptr)
        {
            return Status::OutOfMemory;
        }
        s.ShadowScratch.Reset(workspaceOrNull);
        s.HostBytes += sizeof(ShadowRegionScratch);
    }
    ShadowRegionScratch& regions = *s.ShadowScratch;
    for (usize i = 0; i < description.Count; ++i)
    {
        regions[i] = description.Regions[i];
        auto& region = regions[i];
        LightSelection light;
        const auto status = SelectDirectLights(&region.Light, 1, light);
        if (status != Status::Ready)
        {
            return status;
        }
        region.Light = light.Lights[0];
        const bool hasCubeProjection = region.Light.Kind == LightKind::Point ||
                                       region.Light.Kind == LightKind::AreaOneSided ||
                                       region.Light.Kind == LightKind::AreaTwoSided;
        if (hasCubeProjection == true)
        {
            math::Matrix4 expectedWorldToClip;
            if (BuildPointShadowMatrix(region.Light.Position,
                                       region.CubeFace,
                                       region.NearDistance,
                                       region.Light.Range,
                                       expectedWorldToClip) != Status::Ready ||
                region.WorldToClip != expectedWorldToClip)
            {
                return Status::InvalidDescription;
            }
        }
        else if (region.CubeFace != ~uint32{0})
        {
            return Status::InvalidDescription;
        }
        const auto& r = region.Interior;
        if (math::IsFinite(region.WorldToClip) == false || math::IsFinite(region.Bias) == false || region.Bias < 0 ||
            region.Bias > 0.01F || r.X < 1 || r.Y < 1 || r.Width < 2 || r.Height < 2 || r.X >= description.Width - 1 ||
            r.Y >= description.Height - 1 || r.Width > description.Width - 1 - r.X ||
            r.Height > description.Height - 1 - r.Y)
        {
            return Status::InvalidDescription;
        }
        for (usize j = 0; j < i; ++j)
        {
            const auto& q = regions[j].Interior;
            if (r.X <= q.X + q.Width + 1 && q.X <= r.X + r.Width + 1 && r.Y <= q.Y + q.Height + 1 &&
                q.Y <= r.Y + r.Height + 1)
            {
                return Status::InvalidDescription;
            }
        }
    }
    for (usize i = 0; i < description.Count; ++i)
    {
        const ShadowRegion& region = regions[i];
        if (region.CubeFace == ~uint32{0})
        {
            continue;
        }
        uint32 faces = 0;
        for (usize j = 0; j < description.Count; ++j)
        {
            const ShadowRegion& candidate = regions[j];
            if (candidate.Light.SourceId != region.Light.SourceId)
            {
                continue;
            }
            if (candidate.CubeFace >= POINT_SHADOW_FACES || (faces & (uint32{1} << candidate.CubeFace)) != 0 ||
                candidate.Light.Kind != region.Light.Kind || candidate.Light.Position != region.Light.Position ||
                candidate.Light.Direction != region.Light.Direction || candidate.Light.Color != region.Light.Color ||
                candidate.Light.Intensity != region.Light.Intensity || candidate.Light.Range != region.Light.Range ||
                candidate.NearDistance != region.NearDistance)
            {
                return Status::InvalidDescription;
            }
            faces |= uint32{1} << candidate.CubeFace;
        }
        if (faces != (uint32{1} << POINT_SHADOW_FACES) - 1)
        {
            return Status::InvalidDescription;
        }
    }
    usize freeViews = 0;
    for (const State::ViewRecord& view : s.ViewRecords)
    {
        freeViews += view.Used == false ? 1U : 0U;
    }
    if (description.Count > freeViews)
    {
        return Status::CapacityExceeded;
    }
    for (usize i = 0; i < snapshot->Count; ++i)
    {
        const auto& item = snapshot->Items[i];
        if (item.Input.Pass == Layer::Opaque && item.MaterialSlot != NO_MATERIAL &&
            s.Materials[item.MaterialSlot].Alpha == MaterialAlpha::Masked &&
            s.Materials[item.MaterialSlot].Lit == false)
        {
            return Status::Unsupported;
        }
    }
    usize slot = 0;
    auto status = FreeSlot(s.Shadows, slot);
    if (status != Status::Ready)
    {
        return status;
    }
    auto& atlas = s.Shadows[slot];
    atlas.Used = atlas.Owned = true;
    atlas.SnapshotSlot = Access::Slot(handle);
    ++s.SnapshotRecords[atlas.SnapshotSlot].References;
    atlas.Width = description.Width;
    atlas.Height = description.Height;
    status = rhi::CreateTexture(s.Device,
                                {description.Width, description.Height, rhi::RasterFormat::Rgba8Unorm, true},
                                {},
                                atlas.Texture);
    for (usize i = 0; i < description.Count && Accepted(status); ++i)
    {
        ViewDescription view;
        view.WorldToClip = regions[i].WorldToClip;
        view.Reference = false;
        view.ShadowCasters = true;
        ViewReport report;
        const auto ready = PrepareView(handle, view, atlas.Views[i], report);
        if (Accepted(ready))
        {
            atlas.Regions[i] = regions[i];
            ++atlas.Count;
        }
        if (Accepted(ready) == false || ready == Status::Pending)
        {
            status = ready;
        }
    }
    if (Accepted(status) == false)
    {
        atlas.Owned = false;
        s.DropShadow(slot);
        return status;
    }
    const auto handleOut = Access::Make<ShadowAtlas>({s.Owner, atlas.Generation, slot});
    status = GetStatus(handleOut);
    if (Accepted(status) == false)
    {
        atlas.Owned = false;
        s.DropShadow(slot);
        return status;
    }
    output = handleOut;
    return status;
}
Status Renderer::GetStatus(ShadowAtlas handle) const noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto& s = *mState;
    const auto* atlas = Resolve(s.Shadows, s.Owner, handle);
    if (atlas == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (atlas->Failed)
    {
        return Status::Failed;
    }
    auto status = rhi::GetStatus(s.Device, atlas->Texture);
    for (usize i = 0; i < atlas->Count && status == Status::Ready; ++i)
    {
        status = GetStatus(atlas->Views[i]);
    }
    return status;
}
Status Renderer::DrawShadowAtlas(ShadowAtlas handle, uint32 reservedDraws) noexcept
{
    auto status = GetStatus(handle);
    if (status != Status::Ready)
    {
        return status;
    }
    auto& s = *mState;
    auto& atlas = s.Shadows[Access::Slot(handle)];
    if (atlas.Rendered || atlas.Failed)
    {
        return Status::InvalidState;
    }
    if (rhi::GetFrameInfo().Width == 0)
    {
        return Status::InvalidState;
    }
    rhi::DeviceInfo deviceInfo;
    if (rhi::GetDeviceInfo(s.Device, deviceInfo) != rhi::DeviceStatus::Ready)
    {
        return Status::NotReady;
    }
    usize requiredDraws = reservedDraws;
    for (usize i = 0; i < atlas.Count; ++i)
    {
        requiredDraws += s.ViewRecords[Access::Slot(atlas.Views[i])].DrawCount;
    }
    if (requiredDraws > deviceInfo.Profile.Raster.DrawsPerFrame)
    {
        return Status::CapacityExceeded;
    }
    atlas.Failed = true;
    for (usize region = 0; region < atlas.Count; ++region)
    {
        rhi::RasterPassDescription pass;
        pass.DepthStore = rhi::RasterStore::Store;
        pass.Color = atlas.Texture;
        for (auto& channel : pass.Clear)
        {
            channel = 1;
        }
        pass.ClearDepth = 1;
        pass.ColorLoad = region == 0 ? rhi::RasterLoad::Clear : rhi::RasterLoad::Load;
        pass.DepthLoad = region == 0 ? rhi::RasterLoad::Clear : rhi::RasterLoad::Load;
        pass.UseViewport = pass.UseScissor = true;
        pass.Viewport = pass.Scissor = atlas.Regions[region].Interior;
        status = rhi::BeginRasterPass(s.Device, pass);
        if (status != Status::Ready)
        {
            return status;
        }
        const auto& view = s.ViewRecords[Access::Slot(atlas.Views[region])];
        for (usize i = 0; i < view.DrawCount; ++i)
        {
            const auto& packet = view.Packets[i];
            const auto& mesh = s.MeshRecords[packet.MeshSlot];
            auto pipeline = s.FlatShadow;
            auto bindings = s.Bindings;
            if (packet.MaterialSlot != NO_MATERIAL && s.Materials[packet.MaterialSlot].Alpha == MaterialAlpha::Masked)
            {
                const auto& material = s.Materials[packet.MaterialSlot];
                pipeline = s.Programs[material.ProgramSlot].ShadowPipeline;
                bindings = material.Bindings;
            }
            const rhi::RasterVertexSlice slices[]{
                {mesh.Vertices, 0, mesh.VertexCount * sizeof(Vertex)},
                {view.Instances, packet.First * sizeof(Instance), packet.Count * sizeof(Instance)}};
            status = rhi::DrawIndexed(
                s.Device,
                {pipeline, bindings, slices, mesh.Indices, 0, mesh.IndexCount, mesh.VertexCount, packet.Count});
            if (status != Status::Ready)
            {
                return status;
            }
        }
    }
    atlas.Failed = false;
    atlas.Rendered = true;
    return Status::Ready;
}
Status Renderer::Release(ShadowAtlas& handle) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto* atlas = Resolve(mState->Shadows, mState->Owner, handle);
    if (atlas == nullptr)
    {
        return Status::InvalidHandle;
    }
    atlas->Owned = false;
    mState->DropShadow(Access::Slot(handle));
    handle = {};
    return Status::Ready;
}
} // namespace ludus::graphics::renderer

#include "internal/gpu_preview_services.h"
#include "internal/reference_services.h"

namespace ludus::graphics::renderer
{
Status Renderer::prepareGpuPreviewCamera(GpuPreview handle, const ViewDescription& camera, uint64 revision) noexcept
{
    internal::GpuPreviewRecord* jobOrNull =
        Resolve(mState->GpuPreview->Jobs, internal::GPU_PREVIEW_JOB_COUNT, mState->Owner, handle);
    if (jobOrNull == nullptr || jobOrNull->Setup != Status::Ready || jobOrNull->Info.Pending == true ||
        jobOrNull->HasPendingCamera == true || jobOrNull->ComputeBindingsCreated == false)
    {
        return Status::NotReady;
    }
    internal::GpuPreviewRecord& job = *jobOrNull;
    const usize pixels = usize(job.Width) * job.Height;
    StaticArray<rhi::RasterBindingResource, internal::GPU_PREVIEW_COMPUTE_BINDING_CAPACITY> bindings{
        {{0, job.Parameters, 0, sizeof(internal::GpuPreviewUniform)},
         {1, job.Data, 0, job.DataBytes},
         {2, job.Accumulation, 0, pixels * LUDUS_GPU_PREVIEW_ACCUMULATION_BYTES},
         {LUDUS_GPU_PREVIEW_COLORS_BINDING, job.Colors, 0, pixels * LUDUS_GPU_PREVIEW_VECTOR_BYTES},
         {LUDUS_GPU_PREVIEW_TEXELS_BINDING, job.Texels, 0, job.TexelBytes},
         { .Binding = LUDUS_GPU_PREVIEW_RAY_SCENE_BINDING, .Scene = job.NativeScene }}};
    const usize baseBindingCount =
        LUDUS_GPU_PREVIEW_BINDING_COUNT - (mState->GpuPreview->HasNativeRayQueries == true ? 0 : 1);
    for (usize i = 0; i < job.Storage.GetCount(); ++i)
    {
        bindings[baseBindingCount + i] = job.Storage.GetBinding(i);
    }
    for (usize i = 0; mState->GpuPreview->HasPersistentBuffers == true && i < internal::GPU_GRAPH_BUFFER_COUNT; ++i)
    {
        const GpuPreviewGraphBuffer& buffer = job.GraphBuffers[i];
        bindings[baseBindingCount + i] = {static_cast<uint32>(LUDUS_GRAPH_BUFFER0_BINDING + i),
                                          buffer.Buffer,
                                          0,
                                          buffer.Size};
    }
    if (mState->GpuPreview->HasVisibilityBuffer == true)
    {
        const usize firstTexture = mState->GpuPreview->ComputeBindingCount - LUDUS_GRAPH_VISIBILITY_TEXTURE_COUNT -
                                   (mState->GpuPreview->HasRayPathCapture == true ? 1 : 0) -
                                   (mState->GpuPreview->HasLtcTable == true ? LUDUS_GRAPH_LTC_TABLE_BINDING_COUNT : 0);
        bindings[firstTexture] = { .Binding = LUDUS_GRAPH_VISIBILITY_BINDING, .Texture = job.VisibilityView };
        bindings[firstTexture + 1] =
        {
            .Binding = LUDUS_GRAPH_VISIBILITY_DEPTH_BINDING,
            .Texture = job.VisibilityDepthView,
        };
    }
    if (mState->GpuPreview->HasRayPathCapture == true)
    {
        const usize capture = mState->GpuPreview->ComputeBindingCount - 1 -
                              (mState->GpuPreview->HasLtcTable == true ? LUDUS_GRAPH_LTC_TABLE_BINDING_COUNT : 0);
        bindings[capture] = {LUDUS_RAY_PATH_CAPTURE_BINDING, job.RayPathCapture.Buffer, 0, job.RayPathCapture.Size};
    }
    if (mState->GpuPreview->HasLtcTable == true)
    {
        bindings[mState->GpuPreview->ComputeBindingCount - 1] = {LUDUS_GRAPH_LTC_TABLE_BINDING,
                                                                 mState->GpuPreview->LtcTable,
                                                                 0,
                                                                 sizeof(internal::LTC_TABLE)};
    }
    if (sizeof(internal::GpuPreviewUniform) > mState->GpuPreview->Options.GpuByteBudget - mState->GpuPreview->GpuBytes)
    {
        return Status::CapacityExceeded;
    }
    const Status status = job.CameraUpdate.Prepare(mState->Device,
                                                   job.Uniform,
                                                   camera,
                                                   revision,
                                                   mState->GpuPreview->ComputeLayout,
                                                   {bindings.GetData(), mState->GpuPreview->ComputeBindingCount});
    job.HasPendingCamera = status == Status::Ready || status == Status::Pending;
    if (job.HasPendingCamera == true)
    {
        job.GpuBytes += sizeof(internal::GpuPreviewUniform);
        mState->GpuPreview->GpuBytes += sizeof(internal::GpuPreviewUniform);
    }
    else
    {
        job.CameraUpdate.Reset();
    }
    return status;
}
Status Renderer::pollGpuPreviewCamera(GpuPreview handle) noexcept
{
    return mState->GpuPreview->Jobs[Access::Slot(handle)].CameraUpdate.Poll();
}
void Renderer::commitGpuPreviewCamera(GpuPreview handle) noexcept
{
    internal::GpuPreviewRecord& job = mState->GpuPreview->Jobs[Access::Slot(handle)];
    static_cast<void>(rhi::Destroy(mState->Device, job.ComputeBindings));
    static_cast<void>(rhi::Destroy(mState->Device, job.Parameters));
    job.CameraUpdate.Commit(job.Uniform, job.Parameters, job.ComputeBindings);
    job.HasPendingCamera = false;
    job.GpuBytes -= sizeof(internal::GpuPreviewUniform);
    mState->GpuPreview->GpuBytes -= sizeof(internal::GpuPreviewUniform);
    job.Info.CompletedSamples = 0;
    job.Info.Complete = false;
}
void Renderer::discardGpuPreviewCamera(GpuPreview handle) noexcept
{
    internal::GpuPreviewRecord& job = mState->GpuPreview->Jobs[Access::Slot(handle)];
    if (job.HasPendingCamera == true)
    {
        job.GpuBytes -= sizeof(internal::GpuPreviewUniform);
        mState->GpuPreview->GpuBytes -= sizeof(internal::GpuPreviewUniform);
        job.HasPendingCamera = false;
    }
    job.CameraUpdate.Reset();
}
} // namespace ludus::graphics::renderer

#if defined(LUDUS_RENDERER_TEST_READBACK)
namespace ludus::graphics::renderer::internal
{
rhi::RasterStatus RenderGraphRayPass::GetAccumulationForTesting(Renderer& inoutRenderer,
                                                                GpuPreview preview,
                                                                rhi::BufferHandle& outBuffer) noexcept
{
    if (inoutRenderer.mState.Get() == nullptr || inoutRenderer.mState->GpuPreview.Get() == nullptr)
    {
        return rhi::RasterStatus::InvalidState;
    }
    GpuPreviewRecord* recordOrNull =
        Resolve(inoutRenderer.mState->GpuPreview->Jobs, GPU_PREVIEW_JOB_COUNT, inoutRenderer.mState->Owner, preview);
    if (recordOrNull == nullptr)
    {
        return rhi::RasterStatus::InvalidHandle;
    }
    if (recordOrNull->Info.Pending == true)
    {
        return rhi::RasterStatus::NotReady;
    }
    outBuffer = recordOrNull->Accumulation;
    return rhi::RasterStatus::Ready;
}
} // namespace ludus::graphics::renderer::internal
#endif
