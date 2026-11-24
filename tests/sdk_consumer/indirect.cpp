#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include "indirect_draw.h"
#include "indirect_draw_slots.h"
#include "indirect_mark.h"
#include "indirect_mark_slots.h"
#include "indirect_prepare.h"
#include "indirect_prepare_slots.h"
#include "indirect_prepare_zero.h"

#if defined(LUDUS_PLATFORM_WEB)
#    include <emscripten.h>
#    include <ludus/foundation/base/pointer.hpp>
#    include <ludus/platform/base/window.h>
#endif

// Installed-SDK R12a fixture for expanded indirect draws and dispatches. With
// DeviceFeatures::IndirectRendering it runs the native conformance journey
// (modules/graphics/rhi/tests/portable_raster_conformance_tests.cpp) through the
// public API: one kernel writes offset, strided, zero-instance and non-indexed
// commands plus two dispatch records, and the image must match the direct-draw
// oracle. A profile without HasFirstInstance uses the zero-FirstInstance layout in
// indirect_commands.lusl. Without the feature (WebGL 2) it checks that the profile
// is empty, that the argument buffer is refused, and that the authored direct
// variant draws the same image. The browser harness is indirect-browser-test.mjs;
// see docs/development/fullscreen-rendering.md#expanded-indirect-draws-and-dispatches-r12a.
namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
constexpr logging::LogCategory LOG_INDIRECT{"Indirect-Fixture"};
constexpr uint32 WIDTH = 64;
constexpr uint32 HEIGHT = 48;
constexpr usize CHANNELS = 4;
constexpr usize ROW_BYTES = WIDTH * CHANNELS;
constexpr usize PIXEL_BYTES = usize{HEIGHT} * ROW_BYTES;
#if !defined(LUDUS_PLATFORM_WEB)
constexpr uint64 NATIVE_TIMEOUT_NANOSECONDS = 30 * time::NANOSECONDS_PER_SECOND;
#endif

// Byte layout written by indirect_commands.lusl.
constexpr usize ARGUMENT_BYTES = 112;
constexpr usize INDEXED_STRIDE = 40;
constexpr uint32 INDEXED_COMMANDS = 2;
constexpr usize NON_INDEXED_OFFSET = 64;
constexpr usize DISPATCH_OFFSET = 80;
constexpr usize EMPTY_DISPATCH_OFFSET = 96;
constexpr usize DRAW_ARGUMENT_SPAN = NON_INDEXED_OFFSET + sizeof(rhi::DrawIndirectArguments);
constexpr uint32 MARK_COUNT = 4;
constexpr uint32 MARKED_GROUPS = 3;
constexpr uint32 MARK_BASE = 100;
constexpr usize MARK_BYTES = MARK_COUNT * sizeof(uint32);
// The R12a profile every indirect-capable backend reports.
constexpr usize ARGUMENT_ALIGNMENT = sizeof(uint32);
constexpr uint32 MAX_COMMANDS_PER_DRAW = 64;

// Positions: 0..3 off-image (reached only if BaseVertex were ignored), 4..7 left quad,
// 8..13 right quad as two non-indexed triangles, 14..17 a red full-image quad.
constexpr usize FLOAT2_VALUES = 2;
constexpr float32 OFF = 4;
constexpr usize POSITION_VALUES = 36;
constexpr StaticArray<float32, POSITION_VALUES> POSITIONS{
    OFF, OFF, OFF, OFF, OFF, OFF, OFF, OFF,             // off-image
    -1,  -1,  0,   -1,  0,   1,   -1,  1,               // left quad
    0,   -1,  1,   -1,  1,   1,   0,   -1,  1, 1, 0, 1, // right triangles
    -1,  -1,  1,   -1,  1,   1,   -1,  1,               // full image
};
constexpr uint32 VERTEX_COUNT = POSITION_VALUES / FLOAT2_VALUES;
constexpr uint32 POSITION_STRIDE = sizeof(float32) * FLOAT2_VALUES;
constexpr uint32 LEFT_FIRST_VERTEX = 4;
constexpr uint32 LEFT_VERTEX_COUNT = 4;
constexpr uint32 RIGHT_FIRST_VERTEX = 8;
constexpr uint32 RIGHT_VERTEX_COUNT = 6;
// Instance colors: red, green, blue.
constexpr usize COLOR_VALUES = 12;
constexpr StaticArray<float32, COLOR_VALUES> COLORS{1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
constexpr uint32 INSTANCE_COUNT = COLOR_VALUES / CHANNELS;
constexpr uint32 COLOR_STRIDE = sizeof(float32) * CHANNELS;
constexpr uint32 GREEN_INSTANCE = 1;
constexpr uint32 BLUE_INSTANCE = 2;
// The first three indices are skipped by FirstIndex.
constexpr usize INDEX_VALUES = 9;
constexpr StaticArray<uint32, INDEX_VALUES> INDICES{0, 0, 0, 0, 1, 2, 0, 2, 3};
constexpr usize QUAD_FIRST_INDEX = 3;
constexpr uint32 QUAD_INDEX_COUNT = 6;
// Direct-variant indices for the right quad's six vertices.
constexpr StaticArray<uint32, QUAD_INDEX_COUNT> RIGHT_INDICES{0, 1, 2, 3, 4, 5};
constexpr usize STREAM_COUNT = 2;
constexpr usize DRAW_COUNT = 2;
constexpr uint8 FULL = 255;
constexpr StaticArray<uint8, CHANNELS> LEFT_COLOR{0, FULL, 0, FULL};
constexpr StaticArray<uint8, CHANNELS> RIGHT_COLOR{0, 0, FULL, FULL};

/// How the fixture draws the oracle image.
enum class Path : uint32
{
    /// Not chosen yet; the device is not Ready.
    Unknown = 0,
    /// GPU-written commands with nonzero FirstInstance values.
    Indirect = 1,
    /// GPU-written commands with zero FirstInstance values (no HasFirstInstance).
    IndirectZeroFirstInstance = 2,
    /// The authored direct variant for a profile without IndirectRendering.
    Direct = 3,
};

enum class Stage : uint32
{
    CheckProfile = 0,
    CreateResources = 1,
    CreatePipelines = 2,
    CompileGraph = 3,
    Execute = 4,
    ReadBack = 5,
};

/// One compute dispatch and the objects that create it.
struct Kernel final
{
    rhi::RasterShaderHandle Shader;
    rhi::BindingLayoutHandle Layout;
    rhi::ComputeDispatch Dispatch;
};

rhi::DeviceHandle gDevice;
rhi::SurfaceHandle gSurface;
rhi::BufferHandle gArguments;
rhi::BufferHandle gMarks;
rhi::BufferHandle gEmptyMarks;
rhi::BufferHandle gPositions;
rhi::BufferHandle gColors;
rhi::BufferHandle gIndices;
rhi::BufferHandle gRightIndices;
Kernel gPrepare;
Kernel gMark;
Kernel gEmptyMark;
rhi::RasterShaderHandle gVertex;
rhi::RasterShaderHandle gFragment;
rhi::BindingLayoutHandle gRasterLayout;
rhi::BindingSetHandle gRasterSet;
rhi::RasterPipelineHandle gPipeline;
rhi::TextureHandle gTarget;
rhi::OrderedGraph gGraph;
rhi::ReadbackTicket gImageReadback;
rhi::ReadbackTicket gMarksReadback;
rhi::ReadbackTicket gEmptyMarksReadback;
bool hasImage = false;
bool hasMarks = false;
bool hasEmptyMarks = false;
StaticArray<uint8, PIXEL_BYTES> gPixels{};
StaticArray<uint32, MARK_COUNT> gMarkValues{};
StaticArray<uint32, MARK_COUNT> gEmptyMarkValues{};
Path gPath = Path::Unknown;
Stage gStage = Stage::CheckProfile;
bool shouldForceZeroFirstInstance = false;
bool isRequiredRejected = false;
usize gMismatchCount = 0;
usize gFirstMismatch = 0;
bool hasFailed = false;
bool isDone = false;

#if defined(LUDUS_PLATFORM_WEB)
core::UniquePtr<ludus::platform::Window> gWindow;
// Test reporting belongs to the application, outside the engine API.
// clang-format off
EM_JS(void, ReportValue, (const char* key, uint32 value), {
    document.querySelector('#status').dataset[UTF8ToString(key)] = value;
});
EM_JS(void, ReportState, (const char* state), {
    const node = document.querySelector('#status');
    node.dataset.state = UTF8ToString(state);
    node.textContent = node.dataset.state;
});
EM_JS(int32, GetSelection, (), {
    const value = new URL(location.href).searchParams.get('backend');
    return value === 'webgpu' ? 1 : value === 'webgl2' ? 2 : 0;
});
// clang-format on
#else
void ReportValue(const char*, uint32) noexcept {}
void ReportState(const char*) noexcept {}
int32 GetSelection() noexcept
{
    return 0;
}
#endif
constexpr int32 SELECT_WEBGPU = 1;
constexpr int32 SELECT_WEBGL2 = 2;

template <typename T, usize N>
BorrowedView<const uint8> GetBytes(const StaticArray<T, N>& values) noexcept
{
    return {reinterpret_cast<const uint8*>(values.GetData()), sizeof(T) * N};
}
bool IsAccepted(rhi::RasterStatus status) noexcept
{
    return status == rhi::RasterStatus::Ready || status == rhi::RasterStatus::Pending;
}
void Stop(bool isFailure) noexcept
{
    hasFailed = isFailure;
    isDone = true;
    LUDUS_LOG_INFO(
        LOG_INDIRECT,
        "Indirect passed {}; stage {}; path {}; mismatches {} first {}; marks {} {} {} {}; empty {} {} {} {}",
        isFailure == false,
        static_cast<uint32>(gStage),
        static_cast<uint32>(gPath),
        gMismatchCount,
        gFirstMismatch,
        gMarkValues[0],
        gMarkValues[1],
        gMarkValues[2],
        gMarkValues[3],
        gEmptyMarkValues[0],
        gEmptyMarkValues[1],
        gEmptyMarkValues[2],
        gEmptyMarkValues[3]);
    ReportValue("stage", static_cast<uint32>(gStage));
    ReportValue("path", static_cast<uint32>(gPath));
    ReportValue("mismatches", static_cast<uint32>(gMismatchCount));
    ReportValue("firstMismatch", static_cast<uint32>(gFirstMismatch));
    ReportValue("requiredRejected", isRequiredRejected == true ? 1 : 0);
    const StaticArray<const char*, MARK_COUNT> markKeys{"mark0", "mark1", "mark2", "mark3"};
    const StaticArray<const char*, MARK_COUNT> emptyKeys{"emptyMark0", "emptyMark1", "emptyMark2", "emptyMark3"};
    for (usize i = 0; i < MARK_COUNT; ++i)
    {
        ReportValue(markKeys[i], gMarkValues[i]);
        ReportValue(emptyKeys[i], gEmptyMarkValues[i]);
    }
    ReportState(isFailure == true ? "failed" : "passed");
    (void)rhi::DestroyDevice(gDevice);
    (void)logging::LogSystem::Shutdown();
#if defined(LUDUS_PLATFORM_WEB)
    gWindow.Reset();
    emscripten_cancel_main_loop();
#endif
}
void Fail(const char* reason) noexcept
{
    LUDUS_LOG_ERROR(LOG_INDIRECT, "Indirect fixture failed: {}", reason);
    Stop(true);
}
template <typename T>
bool IsReady(T handle) noexcept
{
    const rhi::RasterStatus status = rhi::GetStatus(gDevice, handle);
    if (IsAccepted(status) == false)
    {
        LUDUS_LOG_ERROR(LOG_INDIRECT,
                        "Status {} at stage {}",
                        static_cast<uint32>(status),
                        static_cast<uint32>(gStage));
        Stop(true);
    }
    return status == rhi::RasterStatus::Ready;
}
bool IsIndirectPath() noexcept
{
    return gPath == Path::Indirect || gPath == Path::IndirectZeroFirstInstance;
}
// Report the negotiated R12a profile and choose the path it admits.
bool CheckProfile(const rhi::DeviceInfo& info) noexcept
{
    const rhi::IndirectCapabilities& indirect = info.Profile.Indirect;
    ReportValue("backend", static_cast<uint32>(info.Startup.SelectedBackend));
    ReportValue("indirectRendering", info.Enabled.IndirectRendering == true ? 1 : 0);
    ReportValue("unmetIndirect", info.UnmetPreferences.IndirectRendering == true ? 1 : 0);
    ReportValue("alignment", static_cast<uint32>(indirect.ArgumentAlignment));
    ReportValue("maxCommands", indirect.MaxCommandsPerDraw);
    ReportValue("nonIndexed", indirect.HasNonIndexedDraws == true ? 1 : 0);
    ReportValue("offsets", indirect.HasDrawOffsets == true ? 1 : 0);
    ReportValue("firstInstance", indirect.HasFirstInstance == true ? 1 : 0);
    ReportValue("dispatch", indirect.HasIndirectDispatch == true ? 1 : 0);
    if (info.Enabled.IndirectRendering == false)
    {
        // WebGL 2 keeps the authored direct variant: nothing in the profile, no argument buffers.
        const bool isEmpty = indirect.ArgumentAlignment == 0 && indirect.MaxCommandsPerDraw == 0 &&
                             indirect.HasNonIndexedDraws == false && indirect.HasDrawOffsets == false &&
                             indirect.HasFirstInstance == false && indirect.HasIndirectDispatch == false;
        rhi::BufferHandle rejected;
        const StaticArray<uint8, ARGUMENT_BYTES> zero{};
        const rhi::RasterStatus storage = rhi::CreateBuffer(gDevice,
                                                            {
                                                                .Role = rhi::BufferRole::StorageIndirect,
                                                                .Size = ARGUMENT_BYTES,
                                                            },
                                                            GetBytes(zero),
                                                            rejected);
        ReportValue("storageIndirect", static_cast<uint32>(storage));
        gPath = Path::Direct;
        return isEmpty == true && info.Enabled.Compute == false && info.UnmetPreferences.IndirectRendering == true &&
               storage == rhi::RasterStatus::Unsupported && rejected == rhi::BufferHandle{};
    }
    if (indirect.ArgumentAlignment != ARGUMENT_ALIGNMENT || indirect.MaxCommandsPerDraw != MAX_COMMANDS_PER_DRAW ||
        indirect.HasNonIndexedDraws == false || indirect.HasDrawOffsets == false ||
        indirect.HasIndirectDispatch == false || info.UnmetPreferences.IndirectRendering == true)
    {
        return false;
    }
    gPath = indirect.HasFirstInstance == true && shouldForceZeroFirstInstance == false
                ? Path::Indirect
                : Path::IndirectZeroFirstInstance;
    return true;
}
bool CreateKernel(const rhi::RasterShaderDescription& description, Kernel& outKernel) noexcept
{
    return description.Bindings.GetSize() == 1 &&
           IsAccepted(rhi::CreateRasterShader(gDevice, description, outKernel.Shader)) == true &&
           IsAccepted(rhi::CreateBindingLayout(gDevice, description.Bindings, outKernel.Layout)) == true;
}
bool CreateResources() noexcept
{
    const StaticArray<uint8, ARGUMENT_BYTES> zeroArguments{};
    const StaticArray<uint32, MARK_COUNT> zeroMarks{};
    const bool isCommon =
        IsAccepted(rhi::CreateBuffer(gDevice,
                                     { .Role = rhi::BufferRole::Vertex, .Size = sizeof(POSITIONS) },
                                     GetBytes(POSITIONS),
                                     gPositions)) == true &&
        IsAccepted(rhi::CreateBuffer(gDevice,
                                     { .Role = rhi::BufferRole::Vertex, .Size = sizeof(COLORS) },
                                     GetBytes(COLORS),
                                     gColors)) == true &&
        IsAccepted(rhi::CreateBuffer(gDevice,
                                     { .Role = rhi::BufferRole::Index32, .Size = sizeof(INDICES) },
                                     GetBytes(INDICES),
                                     gIndices)) == true &&
        IsAccepted(rhi::CreateRasterShader(gDevice, ludus::shaders::indirect_draw::Vertex(), gVertex)) == true &&
        IsAccepted(rhi::CreateRasterShader(gDevice, ludus::shaders::indirect_draw::Fragment(), gFragment)) == true &&
        IsAccepted(rhi::CreateBindingLayout(gDevice, {}, gRasterLayout)) == true &&
        IsAccepted(rhi::CreateTexture(gDevice,
                                      {
                                          .Width = WIDTH,
                                          .Height = HEIGHT,
                                          .Attachment = true,
                                      },
                                      {},
                                      gTarget)) == true;
    if (isCommon == false)
    {
        return false;
    }
    if (gPath == Path::Direct)
    {
        return IsAccepted(rhi::CreateBuffer(gDevice,
                                            { .Role = rhi::BufferRole::Index32, .Size = sizeof(RIGHT_INDICES) },
                                            GetBytes(RIGHT_INDICES),
                                            gRightIndices)) == true;
    }
    const rhi::RasterShaderDescription prepare = gPath == Path::Indirect
                                                     ? ludus::shaders::indirect_prepare::Compute()
                                                     : ludus::shaders::indirect_prepare_zero::Compute();
    return IsAccepted(rhi::CreateBuffer(gDevice,
                                        { .Role = rhi::BufferRole::StorageIndirect, .Size = ARGUMENT_BYTES },
                                        GetBytes(zeroArguments),
                                        gArguments)) == true &&
           IsAccepted(rhi::CreateBuffer(gDevice,
                                        { .Role = rhi::BufferRole::Storage, .Size = MARK_BYTES },
                                        GetBytes(zeroMarks),
                                        gMarks)) == true &&
           IsAccepted(rhi::CreateBuffer(gDevice,
                                        { .Role = rhi::BufferRole::Storage, .Size = MARK_BYTES },
                                        GetBytes(zeroMarks),
                                        gEmptyMarks)) == true &&
           CreateKernel(prepare, gPrepare) == true &&
           CreateKernel(ludus::shaders::indirect_mark::Compute(), gMark) == true &&
           CreateKernel(ludus::shaders::indirect_mark::Compute(), gEmptyMark) == true;
}
bool AreResourcesReady() noexcept
{
    const bool isCommonReady = IsReady(gPositions) == true && IsReady(gColors) == true && IsReady(gIndices) == true &&
                               IsReady(gVertex) == true && IsReady(gFragment) == true &&
                               IsReady(gRasterLayout) == true && IsReady(gTarget) == true;
    if (gPath == Path::Direct)
    {
        return isCommonReady == true && IsReady(gRightIndices) == true;
    }
    return isCommonReady == true && IsReady(gArguments) == true && IsReady(gMarks) == true &&
           IsReady(gEmptyMarks) == true && IsReady(gPrepare.Shader) == true && IsReady(gPrepare.Layout) == true &&
           IsReady(gMark.Shader) == true && IsReady(gMark.Layout) == true && IsReady(gEmptyMark.Shader) == true &&
           IsReady(gEmptyMark.Layout) == true;
}
bool CreateDispatch(uint32 slot, rhi::BufferHandle buffer, usize size, Kernel& inoutKernel) noexcept
{
    const rhi::RasterBindingResource resource{slot, buffer, 0, size};
    return IsAccepted(
               rhi::CreateBindingSet(gDevice, inoutKernel.Layout, {&resource, 1}, inoutKernel.Dispatch.Bindings)) ==
               true &&
           IsAccepted(rhi::CreateComputePipeline(gDevice,
                                                 { .Shader = inoutKernel.Shader, .Layout = inoutKernel.Layout },
                                                 inoutKernel.Dispatch.Pipeline)) == true;
}
bool CreatePipelines() noexcept
{
    const StaticArray<rhi::RasterVertexStream, STREAM_COUNT> streams{{{POSITION_STRIDE, false}, {COLOR_STRIDE, true}}};
    const StaticArray<rhi::RasterVertexAttribute, STREAM_COUNT> attributes{
        {{ludus::shaders::indirect_draw::locations::INPUT_POSITION_CLIP, 0, 0, rhi::RasterVertexFormat::Float2},
         {ludus::shaders::indirect_draw::locations::INPUT_COLOR, 1, 0, rhi::RasterVertexFormat::Float4}}};
    const bool isRasterCreated = IsAccepted(rhi::CreateBindingSet(gDevice, gRasterLayout, {}, gRasterSet)) == true &&
                                 IsAccepted(rhi::CreateRasterPipeline(gDevice,
                                                                      {gVertex,
                                                                       gFragment,
                                                                       gRasterLayout,
                                                                       {streams.GetData(), streams.GetSize()},
                                                                       {attributes.GetData(), attributes.GetSize()},
                                                                       false,
                                                                       false,
                                                                       rhi::RasterTarget::Rgba8Unorm},
                                                                      gPipeline)) == true;
    if (isRasterCreated == false || gPath == Path::Direct)
    {
        return isRasterCreated;
    }
    // Both prepare programs come from indirect_commands.lusl and share its slot table.
    constexpr uint32 ARGUMENT_SLOT = ludus::shaders::indirect_prepare::slots::ARGUMENTS;
    constexpr uint32 MARK_SLOT = ludus::shaders::indirect_mark::slots::MARKS;
    if (CreateDispatch(ARGUMENT_SLOT, gArguments, ARGUMENT_BYTES, gPrepare) == false ||
        CreateDispatch(MARK_SLOT, gMarks, MARK_BYTES, gMark) == false ||
        CreateDispatch(MARK_SLOT, gEmptyMarks, MARK_BYTES, gEmptyMark) == false)
    {
        return false;
    }
    for (Kernel* kernel : {&gMark, &gEmptyMark})
    {
        kernel->Dispatch.Groups[0] = MARK_COUNT;
        kernel->Dispatch.Indirect = gArguments;
    }
    gMark.Dispatch.IndirectOffset = DISPATCH_OFFSET;
    gEmptyMark.Dispatch.IndirectOffset = EMPTY_DISPATCH_OFFSET;
    return true;
}
bool ArePipelinesReady() noexcept
{
    const bool isRasterReady = IsReady(gRasterSet) == true && IsReady(gPipeline) == true;
    if (gPath == Path::Direct)
    {
        return isRasterReady;
    }
    return isRasterReady == true && IsReady(gPrepare.Dispatch.Bindings) == true &&
           IsReady(gPrepare.Dispatch.Pipeline) == true && IsReady(gMark.Dispatch.Bindings) == true &&
           IsReady(gMark.Dispatch.Pipeline) == true && IsReady(gEmptyMark.Dispatch.Bindings) == true &&
           IsReady(gEmptyMark.Dispatch.Pipeline) == true;
}
bool AddComputePasses(rhi::GraphVersion argumentsInput,
                      rhi::GraphVersion& outArguments,
                      rhi::GraphVersion& outMarks,
                      rhi::GraphVersion& outEmptyMarks) noexcept
{
    rhi::GraphVersion marksInput;
    rhi::GraphVersion emptyMarksInput;
    if (rhi::ImportGraphBuffer(gDevice, gGraph, gMarks, marksInput) != rhi::RasterStatus::Ready ||
        rhi::ImportGraphBuffer(gDevice, gGraph, gEmptyMarks, emptyMarksInput) != rhi::RasterStatus::Ready ||
        rhi::NextGraphVersion(gDevice, gGraph, argumentsInput, outArguments) != rhi::RasterStatus::Ready ||
        rhi::NextGraphVersion(gDevice, gGraph, marksInput, outMarks) != rhi::RasterStatus::Ready ||
        rhi::NextGraphVersion(gDevice, gGraph, emptyMarksInput, outEmptyMarks) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    const rhi::GraphUse prepareUse{outArguments,
                                   rhi::GraphAccessMode::StorageReadWrite,
                                   0,
                                   ARGUMENT_BYTES,
                                   rhi::RasterVisibility::Compute};
    const StaticArray<rhi::GraphUse, 2> markUses{{
        {outMarks, rhi::GraphAccessMode::StorageReadWrite, 0, MARK_BYTES, rhi::RasterVisibility::Compute},
        {outArguments, rhi::GraphAccessMode::Indirect, DISPATCH_OFFSET, sizeof(rhi::DispatchIndirectArguments)},
    }};
    const StaticArray<rhi::GraphUse, 2> emptyMarkUses{{
        {outEmptyMarks, rhi::GraphAccessMode::StorageReadWrite, 0, MARK_BYTES, rhi::RasterVisibility::Compute},
        {outArguments, rhi::GraphAccessMode::Indirect, EMPTY_DISPATCH_OFFSET, sizeof(rhi::DispatchIndirectArguments)},
    }};
    return rhi::AddGraphPass(gDevice,
                             gGraph,
                             {
                                 .Name = "write indirect commands",
                                 .Attachment = {},
                                 .Uses = &prepareUse,
                                 .UseCount = 1,
                                 .Dispatch = &gPrepare.Dispatch,
                             }) == rhi::RasterStatus::Ready &&
           rhi::AddGraphPass(gDevice,
                             gGraph,
                             {
                                 .Name = "indirect dispatch",
                                 .Attachment = {},
                                 .Uses = markUses.GetData(),
                                 .UseCount = markUses.GetSize(),
                                 .Dispatch = &gMark.Dispatch,
                             }) == rhi::RasterStatus::Ready &&
           rhi::AddGraphPass(gDevice,
                             gGraph,
                             {
                                 .Name = "empty indirect dispatch",
                                 .Attachment = {},
                                 .Uses = emptyMarkUses.GetData(),
                                 .UseCount = emptyMarkUses.GetSize(),
                                 .Dispatch = &gEmptyMark.Dispatch,
                             }) == rhi::RasterStatus::Ready;
}
// The R12a commands: two strided indexed commands (the second culled) and one non-indexed command.
bool AddIndirectDraws(rhi::GraphVersion positions,
                      rhi::GraphVersion colors,
                      rhi::GraphVersion indices,
                      rhi::GraphVersion arguments,
                      rhi::GraphVersion target) noexcept
{
    const StaticArray<rhi::GraphUse, 5> uses{{
        {positions, rhi::GraphAccessMode::Vertex, 0, sizeof(POSITIONS)},
        {colors, rhi::GraphAccessMode::Vertex, 0, sizeof(COLORS)},
        {indices, rhi::GraphAccessMode::Index, 0, sizeof(INDICES)},
        // One declaration covers both packets: indexed commands at 0 and 40, non-indexed at 64.
        {arguments, rhi::GraphAccessMode::Indirect, 0, DRAW_ARGUMENT_SPAN},
        {target, rhi::GraphAccessMode::ColorWrite},
    }};
    const bool isZero = gPath == Path::IndirectZeroFirstInstance;
    // Without HasFirstInstance each packet's instance slice starts at its color.
    const StaticArray<rhi::RasterVertexSlice, STREAM_COUNT> indexedSlices{{
        {gPositions, 0, sizeof(POSITIONS)},
        isZero == true ? rhi::RasterVertexSlice{gColors, usize{GREEN_INSTANCE} * COLOR_STRIDE, COLOR_STRIDE}
                       : rhi::RasterVertexSlice{gColors, 0, sizeof(COLORS)},
    }};
    const StaticArray<rhi::RasterVertexSlice, STREAM_COUNT> nonIndexedSlices{{
        {gPositions, 0, sizeof(POSITIONS)},
        isZero == true ? rhi::RasterVertexSlice{gColors, usize{BLUE_INSTANCE} * COLOR_STRIDE, COLOR_STRIDE}
                       : rhi::RasterVertexSlice{gColors, 0, sizeof(COLORS)},
    }};
    const rhi::RasterDraw indexedDraw
    {
        .Pipeline = gPipeline,
        .Bindings = gRasterSet,
        .Vertices = {indexedSlices.GetData(), indexedSlices.GetSize()},
        .Indices = gIndices,
        .IndexCount = static_cast<uint32>(INDEX_VALUES),
        .VertexCount = VERTEX_COUNT,
        .InstanceCount = isZero == true ? 1 : INSTANCE_COUNT,
        .Indirect = gArguments,
        .IndirectCount = INDEXED_COMMANDS,
        .IndirectStride = INDEXED_STRIDE,
    };
    const rhi::RasterDraw nonIndexedDraw
    {
        .Pipeline = gPipeline,
        .Bindings = gRasterSet,
        .Vertices = {nonIndexedSlices.GetData(), nonIndexedSlices.GetSize()},
        .VertexCount = VERTEX_COUNT,
        .InstanceCount = isZero == true ? 1 : INSTANCE_COUNT,
        .Indirect = gArguments,
        .IndirectOffset = NON_INDEXED_OFFSET,
    };
    // The zero layout draws the right half first, so a poison or culled full-image command
    // (in the indexed packet's green) would cover it.
    const StaticArray<rhi::RasterDraw, DRAW_COUNT> draws =
        isZero == true ? StaticArray<rhi::RasterDraw, DRAW_COUNT>{{nonIndexedDraw, indexedDraw}}
                       : StaticArray<rhi::RasterDraw, DRAW_COUNT>{{indexedDraw, nonIndexedDraw}};
    return rhi::AddGraphPass(gDevice,
                             gGraph,
                             {
                                 .Name = "draw indirect commands",
                                 .Attachment = { .Color = gTarget },
                                 .Uses = uses.GetData(),
                                 .UseCount = uses.GetSize(),
                                 .Draws = draws.GetData(),
                                 .DrawCount = draws.GetSize(),
                             }) == rhi::RasterStatus::Ready;
}
// The authored direct variant: the same two quads with CPU-chosen offsets and colors.
bool AddDirectDraws(rhi::GraphVersion positions,
                    rhi::GraphVersion colors,
                    rhi::GraphVersion indices,
                    rhi::GraphVersion rightIndices,
                    rhi::GraphVersion target) noexcept
{
    const StaticArray<rhi::GraphUse, 5> uses{{
        {positions, rhi::GraphAccessMode::Vertex, 0, sizeof(POSITIONS)},
        {colors, rhi::GraphAccessMode::Vertex, 0, sizeof(COLORS)},
        {indices, rhi::GraphAccessMode::Index, 0, sizeof(INDICES)},
        {rightIndices, rhi::GraphAccessMode::Index, 0, sizeof(RIGHT_INDICES)},
        {target, rhi::GraphAccessMode::ColorWrite},
    }};
    const StaticArray<rhi::RasterVertexSlice, STREAM_COUNT> leftSlices{{
        {gPositions, usize{LEFT_FIRST_VERTEX} * POSITION_STRIDE, usize{LEFT_VERTEX_COUNT} * POSITION_STRIDE},
        {gColors, usize{GREEN_INSTANCE} * COLOR_STRIDE, COLOR_STRIDE},
    }};
    const StaticArray<rhi::RasterVertexSlice, STREAM_COUNT> rightSlices{{
        {gPositions, usize{RIGHT_FIRST_VERTEX} * POSITION_STRIDE, usize{RIGHT_VERTEX_COUNT} * POSITION_STRIDE},
        {gColors, usize{BLUE_INSTANCE} * COLOR_STRIDE, COLOR_STRIDE},
    }};
    const StaticArray<rhi::RasterDraw, DRAW_COUNT> draws{{
        {
            .Pipeline = gPipeline,
            .Bindings = gRasterSet,
            .Vertices = {leftSlices.GetData(), leftSlices.GetSize()},
            .Indices = gIndices,
            .IndexOffset = QUAD_FIRST_INDEX * sizeof(uint32),
            .IndexCount = QUAD_INDEX_COUNT,
            .VertexCount = LEFT_VERTEX_COUNT,
            .InstanceCount = 1,
        },
        {
            .Pipeline = gPipeline,
            .Bindings = gRasterSet,
            .Vertices = {rightSlices.GetData(), rightSlices.GetSize()},
            .Indices = gRightIndices,
            .IndexCount = QUAD_INDEX_COUNT,
            .VertexCount = RIGHT_VERTEX_COUNT,
            .InstanceCount = 1,
        },
    }};
    return rhi::AddGraphPass(gDevice,
                             gGraph,
                             {
                                 .Name = "draw direct variant",
                                 .Attachment = { .Color = gTarget },
                                 .Uses = uses.GetData(),
                                 .UseCount = uses.GetSize(),
                                 .Draws = draws.GetData(),
                                 .DrawCount = draws.GetSize(),
                             }) == rhi::RasterStatus::Ready;
}
bool CompileGraph() noexcept
{
    rhi::GraphVersion positions;
    rhi::GraphVersion colors;
    rhi::GraphVersion indices;
    rhi::GraphVersion targetInput;
    rhi::GraphVersion target;
    rhi::RasterTextureState targetState;
    if (rhi::GetTextureState(gDevice, gTarget, targetState) != rhi::RasterStatus::Ready ||
        rhi::CreateOrderedGraph(gDevice, gGraph) != rhi::RasterStatus::Ready ||
        rhi::ImportGraphBuffer(gDevice, gGraph, gPositions, positions) != rhi::RasterStatus::Ready ||
        rhi::ImportGraphBuffer(gDevice, gGraph, gColors, colors) != rhi::RasterStatus::Ready ||
        rhi::ImportGraphBuffer(gDevice, gGraph, gIndices, indices) != rhi::RasterStatus::Ready ||
        rhi::ImportGraphTexture(gDevice,
                                gGraph,
                                gTarget,
                                targetState,
                                rhi::RasterTextureUse::ColorAttachment,
                                targetInput) != rhi::RasterStatus::Ready ||
        rhi::NextGraphVersion(gDevice, gGraph, targetInput, target) != rhi::RasterStatus::Ready)
    {
        return false;
    }
    if (IsIndirectPath() == true)
    {
        rhi::GraphVersion argumentsInput;
        rhi::GraphVersion arguments;
        rhi::GraphVersion marks;
        rhi::GraphVersion emptyMarks;
        if (rhi::ImportGraphBuffer(gDevice, gGraph, gArguments, argumentsInput) != rhi::RasterStatus::Ready ||
            AddComputePasses(argumentsInput, arguments, marks, emptyMarks) == false ||
            AddIndirectDraws(positions, colors, indices, arguments, target) == false ||
            rhi::AddGraphRoot(gDevice, gGraph, marks, rhi::GraphRoot::Readback) != rhi::RasterStatus::Ready ||
            rhi::AddGraphRoot(gDevice, gGraph, emptyMarks, rhi::GraphRoot::Readback) != rhi::RasterStatus::Ready)
        {
            return false;
        }
    }
    else
    {
        rhi::GraphVersion rightIndices;
        if (rhi::ImportGraphBuffer(gDevice, gGraph, gRightIndices, rightIndices) != rhi::RasterStatus::Ready ||
            AddDirectDraws(positions, colors, indices, rightIndices, target) == false)
        {
            return false;
        }
    }
    rhi::GraphReport report;
    const rhi::RasterStatus root = rhi::AddGraphRoot(gDevice, gGraph, target, rhi::GraphRoot::Readback);
    const rhi::RasterStatus compiled =
        root == rhi::RasterStatus::Ready ? rhi::CompileOrderedGraph(gDevice, gGraph, report) : root;
    ReportValue("computePasses", report.ComputePasses);
    ReportValue("rasterPasses", report.RasterPasses);
    ReportValue("draws", report.DrawCount);
    if (compiled != rhi::RasterStatus::Ready)
    {
        LUDUS_LOG_ERROR(LOG_INDIRECT,
                        "Graph compile {}; error pass {} resource {}",
                        static_cast<uint32>(compiled),
                        report.ErrorPass,
                        report.ErrorResource);
        return false;
    }
    const uint32 expectedComputePasses = IsIndirectPath() == true ? 3 : 0;
    return report.ComputePasses == expectedComputePasses && report.RasterPasses == 1 && report.DrawCount == DRAW_COUNT;
}
bool RequestReadbacks() noexcept
{
    if (rhi::RequestTextureReadback(gDevice, gTarget, {0, 0, 0, WIDTH, HEIGHT}, ROW_BYTES, gImageReadback) !=
        rhi::RasterStatus::Pending)
    {
        return false;
    }
    hasImage = false;
    hasMarks = IsIndirectPath() == false;
    hasEmptyMarks = IsIndirectPath() == false;
    return IsIndirectPath() == false ||
           (rhi::RequestBufferReadback(gDevice, gMarks, 0, MARK_BYTES, gMarksReadback) == rhi::RasterStatus::Pending &&
            rhi::RequestBufferReadback(gDevice, gEmptyMarks, 0, MARK_BYTES, gEmptyMarksReadback) ==
                rhi::RasterStatus::Pending);
}
// Copy a ready readback once; false on failure. inoutHasCopied stays false while Pending.
bool TryCopy(rhi::ReadbackTicket ticket, uint8* destination, usize size, bool& inoutHasCopied) noexcept
{
    if (inoutHasCopied == true)
    {
        return true;
    }
    const rhi::RasterStatus copied = rhi::CopyReadback(gDevice, ticket, destination, size);
    if (copied == rhi::RasterStatus::Pending)
    {
        return true;
    }
    inoutHasCopied = true;
    return copied == rhi::RasterStatus::Ready && rhi::Release(gDevice, ticket) == rhi::RasterStatus::Ready;
}
bool Verify() noexcept
{
    for (uint32 group = 0; group < MARK_COUNT && IsIndirectPath() == true; ++group)
    {
        const uint32 expected = group < MARKED_GROUPS ? MARK_BASE + group : 0;
        if (gMarkValues[group] != expected || gEmptyMarkValues[group] != 0)
        {
            return false;
        }
    }
    for (usize index = 0; index < gPixels.GetSize(); ++index)
    {
        const usize column = (index / CHANNELS) % WIDTH;
        const StaticArray<uint8, CHANNELS>& expected = column < WIDTH / 2 ? LEFT_COLOR : RIGHT_COLOR;
        if (gPixels[index] != expected[index % CHANNELS])
        {
            if (gMismatchCount == 0)
            {
                gFirstMismatch = index;
            }
            ++gMismatchCount;
        }
    }
    return gMismatchCount == 0;
}
void Tick() noexcept
{
    if (isDone == true)
    {
        return;
    }
    rhi::DeviceInfo info;
    const rhi::DeviceStatus status = rhi::GetDeviceInfo(gDevice, info);
    if (status != rhi::DeviceStatus::Ready)
    {
        if (status != rhi::DeviceStatus::Pending)
        {
            Fail("device not ready");
        }
        return;
    }
    if (rhi::PollLifetime(gDevice) != rhi::RasterStatus::Ready)
    {
        Fail("lifetime poll");
        return;
    }
    if (gStage == Stage::CheckProfile)
    {
        if (CheckProfile(info) == false)
        {
            Fail("unexpected indirect profile");
            return;
        }
        gStage = Stage::CreateResources;
    }
    if (gStage == Stage::CreateResources)
    {
        if (CreateResources() == false)
        {
            Fail("resource creation");
            return;
        }
        gStage = Stage::CreatePipelines;
    }
    if (gStage == Stage::CreatePipelines)
    {
        if (AreResourcesReady() == false)
        {
            return;
        }
        if (CreatePipelines() == false)
        {
            Fail("pipeline creation");
            return;
        }
        gStage = Stage::CompileGraph;
    }
    if (gStage == Stage::CompileGraph)
    {
        if (ArePipelinesReady() == false)
        {
            return;
        }
        if (rhi::SetFrameTarget(gDevice, gSurface, {WIDTH, HEIGHT, 0, 0, 0, 1}) != rhi::DeviceStatus::Ready ||
            CompileGraph() == false)
        {
            Fail("graph compilation");
            return;
        }
        gStage = Stage::Execute;
    }
    if (gStage == Stage::Execute)
    {
        rhi::SubmissionToken completion;
        const rhi::RasterStatus executed = rhi::ExecuteOrderedGraph(gDevice, gSurface, gGraph, completion);
        if (executed == rhi::RasterStatus::NotReady || executed == rhi::RasterStatus::CapacityExceeded)
        {
            return;
        }
        if (executed != rhi::RasterStatus::Ready || RequestReadbacks() == false)
        {
            Fail("graph execution");
            return;
        }
        gStage = Stage::ReadBack;
    }
    if (gStage == Stage::ReadBack)
    {
        if (TryCopy(gImageReadback, gPixels.GetData(), PIXEL_BYTES, hasImage) == false ||
            TryCopy(gMarksReadback, reinterpret_cast<uint8*>(gMarkValues.GetData()), MARK_BYTES, hasMarks) == false ||
            TryCopy(gEmptyMarksReadback,
                    reinterpret_cast<uint8*>(gEmptyMarkValues.GetData()),
                    MARK_BYTES,
                    hasEmptyMarks) == false)
        {
            Fail("readback");
            return;
        }
        if (hasImage == false || hasMarks == false || hasEmptyMarks == false)
        {
            return;
        }
        Stop(Verify() == false);
    }
}
rhi::DeviceDescription MakeDescription(rhi::BackendSelection selection) noexcept
{
    rhi::DeviceDescription description;
    description.Selection = selection;
    description.Required.PortableRaster = true;
    description.Required.OrderedRasterGraph = true;
    description.Required.IndirectRendering = selection == rhi::BackendSelection::WebGPU;
    description.Preferred.Compute = true;
    description.Preferred.IndirectRendering = true;
    description.RequiredProfile.Raster.MinVertexBuffers = STREAM_COUNT;
    description.RequiredProfile.FormatCount = 1;
    description.RequiredProfile.Formats[0] =
    {
        .Format = rhi::RasterFormat::Rgba8Unorm,
        .Uses = { .ColorAttachment = true, .CopySource = true },
        .MinDimension2D = WIDTH,
    };
    return description;
}
// WebGL 2 cannot satisfy a required IndirectRendering workload; admission fails before startup.
bool IsRequiredIndirectRejected(const rhi::WindowInfo& window) noexcept
{
    rhi::DeviceDescription description = MakeDescription(rhi::BackendSelection::WebGL2);
    description.Required.IndirectRendering = true;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    return rhi::CreateDevice({}, window, description, device, surface) == rhi::DeviceStatus::Unsupported &&
           device == rhi::DeviceHandle{};
}
bool AreEqual(const char* left, const char* right) noexcept
{
    usize i = 0;
    while (left[i] != '\0' && left[i] == right[i])
    {
        ++i;
    }
    return left[i] == right[i];
}
} // namespace
int main(int argumentCount, char** arguments)
{
    (void)logging::LogSystem::Initialize({ .EnableFile = false, .Directory = {} });
    // Native ctest also runs the zero-FirstInstance layout on a device that has the feature.
    shouldForceZeroFirstInstance = argumentCount > 1 && AreEqual(arguments[1], "--zero-first-instance") == true;
    rhi::WindowInfo window{ .Width = WIDTH, .Height = HEIGHT };
#if defined(LUDUS_PLATFORM_WEB)
    ludus::platform::WindowManager manager;
    ludus::platform::WindowBase::CreateInfo create;
    create.Width = WIDTH;
    create.Height = HEIGHT;
    create.CanvasSelector = "#canvas";
    create.CaptureBrowserInput = false;
    if (manager.Initialize({}) == false || manager.CreateWindow(create, gWindow) == false)
    {
        return 1;
    }
    window = gWindow->GetNativeWindowInfo();
#endif
    const int32 selection = GetSelection();
    if (selection == SELECT_WEBGL2)
    {
        isRequiredRejected = IsRequiredIndirectRejected(window);
        if (isRequiredRejected == false)
        {
            Fail("WebGL 2 admitted a required IndirectRendering workload");
            return 1;
        }
    }
    const rhi::DeviceDescription description =
        MakeDescription(selection == SELECT_WEBGPU   ? rhi::BackendSelection::WebGPU
                        : selection == SELECT_WEBGL2 ? rhi::BackendSelection::WebGL2
                                                     : rhi::BackendSelection::Auto);
    rhi::StartupInfo failure;
    const rhi::DeviceStatus created = rhi::CreateDevice({}, window, description, gDevice, gSurface, &failure);
    if (created != rhi::DeviceStatus::Ready && created != rhi::DeviceStatus::Pending)
    {
#if !defined(LUDUS_PLATFORM_WEB)
        if (failure.Error == rhi::StartupError::AdapterUnavailable)
        {
            return 0;
        }
#endif
        LUDUS_LOG_ERROR(LOG_INDIRECT, "Device creation failed: {}", static_cast<uint32>(created));
        Stop(true);
        return 1;
    }
#if defined(LUDUS_PLATFORM_WEB)
    emscripten_set_main_loop(Tick, 0, 1);
#else
    const uint64 started = time::NowTicks();
    while (isDone == false && time::NowTicks() - started < NATIVE_TIMEOUT_NANOSECONDS)
    {
        Tick();
    }
    if (isDone == false)
    {
        Fail("timeout");
    }
#endif
    return hasFailed == true ? 1 : 0;
}
