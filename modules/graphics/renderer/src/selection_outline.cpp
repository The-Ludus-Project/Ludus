#include <ludus/graphics/renderer/selection_outline.hpp>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <atomic>
#include <new>

namespace ludus::graphics::renderer
{
using namespace foundation;
namespace math = foundation::math;
using Status = rhi::RasterStatus;
namespace
{
/// The mask shader reads a position and four matrix rows; the edge shader reads four Float4 fields per vertex.
constexpr usize MASK_VERTEX_INPUT_COUNT = 5;
constexpr usize EDGE_VERTEX_INPUT_COUNT = 4;
constexpr usize EDGE_FRAGMENT_BINDING_COUNT = 2;
constexpr usize MATRIX_ROWS = 4;
constexpr usize INDICES_PER_TRIANGLE = 3;
constexpr usize ALPHA_CHANNEL = 3;
constexpr usize LAST_MATRIX_COLUMN = 3;
constexpr usize MAXIMUM_VERSIONS = 4;
constexpr usize QUAD_VERTEX_COUNT = 4;
constexpr usize QUAD_INDEX_COUNT = 6;
constexpr usize BOX_CORNER_COUNT = 8;
/// Pipelines: the mask, and the edge pass for the acquired surface and for an RGBA8 attachment.
constexpr usize PIPELINE_COUNT = 3;
constexpr usize MASK_PIPELINE = 0;
constexpr usize EDGE_SURFACE_PIPELINE = 1;
constexpr usize EDGE_ATTACHMENT_PIPELINE = 2;
/// Homogeneous w at or below which a bounds corner counts as behind the camera.
constexpr float32 MINIMUM_CLIP_W = 1e-6F;
/// Pixels of the edge quad beyond the stroke, so the quad's border never cuts a stroke.
constexpr float32 QUAD_MARGIN_PIXELS = 1;
/// Clip-space extent of a full axis, from -1 to 1, in units of one half.
constexpr float32 CLIP_SPAN = 2;
constexpr float32 HALF = 0.5F;
constexpr float32 MASK_CLEAR = 0;

/// One edge-pass vertex: position and mask UV, the two stroke colours and the search parameters.
struct EdgeVertex final
{
    math::Vector4 PositionUv;
    math::Vector4 OutlineColor;
    math::Vector4 CasingColor;
    math::Vector4 Parameters;
};
/// One mask-pass instance: the four mesh-to-clip rows.
struct MaskInstance final
{
    StaticArray<math::Vector4, MATRIX_ROWS> Rows;
};
constexpr usize EDGE_VERTICES_OFFSET = sizeof(MaskInstance);
constexpr usize VERSION_BYTES = sizeof(MaskInstance) + QUAD_VERTEX_COUNT * sizeof(EdgeVertex);
static_assert(sizeof(math::Vector3) == 12 && sizeof(EdgeVertex) == 64 && sizeof(MaskInstance) == 64);

bool Accepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
bool IsUnitColor(const math::Vector4& color) noexcept
{
    return math::IsFinite(color) == true && color.X >= 0 && color.X <= 1 && color.Y >= 0 && color.Y <= 1 &&
           color.Z >= 0 && color.Z <= 1 && color.W >= 0 && color.W <= 1;
}
bool IsValidStyle(const SelectionOutlineStyle& style) noexcept
{
    const float32 maximum = SelectionOutlineRenderer::MAX_STROKE_WIDTH_PIXELS;
    return IsUnitColor(style.OutlineColor) == true && IsUnitColor(style.CasingColor) == true &&
           math::IsFinite(style.OutlineWidthPixels) == true && math::IsFinite(style.CasingWidthPixels) == true &&
           style.OutlineWidthPixels > 0 && style.OutlineWidthPixels <= maximum &&
           style.CasingWidthPixels >= style.OutlineWidthPixels && style.CasingWidthPixels <= maximum;
}
bool IsFiniteMatrix(const math::Matrix4& matrix) noexcept
{
    for (usize row = 0; row < MATRIX_ROWS; ++row)
    {
        for (usize column = 0; column < MATRIX_ROWS; ++column)
        {
            if (math::IsFinite(matrix.At(row, column)) == false)
            {
                return false;
            }
        }
    }
    return true;
}
std::atomic<uint64> gSelectionOutlineOwner{1};
uint64 NextOwner() noexcept
{
    uint64 value = gSelectionOutlineOwner.load(std::memory_order_relaxed);
    while (value != ~uint64{0})
    {
        if (gSelectionOutlineOwner.compare_exchange_weak(value, value + 1, std::memory_order_relaxed) == true)
        {
            return value;
        }
    }
    return 0;
}
} // namespace

namespace internal
{
struct SelectionOutlineAccess final
{
    struct Identity final
    {
        uint64 Owner;
        uint64 Generation;
        uint32 Slot;
    };
    static PreparedSelectionOutline Make(const Identity& identity) noexcept
    {
        PreparedSelectionOutline value;
        value.mOwner = identity.Owner;
        value.mGeneration = identity.Generation;
        value.mSlot = identity.Slot;
        return value;
    }
    static uint64 GetOwner(PreparedSelectionOutline value) noexcept
    {
        return value.mOwner;
    }
    static uint64 GetGeneration(PreparedSelectionOutline value) noexcept
    {
        return value.mGeneration;
    }
    static uint32 GetSlot(PreparedSelectionOutline value) noexcept
    {
        return value.mSlot;
    }
};
} // namespace internal

struct SelectionOutlineRenderer::State final
{
    struct Version final
    {
        uint64 Generation = 1;
        bool IsUsed = false;
        /// False when the mesh's bounds miss the target, so Draw records nothing.
        bool IsVisible = true;
        rhi::BufferHandle Instance;
        Status Setup = Status::Pending;
        uint32 Width = 0;
        uint32 Height = 0;
    };
    uint64 Identity = 0;
    rhi::DeviceHandle Device;
    rhi::RasterShaderHandle MaskVertex;
    rhi::RasterShaderHandle MaskFragment;
    rhi::RasterShaderHandle EdgeVertex;
    rhi::RasterShaderHandle EdgeFragment;
    rhi::BindingLayoutHandle EmptyLayout;
    rhi::BindingSetHandle EmptyBindings;
    rhi::BindingLayoutHandle EdgeLayout;
    rhi::SamplerHandle Sampler;
    rhi::BufferHandle QuadIndices;
    StaticArray<rhi::RasterPipelineHandle, PIPELINE_COUNT> Pipelines;
    rhi::BufferHandle MeshVertices;
    rhi::BufferHandle MeshIndices;
    uint32 MeshVertexCount = 0;
    uint32 MeshIndexCount = 0;
    math::Aabb3 MeshBoundsMesh;
    bool HasMesh = false;
    rhi::TextureHandle MaskTexture;
    rhi::TextureViewHandle MaskView;
    rhi::BindingSetHandle MaskBindings;
    uint32 MaskWidth = 0;
    uint32 MaskHeight = 0;
    StaticArray<Version, MAXIMUM_VERSIONS> Versions;
    Status Setup = Status::Pending;
    usize Phase = 0;

    Version* resolve(PreparedSelectionOutline value) noexcept
    {
        const uint32 index = internal::SelectionOutlineAccess::GetSlot(value);
        if (internal::SelectionOutlineAccess::GetOwner(value) != Identity || index >= MAXIMUM_VERSIONS)
        {
            return nullptr;
        }
        Version& version = Versions[index];
        return version.IsUsed == true && version.Generation == internal::SelectionOutlineAccess::GetGeneration(value)
                   ? &version
                   : nullptr;
    }
    void drop(Version& inoutVersion) noexcept
    {
        static_cast<void>(rhi::Destroy(Device, inoutVersion.Instance));
        const uint64 generation = inoutVersion.Generation;
        inoutVersion = {};
        inoutVersion.Generation = generation + 1;
    }
    void dropMask() noexcept
    {
        static_cast<void>(rhi::Destroy(Device, MaskBindings));
        static_cast<void>(rhi::Destroy(Device, MaskView));
        static_cast<void>(rhi::Destroy(Device, MaskTexture));
        MaskBindings = {};
        MaskView = {};
        MaskTexture = {};
        MaskWidth = 0;
        MaskHeight = 0;
    }
    void dropMesh() noexcept
    {
        static_cast<void>(rhi::Destroy(Device, MeshVertices));
        static_cast<void>(rhi::Destroy(Device, MeshIndices));
        MeshVertices = {};
        MeshIndices = {};
        MeshVertexCount = 0;
        MeshIndexCount = 0;
        HasMesh = false;
    }
    /// Make the mask texture, its view and its edge-pass bindings exist for width by height.
    Status ensureMask(uint32 width, uint32 height) noexcept
    {
        if (MaskWidth == width && MaskHeight == height && rhi::IsNull(MaskTexture) == false)
        {
            return Status::Ready;
        }
        dropMask();
        rhi::TextureDescription description;
        description.Width = width;
        description.Height = height;
        description.Attachment = true;
        Status status = rhi::CreateTexture(Device, description, {}, MaskTexture);
        if (Accepted(status) == false)
        {
            return status;
        }
        const Status viewStatus = rhi::CreateTextureView(Device, MaskTexture, MaskView);
        if (Accepted(viewStatus) == false)
        {
            dropMask();
            return viewStatus;
        }
        const StaticArray<rhi::RasterBindingResource, EDGE_FRAGMENT_BINDING_COUNT> resources{
            {{ .Texture = MaskView }, { .Binding = 1, .Sampler = Sampler }}};
        status = rhi::CreateBindingSet(Device, EdgeLayout, resources.AsSpan(), MaskBindings);
        if (Accepted(status) == false)
        {
            dropMask();
            return status;
        }
        MaskWidth = width;
        MaskHeight = height;
        return Status::Pending;
    }
    ~State()
    {
        for (Version& version : Versions)
        {
            if (version.IsUsed == true)
            {
                drop(version);
            }
        }
        dropMesh();
        dropMask();
        for (rhi::RasterPipelineHandle& pipeline : Pipelines)
        {
            static_cast<void>(rhi::Destroy(Device, pipeline));
        }
        static_cast<void>(rhi::Destroy(Device, QuadIndices));
        static_cast<void>(rhi::Destroy(Device, Sampler));
        static_cast<void>(rhi::Destroy(Device, EmptyBindings));
        static_cast<void>(rhi::Destroy(Device, EmptyLayout));
        static_cast<void>(rhi::Destroy(Device, EdgeLayout));
        static_cast<void>(rhi::Destroy(Device, EdgeFragment));
        static_cast<void>(rhi::Destroy(Device, EdgeVertex));
        static_cast<void>(rhi::Destroy(Device, MaskFragment));
        static_cast<void>(rhi::Destroy(Device, MaskVertex));
    }
};

SelectionOutlineRenderer::SelectionOutlineRenderer() noexcept = default;
SelectionOutlineRenderer::~SelectionOutlineRenderer() = default;

void SelectionOutlineRenderer::Reset() noexcept
{
    mState.Reset();
}

bool SelectionOutlineRenderer::IsReady() const noexcept
{
    return mState.Get() != nullptr && mState->Setup == Status::Ready;
}

Status SelectionOutlineRenderer::Initialize(rhi::DeviceHandle device,
                                            const rhi::RasterShaderDescription& maskVertex,
                                            const rhi::RasterShaderDescription& maskFragment,
                                            const rhi::RasterShaderDescription& edgeVertex,
                                            const rhi::RasterShaderDescription& edgeFragment) noexcept
{
    if (mState.Get() != nullptr)
    {
        return Status::InvalidState;
    }
    if (maskVertex.Inputs.size() != MASK_VERTEX_INPUT_COUNT || maskVertex.Bindings.empty() == false ||
        maskFragment.Inputs.empty() == false || maskFragment.Bindings.empty() == false ||
        edgeVertex.Inputs.size() != EDGE_VERTEX_INPUT_COUNT || edgeVertex.Bindings.empty() == false ||
        edgeFragment.Inputs.empty() == false || edgeFragment.Bindings.size() != EDGE_FRAGMENT_BINDING_COUNT ||
        maskVertex.Artifact.Stage != rhi::ShaderStage::Vertex ||
        maskFragment.Artifact.Stage != rhi::ShaderStage::Fragment ||
        edgeVertex.Artifact.Stage != rhi::ShaderStage::Vertex ||
        edgeFragment.Artifact.Stage != rhi::ShaderStage::Fragment)
    {
        return Status::InvalidDescription;
    }
    if (maskVertex.Inputs[0].Format != rhi::RasterVertexFormat::Float3)
    {
        return Status::InvalidDescription;
    }
    const rhi::RasterBinding expected[]{{0, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
                                        {1, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}};
    for (usize i = 0; i < EDGE_FRAGMENT_BINDING_COUNT; ++i)
    {
        const rhi::RasterBinding& binding = edgeFragment.Bindings[i];
        if (binding.Binding != expected[i].Binding || binding.Kind != expected[i].Kind ||
            binding.Visibility != expected[i].Visibility || binding.MinSize != 0)
        {
            return Status::InvalidDescription;
        }
    }
    mState.Reset(new (std::nothrow) State);
    if (mState.Get() == nullptr)
    {
        return Status::OutOfMemory;
    }
    State& state = *mState;
    state.Device = device;
    state.Identity = NextOwner();
    if (state.Identity == 0)
    {
        return state.Setup = Status::IdentityExhausted;
    }
    Status status = rhi::CreateRasterShader(device, maskVertex, state.MaskVertex);
    if (Accepted(status) == true)
    {
        status = rhi::CreateRasterShader(device, maskFragment, state.MaskFragment);
    }
    if (Accepted(status) == true)
    {
        status = rhi::CreateRasterShader(device, edgeVertex, state.EdgeVertex);
    }
    if (Accepted(status) == true)
    {
        status = rhi::CreateRasterShader(device, edgeFragment, state.EdgeFragment);
    }
    if (Accepted(status) == true)
    {
        status = rhi::CreateBindingLayout(device, {}, state.EmptyLayout);
    }
    if (Accepted(status) == true)
    {
        status = rhi::CreateBindingSet(device, state.EmptyLayout, {}, state.EmptyBindings);
    }
    if (Accepted(status) == true)
    {
        status = rhi::CreateBindingLayout(device, expected, state.EdgeLayout);
    }
    if (Accepted(status) == true)
    {
        status = rhi::CreateSampler(device, {rhi::RasterFilter::Nearest}, state.Sampler);
    }
    const StaticArray<uint32, QUAD_INDEX_COUNT> indices{{0, 1, 2, 0, 2, 3}};
    if (Accepted(status) == true)
    {
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Index32, sizeof(indices)},
                                   {reinterpret_cast<const uint8*>(indices.GetData()), sizeof(indices)},
                                   state.QuadIndices);
    }
    if (Accepted(status) == false)
    {
        return state.Setup = status;
    }
    return Poll();
}

Status SelectionOutlineRenderer::Poll() noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::NotReady;
    }
    State& state = *mState;
    const Status lifetime = rhi::PollLifetime(state.Device);
    if (lifetime != Status::Ready)
    {
        return lifetime;
    }
    if (state.Setup != Status::Pending)
    {
        return state.Setup;
    }
    const StaticArray<rhi::RasterVertexStream, 2> maskStreams{
        {{sizeof(math::Vector3), false}, {sizeof(MaskInstance), true}}};
    const StaticArray<rhi::RasterVertexAttribute, MASK_VERTEX_INPUT_COUNT> maskAttributes{
        {{0, 0, 0, rhi::RasterVertexFormat::Float3},
         {1, 1, 0, rhi::RasterVertexFormat::Float4},
         {2, 1, sizeof(math::Vector4), rhi::RasterVertexFormat::Float4},
         {3, 1, 2 * sizeof(math::Vector4), rhi::RasterVertexFormat::Float4},
         {4, 1, 3 * sizeof(math::Vector4), rhi::RasterVertexFormat::Float4}}};
    const StaticArray<rhi::RasterVertexStream, 1> edgeStreams{{{sizeof(EdgeVertex), false}}};
    const StaticArray<rhi::RasterVertexAttribute, EDGE_VERTEX_INPUT_COUNT> edgeAttributes{
        {{0, 0, 0, rhi::RasterVertexFormat::Float4},
         {1, 0, sizeof(math::Vector4), rhi::RasterVertexFormat::Float4},
         {2, 0, 2 * sizeof(math::Vector4), rhi::RasterVertexFormat::Float4},
         {3, 0, 3 * sizeof(math::Vector4), rhi::RasterVertexFormat::Float4}}};
    while (state.Phase <= PIPELINE_COUNT)
    {
        // Each step waits for the previous resource, so a pending native creation is never overtaken.
        const Status prerequisite = state.Phase == 0 ? rhi::GetStatus(state.Device, state.QuadIndices)
                                                     : rhi::GetStatus(state.Device, state.Pipelines[state.Phase - 1]);
        if (prerequisite != Status::Ready)
        {
            if (prerequisite != Status::Pending)
            {
                state.Setup = prerequisite;
            }
            return prerequisite;
        }
        if (state.Phase == 0)
        {
            const Status prerequisites[]{rhi::GetStatus(state.Device, state.MaskVertex),
                                         rhi::GetStatus(state.Device, state.MaskFragment),
                                         rhi::GetStatus(state.Device, state.EdgeVertex),
                                         rhi::GetStatus(state.Device, state.EdgeFragment),
                                         rhi::GetStatus(state.Device, state.EmptyLayout),
                                         rhi::GetStatus(state.Device, state.EmptyBindings),
                                         rhi::GetStatus(state.Device, state.EdgeLayout),
                                         rhi::GetStatus(state.Device, state.Sampler)};
            for (const Status ready : prerequisites)
            {
                if (ready != Status::Ready)
                {
                    if (ready != Status::Pending)
                    {
                        state.Setup = ready;
                    }
                    return ready;
                }
            }
        }
        if (state.Phase == PIPELINE_COUNT)
        {
            return state.Setup = Status::Ready;
        }
        rhi::RasterPipelineDescription description;
        if (state.Phase == MASK_PIPELINE)
        {
            description.Vertex = state.MaskVertex;
            description.Fragment = state.MaskFragment;
            description.Layout = state.EmptyLayout;
            description.Streams = maskStreams.AsSpan();
            description.Attributes = maskAttributes.AsSpan();
            description.Blend = false;
            description.Target = rhi::RasterTarget::Rgba8Unorm;
        }
        else
        {
            description.Vertex = state.EdgeVertex;
            description.Fragment = state.EdgeFragment;
            description.Layout = state.EdgeLayout;
            description.Streams = edgeStreams.AsSpan();
            description.Attributes = edgeAttributes.AsSpan();
            description.Blend = true;
            description.Target =
                state.Phase == EDGE_SURFACE_PIPELINE ? rhi::RasterTarget::Surface : rhi::RasterTarget::Rgba8Unorm;
        }
        const Status status = rhi::CreateRasterPipeline(state.Device, description, state.Pipelines[state.Phase]);
        if (Accepted(status) == false)
        {
            return state.Setup = status;
        }
        ++state.Phase;
        if (status == Status::Pending)
        {
            return status;
        }
    }
    return state.Setup;
}

Status SelectionOutlineRenderer::SetMesh(BorrowedView<const math::Vector3> positionsMesh,
                                         BorrowedView<const uint32> indices) noexcept
{
    if (IsReady() == false)
    {
        return Status::NotReady;
    }
    State& state = *mState;
    const usize vertexCount = positionsMesh.GetSize();
    const usize indexCount = indices.GetSize();
    if (positionsMesh.IsValid() == false || indices.IsValid() == false || vertexCount == 0 || indexCount == 0 ||
        indexCount % INDICES_PER_TRIANGLE != 0 || vertexCount > ~uint32{0} || indexCount > ~uint32{0})
    {
        return Status::InvalidDescription;
    }
    math::Aabb3 bounds{positionsMesh[0], positionsMesh[0]};
    for (usize i = 0; i < vertexCount; ++i)
    {
        const math::Vector3& position = positionsMesh[i];
        if (math::IsFinite(position) == false)
        {
            return Status::InvalidDescription;
        }
        bounds.Min = {math::Min(bounds.Min.X, position.X),
                      math::Min(bounds.Min.Y, position.Y),
                      math::Min(bounds.Min.Z, position.Z)};
        bounds.Max = {math::Max(bounds.Max.X, position.X),
                      math::Max(bounds.Max.Y, position.Y),
                      math::Max(bounds.Max.Z, position.Z)};
    }
    for (usize i = 0; i < indexCount; ++i)
    {
        if (indices[i] >= vertexCount)
        {
            return Status::InvalidDescription;
        }
    }
    rhi::BufferHandle vertices;
    rhi::BufferHandle triangles;
    const usize vertexBytes = vertexCount * sizeof(math::Vector3);
    const usize indexBytes = indexCount * sizeof(uint32);
    Status status = rhi::CreateBuffer(state.Device,
                                      {rhi::BufferRole::Vertex, vertexBytes},
                                      {reinterpret_cast<const uint8*>(positionsMesh.GetDataOrNull()), vertexBytes},
                                      vertices);
    if (Accepted(status) == true)
    {
        status = rhi::CreateBuffer(state.Device,
                                   {rhi::BufferRole::Index32, indexBytes},
                                   {reinterpret_cast<const uint8*>(indices.GetDataOrNull()), indexBytes},
                                   triangles);
    }
    if (Accepted(status) == false)
    {
        static_cast<void>(rhi::Destroy(state.Device, vertices));
        static_cast<void>(rhi::Destroy(state.Device, triangles));
        return status;
    }
    state.dropMesh();
    state.MeshVertices = vertices;
    state.MeshIndices = triangles;
    state.MeshVertexCount = static_cast<uint32>(vertexCount);
    state.MeshIndexCount = static_cast<uint32>(indexCount);
    state.MeshBoundsMesh = bounds;
    state.HasMesh = true;
    return GetMeshStatus();
}

void SelectionOutlineRenderer::ClearMesh() noexcept
{
    if (mState.Get() != nullptr)
    {
        mState->dropMesh();
    }
}

Status SelectionOutlineRenderer::GetMeshStatus() noexcept
{
    if (mState.Get() == nullptr || mState->HasMesh == false)
    {
        return Status::NotReady;
    }
    State& state = *mState;
    const Status vertices = rhi::GetStatus(state.Device, state.MeshVertices);
    if (vertices != Status::Ready)
    {
        return vertices;
    }
    return rhi::GetStatus(state.Device, state.MeshIndices);
}

Status SelectionOutlineRenderer::Prepare(const SelectionOutlineView& view,
                                         PreparedSelectionOutline& outPrepared) noexcept
{
    if (IsReady() == false || mState->HasMesh == false)
    {
        return Status::NotReady;
    }
    if (internal::SelectionOutlineAccess::GetOwner(outPrepared) != 0)
    {
        return Status::InvalidState;
    }
    if (view.Width == 0 || view.Height == 0 || IsFiniteMatrix(view.MeshToClip) == false ||
        IsValidStyle(view.Style) == false)
    {
        return Status::InvalidDescription;
    }
    State& state = *mState;
    usize index = 0;
    while (index < MAXIMUM_VERSIONS &&
           (state.Versions[index].IsUsed == true || state.Versions[index].Generation == ~uint64{0}))
    {
        ++index;
    }
    if (index == MAXIMUM_VERSIONS)
    {
        return Status::CapacityExceeded;
    }
    const Status mask = state.ensureMask(view.Width, view.Height);
    if (Accepted(mask) == false)
    {
        return mask;
    }
    // The projected bounds of the mesh; a corner behind the camera leaves the whole target to the strokes.
    const float32 width = static_cast<float32>(view.Width);
    const float32 height = static_cast<float32>(view.Height);
    float32 left = 1;
    float32 right = -1;
    float32 bottom = 1;
    float32 top = -1;
    bool isWholeTarget = false;
    for (usize corner = 0; corner < BOX_CORNER_COUNT; ++corner)
    {
        const math::Vector4 cornerMesh{(corner & 1U) != 0 ? state.MeshBoundsMesh.Max.X : state.MeshBoundsMesh.Min.X,
                                       (corner & 2U) != 0 ? state.MeshBoundsMesh.Max.Y : state.MeshBoundsMesh.Min.Y,
                                       (corner & 4U) != 0 ? state.MeshBoundsMesh.Max.Z : state.MeshBoundsMesh.Min.Z,
                                       1};
        const math::Vector4 clip = view.MeshToClip * cornerMesh;
        if (math::IsFinite(clip) == false || clip.W <= MINIMUM_CLIP_W)
        {
            isWholeTarget = true;
            break;
        }
        left = math::Min(left, clip.X / clip.W);
        right = math::Max(right, clip.X / clip.W);
        bottom = math::Min(bottom, clip.Y / clip.W);
        top = math::Max(top, clip.Y / clip.W);
    }
    const float32 marginPixels = view.Style.CasingWidthPixels * HALF + QUAD_MARGIN_PIXELS;
    if (isWholeTarget == true)
    {
        left = -1;
        right = 1;
        bottom = -1;
        top = 1;
    }
    else
    {
        left = math::Max(left - CLIP_SPAN * marginPixels / width, -1.0F);
        right = math::Min(right + CLIP_SPAN * marginPixels / width, 1.0F);
        bottom = math::Max(bottom - CLIP_SPAN * marginPixels / height, -1.0F);
        top = math::Min(top + CLIP_SPAN * marginPixels / height, 1.0F);
    }
    const bool isVisible = left < right && bottom < top;
    StaticArray<uint8, VERSION_BYTES> bytes{};
    MaskInstance instance;
    for (usize row = 0; row < MATRIX_ROWS; ++row)
    {
        instance.Rows[row] = {view.MeshToClip.At(row, 0),
                              view.MeshToClip.At(row, 1),
                              view.MeshToClip.At(row, 2),
                              view.MeshToClip.At(row, LAST_MATRIX_COLUMN)};
    }
    const math::Vector4 parameters{1 / width,
                                   1 / height,
                                   view.Style.OutlineWidthPixels * HALF,
                                   view.Style.CasingWidthPixels * HALF};
    const auto uvOf = [](float32 clipX, float32 clipY) {
        return math::Vector2{clipX * HALF + HALF, HALF - clipY * HALF};
    };
    const math::Vector2 uvLeftTop = uvOf(left, top);
    const math::Vector2 uvRightBottom = uvOf(right, bottom);
    StaticArray<EdgeVertex, QUAD_VERTEX_COUNT> quad{};
    const StaticArray<math::Vector4, QUAD_VERTEX_COUNT> corners{{{left, top, uvLeftTop.X, uvLeftTop.Y},
                                                                 {right, top, uvRightBottom.X, uvLeftTop.Y},
                                                                 {right, bottom, uvRightBottom.X, uvRightBottom.Y},
                                                                 {left, bottom, uvLeftTop.X, uvRightBottom.Y}}};
    for (usize vertex = 0; vertex < QUAD_VERTEX_COUNT; ++vertex)
    {
        quad[vertex] = {corners[vertex], view.Style.OutlineColor, view.Style.CasingColor, parameters};
    }
    for (usize i = 0; i < sizeof(MaskInstance); ++i)
    {
        bytes[i] = reinterpret_cast<const uint8*>(&instance)[i];
    }
    for (usize i = 0; i < sizeof(quad); ++i)
    {
        bytes[EDGE_VERTICES_OFFSET + i] = reinterpret_cast<const uint8*>(quad.GetData())[i];
    }
    State::Version& version = state.Versions[index];
    const Status created = rhi::CreateBuffer(state.Device,
                                             {rhi::BufferRole::Vertex, VERSION_BYTES},
                                             {bytes.GetData(), VERSION_BYTES},
                                             version.Instance);
    if (Accepted(created) == false)
    {
        return created;
    }
    version.IsUsed = true;
    version.IsVisible = isVisible;
    version.Width = view.Width;
    version.Height = view.Height;
    version.Setup = Status::Pending;
    const PreparedSelectionOutline prepared =
        internal::SelectionOutlineAccess::Make({state.Identity, version.Generation, static_cast<uint32>(index)});
    const Status status = GetStatus(prepared);
    if (Accepted(status) == false)
    {
        state.drop(version);
        return status;
    }
    outPrepared = prepared;
    return status;
}

Status SelectionOutlineRenderer::GetStatus(PreparedSelectionOutline prepared) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    State& state = *mState;
    State::Version* versionOrNull = state.resolve(prepared);
    if (versionOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (versionOrNull->Setup == Status::Ready && state.HasMesh == false)
    {
        return Status::NotReady;
    }
    if (versionOrNull->Width != state.MaskWidth || versionOrNull->Height != state.MaskHeight)
    {
        // A later version of another extent replaced the mask this one was prepared for.
        return Status::InvalidState;
    }
    const Status resources[]{rhi::GetStatus(state.Device, versionOrNull->Instance),
                             rhi::GetStatus(state.Device, state.MaskTexture),
                             rhi::GetStatus(state.Device, state.MaskView),
                             rhi::GetStatus(state.Device, state.MaskBindings),
                             GetMeshStatus()};
    for (const Status status : resources)
    {
        if (status != Status::Ready)
        {
            return status;
        }
    }
    versionOrNull->Setup = Status::Ready;
    return Status::Ready;
}

Status SelectionOutlineRenderer::Draw(PreparedSelectionOutline prepared,
                                      const rhi::RasterPassDescription& description) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    State& state = *mState;
    const State::Version* versionOrNull = state.resolve(prepared);
    if (versionOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (versionOrNull->Setup != Status::Ready)
    {
        return versionOrNull->Setup == Status::Pending ? Status::NotReady : versionOrNull->Setup;
    }
    if (state.HasMesh == false || versionOrNull->Width != state.MaskWidth || versionOrNull->Height != state.MaskHeight)
    {
        return Status::InvalidState;
    }
    if (versionOrNull->IsVisible == false)
    {
        return Status::Ready;
    }
    const bool isSurface = rhi::IsNull(description.Color);
    if (isSurface == false)
    {
        rhi::TextureDescription target;
        const Status described = rhi::GetTextureDescription(state.Device, description.Color, target);
        if (described != Status::Ready)
        {
            return described;
        }
        if (target.Attachment == false || target.Format != rhi::RasterFormat::Rgba8Unorm)
        {
            return Status::Unsupported;
        }
        if (target.Width != versionOrNull->Width || target.Height != versionOrNull->Height)
        {
            return Status::InvalidState;
        }
    }
    rhi::RasterPassDescription maskPass;
    maskPass.Color = state.MaskTexture;
    maskPass.ColorLoad = rhi::RasterLoad::Clear;
    maskPass.ColorStore = rhi::RasterStore::Store;
    maskPass.Clear[0] = MASK_CLEAR;
    maskPass.Clear[1] = MASK_CLEAR;
    maskPass.Clear[2] = MASK_CLEAR;
    maskPass.Clear[ALPHA_CHANNEL] = MASK_CLEAR;
    Status status = rhi::BeginRasterPass(state.Device, maskPass);
    if (status != Status::Ready)
    {
        return status;
    }
    const StaticArray<rhi::RasterVertexSlice, 2> maskSlices{
        {{state.MeshVertices, 0, state.MeshVertexCount * sizeof(math::Vector3)},
         {versionOrNull->Instance, 0, sizeof(MaskInstance)}}};
    status = rhi::DrawIndexed(state.Device,
                              {state.Pipelines[MASK_PIPELINE],
                               state.EmptyBindings,
                               maskSlices.AsSpan(),
                               state.MeshIndices,
                               0,
                               state.MeshIndexCount,
                               state.MeshVertexCount,
                               1});
    if (status != Status::Ready)
    {
        return status;
    }
    rhi::RasterPassDescription edgePass = description;
    edgePass.ColorLoad = rhi::RasterLoad::Load;
    edgePass.ColorStore = rhi::RasterStore::Store;
    edgePass.DepthLoad = rhi::RasterLoad::Discard;
    edgePass.DepthStore = rhi::RasterStore::Discard;
    edgePass.DepthReadOnly = false;
    status = rhi::BeginRasterPass(state.Device, edgePass);
    if (status != Status::Ready)
    {
        return status;
    }
    const StaticArray<rhi::RasterVertexSlice, 1> edgeSlices{
        {{versionOrNull->Instance, EDGE_VERTICES_OFFSET, QUAD_VERTEX_COUNT * sizeof(EdgeVertex)}}};
    return rhi::DrawIndexed(state.Device,
                            {state.Pipelines[isSurface == true ? EDGE_SURFACE_PIPELINE : EDGE_ATTACHMENT_PIPELINE],
                             state.MaskBindings,
                             edgeSlices.AsSpan(),
                             state.QuadIndices,
                             0,
                             QUAD_INDEX_COUNT,
                             QUAD_VERTEX_COUNT,
                             1});
}

Status SelectionOutlineRenderer::Release(PreparedSelectionOutline& inoutPrepared) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    State::Version* versionOrNull = mState->resolve(inoutPrepared);
    if (versionOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    mState->drop(*versionOrNull);
    inoutPrepared = {};
    return Status::Ready;
}
} // namespace ludus::graphics::renderer
