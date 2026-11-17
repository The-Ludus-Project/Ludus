// GPU evidence for LUSL L5a (docs/modules/tooling/shader-language.md#resources-and-bindings):
// texture and buffer size queries, every Interlocked operation and both group barriers. Each LUSL
// kernel in tests/lusl_shared_memory runs beside its Slang equivalent with the same inputs; the
// results must match word for word and match the values the host computes.

#include "lusl_atomics.h"
#include "lusl_atomics_slots.h"
#include "lusl_barriers.h"
#include "lusl_barriers_slots.h"
#include "lusl_buffer_queries.h"
#include "lusl_buffer_queries_slots.h"
#include "lusl_queries.h"
#include "lusl_queries_slots.h"
#include "slang_atomics.h"
#include "slang_barriers.h"
#include "slang_buffer_queries.h"
#include "slang_queries.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/compute.h>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace shaders = ludus::shaders;
using S = rhi::RasterStatus;

constexpr uint64 TIMEOUT_NANOSECONDS = 10 * time::NANOSECONDS_PER_SECOND;
/// Most buffers one kernel binds.
constexpr usize MAX_KERNEL_BUFFERS = 3;
/// Threads per group of the atomics and barriers kernels, and the groups they run.
constexpr uint32 GROUP_SIZE = 64;
constexpr uint32 GROUP_COUNT = 2;
constexpr uint32 THREAD_COUNT = GROUP_SIZE * GROUP_COUNT;
/// Every bit set: the seed of minimum and AND reductions.
constexpr uint32 ALL_BITS = 0xffffffffU;

template <typename Handle>
void WaitReady(rhi::DeviceHandle device, Handle handle)
{
    const uint64 start = time::NowTicks();
    S status = rhi::GetStatus(device, handle);
    while (status == S::Pending && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
    {
        REQUIRE(rhi::PollLifetime(device) == S::Ready);
        status = rhi::GetStatus(device, handle);
    }
    REQUIRE(status == S::Ready);
}

bool IsAccepted(S status) noexcept
{
    return status == S::Ready || status == S::Pending;
}

/// One storage buffer of a kernel: its slot, initial words and whether the kernel writes it.
struct KernelBuffer final
{
    uint32 Slot = 0;
    Array<uint32> Words;
    bool IsWritten = true;
};

/// The storage buffers of one kernel run, in binding order.
using KernelBuffers = Array<KernelBuffer>;

/// An optional sampled texture of a kernel.
struct KernelTexture final
{
    uint32 Slot = 0;
    uint32 Width = 0;
    uint32 Height = 0;
};

class Device final
{
public:
    Device()
    {
        rhi::Shutdown();
        rhi::DeviceDescription description;
        description.Required.Compute = true;
        mStarted = rhi::CreateDevice({}, { .Width = 8, .Height = 8 }, description, mDevice, mSurface);
    }
    ~Device()
    {
        rhi::Shutdown();
    }
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] bool IsAvailable() const noexcept
    {
#if defined(LUDUS_TEST_METAL)
        if (mStarted != rhi::DeviceStatus::Ready && rhi::GetStartup().Error == rhi::StartupError::AdapterUnavailable)
        {
            return false;
        }
#endif
        return true;
    }

    /// Run `shader` over `groups` groups and return the final words of every buffer, in order.
    void Run(const rhi::RasterShaderDescription& shader,
             KernelBuffers& inoutBuffers,
             const KernelTexture* textureOrNull,
             uint32 groups)
    {
        REQUIRE(mStarted == rhi::DeviceStatus::Ready);
        StaticArray<rhi::BufferHandle, MAX_KERNEL_BUFFERS> handles{};
        REQUIRE(inoutBuffers.GetSize() <= MAX_KERNEL_BUFFERS);
        for (usize i = 0; i < inoutBuffers.GetSize(); ++i)
        {
            const Array<uint32>& words = inoutBuffers[i].Words;
            const BorrowedView<const uint8> bytes{reinterpret_cast<const uint8*>(words.GetData()),
                                                  words.GetSize() * sizeof(uint32)};
            REQUIRE(IsAccepted(rhi::CreateBuffer(mDevice,
                                                 { .Role = rhi::BufferRole::Storage, .Size = bytes.GetSize() },
                                                 bytes,
                                                 handles[i])) == true);
            WaitReady(mDevice, handles[i]);
        }
        rhi::TextureHandle texture;
        rhi::TextureViewHandle view;
        Array<uint8> texels;
        if (textureOrNull != nullptr)
        {
            constexpr usize RGBA_BYTES = 4;
            const usize rowBytes = usize{textureOrNull->Width} * RGBA_BYTES;
            REQUIRE(texels.TryResize(rowBytes * textureOrNull->Height, uint8{0}) == true);
            const rhi::TextureUpload upload{{texels.GetData(), texels.GetSize()}, rowBytes};
            REQUIRE(IsAccepted(rhi::CreateTexture(mDevice,
                                                  { .Width = textureOrNull->Width, .Height = textureOrNull->Height },
                                                  upload,
                                                  texture)) == true);
            WaitReady(mDevice, texture);
            REQUIRE(rhi::CreateTextureView(mDevice, texture, view) == S::Ready);
        }
        rhi::RasterShaderHandle shaderHandle;
        rhi::BindingLayoutHandle layout;
        REQUIRE(IsAccepted(rhi::CreateRasterShader(mDevice, shader, shaderHandle)) == true);
        REQUIRE(IsAccepted(rhi::CreateBindingLayout(mDevice, shader.Bindings, layout)) == true);
        WaitReady(mDevice, shaderHandle);
        WaitReady(mDevice, layout);
        StaticArray<rhi::RasterBindingResource, MAX_KERNEL_BUFFERS + 1> resources{};
        usize resourceCount = 0;
        for (usize i = 0; i < inoutBuffers.GetSize(); ++i)
        {
            resources[resourceCount] =
            {
                .Binding = inoutBuffers[i].Slot,
                .Buffer = handles[i],
                .Size = inoutBuffers[i].Words.GetSize() * sizeof(uint32),
            };
            ++resourceCount;
        }
        if (textureOrNull != nullptr)
        {
            resources[resourceCount] = { .Binding = textureOrNull->Slot, .Texture = view };
            ++resourceCount;
        }
        rhi::BindingSetHandle set;
        rhi::ComputePipelineHandle pipeline;
        REQUIRE(IsAccepted(rhi::CreateBindingSet(mDevice, layout, {resources.GetData(), resourceCount}, set)) == true);
        REQUIRE(IsAccepted(rhi::CreateComputePipeline(mDevice,
                                                      {
                                                          .Shader = shaderHandle,
                                                          .Layout = layout,
                                                      },
                                                      pipeline)) == true);
        WaitReady(mDevice, set);
        WaitReady(mDevice, pipeline);

        rhi::OrderedGraph graph;
        REQUIRE(rhi::CreateOrderedGraph(mDevice, graph) == S::Ready);
        StaticArray<rhi::GraphUse, MAX_KERNEL_BUFFERS + 1> uses;
        usize useCount = 0;
        for (usize i = 0; i < inoutBuffers.GetSize(); ++i)
        {
            rhi::GraphVersion input;
            REQUIRE(rhi::ImportGraphBuffer(mDevice, graph, handles[i], input) == S::Ready);
            rhi::GraphVersion written;
            if (inoutBuffers[i].IsWritten == true)
            {
                REQUIRE(rhi::NextGraphVersion(mDevice, graph, input, written) == S::Ready);
            }
            const rhi::GraphVersion used = inoutBuffers[i].IsWritten == true ? written : input;
            uses[useCount] = {used,
                              inoutBuffers[i].IsWritten == true ? rhi::GraphAccessMode::StorageReadWrite
                                                                : rhi::GraphAccessMode::StorageRead,
                              0,
                              inoutBuffers[i].Words.GetSize() * sizeof(uint32),
                              rhi::RasterVisibility::Compute};
            ++useCount;
        }
        if (textureOrNull != nullptr)
        {
            rhi::RasterTextureState state;
            REQUIRE(rhi::GetTextureState(mDevice, texture, state) == S::Ready);
            rhi::GraphVersion sampled;
            REQUIRE(rhi::ImportGraphTexture(mDevice,
                                            graph,
                                            texture,
                                            state,
                                            rhi::RasterTextureUse::SampledCompute,
                                            sampled) == S::Ready);
            uses[useCount] =
            {
                .Resource = sampled,
                .Access = rhi::GraphAccessMode::Sampled,
                .Visibility = rhi::RasterVisibility::Compute,
            };
            ++useCount;
        }
        const rhi::ComputeDispatch dispatch
        {
            .Pipeline = pipeline,
            .Bindings = set,
            .Groups = {groups, 1, 1},
        };
        rhi::GraphPassDescription pass;
        pass.Name = "lusl shared memory";
        pass.Uses = uses.GetData();
        pass.UseCount = useCount;
        pass.SideEffect = true;
        pass.Dispatch = &dispatch;
        REQUIRE(rhi::AddGraphPass(mDevice, graph, pass) == S::Ready);
        rhi::GraphReport report;
        REQUIRE(rhi::CompileOrderedGraph(mDevice, graph, report) == S::Ready);
        REQUIRE(rhi::SetFrameTarget(mDevice, mSurface, {16, 16, 0, 0, 0, 1}) == rhi::DeviceStatus::Ready);
        rhi::SubmissionToken completion;
        const uint64 start = time::NowTicks();
        S executed = rhi::ExecuteOrderedGraph(mDevice, mSurface, graph, completion);
        while (executed == S::NotReady && time::NowTicks() - start < TIMEOUT_NANOSECONDS)
        {
            executed = rhi::ExecuteOrderedGraph(mDevice, mSurface, graph, completion);
        }
        REQUIRE(executed == S::Ready);

        for (usize i = 0; i < inoutBuffers.GetSize(); ++i)
        {
            Array<uint32>& words = inoutBuffers[i].Words;
            const usize size = words.GetSize() * sizeof(uint32);
            rhi::ReadbackTicket ticket;
            S readback = rhi::RequestBufferReadback(mDevice, handles[i], 0, size, ticket);
            const uint64 readbackStart = time::NowTicks();
            while (readback == S::Pending && time::NowTicks() - readbackStart < TIMEOUT_NANOSECONDS)
            {
                REQUIRE(rhi::PollLifetime(mDevice) == S::Ready);
                readback = rhi::GetStatus(mDevice, ticket);
            }
            REQUIRE(readback == S::Ready);
            REQUIRE(rhi::CopyReadback(mDevice, ticket, reinterpret_cast<uint8*>(words.GetData()), size) == S::Ready);
            REQUIRE(rhi::Release(mDevice, ticket) == S::Ready);
        }
        REQUIRE(rhi::Destroy(mDevice, pipeline) == S::Ready);
        REQUIRE(rhi::Destroy(mDevice, set) == S::Ready);
        REQUIRE(rhi::Destroy(mDevice, layout) == S::Ready);
        REQUIRE(rhi::Destroy(mDevice, shaderHandle) == S::Ready);
    }

private:
    rhi::DeviceHandle mDevice;
    rhi::SurfaceHandle mSurface;
    rhi::DeviceStatus mStarted = rhi::DeviceStatus::Failed;
};

/// A buffer at `slot` of `count` words, each `value`.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): slot, then count, as KernelBuffer stores them.
KernelBuffer MakeBuffer(uint32 slot, usize count, uint32 value = 0, bool isWritten = true)
{
    KernelBuffer buffer;
    buffer.Slot = slot;
    buffer.IsWritten = isWritten;
    REQUIRE(buffer.Words.TryResize(count, value) == true);
    return buffer;
}

/// Run the LUSL kernel and its Slang equivalent on copies of `buffers`; require equal results.
KernelBuffers RunBoth(Device& inoutDevice,
                      const rhi::RasterShaderDescription& lusl,
                      const rhi::RasterShaderDescription& slang,
                      const KernelBuffers& buffers,
                      const KernelTexture* textureOrNull,
                      uint32 groups)
{
    KernelBuffers luslBuffers;
    KernelBuffers slangBuffers;
    for (const KernelBuffer& buffer : buffers)
    {
        KernelBuffer copy = MakeBuffer(buffer.Slot, buffer.Words.GetSize(), 0, buffer.IsWritten);
        for (usize i = 0; i < buffer.Words.GetSize(); ++i)
        {
            copy.Words[i] = buffer.Words[i];
        }
        KernelBuffer second = MakeBuffer(buffer.Slot, buffer.Words.GetSize(), 0, buffer.IsWritten);
        for (usize i = 0; i < buffer.Words.GetSize(); ++i)
        {
            second.Words[i] = buffer.Words[i];
        }
        REQUIRE(luslBuffers.TryAdd(Move(copy)) == true);
        REQUIRE(slangBuffers.TryAdd(Move(second)) == true);
    }
    inoutDevice.Run(lusl, luslBuffers, textureOrNull, groups);
    inoutDevice.Run(slang, slangBuffers, textureOrNull, groups);
    for (usize b = 0; b < luslBuffers.GetSize(); ++b)
    {
        for (usize i = 0; i < luslBuffers[b].Words.GetSize(); ++i)
        {
            CAPTURE(b, i);
            CHECK(luslBuffers[b].Words[i] == slangBuffers[b].Words[i]);
        }
    }
    return luslBuffers;
}

/// The bindings of a LUSL kernel are numbered in declaration order; its Slang equivalent spells
/// the same slots.
void RequireSameSlots(const rhi::RasterShaderDescription& lusl, const rhi::RasterShaderDescription& slang)
{
    REQUIRE(lusl.Bindings.GetSize() == slang.Bindings.GetSize());
    for (usize i = 0; i < lusl.Bindings.GetSize(); ++i)
    {
        CHECK(lusl.Bindings[i].Binding == slang.Bindings[i].Binding);
        CHECK(lusl.Bindings[i].Kind == slang.Bindings[i].Kind);
    }
}
} // namespace

TEST_CASE("LUSL GetDimensions matches Slang on the GPU", "[renderer][gpu][lusl]")
{
    Device device;
    if (device.IsAvailable() == false)
    {
        SKIP("No Metal adapter");
    }
    constexpr uint32 WIDTH = 5;
    constexpr uint32 HEIGHT = 3;
    constexpr usize RESULT_WORDS = 2;
    namespace queries = shaders::lusl_queries::slots;
    static_assert(queries::IMAGE == 0 && queries::RESULTS == 1);
    RequireSameSlots(shaders::lusl_queries::Compute(), shaders::slang_queries::Compute());
    const KernelTexture texture{queries::IMAGE, WIDTH, HEIGHT};
    KernelBuffers buffers;
    REQUIRE(buffers.TryAdd(MakeBuffer(queries::RESULTS, RESULT_WORDS)) == true);
    const KernelBuffers sizes =
        RunBoth(device, shaders::lusl_queries::Compute(), shaders::slang_queries::Compute(), buffers, &texture, 1);
    CHECK(sizes[0].Words[0] == WIDTH);
    CHECK(sizes[0].Words[1] == HEIGHT);

    constexpr usize VALUE_COUNT = 5;
    constexpr usize FLOAT4_WORDS = 4;
    namespace bufferQueries = shaders::lusl_buffer_queries::slots;
    static_assert(bufferQueries::VALUES == 0 && bufferQueries::RESULTS == 1);
    RequireSameSlots(shaders::lusl_buffer_queries::Compute(), shaders::slang_buffer_queries::Compute());
    KernelBuffers counted;
    REQUIRE(counted.TryAdd(MakeBuffer(bufferQueries::VALUES, VALUE_COUNT * FLOAT4_WORDS, 0, false)) == true);
    REQUIRE(counted.TryAdd(MakeBuffer(bufferQueries::RESULTS, FLOAT4_WORDS)) == true);
    const KernelBuffers counts = RunBoth(device,
                                         shaders::lusl_buffer_queries::Compute(),
                                         shaders::slang_buffer_queries::Compute(),
                                         counted,
                                         nullptr,
                                         1);
    CHECK(counts[1].Words[0] == VALUE_COUNT);
    CHECK(counts[1].Words[1] == FLOAT4_WORDS);
}

TEST_CASE("LUSL atomics match Slang on the GPU", "[renderer][gpu][lusl]")
{
    Device device;
    if (device.IsAvailable() == false)
    {
        SKIP("No Metal adapter");
    }
    namespace slots = shaders::lusl_atomics::slots;
    static_assert(slots::COUNTERS == 0 && slots::RESULTS == 1 && slots::BALANCES == 2);
    RequireSameSlots(shaders::lusl_atomics::Compute(), shaders::slang_atomics::Compute());
    // Word indices of atomics.lusl.
    constexpr usize COUNTER_WORDS = 16;
    constexpr usize MINIMUM = 2;
    constexpr usize AND_BITS = 5;
    constexpr usize BINS = 11;
    constexpr usize BIN_COUNT = 4;
    constexpr usize BALANCE_WORDS = 2;
    constexpr uint32 CLAIM_VALUE = 9;
    constexpr uint32 SWAP_VALUE = 5;
    KernelBuffer counters = MakeBuffer(slots::COUNTERS, COUNTER_WORDS);
    counters.Words[MINIMUM] = ALL_BITS;
    counters.Words[AND_BITS] = ALL_BITS;
    KernelBuffers buffers;
    REQUIRE(buffers.TryAdd(Move(counters)) == true);
    REQUIRE(buffers.TryAdd(MakeBuffer(slots::RESULTS, THREAD_COUNT)) == true);
    REQUIRE(buffers.TryAdd(MakeBuffer(slots::BALANCES, BALANCE_WORDS)) == true);
    const KernelBuffers results = RunBoth(device,
                                          shaders::lusl_atomics::Compute(),
                                          shaders::slang_atomics::Compute(),
                                          buffers,
                                          nullptr,
                                          GROUP_COUNT);
    const Array<uint32>& words = results[0].Words;
    CHECK(words[0] == THREAD_COUNT);
    CHECK(words[1] == THREAD_COUNT - 1);
    CHECK(words[MINIMUM] == 0);
    CHECK(words[3] == ALL_BITS);
    // The XOR of 0 to 127 is zero, since every group of four consecutive values cancels.
    CHECK(words[4] == 0);
    CHECK(words[AND_BITS] == 0);
    CHECK(words[6] == 1);
    CHECK(words[7] == CLAIM_VALUE);
    CHECK(words[8] == 1);
    CHECK(words[9] == SWAP_VALUE);
    CHECK(words[10] == SWAP_VALUE * (THREAD_COUNT - 1));
    for (usize bin = 0; bin < BIN_COUNT; ++bin)
    {
        CHECK(words[BINS + bin] == THREAD_COUNT / BIN_COUNT);
    }
    // Every ticket is handed out once.
    for (uint32 ticket = 0; ticket < THREAD_COUNT; ++ticket)
    {
        CAPTURE(ticket);
        CHECK(results[1].Words[ticket] == ticket + 1);
    }
    CHECK(static_cast<int32>(results[2].Words[0]) == -static_cast<int32>(THREAD_COUNT));
    CHECK(static_cast<int32>(results[2].Words[1]) == -static_cast<int32>(THREAD_COUNT - 1));
}

TEST_CASE("LUSL barriers match Slang on the GPU", "[renderer][gpu][lusl]")
{
    Device device;
    if (device.IsAvailable() == false)
    {
        SKIP("No Metal adapter");
    }
    namespace slots = shaders::lusl_barriers::slots;
    static_assert(slots::SUMS == 0 && slots::EXCHANGED == 1 && slots::NEIGHBORS == 2);
    RequireSameSlots(shaders::lusl_barriers::Compute(), shaders::slang_barriers::Compute());
    constexpr uint32 SCALE = 3;
    KernelBuffers buffers;
    REQUIRE(buffers.TryAdd(MakeBuffer(slots::SUMS, GROUP_COUNT)) == true);
    REQUIRE(buffers.TryAdd(MakeBuffer(slots::EXCHANGED, THREAD_COUNT)) == true);
    REQUIRE(buffers.TryAdd(MakeBuffer(slots::NEIGHBORS, THREAD_COUNT)) == true);
    const KernelBuffers results = RunBoth(device,
                                          shaders::lusl_barriers::Compute(),
                                          shaders::slang_barriers::Compute(),
                                          buffers,
                                          nullptr,
                                          GROUP_COUNT);
    for (uint32 group = 0; group < GROUP_COUNT; ++group)
    {
        // The sum of id + 1 over the group's threads.
        const uint32 first = group * GROUP_SIZE + 1;
        const uint32 last = first + GROUP_SIZE - 1;
        CHECK(results[0].Words[group] == (first + last) * GROUP_SIZE / 2);
    }
    for (uint32 thread = 0; thread < THREAD_COUNT; ++thread)
    {
        CAPTURE(thread);
        CHECK(results[2].Words[thread] == (thread ^ 1U) * SCALE);
    }
}
