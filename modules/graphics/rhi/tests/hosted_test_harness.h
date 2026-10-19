#pragma once

// Editor side of the two-process hosted surface tests: a HostedWindowEndpoint,
// a HostedSurfaceConsumer on the test's own device, and the producer child
// (hosted_producer.cpp) started with the endpoint's channel address. Only public
// Platform and RHI APIs are used, exactly as the editor will use them.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/hosted_surface.h>
#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/platform/hosted_window.h>

#include "hosted_test_protocol.h"

#include <cstring>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

// posix_spawn(2) receives the child's environment explicitly.
extern char** environ; // NOLINT(readability-redundant-declaration)

namespace ludus::graphics::rhi::hosted_test
{
using foundation::int32;

inline constexpr foundation::uint64 NANOSECONDS_PER_MILLISECOND = 1'000'000;
/// Longest wait for any expected event from the producer.
inline constexpr foundation::uint64 EVENT_TIMEOUT_NANOSECONDS = 20'000 * NANOSECONDS_PER_MILLISECOND;
/// Time during which a producer without free images must announce nothing.
inline constexpr foundation::uint64 QUIET_NANOSECONDS = 300 * NANOSECONDS_PER_MILLISECOND;
/// A Skipped BeginFrame must return within this time; it never waits for the editor.
inline constexpr foundation::uint64 MAX_SKIPPED_NANOSECONDS = 100 * NANOSECONDS_PER_MILLISECOND;
inline constexpr foundation::usize MAX_ENVIRONMENT_ENTRIES = 512;
inline constexpr foundation::usize MAX_VARIABLE_BYTES = 256;

/// The producer child process.
class ProducerProcess final
{
public:
    ProducerProcess() = default;
    ~ProducerProcess()
    {
        if (mProcessId > 0)
        {
            static_cast<void>(::kill(mProcessId, SIGKILL));
            int32 status = 0;
            static_cast<void>(::waitpid(mProcessId, &status, 0));
        }
        if (mOutput >= 0)
        {
            static_cast<void>(::close(mOutput));
        }
    }
    ProducerProcess(const ProducerProcess&) = delete;
    ProducerProcess& operator=(const ProducerProcess&) = delete;
    ProducerProcess(ProducerProcess&&) = delete;
    ProducerProcess& operator=(ProducerProcess&&) = delete;

    /// Start the producer with `address` in LUDUS_HOSTED_WINDOW.
    void Spawn(std::string_view mode, std::string_view address)
    {
        foundation::StaticString<MAX_VARIABLE_BYTES> variable;
        REQUIRE(variable.TryAssign(platform::HOSTED_WINDOW_ENVIRONMENT_VARIABLE) == foundation::StringStatus::Ok);
        REQUIRE(variable.TryAppend("=") == foundation::StringStatus::Ok);
        REQUIRE(variable.TryAppend(address) == foundation::StringStatus::Ok);
        foundation::StaticArray<char*, MAX_ENVIRONMENT_ENTRIES> environment{};
        foundation::usize count = 0;
        for (char** entry = environ; *entry != nullptr && count + 2 < MAX_ENVIRONMENT_ENTRIES; ++entry)
        {
            environment[count] = *entry;
            ++count;
        }
        environment[count] = const_cast<char*>(variable.GetData());
        environment[count + 1] = nullptr;
        foundation::StaticString<MAX_VARIABLE_BYTES> modeText;
        REQUIRE(modeText.TryAssign(mode) == foundation::StringStatus::Ok);

        int32 output[2] = {-1, -1};
        REQUIRE(::pipe(output) == 0);
        posix_spawn_file_actions_t actions{};
        REQUIRE(::posix_spawn_file_actions_init(&actions) == 0);
        REQUIRE(::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO) == 0);
        REQUIRE(::posix_spawn_file_actions_addclose(&actions, output[0]) == 0);
        char program[] = LUDUS_HOSTED_PRODUCER;
        char* arguments[] = {program, const_cast<char*>(modeText.GetData()), nullptr};
        pid_t processId = 0;
        const int32 result = ::posix_spawn(&processId, program, &actions, nullptr, arguments, environment.GetData());
        static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
        static_cast<void>(::close(output[1]));
        mOutput = output[0];
        REQUIRE(result == 0);
        mProcessId = static_cast<int32>(processId);
    }

    /// True once the child has exited; records its exit status.
    [[nodiscard]] bool HasExited()
    {
        if (mProcessId <= 0)
        {
            return true;
        }
        int32 status = 0;
        if (::waitpid(mProcessId, &status, WNOHANG) != mProcessId)
        {
            return false;
        }
        mProcessId = -1;
        mExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return true;
    }
    [[nodiscard]] int32 GetExitCode() const noexcept
    {
        return mExitCode;
    }

    /// Read the statistics the child wrote before exiting.
    [[nodiscard]] bool TryReadStatistics(ProducerStatistics& outStatistics)
    {
        foundation::usize received = 0;
        foundation::uint8* bytes = reinterpret_cast<foundation::uint8*>(&outStatistics);
        while (received < sizeof(outStatistics))
        {
            const ssize_t count = ::read(mOutput, bytes + received, sizeof(outStatistics) - received);
            if (count <= 0)
            {
                return false;
            }
            received += static_cast<foundation::usize>(count);
        }
        return true;
    }

private:
    int32 mProcessId = -1;
    int32 mOutput = -1;
    int32 mExitCode = -1;
};

/// One Play session: endpoint, consumer and producer.
class HostedSession final
{
public:
    HostedSession() = default;
    ~HostedSession()
    {
        mConsumer.Stop();
        mEndpoint.Stop();
    }
    HostedSession(const HostedSession&) = delete;
    HostedSession& operator=(const HostedSession&) = delete;
    HostedSession(HostedSession&&) = delete;
    HostedSession& operator=(HostedSession&&) = delete;

    /// Start the endpoint and consumer, then the producer child, and wait for the connection.
    void Start(const NativeDeviceIdentity& device, std::string_view mode)
    {
        platform::HostedWindowEndpoint::StartInfo info;
        // A short parent keeps the socket path within sockaddr_un.
        info.ParentDirectory = "/tmp";
        info.Width = INITIAL_WIDTH;
        info.Height = INITIAL_HEIGHT;
        REQUIRE(mEndpoint.Start(info) == platform::HostedEndpointStatus::Ok);
        REQUIRE(mConsumer.Start(mEndpoint, device) == HostedConsumerStatus::Ok);
        mProducer.Spawn(mode, mEndpoint.GetChannelAddress());
        REQUIRE(PumpUntil([this]() { return mEndpoint.GetState() == platform::HostedConnectionState::Connected; }));
    }

    /// Pump endpoint and consumer until `predicate` holds or the timeout expires.
    template <typename PredicateType>
    [[nodiscard]] bool PumpUntil(PredicateType predicate, foundation::uint64 timeout = EVENT_TIMEOUT_NANOSECONDS)
    {
        const foundation::time::Timestamp start = foundation::time::Now();
        while (foundation::time::Now().Nanoseconds - start.Nanoseconds < timeout)
        {
            Pump();
            if (predicate() == true)
            {
                return true;
            }
            static_cast<void>(::usleep(200));
        }
        return false;
    }

    void Pump()
    {
        static_cast<void>(mEndpoint.Pump());
        mLastStatus = mConsumer.Pump();
    }

    /// Wait for and acquire the next frame.
    [[nodiscard]] bool TryAcquireNext(HostedFrame& outFrame)
    {
        return PumpUntil([this, &outFrame]() { return mConsumer.AcquireFrame(outFrame) == HostedConsumerStatus::Ok; });
    }

    /// Close the game's window and collect its statistics.
    void Close(ProducerStatistics& outStatistics)
    {
        REQUIRE(mEndpoint.SendClose() == platform::HostedEndpointStatus::Ok);
        REQUIRE(PumpUntil([this]() { return mProducer.HasExited(); }));
        CHECK(mProducer.GetExitCode() == 0);
        REQUIRE(mProducer.TryReadStatistics(outStatistics));
    }

    [[nodiscard]] platform::HostedWindowEndpoint& GetEndpoint() noexcept
    {
        return mEndpoint;
    }
    [[nodiscard]] HostedSurfaceConsumer& GetConsumer() noexcept
    {
        return mConsumer;
    }
    [[nodiscard]] HostedConsumerStatus GetLastStatus() const noexcept
    {
        return mLastStatus;
    }

private:
    platform::HostedWindowEndpoint mEndpoint;
    HostedSurfaceConsumer mConsumer;
    ProducerProcess mProducer;
    HostedConsumerStatus mLastStatus = HostedConsumerStatus::Ok;
};
} // namespace ludus::graphics::rhi::hosted_test
