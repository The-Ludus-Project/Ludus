#include <ludus/graphics/renderer/overlays.hpp>

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
static_assert(sizeof(OverlayVertex) == 48);
namespace
{
/// Overlay shaders take three Float4 vertex inputs and sample one texture with one sampler.
constexpr usize OVERLAY_VERTEX_INPUT_COUNT = 3;
constexpr usize OVERLAY_FRAGMENT_BINDING_COUNT = 2;
bool Accepted(Status status) noexcept
{
    return status == Status::Ready || status == Status::Pending;
}
bool Color(math::Vector4 color) noexcept
{
    return math::IsFinite(color) && color.X >= 0 && color.X <= 1 && color.Y >= 0 && color.Y <= 1 && color.Z >= 0 &&
           color.Z <= 1 && color.W >= 0 && color.W <= 1;
}
bool Same(const OverlayStyle& a, const OverlayStyle& b) noexcept
{
    return a.Depth == b.Depth && a.Clip == b.Clip &&
           (a.Clip == false || (a.Scissor.X == b.Scissor.X && a.Scissor.Y == b.Scissor.Y &&
                                a.Scissor.Width == b.Scissor.Width && a.Scissor.Height == b.Scissor.Height));
}
rhi::RasterRectangle Intersection(rhi::RasterRectangle a, rhi::RasterRectangle b) noexcept
{
    const auto x = math::Max(a.X, b.X), y = math::Max(a.Y, b.Y);
    const auto right = math::Min(uint64{a.X} + a.Width, uint64{b.X} + b.Width);
    const auto bottom = math::Min(uint64{a.Y} + a.Height, uint64{b.Y} + b.Height);
    return {x, y, static_cast<uint32>(right > x ? right - x : 0), static_cast<uint32>(bottom > y ? bottom - y : 0)};
}
std::atomic<uint64> gOverlayOwner{1};
uint64 Owner() noexcept
{
    auto value = gOverlayOwner.load(std::memory_order_relaxed);
    while (value != ~uint64{0})
    {
        if (gOverlayOwner.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
        {
            return value;
        }
    }
    return 0;
}
} // namespace
struct OverlayList::State final
{
    OverlayVertex Vertices[MAX_VERTICES];
    OverlayCommand Commands[MAX_COMMANDS];
    usize VerticesUsed = 0, CommandsUsed = 0;
    uint64 Dropped = 0;
};
OverlayList::OverlayList() noexcept = default;
OverlayList::~OverlayList() = default;
Status OverlayList::Initialize() noexcept
{
    if (mState.Get() == nullptr)
    {
        mState.Reset(new (std::nothrow) State);
    }
    return mState ? Status::Ready : Status::OutOfMemory;
}
void OverlayList::Clear() noexcept
{
    if (mState)
    {
        mState->VerticesUsed = mState->CommandsUsed = 0;
        mState->Dropped = 0;
    }
}
const OverlayVertex* OverlayList::Vertices() const noexcept
{
    return mState ? mState->Vertices : nullptr;
}
usize OverlayList::VertexCount() const noexcept
{
    return mState ? mState->VerticesUsed : 0;
}
const OverlayCommand* OverlayList::Commands() const noexcept
{
    return mState ? mState->Commands : nullptr;
}
usize OverlayList::CommandCount() const noexcept
{
    return mState ? mState->CommandsUsed : 0;
}
uint64 OverlayList::DroppedCommands() const noexcept
{
    return mState ? mState->Dropped : 0;
}
// Thanks to Aurelio Reis, "Fast Font Rendering with Instancing", Game Programming
// Gems 8 (2010), section 1.1, pp. 3-11, for compatible batch boundaries and
// painter-order constraints. Merge adjacent compatible commands only; this
// expanded-vertex overlay path does not implement the chapter's instancing or
// D3D9 upload method. Review: docs/architecture/renderer-systems-gems-review.md.
Status OverlayList::Append(const OverlayVertex* vertices, usize count, const OverlayStyle& style) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::NotReady;
    }
    if ((count != 0 && vertices == nullptr) || count % 3 != 0 ||
        static_cast<uint8>(style.Depth) > static_cast<uint8>(OverlayDepth::ReverseZ))
    {
        return Status::InvalidDescription;
    }
    auto& s = *mState;
    const bool merge = s.CommandsUsed != 0 && Same(s.Commands[s.CommandsUsed - 1].Style, style);
    if (count > MAX_VERTICES - s.VerticesUsed || (count != 0 && merge == false && s.CommandsUsed == MAX_COMMANDS))
    {
        if (s.Dropped != ~uint64{0})
        {
            ++s.Dropped;
        }
        return Status::CapacityExceeded;
    }
    for (usize i = 0; i < count; ++i)
    {
        const auto& v = vertices[i];
        if (math::IsFinite(v.Clip) == false || v.Clip.W <= 0 || v.Clip.Z < 0 || v.Clip.Z > v.Clip.W ||
            math::IsFinite(v.Coverage) == false || v.Coverage.X < 0 || v.Coverage.X > 1 || v.Coverage.Y < 0 ||
            v.Coverage.Y > 1 || (v.Coverage.Z != 0 && v.Coverage.Z != 1) || Color(v.Color) == false)
        {
            return Status::InvalidDescription;
        }
    }
    if (count == 0)
    {
        return Status::Ready;
    }
    const auto first = s.VerticesUsed;
    for (usize i = 0; i < count; ++i)
    {
        s.Vertices[s.VerticesUsed++] = vertices[i];
    }
    if (merge)
    {
        s.Commands[s.CommandsUsed - 1].Count += static_cast<uint32>(count);
    }
    else
    {
        s.Commands[s.CommandsUsed++] = {static_cast<uint32>(first), static_cast<uint32>(count), style};
    }
    return Status::Ready;
}
Status OverlayList::Quad(math::Vector4 rectangle,
                         math::Vector4 uv,
                         math::Vector4 color,
                         uint32 width,
                         uint32 height,
                         bool coverage,
                         const OverlayStyle& style) noexcept
{
    if (width == 0 || height == 0 || math::IsFinite(rectangle) == false || math::IsFinite(uv) == false ||
        Color(color) == false || rectangle.Z < rectangle.X || rectangle.W < rectangle.Y || uv.X < 0 || uv.Y < 0 ||
        uv.Z > 1 || uv.W > 1 || uv.Z < uv.X || uv.W < uv.Y)
    {
        return Status::InvalidDescription;
    }
    if (rectangle.Z == rectangle.X || rectangle.W == rectangle.Y)
    {
        return Append(nullptr, 0, style);
    }
    const auto x0 = static_cast<float32>(2.0 * static_cast<float64>(rectangle.X) / width - 1);
    const auto x1 = static_cast<float32>(2.0 * static_cast<float64>(rectangle.Z) / width - 1);
    const auto y0 = static_cast<float32>(1 - 2.0 * static_cast<float64>(rectangle.Y) / height);
    const auto y1 = static_cast<float32>(1 - 2.0 * static_cast<float64>(rectangle.W) / height);
    const float32 mask = coverage ? 1 : 0;
    const OverlayVertex corners[]{{{x0, y0, 0, 1}, {uv.X, uv.Y, mask, 0}, color},
                                  {{x1, y0, 0, 1}, {uv.Z, uv.Y, mask, 0}, color},
                                  {{x1, y1, 0, 1}, {uv.Z, uv.W, mask, 0}, color},
                                  {{x0, y1, 0, 1}, {uv.X, uv.W, mask, 0}, color}};
    const OverlayVertex triangles[]{corners[0], corners[1], corners[2], corners[0], corners[2], corners[3]};
    return Append(triangles, 6, style);
}
Status OverlayList::Line(math::Vector4 start,
                         math::Vector4 end,
                         math::Vector4 color,
                         float32 physicalWidth,
                         uint32 width,
                         uint32 height,
                         const OverlayStyle& style) noexcept
{
    if (math::IsFinite(start) == false || math::IsFinite(end) == false || Color(color) == false ||
        math::IsFinite(physicalWidth) == false || physicalWidth <= 0 || width == 0 || height == 0)
    {
        return Status::InvalidDescription;
    }
    // Clip in homogeneous space before any divide. Double intermediates protect
    // finite binary32 endpoints from overflow and near-plane cancellation.
    float64 a[4]{static_cast<float64>(start.X),
                 static_cast<float64>(start.Y),
                 static_cast<float64>(start.Z),
                 static_cast<float64>(start.W)},
        b[4]{static_cast<float64>(end.X),
             static_cast<float64>(end.Y),
             static_cast<float64>(end.Z),
             static_cast<float64>(end.W)};
    float64 low = 0, high = 1;
    for (usize plane = 0; plane < 3; ++plane)
    {
        const auto da = plane == 0 ? a[3] - 1e-6 : (plane == 1 ? a[2] : a[3] - a[2]);
        const auto db = plane == 0 ? b[3] - 1e-6 : (plane == 1 ? b[2] : b[3] - b[2]);
        if (da < 0 && db < 0)
        {
            return Append(nullptr, 0, style);
        }
        if ((da < 0) != (db < 0))
        {
            const auto t = da / (da - db);
            if (da < 0)
            {
                low = math::Max(low, t);
            }
            else
            {
                high = math::Min(high, t);
            }
        }
    }
    if (low > high)
    {
        return Append(nullptr, 0, style);
    }
    float64 clipped[2][4];
    for (usize i = 0; i < 4; ++i)
    {
        clipped[0][i] = a[i] + (b[i] - a[i]) * low;
        clipped[1][i] = a[i] + (b[i] - a[i]) * high;
    }
    const auto x0 = clipped[0][0] / clipped[0][3], y0 = clipped[0][1] / clipped[0][3];
    const auto x1 = clipped[1][0] / clipped[1][3], y1 = clipped[1][1] / clipped[1][3];
    const auto dx = (x1 - x0) * width * .5, dy = (y1 - y0) * height * .5;
    const auto length = math::Sqrt(dx * dx + dy * dy);
    const float64 half = static_cast<float64>(math::Max(physicalWidth, 1.0F)) * .5;
    const auto ox = length > 1e-6 ? -dy / length * half * 2 / width : half * 2 / width;
    const auto oy = length > 1e-6 ? dx / length * half * 2 / height : 0.0;
    OverlayVertex corners[4];
    const float64 xs[]{x0 - ox, x0 + ox, x1 + ox, x1 - ox};
    const float64 ys[]{y0 - oy, y0 + oy, y1 + oy, y1 - oy};
    for (usize i = 0; i < 4; ++i)
    {
        const auto endpoint = i < 2 ? 0U : 1U;
        const auto z = math::Clamp(clipped[endpoint][2] / clipped[endpoint][3], 0.0, 1.0);
        corners[i] = {{static_cast<float32>(xs[i]), static_cast<float32>(ys[i]), static_cast<float32>(z), 1},
                      {},
                      color};
        if (length <= 1e-6)
        {
            corners[i].Clip.Y = static_cast<float32>(y0 + (i < 2 ? -half : half) * 2 / height);
        }
    }
    const OverlayVertex triangles[]{corners[0], corners[1], corners[2], corners[0], corners[2], corners[3]};
    return Append(triangles, 6, style);
}
namespace internal
{
struct OverlayAccess final
{
    struct Identity final
    {
        uint64 Owner, Generation;
        uint32 Slot;
    };
    static PreparedOverlay Make(const Identity& identity) noexcept
    {
        PreparedOverlay value;
        value.Owner = identity.Owner;
        value.Generation = identity.Generation;
        value.Slot = identity.Slot;
        return value;
    }
    static uint64 Owner(PreparedOverlay value) noexcept
    {
        return value.Owner;
    }
    static uint64 Generation(PreparedOverlay value) noexcept
    {
        return value.Generation;
    }
    static uint32 Slot(PreparedOverlay value) noexcept
    {
        return value.Slot;
    }
};
} // namespace internal
struct OverlayRenderer::State final
{
    struct Version final
    {
        uint64 Generation = 1;
        bool Used = false, UploadOwned = false, BindingCreated = false;
        rhi::UploadTicket Upload;
        rhi::BufferHandle Vertices;
        rhi::TextureViewHandle Atlas;
        rhi::BindingSetHandle Bindings;
        Status Setup = Status::Pending;
        OverlayCommand Commands[OverlayList::MAX_COMMANDS];
        usize Count = 0;
    };
    uint64 Identity = 0;
    rhi::DeviceHandle Device;
    rhi::RasterShaderHandle Vertex, Fragment;
    rhi::BindingLayoutHandle Layout;
    rhi::SamplerHandle Sampler;
    rhi::BufferHandle Indices;
    rhi::TextureHandle White;
    rhi::RasterPipelineHandle Pipelines[6];
    Version Versions[4];
    Status Setup = Status::Pending;
    uint32 Phase = 0;
    Version* Resolve(PreparedOverlay value) noexcept
    {
        const auto index = internal::OverlayAccess::Slot(value);
        if (internal::OverlayAccess::Owner(value) != Identity || index >= 4)
        {
            return nullptr;
        }
        auto& version = Versions[index];
        return version.Used && version.Generation == internal::OverlayAccess::Generation(value) ? &version : nullptr;
    }
    void Drop(Version& v) noexcept
    {
        if (v.UploadOwned)
        {
            static_cast<void>(rhi::Release(Device, v.Upload));
        }
        static_cast<void>(rhi::Destroy(Device, v.Bindings));
        static_cast<void>(rhi::Destroy(Device, v.Atlas));
        static_cast<void>(rhi::Destroy(Device, v.Vertices));
        const auto generation = v.Generation;
        v = {};
        v.Generation = generation + 1;
    }
    ~State()
    {
        for (auto& v : Versions)
        {
            if (v.Used)
            {
                Drop(v);
            }
        }
        for (auto& pipeline : Pipelines)
        {
            static_cast<void>(rhi::Destroy(Device, pipeline));
        }
        static_cast<void>(rhi::Destroy(Device, White));
        static_cast<void>(rhi::Destroy(Device, Indices));
        static_cast<void>(rhi::Destroy(Device, Sampler));
        static_cast<void>(rhi::Destroy(Device, Layout));
        static_cast<void>(rhi::Destroy(Device, Fragment));
        static_cast<void>(rhi::Destroy(Device, Vertex));
    }
};
OverlayRenderer::OverlayRenderer() noexcept = default;
OverlayRenderer::~OverlayRenderer() = default;
void OverlayRenderer::Reset() noexcept
{
    mState.Reset();
}
Status OverlayRenderer::Initialize(rhi::DeviceHandle device,
                                   const rhi::RasterShaderDescription& vertex,
                                   const rhi::RasterShaderDescription& fragment) noexcept
{
    if (mState)
    {
        return Status::InvalidState;
    }
    if (vertex.Inputs.size() != OVERLAY_VERTEX_INPUT_COUNT || vertex.Bindings.empty() == false ||
        fragment.Inputs.empty() == false || fragment.Bindings.size() != OVERLAY_FRAGMENT_BINDING_COUNT ||
        vertex.Artifact.Stage != rhi::ShaderStage::Vertex || fragment.Artifact.Stage != rhi::ShaderStage::Fragment)
    {
        return Status::InvalidDescription;
    }
    for (usize i = 0; i < OVERLAY_VERTEX_INPUT_COUNT; ++i)
    {
        if (vertex.Inputs[i].Location != i || vertex.Inputs[i].Format != rhi::RasterVertexFormat::Float4)
        {
            return Status::InvalidDescription;
        }
    }
    const rhi::RasterBinding expected[]{{0, rhi::RasterBindingKind::Texture2D, rhi::RasterVisibility::Fragment, 0},
                                        {1, rhi::RasterBindingKind::Sampler, rhi::RasterVisibility::Fragment, 0}};
    for (usize i = 0; i < OVERLAY_FRAGMENT_BINDING_COUNT; ++i)
    {
        const auto& binding = fragment.Bindings[i];
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
    auto& s = *mState;
    s.Device = device;
    s.Identity = Owner();
    if (s.Identity == 0)
    {
        return s.Setup = Status::IdentityExhausted;
    }
    auto status = rhi::CreateRasterShader(device, vertex, s.Vertex);
    if (Accepted(status))
    {
        status = rhi::CreateRasterShader(device, fragment, s.Fragment);
    }
    if (Accepted(status))
    {
        status = rhi::CreateBindingLayout(device, expected, s.Layout);
    }
    if (Accepted(status))
    {
        status = rhi::CreateSampler(device, {rhi::RasterFilter::Linear}, s.Sampler);
    }
    const uint8 white[4]{255, 0, 0, 0};
    if (Accepted(status))
    {
        status = rhi::CreateTexture(device, {1, 1, rhi::RasterFormat::R8Unorm}, {white, 4}, s.White);
    }
    uint16 indices[OverlayList::MAX_VERTICES];
    for (usize i = 0; i < OverlayList::MAX_VERTICES; ++i)
    {
        indices[i] = static_cast<uint16>(i);
    }
    if (Accepted(status))
    {
        status = rhi::CreateBuffer(device,
                                   {rhi::BufferRole::Index16, sizeof(indices)},
                                   {reinterpret_cast<const uint8*>(indices), sizeof(indices)},
                                   s.Indices);
    }
    if (Accepted(status) == false)
    {
        return s.Setup = status;
    }
    return Poll();
}
Status OverlayRenderer::Poll() noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::NotReady;
    }
    auto& s = *mState;
    auto status = rhi::PollLifetime(s.Device);
    if (status != Status::Ready)
    {
        return status;
    }
    if (s.Setup != Status::Pending)
    {
        return s.Setup;
    }
    const rhi::RasterVertexStream streams[]{{sizeof(OverlayVertex), false}};
    const rhi::RasterVertexAttribute attributes[]{{0, 0, 0, rhi::RasterVertexFormat::Float4},
                                                  {1, 0, 16, rhi::RasterVertexFormat::Float4},
                                                  {2, 0, 32, rhi::RasterVertexFormat::Float4}};
    while (s.Phase <= 6)
    {
        const auto prerequisite =
            s.Phase == 0 ? rhi::GetStatus(s.Device, s.Indices) : rhi::GetStatus(s.Device, s.Pipelines[s.Phase - 1]);
        if (prerequisite != Status::Ready)
        {
            if (prerequisite != Status::Pending)
            {
                s.Setup = prerequisite;
            }
            return prerequisite;
        }
        if (s.Phase == 0)
        {
            const Status prerequisites[]{rhi::GetStatus(s.Device, s.Vertex),
                                         rhi::GetStatus(s.Device, s.Fragment),
                                         rhi::GetStatus(s.Device, s.Layout),
                                         rhi::GetStatus(s.Device, s.Sampler),
                                         rhi::GetStatus(s.Device, s.White)};
            for (auto ready : prerequisites)
            {
                if (ready != Status::Ready)
                {
                    if (ready != Status::Pending)
                    {
                        s.Setup = ready;
                    }
                    return ready;
                }
            }
        }
        if (s.Phase == 6)
        {
            return s.Setup = Status::Ready;
        }
        const auto depth = s.Phase % 3;
        rhi::RasterPipelineDescription
            description{s.Vertex, s.Fragment, s.Layout, streams, attributes, depth != 0, true};
        description.Target = s.Phase < 3 ? rhi::RasterTarget::Surface : rhi::RasterTarget::Rgba8Unorm;
        description.DepthWrite = false;
        description.DepthCompare =
            depth == 2 ? rhi::RasterDepthCompare::GreaterEqual : rhi::RasterDepthCompare::LessEqual;
        status = rhi::CreateRasterPipeline(s.Device, description, s.Pipelines[s.Phase]);
        if (Accepted(status) == false)
        {
            return s.Setup = status;
        }
        ++s.Phase;
        if (status == Status::Pending)
        {
            return status;
        }
    }
    return s.Setup;
}
Status OverlayRenderer::Prepare(const OverlayList& list, rhi::TextureHandle atlas, PreparedOverlay& output) noexcept
{
    if (mState.Get() == nullptr || mState->Setup != Status::Ready)
    {
        return Status::NotReady;
    }
    if (internal::OverlayAccess::Owner(output) != 0)
    {
        return Status::InvalidState;
    }
    auto& s = *mState;
    const bool solid = rhi::IsNull(atlas);
    if (solid)
    {
        for (usize i = 0; i < list.VertexCount(); ++i)
        {
            if (list.Vertices()[i].Coverage.Z != 0)
            {
                return Status::InvalidDescription;
            }
        }
        atlas = s.White;
    }
    rhi::TextureDescription texture;
    auto status = rhi::GetTextureDescription(s.Device, atlas, texture);
    if (status != Status::Ready)
    {
        return status;
    }
    if (texture.Attachment || texture.Format != rhi::RasterFormat::R8Unorm)
    {
        return Status::InvalidDescription;
    }
    usize index = 0;
    while (index < 4 && (s.Versions[index].Used || s.Versions[index].Generation == ~uint64{0}))
    {
        ++index;
    }
    if (index == 4)
    {
        return Status::CapacityExceeded;
    }
    auto& v = s.Versions[index];
    v.Used = true;
    v.Count = list.CommandCount();

    for (usize i = 0; i < v.Count; ++i)
    {
        v.Commands[i] = list.Commands()[i];
    }
    status = rhi::CreateTextureView(s.Device, atlas, v.Atlas);
    if (Accepted(status) && list.VertexCount() != 0)
    {
        const auto bytes = list.VertexCount() * sizeof(OverlayVertex);
        status = rhi::RequestBufferUpload(s.Device,
                                          {rhi::BufferRole::Vertex, bytes},
                                          reinterpret_cast<const uint8*>(list.Vertices()),
                                          bytes,
                                          v.Upload);
        v.UploadOwned = status == Status::Pending;
    }
    if (Accepted(status) == false)
    {
        s.Drop(v);
        return status;
    }
    const auto prepared = internal::OverlayAccess::Make({s.Identity, v.Generation, static_cast<uint32>(index)});
    status = GetStatus(prepared);
    if (Accepted(status) == false)
    {
        s.Drop(v);
        return status;
    }
    output = prepared;
    return status;
}
Status OverlayRenderer::GetStatus(PreparedOverlay overlay) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto& s = *mState;
    auto* v = s.Resolve(overlay);
    if (v == nullptr)
    {
        return Status::InvalidHandle;
    }
    rhi::RasterCapabilities capabilities;
    const auto device = rhi::GetRasterCapabilities(s.Device, capabilities);
    if (device != Status::Ready)
    {
        return device;
    }
    // No resource creation while an acquired frame is open: Ready versions are immutable.
    if (v->Setup != Status::Pending)
    {
        return v->Setup;
    }
    if (v->UploadOwned)
    {
        auto status = rhi::GetStatus(s.Device, v->Upload);
        if (status != Status::Ready)
        {
            return status;
        }
        status = rhi::TakeUploadedBuffer(s.Device, v->Upload, v->Vertices);
        if (status != Status::Ready)
        {
            return status;
        }
        v->UploadOwned = false;
    }
    auto status = rhi::GetStatus(s.Device, v->Atlas);
    if (status == Status::Ready && v->Count != 0)
    {
        status = rhi::GetStatus(s.Device, v->Vertices);
    }
    if (status != Status::Ready)
    {
        return status;
    }
    if (v->Count == 0)
    {
        return v->Setup = Status::Ready;
    }
    if (v->BindingCreated == false)
    {
        const rhi::RasterBindingResource values[]{{0, {}, 0, 0, v->Atlas, {}}, {1, {}, 0, 0, {}, s.Sampler}};
        status = rhi::CreateBindingSet(s.Device, s.Layout, values, v->Bindings);
        if (Accepted(status) == false)
        {
            return v->Setup = status;
        }
        v->BindingCreated = true;
    }
    status = rhi::GetStatus(s.Device, v->Bindings);
    if (status != Status::Pending)
    {
        v->Setup = status;
    }
    return status;
}
Status OverlayRenderer::Draw(PreparedOverlay overlay, const rhi::RasterPassDescription& description) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto& s = *mState;
    const auto* v = s.Resolve(overlay);
    if (v == nullptr)
    {
        return Status::InvalidHandle;
    }
    if (v->Setup != Status::Ready)
    {
        return v->Setup;
    }
    const bool surface = rhi::IsNull(description.Color);
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
    }
    for (usize i = 0; i < v->Count; ++i)
    {
        const auto& command = v->Commands[i];
        auto pass = description;
        pass.ColorLoad = rhi::RasterLoad::Load;
        pass.ColorStore = rhi::RasterStore::Store;
        pass.DepthLoad = rhi::RasterLoad::Load;
        pass.DepthStore = rhi::RasterStore::Store;
        pass.DepthReadOnly = true;
        if (command.Style.Clip)
        {
            pass.Scissor = pass.UseScissor ? Intersection(pass.Scissor, command.Style.Scissor) : command.Style.Scissor;
            pass.UseScissor = true;
        }
        auto status = rhi::BeginRasterPass(s.Device, pass);
        if (status != Status::Ready)
        {
            return status;
        }
        const rhi::RasterVertexSlice slice[]{
            {v->Vertices, command.First * sizeof(OverlayVertex), command.Count * sizeof(OverlayVertex)}};
        status = rhi::DrawIndexed(s.Device,
                                  {s.Pipelines[(surface ? 0 : 3) + static_cast<usize>(command.Style.Depth)],
                                   v->Bindings,
                                   slice,
                                   s.Indices,
                                   0,
                                   command.Count,
                                   command.Count,
                                   1});
        if (status != Status::Ready)
        {
            return status;
        }
    }
    return Status::Ready;
}
Status OverlayRenderer::Release(PreparedOverlay& overlay) noexcept
{
    if (mState.Get() == nullptr)
    {
        return Status::InvalidHandle;
    }
    auto* v = mState->Resolve(overlay);
    if (v == nullptr)
    {
        return Status::InvalidHandle;
    }
    mState->Drop(*v);
    overlay = {};
    return Status::Ready;
}
} // namespace ludus::graphics::renderer
