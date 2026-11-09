#include "worker_platform.hpp"

#include <ludus/tooling/shader_compiler/compiler.hpp>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/threading/worker_queue.hpp>

#include <cerrno>
#include <cstring>
#include <string_view>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#    include <dispatch/dispatch.h>
#elif defined(__linux__)
#    include <sys/prctl.h>
#endif

namespace ludus::tools::shader_worker
{
using namespace foundation;
namespace compiler = tooling::shader_compiler;
namespace
{
/// How long the accept loop waits before checking again for a lane that finished, when all are busy.
constexpr int64 SATURATED_WAIT_NANOSECONDS = 1000000;
// The bound endpoint, kept where a signal handler can remove it without allocating.
sockaddr_un gAddress{};
volatile sig_atomic_t gHasEndpoint = 0;
// Stops at once, even inside a Slang call: a compile has no safe interruption point, and one
// that never returns must not outlive its owner. Only async-signal-safe calls are used;
// clients treat the closed connection as a worker failure.
[[noreturn]] void StopNow() noexcept
{
    if (gHasEndpoint == 1)
    {
        static_cast<void>(::unlink(gAddress.sun_path));
    }
    ::_exit(0);
}
void HandleStop(int32) noexcept
{
    StopNow();
}
#if defined(__APPLE__)
void HandleParentExit(void*) noexcept
{
    StopNow();
}
#endif
class DescriptorStream final : public Stream
{
public:
    DescriptorStream(int32 input, int32 output, bool isSocket) noexcept
        : mInput(input), mOutput(output), mIsSocket(isSocket)
    {
    }
    bool Transfer(uint8* bytes, usize size, bool isWrite) noexcept override
    {
        const int32 descriptor = isWrite == true ? mOutput : mInput;
        usize offset = 0;
        while (offset < size)
        {
            pollfd pending{descriptor, static_cast<short>(isWrite == true ? POLLOUT : POLLIN), 0};
            const int32 ready = ::poll(&pending, 1, IO_TIMEOUT_MILLISECONDS);
            if (ready < 0 && errno == EINTR)
            {
                continue;
            }
            if (ready <= 0)
            {
                return false;
            }
            ssize_t count = 0;
            if (isWrite == true)
            {
                if (mIsSocket == true)
                {
#if defined(__linux__)
                    count = ::send(descriptor, bytes + offset, size - offset, MSG_NOSIGNAL);
#else
                    count = ::send(descriptor, bytes + offset, size - offset, 0);
#endif
                }
                else
                {
                    count = ::write(descriptor, bytes + offset, size - offset);
                }
            }
            else
            {
                count = ::read(descriptor, bytes + offset, size - offset);
            }
            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                continue;
            }
            if (count <= 0)
            {
                return false;
            }
            offset += static_cast<usize>(count);
        }
        return offset == size;
    }

private:
    int32 mInput = -1;
    int32 mOutput = -1;
    bool mIsSocket = false;
};
/// One concurrently served connection lane: a dedicated thread and its own Slang session.
/// Slang permits concurrent compilation only through different global sessions.
/// The queue is declared last so that it drains its thread before the compiler it uses is destroyed.
struct ConnectionLane final
{
    compiler::Compiler OwnedCompiler;
    compiler::Compiler* CompilerOrNull = nullptr;
    uint32 Lanes = 1;
    int32 Peer = -1;
    bool IsBusy = false;
    threading::WorkerQueue Queue;
};
/// Serve one accepted connection on the lane's thread, then close it.
void ServeLaneConnection(void* contextOrNull) noexcept
{
    ConnectionLane& lane = *static_cast<ConnectionLane*>(contextOrNull);
    DescriptorStream stream{lane.Peer, lane.Peer, true};
    static_cast<void>(ProcessConnection(*lane.CompilerOrNull, stream, lane.Lanes));
    static_cast<void>(::close(lane.Peer));
}
/// Runs on the accept loop's thread once the lane's connection is closed.
void ReleaseLane(void* contextOrNull) noexcept
{
    ConnectionLane& lane = *static_cast<ConnectionLane*>(contextOrNull);
    lane.Peer = -1;
    lane.IsBusy = false;
}
/// Half the online CPUs, between one lane and MAX_CONNECTION_LANES: each lane is one compiler thread.
uint32 GetConnectionLaneCount() noexcept
{
    const int64 processors = ::sysconf(_SC_NPROCESSORS_ONLN);
    const uint32 half = processors > 1 ? static_cast<uint32>(processors / 2) : 1;
    return half < MAX_CONNECTION_LANES ? half : MAX_CONNECTION_LANES;
}
/// Prepare an accepted connection for nonblocking, signal-free transfers; false closes nothing.
bool ConfigurePeer(int32 peer) noexcept
{
    const int32 peerFlags = ::fcntl(peer, F_GETFL, 0);
    if (peerFlags < 0 || ::fcntl(peer, F_SETFL, peerFlags | O_NONBLOCK) < 0)
    {
        return false;
    }
#if defined(__APPLE__)
    const int32 shouldSuppressPipeSignal = 1;
    if (::setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &shouldSuppressPipeSignal, sizeof(shouldSuppressPipeSignal)) != 0)
    {
        return false;
    }
#endif
    return true;
}
/// Wait for an idle lane, starting another while fewer than inoutLaneLimit run. Completions are
/// delivered here, on the accept loop's thread. A lane that cannot start lowers inoutLaneLimit to
/// the lanes already running, which keep serving. Never fails while one lane is ready.
ConnectionLane& AcquireLane(StaticArray<ConnectionLane, MAX_CONNECTION_LANES>& inoutLanes,
                            uint32& inoutReadyLanes,
                            uint32& inoutLaneLimit) noexcept
{
    while (true)
    {
        for (uint32 i = 0; i < inoutReadyLanes; ++i)
        {
            while (inoutLanes[i].Queue.Poll() == true)
            {
            }
        }
        for (uint32 i = 0; i < inoutReadyLanes; ++i)
        {
            if (inoutLanes[i].IsBusy == false)
            {
                return inoutLanes[i];
            }
        }
        if (inoutReadyLanes < inoutLaneLimit)
        {
            ConnectionLane& added = inoutLanes[inoutReadyLanes];
            if (added.OwnedCompiler.Initialize() == compiler::Status::Ready &&
                added.Queue.Initialize() == threading::WorkerQueueStatus::Ready)
            {
                added.CompilerOrNull = &added.OwnedCompiler;
                added.Lanes = inoutLanes[0].Lanes;
                ++inoutReadyLanes;
                return added;
            }
            // Requests beyond the running lanes wait; the advertised concurrency still bounds how many.
            inoutLaneLimit = inoutReadyLanes;
        }
        const timespec wait{0, SATURATED_WAIT_NANOSECONDS};
        static_cast<void>(::nanosleep(&wait, nullptr));
    }
}
} // namespace
bool WriteVersion(std::string_view text) noexcept
{
    return ::write(STDOUT_FILENO, text.data(), text.size()) == static_cast<ssize_t>(text.size());
}
bool InstallStopHandlers() noexcept
{
    struct sigaction action{};
    action.sa_handler = HandleStop;
    sigemptyset(&action.sa_mask);
    return ::sigaction(SIGTERM, &action, nullptr) == 0 && ::sigaction(SIGINT, &action, nullptr) == 0;
}
// A worker launched with --stop-with-parent stops when the process that started it exits,
// including a crash or SIGKILL that never forwards SIGTERM. Otherwise an orphaned worker
// with a long compile would keep a CPU busy indefinitely.
bool WatchParent() noexcept
{
    const pid_t parent = ::getppid();
    if (parent <= 1)
    {
        return false;
    }
#if defined(__APPLE__)
    // The process-exit source runs its handler on a dispatch thread, so it fires during a compile.
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC,
                                                      static_cast<uintptr_t>(parent),
                                                      DISPATCH_PROC_EXIT,
                                                      dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (source == nullptr)
    {
        return false;
    }
    dispatch_source_set_event_handler_f(source, HandleParentExit);
    dispatch_resume(source);
#elif defined(__linux__)
    // prctl(2): the kernel sends the signal when the thread that created this process exits.
    if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0)
    {
        return false;
    }
#else
    return false;
#endif
    // A parent that exited before the watch was armed has already reparented this process.
    return ::getppid() == parent;
}
int32 ServeStandardStreams(compiler::Compiler& inoutCompiler) noexcept
{
    DescriptorStream stream{STDIN_FILENO, STDOUT_FILENO, false};
    while (ProcessConnection(inoutCompiler, stream, 1) == true)
    {
    }
    return 0;
}
int32 ServeEndpoint(compiler::Compiler& inoutCompiler, std::string_view endpoint) noexcept
{
    sockaddr_un& address = gAddress;
    if (endpoint.empty() == true || endpoint.size() >= sizeof(address.sun_path))
    {
        return EXIT_USAGE;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.data(), endpoint.size());
    const int32 listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0)
    {
        return EXIT_FAILURE_STATUS;
    }
    // Refuse occupied paths. Never unlink another worker's socket on startup.
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        static_cast<void>(::close(listener));
        return EXIT_FAILURE_STATUS;
    }
    gHasEndpoint = 1;
    if (::chmod(address.sun_path, S_IRUSR | S_IWUSR) != 0 || ::listen(listener, CONNECTION_BACKLOG) != 0)
    {
        static_cast<void>(::close(listener));
        static_cast<void>(::unlink(address.sun_path));
        return EXIT_FAILURE_STATUS;
    }
    uint32 laneLimit = GetConnectionLaneCount();
    StaticArray<ConnectionLane, MAX_CONNECTION_LANES> connectionLanes{};
    if (connectionLanes[0].Queue.Initialize() != threading::WorkerQueueStatus::Ready)
    {
        static_cast<void>(::close(listener));
        static_cast<void>(::unlink(address.sun_path));
        return EXIT_FAILURE_STATUS;
    }
    connectionLanes[0].CompilerOrNull = &inoutCompiler;
    connectionLanes[0].Lanes = laneLimit;
    uint32 readyLanes = 1;
    while (true)
    {
        const int32 peer = ::accept(listener, nullptr, nullptr);
        if (peer < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (ConfigurePeer(peer) == false)
        {
            static_cast<void>(::close(peer));
            continue;
        }
        ConnectionLane& lane = AcquireLane(connectionLanes, readyLanes, laneLimit);
        lane.Peer = peer;
        lane.IsBusy = true;
        if (lane.Queue.Submit(ServeLaneConnection, ReleaseLane, &lane) != threading::WorkerQueueStatus::Ready)
        {
            lane.Peer = -1;
            lane.IsBusy = false;
            static_cast<void>(::close(peer));
        }
    }
    for (uint32 i = 0; i < readyLanes; ++i)
    {
        static_cast<void>(connectionLanes[i].Queue.Shutdown());
    }
    static_cast<void>(::close(listener));
    static_cast<void>(::unlink(address.sun_path));
    return 0;
}
} // namespace ludus::tools::shader_worker
