#include <ludus/platform/hosted_window.h>

#include <new>

#if defined(LUDUS_PLATFORM_HOSTED)
#    include <ludus/foundation/containers/static_array.hpp>
#    include <ludus/foundation/time/time.hpp>
#    include <ludus/platform/entropy.h>

#    include "internal/hosted_endpoint_session.hpp"
#    include "internal/hosted_protocol.hpp"
#    include "internal/hosted_socket.hpp"

#    include <cerrno>
#    include <cstdlib>
#    include <cstring>
#    include <sys/socket.h>
#    include <sys/stat.h>
#    include <sys/un.h>
#    include <unistd.h>
#endif

namespace ludus::platform
{
#if defined(LUDUS_PLATFORM_HOSTED)
using foundation::int32;
using foundation::uint8;
using foundation::usize;

namespace
{
using hosted::SocketDescriptor;
using hosted::SocketPath;

/// mkdtemp(3) template appended to the parent directory.
constexpr std::string_view DIRECTORY_TEMPLATE = "/ludus-hosted-XXXXXX";
/// Socket file name inside the session directory.
constexpr std::string_view SOCKET_NAME = "/window";
/// Connections the kernel may hold before Pump() accepts them.
constexpr int32 LISTEN_BACKLOG = 4;
/// Refused peers accepted in one Pump() before yielding to the caller.
constexpr usize MAX_ACCEPTS_PER_PUMP = 8;
/// Owner-only socket file permissions inside the owner-only directory.
constexpr mode_t SOCKET_MODE = 0600;

bool IsValidVisibility(WindowVisibility visibility) noexcept
{
    return static_cast<uint8>(visibility) <= static_cast<uint8>(WindowVisibility::Minimized);
}
} // namespace

struct HostedWindowEndpoint::State final
{
    SocketPath Directory;
    SocketPath SocketFile;
    hosted::ChannelAddressText Address;
    SocketDescriptor Listener;
    bool IsSocketLinked = false;
    hosted::EndpointSession Session;

    /// Close the listener and remove the socket file so no other process can connect.
    void Unlink() noexcept
    {
        Listener.Reset();
        if (IsSocketLinked == true)
        {
            static_cast<void>(::unlink(SocketFile.GetData()));
            IsSocketLinked = false;
        }
    }
    ~State() noexcept
    {
        Session.Close();
        Unlink();
        if (Directory.IsEmpty() == false)
        {
            static_cast<void>(::rmdir(Directory.GetData()));
        }
    }
};
#else
struct HostedWindowEndpoint::State final
{
};
#endif

HostedWindowEndpoint::HostedWindowEndpoint() noexcept = default;

HostedWindowEndpoint::~HostedWindowEndpoint() noexcept
{
    Stop();
}

void HostedWindowEndpoint::Stop() noexcept
{
    mState.Reset();
}

#if defined(LUDUS_PLATFORM_HOSTED)
HostedEndpointStatus HostedWindowEndpoint::Start(const StartInfo& info) noexcept
{
    if (mState.Get() != nullptr)
    {
        return HostedEndpointStatus::InvalidState;
    }
    std::string_view parent = info.ParentDirectory;
    while (parent.size() > 1 && parent.back() == '/')
    {
        parent.remove_suffix(1);
    }
    if (parent.empty() == true || parent.front() != '/' || parent.find('\0') != std::string_view::npos ||
        parent.size() + DIRECTORY_TEMPLATE.size() + SOCKET_NAME.size() > hosted::MAX_SOCKET_PATH_BYTES ||
        info.Width > hosted::MAX_EXTENT_PIXELS || info.Height > hosted::MAX_EXTENT_PIXELS ||
        hosted::IsValidScale(info.Scale) == false || IsValidVisibility(info.Visibility) == false)
    {
        return HostedEndpointStatus::InvalidArgument;
    }
    if (parent == "/")
    {
        parent = {};
    }

    State* allocationOrNull = new (std::nothrow) State;
    foundation::core::UniquePtr<State> state(allocationOrNull);
    if (state.Get() == nullptr)
    {
        return HostedEndpointStatus::OutOfMemory;
    }
    hosted::Token token{};
    if (TryFillSystemEntropy(token.GetData(), token.GetSize()) == false)
    {
        return HostedEndpointStatus::OutOfMemory;
    }

    // mkdtemp(3) creates the directory with mode 0700 and fails rather than reuse a path.
    foundation::StaticArray<char, hosted::MAX_SOCKET_PATH_BYTES + 1> directory{};
    std::memcpy(directory.GetData(), parent.data(), parent.size());
    std::memcpy(directory.GetData() + parent.size(), DIRECTORY_TEMPLATE.data(), DIRECTORY_TEMPLATE.size());
    if (::mkdtemp(directory.GetData()) == nullptr ||
        state->Directory.TryAssign({directory.GetData(), parent.size() + DIRECTORY_TEMPLATE.size()}) !=
            foundation::StringStatus::Ok)
    {
        return HostedEndpointStatus::Unavailable;
    }
    if (state->SocketFile.TryAssign(state->Directory.GetView()) != foundation::StringStatus::Ok ||
        state->SocketFile.TryAppend(SOCKET_NAME) != foundation::StringStatus::Ok)
    {
        return HostedEndpointStatus::Unavailable;
    }

    sockaddr_un address{};
    static_assert(hosted::MAX_SOCKET_PATH_BYTES < sizeof(address.sun_path));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, state->SocketFile.GetData(), state->SocketFile.GetSize());
    state->Listener.Reset(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (state->Listener.IsOpen() == false || hosted::TryConfigureSocket(state->Listener.Get()) == false ||
        ::bind(state->Listener.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        return HostedEndpointStatus::Unavailable;
    }
    state->IsSocketLinked = true;
    if (::chmod(state->SocketFile.GetData(), SOCKET_MODE) != 0 || ::listen(state->Listener.Get(), LISTEN_BACKLOG) != 0)
    {
        return HostedEndpointStatus::Unavailable;
    }
    if (hosted::TryFormatChannelAddress(token, state->SocketFile.GetView(), state->Address) == false)
    {
        return HostedEndpointStatus::Unavailable;
    }
    hosted::MirroredWindowState initial;
    initial.Width = info.Width;
    initial.Height = info.Height;
    initial.Scale = info.Scale;
    initial.Visibility = info.Visibility;
    state->Session.Configure(token, hosted::GetCurrentUserId(), initial);
#    if defined(__APPLE__)
    // IOSurface send rights reach the editor through a per-session Mach rendezvous.
    hosted::RendezvousNonce nonce{};
    if (TryFillSystemEntropy(nonce.GetData(), nonce.GetSize()) == false)
    {
        return HostedEndpointStatus::OutOfMemory;
    }
    // A zero nonce means "no rendezvous"; one set bit keeps a random nonce nonzero.
    nonce[0] = static_cast<uint8>(nonce[0] | 1U);
    if (state->Session.TryStartRendezvous(nonce) == false)
    {
        return HostedEndpointStatus::Unavailable;
    }
#    endif
    mState = foundation::Move(state);
    return HostedEndpointStatus::Ok;
}

std::string_view HostedWindowEndpoint::GetChannelAddress() const noexcept
{
    return mState.Get() == nullptr ? std::string_view{} : mState->Address.GetView();
}

std::string_view HostedWindowEndpoint::GetDirectory() const noexcept
{
    return mState.Get() == nullptr ? std::string_view{} : mState->Directory.GetView();
}

HostedConnectionState HostedWindowEndpoint::GetState() const noexcept
{
    if (mState.Get() == nullptr)
    {
        return HostedConnectionState::Stopped;
    }
    switch (mState->Session.GetPhase())
    {
        case hosted::SessionPhase::Connected:
            return HostedConnectionState::Connected;
        case hosted::SessionPhase::Disconnected:
            return HostedConnectionState::Disconnected;
        case hosted::SessionPhase::Idle:
        case hosted::SessionPhase::Handshaking:
            break;
    }
    return HostedConnectionState::Listening;
}

HostedRefusal HostedWindowEndpoint::GetLastRefusal() const noexcept
{
    return mState.Get() == nullptr ? HostedRefusal::None : mState->Session.GetLastRefusal();
}

foundation::uint32 HostedWindowEndpoint::GetRefusalCount() const noexcept
{
    return mState.Get() == nullptr ? 0 : mState->Session.GetRefusalCount();
}

HostedEndpointStatus HostedWindowEndpoint::Pump() noexcept
{
    if (mState.Get() == nullptr)
    {
        return HostedEndpointStatus::InvalidState;
    }
    State& state = *mState;
    for (usize i = 0; i < MAX_ACCEPTS_PER_PUMP; ++i)
    {
        if (state.Listener.IsOpen() == false || state.Session.GetPhase() != hosted::SessionPhase::Idle)
        {
            break;
        }
        const int32 connection = ::accept(state.Listener.Get(), nullptr, nullptr);
        if (connection < 0 && errno == EINTR)
        {
            continue;
        }
        if (connection < 0)
        {
            break;
        }
        state.Session.Admit(SocketDescriptor(connection), foundation::time::Now());
        state.Session.Pump(foundation::time::Now());
    }
    state.Session.Pump(foundation::time::Now());
    if (state.Session.GetPhase() == hosted::SessionPhase::Connected ||
        state.Session.GetPhase() == hosted::SessionPhase::Disconnected)
    {
        state.Unlink();
    }
    return state.Session.GetPhase() == hosted::SessionPhase::Disconnected ? HostedEndpointStatus::NotConnected
                                                                          : HostedEndpointStatus::Ok;
}

HostedEndpointStatus HostedWindowEndpoint::SendResize(foundation::uint32 width,
                                                      foundation::uint32 height,
                                                      foundation::float32 scale) noexcept
{
    return mState.Get() == nullptr ? HostedEndpointStatus::InvalidState
                                   : mState->Session.SendResize(width, height, scale);
}

HostedEndpointStatus HostedWindowEndpoint::SendVisibility(WindowVisibility visibility) noexcept
{
    return mState.Get() == nullptr ? HostedEndpointStatus::InvalidState : mState->Session.SendVisibility(visibility);
}

HostedEndpointStatus HostedWindowEndpoint::SendFocusEntered(const input::FocusBaseline& baseline) noexcept
{
    return mState.Get() == nullptr ? HostedEndpointStatus::InvalidState : mState->Session.SendFocusEntered(baseline);
}

HostedEndpointStatus HostedWindowEndpoint::SendFocusLost() noexcept
{
    return mState.Get() == nullptr ? HostedEndpointStatus::InvalidState : mState->Session.SendFocusLost();
}

HostedEndpointStatus
HostedWindowEndpoint::SendKey(input::Key key, input::KeyTransition transition, bool isRepeat) noexcept
{
    return mState.Get() == nullptr ? HostedEndpointStatus::InvalidState
                                   : mState->Session.SendKey(key, transition, isRepeat);
}

HostedEndpointStatus HostedWindowEndpoint::SendClose() noexcept
{
    return mState.Get() == nullptr ? HostedEndpointStatus::InvalidState : mState->Session.SendClose();
}

bool HostedWindowEndpoint::TryTakeSurfaceEvent(HostedSurfaceEvent& outEvent) noexcept
{
    return mState.Get() != nullptr && mState->Session.TryTakeSurfaceEvent(outEvent) == true;
}

HostedEndpointStatus HostedWindowEndpoint::SendRelease(foundation::uint32 generation, foundation::uint32 image) noexcept
{
    return mState.Get() == nullptr ? HostedEndpointStatus::InvalidState
                                   : mState->Session.SendRelease(generation, image);
}
#else
// Targets without POSIX Unix sockets have no hosted-window channel.
HostedEndpointStatus HostedWindowEndpoint::Start([[maybe_unused]] const StartInfo& info) noexcept
{
    return HostedEndpointStatus::Unavailable;
}
std::string_view HostedWindowEndpoint::GetChannelAddress() const noexcept
{
    return {};
}
std::string_view HostedWindowEndpoint::GetDirectory() const noexcept
{
    return {};
}
HostedConnectionState HostedWindowEndpoint::GetState() const noexcept
{
    return HostedConnectionState::Stopped;
}
HostedRefusal HostedWindowEndpoint::GetLastRefusal() const noexcept
{
    return HostedRefusal::None;
}
foundation::uint32 HostedWindowEndpoint::GetRefusalCount() const noexcept
{
    return 0;
}
HostedEndpointStatus HostedWindowEndpoint::Pump() noexcept
{
    return HostedEndpointStatus::InvalidState;
}
HostedEndpointStatus HostedWindowEndpoint::SendResize([[maybe_unused]] foundation::uint32 width,
                                                      [[maybe_unused]] foundation::uint32 height,
                                                      [[maybe_unused]] foundation::float32 scale) noexcept
{
    return HostedEndpointStatus::InvalidState;
}
HostedEndpointStatus HostedWindowEndpoint::SendVisibility([[maybe_unused]] WindowVisibility visibility) noexcept
{
    return HostedEndpointStatus::InvalidState;
}
HostedEndpointStatus
HostedWindowEndpoint::SendFocusEntered([[maybe_unused]] const input::FocusBaseline& baseline) noexcept
{
    return HostedEndpointStatus::InvalidState;
}
HostedEndpointStatus HostedWindowEndpoint::SendFocusLost() noexcept
{
    return HostedEndpointStatus::InvalidState;
}
HostedEndpointStatus HostedWindowEndpoint::SendKey([[maybe_unused]] input::Key key,
                                                   [[maybe_unused]] input::KeyTransition transition,
                                                   [[maybe_unused]] bool isRepeat) noexcept
{
    return HostedEndpointStatus::InvalidState;
}
HostedEndpointStatus HostedWindowEndpoint::SendClose() noexcept
{
    return HostedEndpointStatus::InvalidState;
}
bool HostedWindowEndpoint::TryTakeSurfaceEvent([[maybe_unused]] HostedSurfaceEvent& outEvent) noexcept
{
    return false;
}
HostedEndpointStatus HostedWindowEndpoint::SendRelease([[maybe_unused]] foundation::uint32 generation,
                                                       [[maybe_unused]] foundation::uint32 image) noexcept
{
    return HostedEndpointStatus::InvalidState;
}
#endif
} // namespace ludus::platform
