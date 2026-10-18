#include <ludus/foundation/filesystem/async.hpp>
#include <ludus/foundation/filesystem/pack.hpp>

#include "pack_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;
namespace
{
struct Control
{
    std::mutex Mutex;
    std::condition_variable Wake;
    bool Entered = false;
    bool Released = false;
    uint64 Order[16]{};
    uint32 Calls = 0;
    std::atomic<uint32> Destroyed{0};
    Status Failure = Status::Ok;
    AsyncReader* Reader = nullptr;
    std::atomic<AsyncStatus> Reentry{AsyncStatus::Ok};
    void Release() noexcept
    {
        const std::lock_guard lock(Mutex);
        Released = true;
        Wake.notify_all();
    }
    bool Wait() noexcept
    {
        std::unique_lock lock(Mutex);
        return Wake.wait_for(lock, std::chrono::seconds(5), [this] { return Entered; });
    }
};
struct Unblock
{
    Control& State;
    ~Unblock() noexcept
    {
        State.Release();
    }
};
class ControlledFile final : public ProviderFile
{
public:
    explicit ControlledFile(Control& control) noexcept : mControl(control) {}
    ~ControlledFile() noexcept override
    {
        ++mControl.Destroyed;
    }
    [[nodiscard]] uint64 Size() const noexcept override
    {
        return 64;
    }
    [[nodiscard]] ReadResult ReadAt(uint64 offset, std::span<uint8> destination) const noexcept override
    {
        std::unique_lock lock(mControl.Mutex);
        if (mControl.Reader != nullptr)
        {
            mControl.Reentry = mControl.Reader->Shutdown();
        }
        if (mControl.Calls < 16)
        {
            mControl.Order[mControl.Calls] = offset;
        }
        ++mControl.Calls;
        if (offset == 0)
        {
            mControl.Entered = true;
            mControl.Wake.notify_all();
            mControl.Wake.wait(lock, [this] { return mControl.Released; });
        }
        const usize bytes = destination.size() < 64 - offset ? destination.size() : static_cast<usize>(64 - offset);
        for (usize index = 0; index < bytes; ++index)
        {
            destination[index] = static_cast<uint8>(offset);
        }
        return {{mControl.Failure, mControl.Failure == Status::Ok ? 0 : 17}, bytes};
    }

private:
    Control& mControl;
};
class ControlledProvider final : public Provider
{
public:
    explicit ControlledProvider(Control& state) noexcept : mState(state) {}
    Result OpenRead(std::string_view, ProviderFile*& output) const noexcept override
    {
        output = new (std::nothrow) ControlledFile(mState);
        return {output == nullptr ? Status::OutOfMemory : Status::Ok, 0};
    }

private:
    Control& mState;
};
VirtualFile Open(const ProviderHandle& provider, std::string_view name = "data")
{
    MountSnapshot snapshot;
    const Mount mount{Root::Assets, {}, 0, 9, provider};
    VirtualPath key;
    VirtualFile file;
    REQUIRE(MountSnapshot::Create({&mount, 1}, 71, snapshot).Succeeded());
    REQUIRE(key.Set(Root::Assets, name).Succeeded());
    REQUIRE(snapshot.OpenRead(key, file).Outcome.Succeeded());
    return file;
}
ReadCompletion Collect(AsyncReader& reader)
{
    ReadCompletion completion;
    const uint64 until = AsyncNow() + 5'000'000'000;
    while (reader.Poll(completion) == false && AsyncNow() < until)
    {
        std::this_thread::yield();
    }
    REQUIRE(completion.Handle.Sequence != 0);
    REQUIRE(completion.State == RequestState::Completed);
    return completion;
}
} // namespace

TEST_CASE("Async cancellation retains revision and byte budget until host collection", "[filesystem][async]")
{
    Control state;
    ProviderHandle provider(new ControlledProvider(state));
    auto file = Open(provider);
    AsyncReader reader;
    REQUIRE(reader.Initialize({2, 1, 8, 5'000'000, 1}) == AsyncStatus::Ok);
    Unblock unblock{state};
    uint8 first[4]{}, second[4]{}, third[4]{};
    RequestHandle a, b, preserved{123, 456};
    REQUIRE(reader.Submit(file, 0, first, a) == AsyncStatus::Ok);
    REQUIRE(state.Wait());
    REQUIRE(reader.Submit(file, 1, second, b) == AsyncStatus::Ok);
    REQUIRE(reader.Cancel(a) == AsyncStatus::Ok);
    REQUIRE(reader.Cancel(a) == AsyncStatus::Ok);
    REQUIRE(reader.Cancel(b) == AsyncStatus::Ok);
    REQUIRE(reader.Submit(file, 2, third, preserved) == AsyncStatus::CapacityExceeded);
    REQUIRE(preserved.Sequence == 456);
    file.Close();
    provider = {};
    REQUIRE(state.Destroyed == 0);
    ReadCompletion completion;
    completion.Tag = 999;
    REQUIRE_FALSE(reader.Poll(completion));
    REQUIRE(completion.Tag == 999);
    AsyncMetrics metrics;
    REQUIRE(reader.Metrics(metrics) == AsyncStatus::Ok);
    REQUIRE(metrics.BytesInFlight == 8);
    REQUIRE(metrics.Submitted == 1);
    REQUIRE(metrics.Queued == 1);
    state.Release();
    const auto one = Collect(reader), two = Collect(reader);
    REQUIRE(one.Disposition == ReadDisposition::Cancelled);
    REQUIRE(two.Disposition == ReadDisposition::Cancelled);
    REQUIRE(one.Read.BytesRead == 0);
    REQUIRE(one.CancellationNanoseconds > 0);
    REQUIRE(one.MountId == 9);
    REQUIRE(one.Generation == 71);
    REQUIRE(state.Calls == 1);
    REQUIRE(state.Destroyed == 1);
    REQUIRE(reader.Cancel(a) == AsyncStatus::InvalidHandle);
    RequestState before = RequestState::Submitted;
    REQUIRE(reader.GetState(b, before) == AsyncStatus::InvalidHandle);
    REQUIRE(before == RequestState::Submitted);
    REQUIRE_FALSE(reader.Poll(completion));
    REQUIRE(reader.Metrics(metrics) == AsyncStatus::Ok);
    REQUIRE(metrics.BytesInFlight == 0);
    REQUIRE(metrics.InFlight == 0);
    REQUIRE(metrics.Cancelled == 2);
    REQUIRE(metrics.Rejected == 1);
    REQUIRE(metrics.DroppedTraces == 1);
    ReadTrace trace;
    REQUIRE(reader.PollTrace(trace));
    REQUIRE(trace.Completion.Handle.Sequence == two.Handle.Sequence);
    REQUIRE_FALSE(reader.PollTrace(trace));
}

TEST_CASE("Async ordering honors deadlines priority FIFO and age-based starvation prevention", "[filesystem][async]")
{
    for (const bool age : {false, true})
    {
        Control state;
        const ProviderHandle provider(new ControlledProvider(state));
        auto file = Open(provider);
        AsyncReader reader;
        REQUIRE(reader.Initialize({6, 1, 64, age ? 1u : 60'000'000'000ULL, 0}) == AsyncStatus::Ok);
        Unblock unblock{state};
        uint8 buffers[6]{};
        RequestHandle handles[6];
        REQUIRE(reader.Submit(file, 0, {buffers, 1}, handles[0]) == AsyncStatus::Ok);
        REQUIRE(state.Wait());
        REQUIRE(reader.Submit(file, 1, {buffers + 1, 1}, handles[1], {-5, 0, 1, nullptr}) == AsyncStatus::Ok);
        REQUIRE(reader.Submit(file, 2, {buffers + 2, 1}, handles[2], {10, 0, 2, nullptr}) == AsyncStatus::Ok);
        REQUIRE(reader.Submit(file, 3, {buffers + 3, 1}, handles[3], {10, 0, 3, nullptr}) == AsyncStatus::Ok);
        REQUIRE(
            reader.Submit(file, 4, {buffers + 4, 1}, handles[4], {-10, AsyncNow() + 30'000'000'000ULL, 4, nullptr}) ==
            AsyncStatus::Ok);
        REQUIRE(reader.Submit(file, 5, {buffers + 5, 1}, handles[5], {99, 1, 5, nullptr}) == AsyncStatus::Ok);
        state.Release();
        ReadCompletion completions[6];
        for (auto& completion : completions)
        {
            completion = Collect(reader);
        }
        if (age)
        {
            REQUIRE(state.Order[1] == 1);
            REQUIRE(state.Order[2] == 2);
            REQUIRE(completions[5].Disposition == ReadDisposition::DeadlineExpired);
        }
        else
        {
            REQUIRE(completions[1].Disposition == ReadDisposition::DeadlineExpired);
            REQUIRE(state.Order[1] == 4);
            REQUIRE(state.Order[2] == 2);
            REQUIRE(state.Order[3] == 3);
            REQUIRE(state.Order[4] == 1);
        }
        REQUIRE(state.Calls == 5);
        REQUIRE(buffers[5] == 0);
    }
}

TEST_CASE("Async shutdown drains running reads cancels queue and preserves terminal records", "[filesystem][async]")
{
    Control state;
    const ProviderHandle provider(new ControlledProvider(state));
    auto file = Open(provider);
    AsyncReader reader;
    REQUIRE(reader.Initialize({2, 1, 8, 1, 0}) == AsyncStatus::Ok);
    Unblock unblock{state};
    uint8 buffers[2]{};
    RequestHandle a, b;
    REQUIRE(reader.Submit(file, 0, {buffers, 1}, a) == AsyncStatus::Ok);
    REQUIRE(state.Wait());
    REQUIRE(reader.Submit(file, 1, {buffers + 1, 1}, b) == AsyncStatus::Ok);
    std::atomic<bool> stopped{false};
    std::thread shutdown([&] { stopped = reader.Shutdown() == AsyncStatus::Ok; });
    // Wait for shutdown's queued cancellation intent without races or sleeping.
    AsyncStatus admission = AsyncStatus::CapacityExceeded;
    const auto until = AsyncNow() + 5'000'000'000ULL;
    while (admission != AsyncStatus::Stopped && AsyncNow() < until)
    {
        RequestHandle spare;
        admission = reader.Submit(file, 2, {}, spare);
        std::this_thread::yield();
    }
    const bool drainedEarly = stopped.load();
    state.Release();
    shutdown.join();
    REQUIRE(admission == AsyncStatus::Stopped);
    REQUIRE_FALSE(drainedEarly);
    REQUIRE(stopped);
    REQUIRE(reader.Shutdown() == AsyncStatus::Ok);
    REQUIRE(Collect(reader).Disposition == ReadDisposition::Read);
    REQUIRE(Collect(reader).Disposition == ReadDisposition::Cancelled);
    REQUIRE(state.Calls == 1);
}

TEST_CASE("Async completed backlog limits admission stale handles never cancel replacement", "[filesystem][async]")
{
    Control state;
    state.Released = true;
    const ProviderHandle provider(new ControlledProvider(state));
    auto file = Open(provider);
    AsyncReader reader, foreign;
    REQUIRE(reader.Initialize({2, 1, 4, 1, 2}) == AsyncStatus::Ok);
    REQUIRE(foreign.Initialize({1, 1, 4, 1, 0}) == AsyncStatus::Ok);
    uint8 buffer[4]{};
    RequestHandle a, b, saved{7, 8};
    VirtualPath path;
    REQUIRE(path.Set(Root::Assets, "data").Succeeded());
    REQUIRE(reader.Submit(file, 64, buffer, a, {0, 0, 42, &path}) == AsyncStatus::Ok);
    REQUIRE(reader.Submit(file, 1, {buffer, 1}, saved) == AsyncStatus::ByteBudgetExceeded);
    REQUIRE(saved.Scheduler == 7);
    RequestState phase = RequestState::Queued;
    const auto until = AsyncNow() + 5'000'000'000ULL;
    while (phase != RequestState::Completing && AsyncNow() < until)
    {
        REQUIRE(reader.GetState(a, phase) == AsyncStatus::Ok);
        std::this_thread::yield();
    }
    REQUIRE(phase == RequestState::Completing);
    REQUIRE(reader.Cancel(a) == AsyncStatus::Ok); // Too late to change the terminal result.
    REQUIRE(foreign.Cancel(a) == AsyncStatus::InvalidHandle);
    REQUIRE(reader.Submit(file, 64, {}, b) == AsyncStatus::Ok); // Empty request still occupies a slot.
    REQUIRE(reader.Submit(file, 64, {}, saved) == AsyncStatus::CapacityExceeded);
    const auto eof = Collect(reader);
    REQUIRE(eof.Disposition == ReadDisposition::Read);
    REQUIRE(eof.Read.BytesRead == 0);
    REQUIRE(eof.Tag == 42);
    ReadTrace trace;
    REQUIRE(reader.PollTrace(trace));
    REQUIRE(trace.Path == path);
    REQUIRE(trace.RequestedBytes == 4);
    REQUIRE(trace.Offset == 64);
    const auto empty = Collect(reader);
    REQUIRE(empty.Read.BytesRead == 0);
    REQUIRE(reader.Submit(file, 1, buffer, saved) == AsyncStatus::Ok);
    REQUIRE(saved.Sequence != a.Sequence);
    REQUIRE(reader.Cancel(a) == AsyncStatus::InvalidHandle);
    REQUIRE(Collect(reader).Read.BytesRead == 4);
}

TEST_CASE("Async read errors retain valid progress and native diagnostics", "[filesystem][async]")
{
    Control state;
    state.Released = true;
    state.Failure = Status::IoError;
    const ProviderHandle provider(new ControlledProvider(state));
    const auto file = Open(provider);
    AsyncReader reader;
    REQUIRE(reader.Initialize({1, 1, 8, 1, 0}) == AsyncStatus::Ok);
    uint8 bytes[3]{};
    RequestHandle handle;
    REQUIRE(reader.Submit(file, 1, bytes, handle) == AsyncStatus::Ok);
    const auto completion = Collect(reader);
    REQUIRE(completion.Disposition == ReadDisposition::Read);
    REQUIRE(completion.Read.Outcome.Code == Status::IoError);
    REQUIRE(completion.Read.Outcome.NativeCode == 17);
    REQUIRE(completion.Read.BytesRead == 3);
}

TEST_CASE("Concurrent async producers cancellation and completion preserve every accepted identity",
          "[filesystem][async]")
{
    const uint8 source[]{1, 2, 3, 4};
    const MemoryEntry entry{"data", source};
    ProviderHandle provider;
    REQUIRE(CreateMemoryProvider({&entry, 1}, provider).Succeeded());
    auto file = Open(provider);
    AsyncReader reader;
    REQUIRE(reader.Initialize({16, 4, 64, 1, 0}) == AsyncStatus::Ok);
    constexpr usize TOTAL = 512;
    uint8 destinations[TOTAL][4]{};
    bool delivered[TOTAL]{};
    std::atomic<bool> failed{false};
    std::thread producers[4];
    for (usize thread = 0; thread < 4; ++thread)
    {
        producers[thread] = std::thread([&, thread] {
            for (usize index = thread; index < TOTAL; index += 4)
            {
                RequestHandle handle;
                AsyncStatus status;
                do
                {
                    status = reader.Submit(file, 0, destinations[index], handle, {0, 0, index, nullptr});
                    std::this_thread::yield();
                } while (status == AsyncStatus::CapacityExceeded);
                if (status != AsyncStatus::Ok)
                {
                    failed = true;
                    return;
                }
                const auto cancel = reader.Cancel(handle);
                if (cancel != AsyncStatus::Ok && cancel != AsyncStatus::InvalidHandle)
                {
                    failed = true;
                }
            }
        });
    }
    usize count = 0;
    bool valid = true;
    const uint64 until = AsyncNow() + 10'000'000'000ULL;
    while (count < TOTAL && AsyncNow() < until)
    {
        ReadCompletion completion;
        if (reader.Poll(completion))
        {
            if (completion.Tag >= TOTAL || delivered[completion.Tag])
            {
                valid = false;
            }
            else
            {
                delivered[completion.Tag] = true;
                if (completion.Disposition == ReadDisposition::Read && destinations[completion.Tag][3] != 4)
                {
                    valid = false;
                }
            }
            ++count;
        }
        else
        {
            std::this_thread::yield();
        }
    }
    // Stop admission before joining on failure, so bounded producers cannot remain stuck.
    REQUIRE(reader.Shutdown() == AsyncStatus::Ok);
    for (auto& producer : producers)
    {
        producer.join();
    }
    REQUIRE_FALSE(failed);
    REQUIRE(valid);
    REQUIRE(count == TOTAL);
    AsyncMetrics metrics;
    REQUIRE(reader.Metrics(metrics) == AsyncStatus::Ok);
    REQUIRE(metrics.Accepted == TOTAL);
    REQUIRE(metrics.Reads + metrics.Cancelled == TOTAL);
    REQUIRE(metrics.PeakRequests <= 16);
    REQUIRE(metrics.PeakBytes <= 64);
}

TEST_CASE("Async pack reads retain unmounted compressed revision with independent destinations", "[filesystem][async]")
{
    const MemoryEntry entry{"archive", pack_fixture::kPack};
    ProviderHandle storage, pack;
    REQUIRE(CreateMemoryProvider({&entry, 1}, storage).Succeeded());
    REQUIRE(CreatePackProvider(storage, "archive", {}, pack).Succeeded());
    auto file = Open(pack, "compressed");
    AsyncReader reader;
    REQUIRE(reader.Initialize({8, 4, 128, 1, 0}) == AsyncStatus::Ok);
    uint8 buffers[8][16]{};
    RequestHandle handles[8];
    for (usize index = 0; index < 8; ++index)
    {
        REQUIRE(reader.Submit(file, 65530, buffers[index], handles[index]) == AsyncStatus::Ok);
    }
    file.Close();
    pack = {};
    storage = {};
    for (usize index = 0; index < 8; ++index)
    {
        REQUIRE(Collect(reader).Read.BytesRead == 16);
    }
    for (const auto& buffer : buffers)
    {
        for (const uint8 byte : buffer)
        {
            REQUIRE(byte == 'A');
        }
    }
}

TEST_CASE("An I/O provider cannot shut down its own reader and join itself", "[filesystem][async]")
{
    Control state;
    state.Released = true;
    const ProviderHandle provider(new ControlledProvider(state));
    const auto file = Open(provider);
    uint8 bytes[2]{};
    AsyncReader reader;
    REQUIRE(reader.Initialize({1, 1, 4, 1, 0}) == AsyncStatus::Ok);
    state.Reader = &reader;
    RequestHandle handle;
    REQUIRE(reader.Submit(file, 1, bytes, handle) == AsyncStatus::Ok);
    REQUIRE(Collect(reader).Read.BytesRead == 2);
    REQUIRE(state.Reentry == AsyncStatus::InsideRead);
    REQUIRE(reader.Submit(file, 2, bytes, handle) == AsyncStatus::Ok);
    REQUIRE(Collect(reader).Read.BytesRead == 2);
    REQUIRE(reader.Shutdown() == AsyncStatus::Ok);
}
