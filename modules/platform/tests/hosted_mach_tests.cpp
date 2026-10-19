// Hosted surface Mach rendezvous probe (macOS). P5 of the play-in-viewport design
// (docs/architecture/editor-gui-systems.md, "Hosted surface channel") required
// this exchange to be confirmed before the Metal backend relies on it:
//
// - the editor registers a per-session bootstrap name with bootstrap_check_in(3)
//   and a child process finds it with bootstrap_look_up(3);
// - IOSurface send rights from IOSurfaceCreateMachPort travel to the editor, and
//   IOSurfaceLookupFromMachPort recovers the surface and its pixels;
// - the surfaces' IDs alone do not let another process look them up, so the
//   design never needs global surfaces (kIOSurfaceIsGlobal is deprecated as
//   insecure in Apple's IOSurface reference);
// - the endpoint rejects a message from any process other than the admitted one,
//   and one with the wrong session token.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/platform/entropy.h>
#include <ludus/platform/hosted_surface.h>

#include "internal/hosted_mach.hpp"
#include "internal/hosted_protocol.hpp"
#include "internal/hosted_socket.hpp"

#include <cstring>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOReturn.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach.h>

#include <catch2/catch_test_macros.hpp>

// posix_spawn(2) passes the parent's environment explicitly.
extern char** environ; // NOLINT(readability-redundant-declaration)

using ludus::foundation::int32;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;
using ludus::platform::HOSTED_SURFACE_IMAGE_COUNT;
using ludus::platform::HostedHandleKind;
using ludus::platform::HostedImageHandle;

namespace hosted = ludus::platform::hosted;

namespace
{
constexpr uint32 PROBE_PIXEL = 0x11223344U;
constexpr uint32 PROBE_GENERATION = 7;
constexpr uint64 NANOSECONDS_PER_SECOND = 1'000'000'000;
constexpr uint64 RECEIVE_DEADLINE_NANOSECONDS = 10 * NANOSECONDS_PER_SECOND;
constexpr int32 READ_TIMEOUT_MILLISECONDS = 10'000;
constexpr std::string_view HEX_DIGITS = "0123456789abcdef";
constexpr uint8 HEX_RADIX = 16;

hosted::RendezvousName MakeUniqueName()
{
    hosted::RendezvousNonce nonce{};
    REQUIRE(ludus::platform::TryFillSystemEntropy(nonce.GetData(), nonce.GetSize()));
    nonce[0] = static_cast<uint8>(nonce[0] | 1U);
    hosted::RendezvousName name;
    REQUIRE(hosted::TryFormatRendezvousName(nonce, name));
    return name;
}

hosted::Token MakeToken()
{
    hosted::Token token{};
    REQUIRE(ludus::platform::TryFillSystemEntropy(token.GetData(), token.GetSize()));
    return token;
}

ludus::foundation::StaticString<hosted::TOKEN_HEX_BYTES> FormatToken(const hosted::Token& token)
{
    ludus::foundation::StaticString<hosted::TOKEN_HEX_BYTES> hex;
    for (const uint8 byte : token)
    {
        REQUIRE(hex.TryAppend(HEX_DIGITS.substr(byte / HEX_RADIX, 1)) == ludus::foundation::StringStatus::Ok);
        REQUIRE(hex.TryAppend(HEX_DIGITS.substr(byte % HEX_RADIX, 1)) == ludus::foundation::StringStatus::Ok);
    }
    return hex;
}

/// A probe child with its standard input and output connected to pipes.
class ProbeChild final
{
public:
    ProbeChild(const hosted::RendezvousName& name, const hosted::Token& token, std::string_view mode)
    {
        int32 input[2] = {-1, -1};
        int32 output[2] = {-1, -1};
        REQUIRE(::pipe(input) == 0);
        REQUIRE(::pipe(output) == 0);
        posix_spawn_file_actions_t actions{};
        REQUIRE(::posix_spawn_file_actions_init(&actions) == 0);
        REQUIRE(::posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO) == 0);
        REQUIRE(::posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO) == 0);
        REQUIRE(::posix_spawn_file_actions_addclose(&actions, input[1]) == 0);
        REQUIRE(::posix_spawn_file_actions_addclose(&actions, output[0]) == 0);
        const ludus::foundation::StaticString<hosted::TOKEN_HEX_BYTES> tokenHex = FormatToken(token);
        ludus::foundation::StaticString<16> modeText;
        REQUIRE(modeText.TryAssign(mode) == ludus::foundation::StringStatus::Ok);
        char program[] = LUDUS_HOSTED_MACH_CHILD;
        char* arguments[] = {
            program,
            const_cast<char*>(name.GetData()),
            const_cast<char*>(tokenHex.GetData()),
            const_cast<char*>(modeText.GetData()),
            nullptr,
        };
        pid_t processId = 0;
        const int32 result = ::posix_spawn(&processId, program, &actions, nullptr, arguments, environ);
        static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
        static_cast<void>(::close(input[0]));
        static_cast<void>(::close(output[1]));
        mInput = input[1];
        mOutput = output[0];
        REQUIRE(result == 0);
        mProcessId = static_cast<int32>(processId);
    }
    ~ProbeChild()
    {
        static_cast<void>(Finish());
        if (mOutput >= 0)
        {
            static_cast<void>(::close(mOutput));
        }
    }
    ProbeChild(const ProbeChild&) = delete;
    ProbeChild& operator=(const ProbeChild&) = delete;
    ProbeChild(ProbeChild&&) = delete;
    ProbeChild& operator=(ProbeChild&&) = delete;

    [[nodiscard]] int32 GetProcessId() const noexcept
    {
        return mProcessId;
    }

    /// Read the surface IDs the child printed.
    [[nodiscard]] bool TryReadSurfaceIds(ludus::foundation::StaticArray<uint32, HOSTED_SURFACE_IMAGE_COUNT>& outIds)
    {
        usize received = 0;
        uint8* bytes = reinterpret_cast<uint8*>(outIds.GetData());
        const usize size = sizeof(uint32) * outIds.GetSize();
        while (received < size)
        {
            pollfd request{};
            request.fd = mOutput;
            request.events = POLLIN;
            if (::poll(&request, 1, READ_TIMEOUT_MILLISECONDS) <= 0)
            {
                return false;
            }
            const ssize_t count = ::read(mOutput, bytes + received, size - received);
            if (count <= 0)
            {
                return false;
            }
            received += static_cast<usize>(count);
        }
        return true;
    }

    /// Close the child's input, so it exits, and return its exit status.
    [[nodiscard]] int32 Finish()
    {
        if (mInput >= 0)
        {
            static_cast<void>(::close(mInput));
            mInput = -1;
        }
        if (mProcessId > 0)
        {
            int32 status = 0;
            static_cast<void>(::waitpid(mProcessId, &status, 0));
            mProcessId = -1;
            mExitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        return mExitStatus;
    }

private:
    int32 mProcessId = -1;
    int32 mInput = -1;
    int32 mOutput = -1;
    int32 mExitStatus = -1;
};

/// Receive one message, waiting for the child to send it.
hosted::RendezvousReceiveStatus ReceiveWithin(hosted::RendezvousServer& inoutServer,
                                              int32 processId,
                                              const hosted::Token& token,
                                              hosted::RendezvousImages& outImages)
{
    const ludus::foundation::time::Timestamp start = ludus::foundation::time::Now();
    while (true)
    {
        const hosted::RendezvousReceiveStatus status =
            inoutServer.TryReceive(processId, hosted::GetCurrentUserId(), token, outImages);
        if (status != hosted::RendezvousReceiveStatus::Empty ||
            ludus::foundation::time::Now().Nanoseconds - start.Nanoseconds > RECEIVE_DEADLINE_NANOSECONDS)
        {
            return status;
        }
        static_cast<void>(::usleep(1000));
    }
}

uint32 ReadFirstPixel(IOSurfaceRef surface)
{
    uint32 pixel = 0;
    REQUIRE(IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) == kIOReturnSuccess);
    std::memcpy(&pixel, IOSurfaceGetBaseAddress(surface), sizeof(pixel));
    REQUIRE(IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr) == kIOReturnSuccess);
    return pixel;
}

uint32 GetSendReferences(uint32 name)
{
    mach_port_urefs_t references = 0;
    REQUIRE(::mach_port_get_refs(::mach_task_self(), name, MACH_PORT_RIGHT_SEND, &references) == KERN_SUCCESS);
    return references;
}
} // namespace

TEST_CASE("IOSurface send rights cross the per-session Mach rendezvous", "[platform][hosted][macos][probe]")
{
    const hosted::RendezvousName name = MakeUniqueName();
    const hosted::Token token = MakeToken();
    hosted::RendezvousServer server;
    REQUIRE(server.TryStart(name));
    // A session name can be registered once.
    hosted::RendezvousServer duplicate;
    CHECK_FALSE(duplicate.TryStart(name));

    ProbeChild child(name, token, "send");
    ludus::foundation::StaticArray<uint32, HOSTED_SURFACE_IMAGE_COUNT> ids{};
    REQUIRE(child.TryReadSurfaceIds(ids));
    // A surface's ID alone grants nothing to another process: the surfaces are not global.
    for (const uint32 id : ids)
    {
        IOSurfaceRef byIdOrNull = IOSurfaceLookup(id);
        CHECK(byIdOrNull == nullptr);
        if (byIdOrNull != nullptr)
        {
            CFRelease(byIdOrNull);
        }
    }

    hosted::RendezvousImages images;
    REQUIRE(ReceiveWithin(server, child.GetProcessId(), token, images) == hosted::RendezvousReceiveStatus::Ok);
    CHECK(images.Generation == PROBE_GENERATION);
    for (usize i = 0; i < HOSTED_SURFACE_IMAGE_COUNT; ++i)
    {
        REQUIRE(images.Handles[i].GetKind() == HostedHandleKind::MachPort);
        IOSurfaceRef surfaceOrNull = IOSurfaceLookupFromMachPort(images.Handles[i].GetMachPort());
        REQUIRE(surfaceOrNull != nullptr);
        CHECK(IOSurfaceGetID(surfaceOrNull) == ids[i]);
        CHECK(IOSurfaceGetWidth(surfaceOrNull) == 4);
        CHECK(ReadFirstPixel(surfaceOrNull) == PROBE_PIXEL + static_cast<uint32>(i));
        CFRelease(surfaceOrNull);
    }
    CHECK(child.Finish() == 0);
}

TEST_CASE("The Mach rendezvous rejects other processes and wrong tokens", "[platform][hosted][macos][probe]")
{
    const hosted::RendezvousName name = MakeUniqueName();
    const hosted::Token token = MakeToken();
    hosted::RendezvousServer server;
    REQUIRE(server.TryStart(name));

    {
        // A genuine message from a process the endpoint did not admit.
        ProbeChild child(name, token, "send");
        ludus::foundation::StaticArray<uint32, HOSTED_SURFACE_IMAGE_COUNT> ids{};
        REQUIRE(child.TryReadSurfaceIds(ids));
        hosted::RendezvousImages images;
        CHECK(ReceiveWithin(server, static_cast<int32>(::getpid()), token, images) ==
              hosted::RendezvousReceiveStatus::Rejected);
        CHECK(images.Generation == 0);
        CHECK(child.Finish() == 0);
    }
    {
        // The admitted process with another session's token.
        ProbeChild child(name, token, "wrong-token");
        ludus::foundation::StaticArray<uint32, HOSTED_SURFACE_IMAGE_COUNT> ids{};
        REQUIRE(child.TryReadSurfaceIds(ids));
        hosted::RendezvousImages images;
        CHECK(ReceiveWithin(server, child.GetProcessId(), token, images) == hosted::RendezvousReceiveStatus::Rejected);
        CHECK(child.Finish() == 0);
    }
    hosted::RendezvousImages images;
    CHECK(server.TryReceive(static_cast<int32>(::getpid()), hosted::GetCurrentUserId(), token, images) ==
          hosted::RendezvousReceiveStatus::Empty);
}

TEST_CASE("A full Mach rendezvous queue reports Busy and keeps every send right", "[platform][hosted][macos]")
{
    const hosted::RendezvousName name = MakeUniqueName();
    const hosted::Token token = MakeToken();
    hosted::RendezvousServer server;
    REQUIRE(server.TryStart(name));
    hosted::RendezvousClient client;
    REQUIRE(client.TryConnect(name));

    // Any port with a send right stands in for an IOSurface port here.
    ludus::foundation::StaticArray<HostedImageHandle, HOSTED_SURFACE_IMAGE_COUNT> handles{};
    for (HostedImageHandle& handle : handles)
    {
        mach_port_t port = MACH_PORT_NULL;
        REQUIRE(::mach_port_allocate(::mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS);
        REQUIRE(::mach_port_insert_right(::mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
        handle = HostedImageHandle::FromMachPort(port);
    }
    const uint32 referencesBefore = GetSendReferences(handles[0].GetMachPort());
    usize sent = 0;
    hosted::RendezvousSendStatus status = hosted::RendezvousSendStatus::Ok;
    while (sent <= hosted::RENDEZVOUS_QUEUE_LIMIT)
    {
        status = client.TrySend(token, static_cast<uint32>(sent + 1), handles);
        if (status != hosted::RendezvousSendStatus::Ok)
        {
            break;
        }
        ++sent;
    }
    CHECK(status == hosted::RendezvousSendStatus::Busy);
    CHECK(sent == hosted::RENDEZVOUS_QUEUE_LIMIT);
    // Queued messages own their copied rights. The refused message was pseudo-received
    // back into this task, and TrySend dropped the copy it returned.
    CHECK(GetSendReferences(handles[0].GetMachPort()) == referencesBefore);

    const int32 self = static_cast<int32>(::getpid());
    for (usize i = 0; i < sent; ++i)
    {
        hosted::RendezvousImages images;
        REQUIRE(server.TryReceive(self, hosted::GetCurrentUserId(), token, images) ==
                hosted::RendezvousReceiveStatus::Ok);
        CHECK(images.Generation == i + 1);
    }
    // Received rights coalesce into this task's names; releasing them restores the count.
    CHECK(GetSendReferences(handles[0].GetMachPort()) == referencesBefore);

    // Stopping the server ends the session's rendezvous.
    server.Stop();
    CHECK(client.TrySend(token, 1, handles) == hosted::RendezvousSendStatus::Failed);
    for (HostedImageHandle& handle : handles)
    {
        const uint32 port = handle.ReleaseMachPort();
        static_cast<void>(::mach_port_mod_refs(::mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1));
        static_cast<void>(::mach_port_deallocate(::mach_task_self(), port));
    }
}
