// Qualify the installed cooker, runtime package, reflected storage layout and actual GPU access.
#include "renderer_composite.h"

#include <ludus/content/render_graph_format/runtime_package.h>
#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/renderer/render_graph_activation.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/ray.h>

#include <bit>
#include <new>
#include <string_view>

namespace
{
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace format = ludus::content::render_graph_format;
namespace composite = ludus::shaders::renderer_composite;
using Status = rhi::RasterStatus;
constexpr logging::LogCategory LOG_STORAGE{"RayStorage"};
constexpr uint32 EXTENT = 2;
constexpr usize PIXEL_COUNT = EXTENT * EXTENT;
constexpr uint64 TIMEOUT = 120 * time::NANOSECONDS_PER_SECOND;
constexpr int SKIPPED = 77;
constexpr StaticArray<math::Vector3, PIXEL_COUNT> EXPECTED{{{14, 6, 10}, {13, 5, 9}, {16, 8, 12}, {15, 7, 11}}};

template <typename Poll>
bool WaitUntilReady(std::string_view operation, Status status, Poll poll) noexcept
{
    const uint64 start = time::NowTicks();
    while (status == Status::Pending && time::NowTicks() - start < TIMEOUT)
    {
        threading::YieldThread();
        status = poll();
    }
    if (status != Status::Ready)
    {
        LUDUS_LOG_ERROR(LOG_STORAGE, "{} returned status {}", operation, static_cast<uint32>(status));
    }
    return status == Status::Ready;
}

bool LoadPackage(std::string_view directoryName, std::string_view filename, renderer::RenderGraphPackage& outPackage)
{
    filesystem::Directory directory;
    filesystem::File file;
    Array<uint8> bytes;
    if (directory.Open(directoryName).Succeeded() == false || directory.OpenRead(filename, file).Succeeded() == false ||
        file.Size() > format::MAX_PACKAGE_BYTES || bytes.TryResize(static_cast<usize>(file.Size())) == false)
    {
        return false;
    }
    const filesystem::ReadResult read = file.ReadAt(0, MakeSpan(bytes));
    return read.Outcome.Succeeded() == true && read.BytesRead == bytes.GetSize() &&
           outPackage.Decode(bytes) == renderer::RenderGraphPackageStatus::Ready;
}

bool CheckOutput(rhi::DeviceHandle device, const renderer::RenderGraph& graph)
{
    constexpr usize WORD_COUNT = PIXEL_COUNT * renderer::RENDER_GRAPH_ACCUMULATION_WORDS;
    StaticArray<math::Vector4, WORD_COUNT> records{};
    rhi::BufferHandle buffer;
    if (graph.GetAccumulation(0, buffer) != Status::Ready)
    {
        return false;
    }
    rhi::ReadbackTicket ticket;
    const Status requested = rhi::RequestBufferReadback(device, buffer, 0, sizeof(records), ticket);
    if (WaitUntilReady("readback", requested, [&] { return rhi::GetStatus(device, ticket); }) == false)
    {
        return false;
    }
    const Status copied =
        rhi::CopyReadback(device, ticket, reinterpret_cast<uint8*>(records.GetData()), sizeof(records));
    if (rhi::Release(device, ticket) != Status::Ready || copied != Status::Ready)
    {
        return false;
    }
    for (usize pixel = 0; pixel < PIXEL_COUNT; ++pixel)
    {
        const math::Vector4 sum = records[pixel * renderer::RENDER_GRAPH_ACCUMULATION_WORDS];
        const math::Vector3 value{sum.X, sum.Y, sum.Z};
        if (value != EXPECTED[pixel] || std::bit_cast<uint32>(sum.W) != 2)
        {
            LUDUS_LOG_ERROR(LOG_STORAGE,
                            "Pixel {} returned ({}, {}, {}) after {} samples, expected ({}, {}, {}) after two",
                            pixel,
                            sum.X,
                            sum.Y,
                            sum.Z,
                            std::bit_cast<uint32>(sum.W),
                            EXPECTED[pixel].X,
                            EXPECTED[pixel].Y,
                            EXPECTED[pixel].Z);
            return false;
        }
    }
    return true;
}

int Run(std::string_view directory, std::string_view filename)
{
    renderer::RenderGraphPackage* allocationOrNull = new (std::nothrow) renderer::RenderGraphPackage;
    core::UniquePtr<renderer::RenderGraphPackage> package{allocationOrNull};
    if (package.Get() == nullptr || LoadPackage(directory, filename, *package) == false)
    {
        LUDUS_LOG_ERROR(LOG_STORAGE, "Could not decode {}", filename);
        return 1;
    }
    const BorrowedView<const format::StorageBuffer> storage = package->GetPlan().StorageBuffers;
    constexpr uint32 RECORD_BYTES = 16;
    if (storage.GetSize() != 2)
    {
        return 1;
    }
    for (usize index = 0; index < storage.GetSize(); ++index)
    {
        if (storage[index].Slot != format::FIRST_STORAGE_SLOT + index || storage[index].Stride != RECORD_BYTES ||
            storage[index].Count != 0 || storage[index].IsReadOnly != (index == 0 ? 1U : 0U))
        {
            LUDUS_LOG_ERROR(LOG_STORAGE, "Unexpected slot, stride, count or access for binding {}", index);
            return 1;
        }
    }
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    description.Preferred.RayQueries = true;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    if (rhi::CreateDevice({}, { .Width = EXTENT, .Height = EXTENT }, description, device, surface, &failure) !=
        rhi::DeviceStatus::Ready)
    {
        return failure.Error == rhi::StartupError::AdapterUnavailable ? SKIPPED : 1;
    }
    rhi::DeviceInfo info;
    if (rhi::GetDeviceInfo(device, info) != rhi::DeviceStatus::Ready)
    {
        return 1;
    }
    LUDUS_LOG_INFO(LOG_STORAGE, "Testing {} on {}", filename, info.Adapter.Name.GetData());
    rhi::RayCapabilities capabilities;
    const Status rays = rhi::GetRayCapabilities(device, capabilities);
    if (rays == Status::Unsupported)
    {
        LUDUS_LOG_INFO(LOG_STORAGE, "Native ray queries unavailable on this adapter");
        return SKIPPED;
    }
    renderer::RenderGraphActivation activation;
    if (rays != Status::Ready ||
        activation.Initialize(device, package->GetInterfaceIdentity(), composite::Vertex(), composite::Fragment()) !=
            Status::Ready ||
        activation.Resize(EXTENT, EXTENT) != Status::Ready ||
        WaitUntilReady("staging the package", activation.Stage(Move(package)), [&] { return activation.Poll(); }) ==
            false)
    {
        return 1;
    }
    renderer::ViewDescription camera;
    camera.OrthographicCamera = true;
    renderer::SceneShaderInputs scene;
    renderer::RenderGraphReport report;
    const Status updated =
        activation
            .Update({}, {}, {&camera, 1}, 1, renderer::RenderGraphCoveragePolicy::RejectIncomplete, report, &scene);
    if (WaitUntilReady("publishing the scene", updated, [&] { return activation.Poll(); }) == false)
    {
        return 1;
    }
    renderer::RenderGraph* graphOrNull = activation.GetActiveOrNull();
    if (graphOrNull == nullptr)
    {
        return 1;
    }
    // The first submission writes four records. Complete it before any invocation reads its neighbor.
    for (uint32 frame = 0; frame < 2; ++frame)
    {
        rhi::SubmissionToken completion;
        if (graphOrNull->Render(surface, EXTENT, EXTENT, completion) != Status::Ready ||
            WaitUntilReady("waiting for submission", Status::Pending, [&] {
                return rhi::GetStatus(device, completion);
            }) == false)
        {
            return 1;
        }
    }
    return CheckOutput(device, *graphOrNull) == true ? 0 : 1;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    constexpr int REQUIRED_ARGUMENTS = 3;
    if (argumentCount != REQUIRED_ARGUMENTS)
    {
        return 1;
    }
    using namespace ludus::foundation;
    static_cast<void>(logging::LogSystem::Initialize({ .EnableFile = false, .Directory = {} }));
    const int result = Run(arguments[1], arguments[2]);
    rhi::Shutdown();
    static_cast<void>(logging::LogSystem::Shutdown());
    return result;
}
