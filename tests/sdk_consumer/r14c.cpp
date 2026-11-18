#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>
#include <ludus/graphics/rhi/rhi.h>

#include <emscripten.h>

// R14c browser device owners. Two owners present to two canvases at once, on WebGPU, on
// WebGL 2, or one of each. Each owner reads back its own buffer, rejects the other's
// handles and clears its own canvas. A selector that aliases a live owner's canvas is
// refused. Destroying one owner leaves the other working, and a replacement starts on the
// freed canvas. raster-browser-test.mjs checks the canvases' colors at the drawn and
// passed stages.
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
constexpr logging::LogCategory LOG_R14C{"R14c-Fixture"};
constexpr usize OWNER_COUNT = 2;
constexpr uint32 CANVAS_WIDTH = 64;
constexpr uint32 CANVAS_HEIGHT = 48;
constexpr usize BUFFER_BYTES = 16;
/// First byte of each owner's buffer; the ranges never overlap.
constexpr StaticArray<uint8, OWNER_COUNT> OWNER_SEEDS{{1, 101}};
/// Seed of the replacement owner's buffer.
constexpr uint8 REPLACEMENT_SEED = 201;
constexpr StaticArray<const char*, OWNER_COUNT> CANVAS_SELECTORS{{"#first", "#second"}};
/// A distinct selector string that names the first canvas.
constexpr const char* ALIAS_SELECTOR = "canvas#first";

/// Backends of the two owners; the query string selects one (?mode=webgpu|webgl2|mixed).
enum class Mode : uint32
{
    WebGpu,
    WebGl2,
    Mixed,
};
enum class Stage : uint32
{
    Starting,
    Buffers,
    Readbacks,
    Drawn,
    Replacing,
    ReplacementBuffers,
    ReplacementReadbacks,
    Passed,
};
struct Owner final
{
    rhi::DeviceHandle Device;
    rhi::SurfaceHandle Surface;
    rhi::BufferHandle Buffer;
    rhi::ReadbackTicket Ticket;
    rhi::BackendSelection Selection = rhi::BackendSelection::Auto;
    uint8 Seed = 0;
    float64 Red = 0;
    float64 Green = 0;
    float64 Blue = 0;
    bool IsRead = false;
};
/// Clear color of an owner's canvas, one full channel each.
enum class Color : uint32
{
    Red,
    Green,
    Blue,
};
Owner MakeOwner(rhi::BackendSelection selection, uint8 seed, Color color) noexcept
{
    Owner owner;
    owner.Selection = selection;
    owner.Seed = seed;
    owner.Red = color == Color::Red ? 1 : 0;
    owner.Green = color == Color::Green ? 1 : 0;
    owner.Blue = color == Color::Blue ? 1 : 0;
    return owner;
}
StaticArray<Owner, OWNER_COUNT> gOwners{};
/// Progress of the fixture; IsDone stops further ticks from acting after a failure.
struct Fixture final
{
    Stage Current = Stage::Starting;
    bool IsDone = false;
};
Fixture gFixture;

// clang-format off
EM_JS(uint32, ReadMode, (), {
    const mode = new URLSearchParams(location.search).get('mode');
    return mode === 'webgl2' ? 1 : mode === 'mixed' ? 2 : 0;
});
EM_JS(void, Report, (uint32 stage, const char* state, const char* reason), {
    const node = document.querySelector('#status');
    node.dataset.stage = stage;
    node.dataset.reason = UTF8ToString(reason);
    node.dataset.state = UTF8ToString(state);
});
EM_JS(void, ReportBackend, (uint32 owner, uint32 backend), {
    document.querySelector('#status').dataset['owner' + owner] = backend;
});
EM_JS(int32, IsResumed, (), { return window.__r14cResume === true ? 1 : 0; });
// clang-format on

void Fail(const char* reason) noexcept
{
    gFixture.IsDone = true;
    LUDUS_LOG_ERROR(LOG_R14C, "R14c failed at stage {}: {}", static_cast<uint32>(gFixture.Current), reason);
    Report(static_cast<uint32>(gFixture.Current), "failed", reason);
    rhi::Shutdown();
    emscripten_cancel_main_loop();
}
void Advance(Stage stage) noexcept
{
    gFixture.Current = stage;
    const char* state = stage == Stage::Passed ? "passed" : stage == Stage::Drawn ? "drawn" : "running";
    Report(static_cast<uint32>(stage), state, "");
}
rhi::DeviceDescription MakeDescription(rhi::BackendSelection selection) noexcept
{
    rhi::DeviceDescription description;
    description.Selection = selection;
    description.Required.PortableRaster = true;
    return description;
}
rhi::WindowInfo MakeWindow(const char* selector) noexcept
{
    return
    {
        .System = ludus::platform::WindowSystem::WebCanvas,
        .Width = CANVAS_WIDTH,
        .Height = CANVAS_HEIGHT,
        .CanvasSelector = selector,
    };
}
bool StartOwner(Owner& inoutOwner, const char* selector) noexcept
{
    const rhi::DeviceStatus status = rhi::CreateDevice({},
                                                       MakeWindow(selector),
                                                       MakeDescription(inoutOwner.Selection),
                                                       inoutOwner.Device,
                                                       inoutOwner.Surface);
    return status == rhi::DeviceStatus::Ready || status == rhi::DeviceStatus::Pending;
}
/// True once the owner is Ready; fails the fixture when it failed or was lost.
bool IsOwnerReady(const Owner& owner) noexcept
{
    rhi::DeviceInfo info;
    const rhi::DeviceStatus status = rhi::GetDeviceInfo(owner.Device, info);
    if (status != rhi::DeviceStatus::Ready && status != rhi::DeviceStatus::Pending)
    {
        Fail("owner startup failed");
        return false;
    }
    return status == rhi::DeviceStatus::Ready;
}
bool HasSelectedBackend(const Owner& owner, uint32 index) noexcept
{
    rhi::DeviceInfo info;
    if (rhi::GetDeviceInfo(owner.Device, info) != rhi::DeviceStatus::Ready)
    {
        return false;
    }
    ReportBackend(index, static_cast<uint32>(info.Startup.SelectedBackend));
    const rhi::Backend expected =
        owner.Selection == rhi::BackendSelection::WebGL2 ? rhi::Backend::WebGL2 : rhi::Backend::WebGPU;
    return info.Startup.SelectedBackend == expected;
}
StaticArray<uint8, BUFFER_BYTES> MakeBytes(uint8 seed) noexcept
{
    StaticArray<uint8, BUFFER_BYTES> bytes{};
    for (usize i = 0; i < bytes.GetSize(); ++i)
    {
        bytes[i] = static_cast<uint8>(seed + i);
    }
    return bytes;
}
bool CreateOwnerBuffer(Owner& inoutOwner) noexcept
{
    // Creation fills only an empty handle; retire the owner's previous buffer first.
    if (inoutOwner.Buffer != rhi::BufferHandle{})
    {
        static_cast<void>(rhi::Destroy(inoutOwner.Device, inoutOwner.Buffer));
        inoutOwner.Buffer = {};
    }
    const StaticArray<uint8, BUFFER_BYTES> bytes = MakeBytes(inoutOwner.Seed);
    const rhi::RasterStatus status = rhi::CreateBuffer(inoutOwner.Device,
                                                       { .Role = rhi::BufferRole::Vertex, .Size = BUFFER_BYTES },
                                                       {bytes.GetData(), bytes.GetSize()},
                                                       inoutOwner.Buffer);
    inoutOwner.IsRead = false;
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
/// True once the owner's buffer is Ready; fails the fixture when its creation failed.
bool IsBufferReady(const Owner& owner) noexcept
{
    const rhi::RasterStatus status = rhi::GetStatus(owner.Device, owner.Buffer);
    if (status != rhi::RasterStatus::Ready && status != rhi::RasterStatus::Pending)
    {
        Fail("buffer creation failed");
        return false;
    }
    return status == rhi::RasterStatus::Ready;
}
bool RequestOwnerReadback(Owner& inoutOwner) noexcept
{
    return rhi::RequestBufferReadback(inoutOwner.Device, inoutOwner.Buffer, 0, BUFFER_BYTES, inoutOwner.Ticket) ==
           rhi::RasterStatus::Pending;
}
/// Poll one owner's readback without a host wait; true once its bytes matched.
bool IsReadbackVerified(Owner& inoutOwner) noexcept
{
    if (inoutOwner.IsRead == true)
    {
        return true;
    }
    static_cast<void>(rhi::PollLifetime(inoutOwner.Device));
    const rhi::RasterStatus status = rhi::GetStatus(inoutOwner.Device, inoutOwner.Ticket);
    if (status == rhi::RasterStatus::Pending)
    {
        return false;
    }
    StaticArray<uint8, BUFFER_BYTES> actual{};
    if (status != rhi::RasterStatus::Ready ||
        rhi::CopyReadback(inoutOwner.Device, inoutOwner.Ticket, actual.GetData(), actual.GetSize()) !=
            rhi::RasterStatus::Ready ||
        rhi::Release(inoutOwner.Device, inoutOwner.Ticket) != rhi::RasterStatus::Ready)
    {
        Fail("readback failed");
        return false;
    }
    const StaticArray<uint8, BUFFER_BYTES> expected = MakeBytes(inoutOwner.Seed);
    for (usize i = 0; i < actual.GetSize(); ++i)
    {
        if (actual[i] != expected[i])
        {
            Fail("readback bytes differ");
            return false;
        }
    }
    inoutOwner.IsRead = true;
    return true;
}
/// Clear the owner's canvas to its color; the browser presents it after this task.
bool DrawOwner(const Owner& owner) noexcept
{
    const rhi::FrameTarget target
    {
        .Width = CANVAS_WIDTH,
        .Height = CANVAS_HEIGHT,
        .Red = owner.Red,
        .Green = owner.Green,
        .Blue = owner.Blue,
    };
    if (rhi::SetFrameTarget(owner.Device, owner.Surface, target) != rhi::DeviceStatus::Ready ||
        rhi::BeginFrame(owner.Device, owner.Surface) != rhi::DeviceStatus::Ready ||
        rhi::EndFrame(owner.Device) != rhi::DeviceStatus::Ready)
    {
        Fail("frame failed");
        return false;
    }
    return true;
}
bool DrawOwners() noexcept
{
    for (const Owner& owner : gOwners)
    {
        if (DrawOwner(owner) == false)
        {
            return false;
        }
    }
    return true;
}
/// A distinct selector naming the first owner's canvas must not start another owner.
bool IsAliasRejected() noexcept
{
    rhi::DeviceHandle alias;
    rhi::SurfaceHandle aliasSurface;
    rhi::StartupInfo failure;
    const rhi::DeviceStatus status = rhi::CreateDevice({},
                                                       MakeWindow(ALIAS_SELECTOR),
                                                       MakeDescription(gOwners[0].Selection),
                                                       alias,
                                                       aliasSurface,
                                                       &failure);
    if (status == rhi::DeviceStatus::Ready || status == rhi::DeviceStatus::Pending)
    {
        static_cast<void>(rhi::DestroyDevice(alias));
        return false;
    }
    return failure.Error == rhi::StartupError::InvalidWindow && alias == rhi::DeviceHandle{};
}
void TickStarting() noexcept
{
    if (IsOwnerReady(gOwners[0]) == false || IsOwnerReady(gOwners[1]) == false)
    {
        return;
    }
    if (HasSelectedBackend(gOwners[0], 0) == false || HasSelectedBackend(gOwners[1], 1) == false)
    {
        Fail("owner selected another backend");
        return;
    }
    if (IsAliasRejected() == false)
    {
        Fail("an aliased canvas selector started an owner");
        return;
    }
    // Each WebGL 2 owner has its own context. Interleave every owner's frame with its buffer
    // creation, so the readbacks that follow run while another owner's context was current.
    for (Owner& owner : gOwners)
    {
        if (DrawOwner(owner) == false || CreateOwnerBuffer(owner) == false)
        {
            Fail("buffer creation rejected");
            return;
        }
    }
    // Handles carry their owner: neither owner resolves the other's buffer.
    if (rhi::GetStatus(gOwners[1].Device, gOwners[0].Buffer) != rhi::RasterStatus::InvalidHandle ||
        rhi::GetStatus(gOwners[0].Device, gOwners[1].Buffer) != rhi::RasterStatus::InvalidHandle)
    {
        Fail("an owner resolved another owner's buffer");
        return;
    }
    Advance(Stage::Buffers);
}
/// Request readbacks of every owner's ready buffer and move to `next`.
void TickBuffers(Stage next) noexcept
{
    for (const Owner& owner : gOwners)
    {
        if (IsBufferReady(owner) == false)
        {
            return;
        }
    }
    for (Owner& owner : gOwners)
    {
        if (RequestOwnerReadback(owner) == false)
        {
            Fail("readback rejected");
            return;
        }
    }
    Advance(next);
}
/// Verify every owner's readback, clear both canvases and move to `next`.
void TickReadbacks(Stage next) noexcept
{
    bool isVerified = true;
    for (Owner& owner : gOwners)
    {
        isVerified = IsReadbackVerified(owner) == true && isVerified == true;
    }
    if (gFixture.IsDone == true || isVerified == false || DrawOwners() == false)
    {
        return;
    }
    Advance(next);
}
void TickDrawn() noexcept
{
    if (DrawOwners() == false || IsResumed() == 0)
    {
        return;
    }
    // Retire the first owner; the second keeps its buffer and canvas.
    const rhi::BufferHandle retiredBuffer = gOwners[0].Buffer;
    if (rhi::DestroyDevice(gOwners[0].Device) != rhi::DeviceStatus::Ready ||
        rhi::GetStatus(gOwners[1].Device, gOwners[1].Buffer) != rhi::RasterStatus::Ready ||
        DrawOwner(gOwners[1]) == false)
    {
        Fail("destroying one owner disturbed the other");
        return;
    }
    gOwners[0] = MakeOwner(gOwners[0].Selection, REPLACEMENT_SEED, Color::Blue);
    gOwners[0].Buffer = retiredBuffer;
    if (StartOwner(gOwners[0], CANVAS_SELECTORS[0]) == false)
    {
        Fail("replacement owner rejected");
        return;
    }
    Advance(Stage::Replacing);
}
void TickReplacing() noexcept
{
    if (DrawOwner(gOwners[1]) == false || IsOwnerReady(gOwners[0]) == false)
    {
        return;
    }
    // The destroyed owner's handle stays stale in the owner that took its canvas.
    if (rhi::GetStatus(gOwners[0].Device, gOwners[0].Buffer) != rhi::RasterStatus::InvalidHandle)
    {
        Fail("a replacement resolved a destroyed owner's buffer");
        return;
    }
    if (CreateOwnerBuffer(gOwners[0]) == false || CreateOwnerBuffer(gOwners[1]) == false)
    {
        Fail("buffer creation rejected after replacement");
        return;
    }
    Advance(Stage::ReplacementBuffers);
}
void Tick() noexcept
{
    switch (gFixture.Current)
    {
        case Stage::Starting:
            TickStarting();
            break;
        case Stage::Buffers:
            TickBuffers(Stage::Readbacks);
            break;
        case Stage::Readbacks:
            TickReadbacks(Stage::Drawn);
            break;
        case Stage::Drawn:
            TickDrawn();
            break;
        case Stage::Replacing:
            TickReplacing();
            break;
        case Stage::ReplacementBuffers:
            TickBuffers(Stage::ReplacementReadbacks);
            break;
        case Stage::ReplacementReadbacks:
            TickReadbacks(Stage::Passed);
            if (gFixture.Current == Stage::Passed)
            {
                LUDUS_LOG_INFO(LOG_R14C, "R14c passed: two owners, alias rejection and replacement");
            }
            break;
        case Stage::Passed:
            static_cast<void>(DrawOwners());
            break;
    }
}
} // namespace
int main()
{
    (void)logging::LogSystem::Initialize({ .EnableFile = false, .Directory = {} });
    const Mode mode = static_cast<Mode>(ReadMode());
    gOwners[0] = MakeOwner(mode == Mode::WebGl2 ? rhi::BackendSelection::WebGL2 : rhi::BackendSelection::WebGPU,
                           OWNER_SEEDS[0],
                           Color::Red);
    gOwners[1] = MakeOwner(mode == Mode::WebGpu ? rhi::BackendSelection::WebGPU : rhi::BackendSelection::WebGL2,
                           OWNER_SEEDS[1],
                           Color::Green);
    for (usize i = 0; i < OWNER_COUNT; ++i)
    {
        if (StartOwner(gOwners[i], CANVAS_SELECTORS[i]) == false)
        {
            Fail("owner creation rejected");
            return 1;
        }
    }
    Advance(Stage::Starting);
    emscripten_set_main_loop(Tick, 0, 1);
    return 0;
}
