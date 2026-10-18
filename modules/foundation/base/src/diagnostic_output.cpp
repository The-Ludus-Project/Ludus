#include <ludus/foundation/base/config.h>
#include <ludus/foundation/base/diagnostic_output.hpp>

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <span>
#include <sys/socket.h>
#include <unistd.h>
#if defined(LUDUS_PLATFORM_MACOS)
#    include <time.h>
#endif

namespace ludus::foundation::diagnostics
{
namespace
{
// Configuration is explicitly single-threaded before diagnostics are launched.
constinit int gEmergencySocket = -1;
constinit int gControlSocket = -1;
constinit ControlState gControlState = ControlState::Unconfigured;

// Bounded startup handshake: never block engine launch indefinitely on a
// misbehaving helper. This is a startup wait, not a failure-path bound.
constexpr int CONTROL_HANDSHAKE_TIMEOUT_MS = 2000;

#if defined(LUDUS_PLATFORM_MACOS)
// Thanks to Apple, socket(2), DESCRIPTION, Mac OS X BSD System Calls Manual,
// and XNU bsd/sys/socket.h, SO_NOSIGPIPE (no SIGPIPE on EPIPE):
// https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/socket.2.html
// https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/socket.h
// Darwin AF_UNIX has no SEQPACKET. Retain the wire codec over framed streams;
// suppress broken-peer SIGPIPE on owned sockets, without process signal handlers.
constexpr int CONTROL_SOCKET_TYPE = SOCK_STREAM;
constexpr int SEND_FLAGS = 0;

bool SuppressSocketSignal(int descriptor) noexcept
{
    const int enabled = 1;
    return ::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0;
}

int64 MonotonicMilliseconds() noexcept
{
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return -1;
    }
    return static_cast<int64>(now.tv_sec) * 1000 + static_cast<int64>(now.tv_nsec) / 1000000;
}

// Exact reads preserve stream fragmentation/coalescing. Startup uses one shared
// deadline for header and payload; decisions wait for explicit human input.
bool ReadStreamBytes(int descriptor, std::span<char> bytes, int64 deadline) noexcept
{
    usize offset = 0;
    while (offset < bytes.size())
    {
        int wait_ms = -1;
        if (deadline >= 0)
        {
            const int64 now = MonotonicMilliseconds();
            if (now < 0 || now >= deadline)
            {
                return false;
            }
            wait_ms = static_cast<int>(deadline - now);
        }
        pollfd waiter{};
        waiter.fd = descriptor;
        waiter.events = POLLIN;
        const int ready = ::poll(&waiter, 1, wait_ms);
        if (ready < 0 && errno == EINTR)
        {
            continue;
        }
        if (ready <= 0 || (waiter.revents & POLLIN) == 0)
        {
            return false;
        }
        const auto count = ::recv(descriptor, bytes.data() + offset, bytes.size() - offset, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        {
            continue;
        }
        if (count <= 0)
        {
            return false;
        }
        offset += static_cast<usize>(count);
    }
    return true;
}

bool ReadControlFrame(int descriptor, char* frame, usize& size, int timeout_ms) noexcept
{
    const int64 now = timeout_ms >= 0 ? MonotonicMilliseconds() : 0;
    if (now < 0)
    {
        return false;
    }
    const int64 deadline = timeout_ms >= 0 ? now + timeout_ms : -1;
    if (ReadStreamBytes(descriptor, {frame, CONTROL_HEADER_SIZE}, deadline) == false)
    {
        return false;
    }
    ControlHeader header{};
    // The decoder reads only header bytes; capacity validates the declared
    // payload bound before receiving any payload into the remaining storage.
    if (DecodeControlHeader(frame, CONTROL_HEADER_SIZE + CONTROL_MAX_PAYLOAD, header) == false)
    {
        return false;
    }
    size = CONTROL_HEADER_SIZE + header.Length;
    return ReadStreamBytes(descriptor, {frame + CONTROL_HEADER_SIZE, header.Length}, deadline);
}
#else
constexpr int CONTROL_SOCKET_TYPE = SOCK_SEQPACKET;
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
bool SuppressSocketSignal(int) noexcept
{
    return true; // Linux suppresses SIGPIPE per send instead.
}
#endif

// Bounded nonblocking writes. A partial stream frame is completed within this
// cap or treated as a transport failure, never as permission to continue.
bool SendControlFrame(int descriptor, const char* bytes, usize size) noexcept
{
    usize offset = 0;
    for (uint32 attempt = 0; attempt < 4 && offset < size; ++attempt)
    {
        const auto sent = ::send(descriptor, bytes + offset, size - offset, SEND_FLAGS | MSG_DONTWAIT);
        if (sent > 0)
        {
            offset += static_cast<usize>(sent);
        }
        else if (sent < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            return false;
        }
    }
    return offset == size;
}

} // namespace

bool ConfigureEmergencySocket(int descriptor) noexcept
{
    if (gEmergencySocket != -1)
    {
        return false;
    }
    const int saved_errno = errno;
    int type = 0;
    socklen_t length = sizeof(type);
    sockaddr_storage peer{};
    socklen_t peer_length = sizeof(peer);
    const bool valid = getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &length) == 0 && type == SOCK_DGRAM &&
                       getpeername(descriptor, reinterpret_cast<sockaddr*>(&peer), &peer_length) == 0 &&
                       peer.ss_family == AF_UNIX;
    if (valid)
    {
        const int owned = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
        if (owned >= 0 && SuppressSocketSignal(owned))
        {
            gEmergencySocket = owned;
        }
        else if (owned >= 0)
        {
            (void)::close(owned);
        }
    }
    errno = saved_errno;
    return gEmergencySocket != -1;
}

DeliveryStatus TryWriteEmergencyBytes(const char* data, usize size) noexcept
{
    if (gEmergencySocket == -1)
    {
        return DeliveryStatus::Unavailable;
    }
    if (data == nullptr || size > 2048)
    {
        return DeliveryStatus::Failed;
    }
    const int saved_errno = errno;
    DeliveryStatus status = DeliveryStatus::Failed;
    for (uint32 attempt = 0; attempt < 4; ++attempt)
    {
        const auto written = send(gEmergencySocket, data, size, MSG_DONTWAIT | SEND_FLAGS);
        if (written >= 0)
        {
            // Datagram writes are atomic: never split a record into new messages.
            status = static_cast<usize>(written) == size ? DeliveryStatus::Delivered : DeliveryStatus::Failed;
            break;
        }
        if (errno != EINTR)
        {
            break;
        }
    }
    errno = saved_errno;
    return status;
}

bool WriteEmergencyBytes(const char* data, usize size) noexcept
{
    const auto status = TryWriteEmergencyBytes(data, size);
    if (status != DeliveryStatus::Unavailable)
    {
        return status == DeliveryStatus::Delivered;
    }
    if (size == 0)
    {
        return true;
    }
    if (data == nullptr)
    {
        return false;
    }

    // Do not turn an ordinary log failure into SIGPIPE termination or install a
    // process-wide signal handler. Temporarily block SIGPIPE on this thread,
    // preserving pre-existing pending signals and the caller's mask/errno.
    const int saved_errno = errno;
    sigset_t blocked{};
    sigset_t previous{};
    sigset_t pending{};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0)
    {
        errno = saved_errno;
        return false;
    }
    bool delivered = false;
    if (sigpending(&pending) == 0)
    {
        const bool already_pending = sigismember(&pending, SIGPIPE) == 1;
        usize offset = 0;
        for (uint32 attempt = 0; attempt < 4 && offset < size; ++attempt)
        {
            const auto written = ::write(STDERR_FILENO, data + offset, size - offset);
            if (written > 0)
            {
                offset += static_cast<usize>(written);
            }
            else if (written < 0 && errno == EINTR)
            {
                continue;
            }
            else
            {
                if (written < 0 && errno == EPIPE && already_pending == false)
                {
#if defined(LUDUS_PLATFORM_MACOS)
                    // Darwin has no sigtimedwait. Drain only a signal confirmed
                    // pending while blocked, preserving an earlier caller signal.
                    sigset_t generated{};
                    if (sigpending(&generated) == 0 && sigismember(&generated, SIGPIPE) == 1)
                    {
                        int signal = 0;
                        (void)sigwait(&blocked, &signal);
                    }
#else
                    const timespec no_wait{};
                    // Drain our SIGPIPE before restoring the caller's mask. An
                    // EINTR retry cap here could deliver that signal and kill a
                    // logging caller. This safety cleanup can be delayed by
                    // a signal storm, another reason stderr is not time-bounded.
                    while (sigtimedwait(&blocked, nullptr, &no_wait) < 0 && errno == EINTR)
                    {
                    }
#endif
                }
                break;
            }
        }
        delivered = offset == size;
    }
    (void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    errno = saved_errno;
    return delivered;
}

namespace
{
// Bounded, EINTR-aware receive of exactly one framed message within the deadline.
// Returns true only for a complete, well-formed header of the expected kind with
// no trailing payload (Hello/HelloAck carry none). Never blocks past the
// deadline; a slow/absent/malformed helper fails.
bool ReceiveControlMessage(int descriptor, ControlMessageType expected, int timeout_ms) noexcept
{
#if defined(LUDUS_PLATFORM_MACOS)
    char frame[CONTROL_HEADER_SIZE + CONTROL_MAX_PAYLOAD];
    usize count = 0;
    if (ReadControlFrame(descriptor, frame, count, timeout_ms) == false)
    {
        return false;
    }
#else
    pollfd waiter{};
    waiter.fd = descriptor;
    waiter.events = POLLIN;
    const int ready = ::poll(&waiter, 1, timeout_ms);
    if (ready <= 0 || (waiter.revents & POLLIN) == 0)
    {
        return false;
    }
    char frame[CONTROL_HEADER_SIZE + CONTROL_MAX_PAYLOAD];
    // SEQPACKET preserves message boundaries: one recv yields one whole message.
    const auto count = ::recv(descriptor, frame, sizeof(frame), MSG_DONTWAIT);
    if (count < static_cast<ssize_t>(CONTROL_HEADER_SIZE))
    {
        return false;
    }
#endif
    ControlHeader header{};
    if (DecodeControlHeader(frame, static_cast<usize>(count), header) == false)
    {
        return false;
    }
    // Handshake frames carry no payload and no incident id.
    return header.Kind == static_cast<uint16>(expected) && header.IncidentId == 0 && header.Length == 0 &&
           static_cast<usize>(count) == CONTROL_HEADER_SIZE;
}
} // namespace

ControlState ConfigureControlEndpoint(int descriptor) noexcept
{
    if (gControlSocket != -1)
    {
        return gControlState;
    }
    const int saved_errno = errno;
    int type = 0;
    socklen_t length = sizeof(type);
    sockaddr_storage peer{};
    socklen_t peer_length = sizeof(peer);
    const bool valid =
        getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &length) == 0 && type == CONTROL_SOCKET_TYPE &&
        getpeername(descriptor, reinterpret_cast<sockaddr*>(&peer), &peer_length) == 0 && peer.ss_family == AF_UNIX;
    if (valid == false)
    {
        gControlState = ControlState::Failed;
        errno = saved_errno;
        return gControlState;
    }
    const int owned = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
    if (owned == -1 || SuppressSocketSignal(owned) == false)
    {
        if (owned >= 0)
        {
            (void)::close(owned);
        }
        gControlState = ControlState::Failed;
        errno = saved_errno;
        return gControlState;
    }
    gControlSocket = owned;

    char hello[CONTROL_HEADER_SIZE];
    const usize hello_size = EncodeControlHeader(hello, sizeof(hello), ControlMessageType::Hello, 0, 0);
    if (SendControlFrame(gControlSocket, hello, hello_size) == false)
    {
        gControlState = ControlState::Failed;
        errno = saved_errno;
        return gControlState;
    }

    const bool acked =
        ReceiveControlMessage(gControlSocket, ControlMessageType::HelloAck, CONTROL_HANDSHAKE_TIMEOUT_MS);
    gControlState = acked ? ControlState::Ready : ControlState::Failed;
    errno = saved_errno;
    return gControlState;
}

ControlState ControlEndpointState() noexcept
{
    return gControlState;
}

bool IsContinuousIntegration() noexcept
{
    const int saved_errno = errno;
    // Common CI signals. The generic CI variable covers GitHub Actions, GitLab,
    // CircleCI, Travis, and others; the rest catch environments that omit it.
    static const char* const signals[] =
        {"CI", "CONTINUOUS_INTEGRATION", "GITHUB_ACTIONS", "GITLAB_CI", "BUILDKITE", "JENKINS_URL", "TEAMCITY_VERSION"};
    bool detected = false;
    for (const char* signal : signals)
    {
        const char* value = ::getenv(signal);
        if (value != nullptr && value[0] != '\0')
        {
            detected = true;
            break;
        }
    }
    // Explicit engine opt-out/opt-in wins over heuristics (LUDUS_CI=0/1).
    if (const char* forced = ::getenv("LUDUS_CI"); forced != nullptr && forced[0] != '\0')
    {
        detected = forced[0] != '0';
    }
    errno = saved_errno;
    return detected;
}

ControlDecision RequestAssertDecision(uint32 incidentId, const char* report, usize size) noexcept
{
    if (gControlState != ControlState::Ready || gControlSocket == -1)
    {
        return ControlDecision::Terminate;
    }
    if (size > CONTROL_MAX_PAYLOAD)
    {
        size = CONTROL_MAX_PAYLOAD; // Bound the request; a longer report is clipped.
    }
    const int saved_errno = errno;

    // Build request: header + bounded owned report bytes. One SEQPACKET message.
    char frame[CONTROL_HEADER_SIZE + CONTROL_MAX_PAYLOAD];
    const usize header_size = EncodeControlHeader(frame,
                                                  sizeof(frame),
                                                  ControlMessageType::DecisionRequest,
                                                  incidentId,
                                                  static_cast<uint32>(report != nullptr ? size : 0));
    usize frame_size = header_size;
    if (report != nullptr && size != 0)
    {
        for (usize i = 0; i < size; ++i)
        {
            frame[header_size + i] = report[i];
        }
        frame_size += size;
    }

    if (SendControlFrame(gControlSocket, frame, frame_size) == false)
    {
        errno = saved_errno;
        return ControlDecision::Terminate; // Helper gone / short send => never continue.
    }

    // Block for the reply. No timeout: a healthy dialog waits for a human. A
    // closed helper makes recv return 0 (=> Terminate); EINTR is retried.
    ControlDecision decision = ControlDecision::Terminate;
    for (;;)
    {
        char reply[CONTROL_HEADER_SIZE + CONTROL_MAX_PAYLOAD];
#if defined(LUDUS_PLATFORM_MACOS)
        usize count = 0;
        if (ReadControlFrame(gControlSocket, reply, count, -1) == false)
        {
            break;
        }
#else
        const auto count = ::recv(gControlSocket, reply, sizeof(reply), 0);
        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break; // Transport error => Terminate.
        }
        if (count == 0)
        {
            break; // Helper closed the channel => Terminate.
        }
#endif
        ControlHeader header{};
        if (DecodeControlHeader(reply, static_cast<usize>(count), header) == false)
        {
            break; // Malformed => Terminate; do not loop on garbage.
        }
        // Only the active incident's explicit ContinueOnce authorizes a resume.
        if (header.Kind == static_cast<uint16>(ControlMessageType::DecisionReply) && header.IncidentId == incidentId &&
            header.Length == 1 &&
            static_cast<uint8>(reply[CONTROL_HEADER_SIZE]) == static_cast<uint8>(ControlDecision::ContinueOnce))
        {
            decision = ControlDecision::ContinueOnce;
        }
        break; // A stale/duplicate/wrong-id/wrong-kind reply falls through as Terminate.
    }
    errno = saved_errno;
    return decision;
}
} // namespace ludus::foundation::diagnostics
