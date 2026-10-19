#pragma once

// Hosted windows: a platform::Window whose events come from a host channel
// instead of an OS window server. The editor owns a HostedWindowEndpoint and
// passes its channel address to the game process in
// HOSTED_WINDOW_ENVIRONMENT_VARIABLE; the game's WindowManager then creates a
// hosted window instead of a native one. The design and the channel protocol are
// owned by docs/architecture/editor-gui-systems.md, "The hosted window".

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/base/pointer.hpp>

#include <ludus/input/key.h>
#include <ludus/input/keyboard_event.h>
#include <ludus/platform/base/window.h>

#include <string_view>

namespace ludus::platform
{
/// @brief Environment variable that selects the hosted window backend.
///
/// Its value is the channel address that HostedWindowEndpoint::GetChannelAddress()
/// returns. When the variable is present, WindowManager::Initialize() and
/// WindowManager::CreateWindow() use the hosted backend and never open a native
/// window; an invalid value or a refused connection makes them fail instead of
/// falling back. When it is absent, the native backend runs unchanged.
/// It is a zero-terminated character array so it can be passed to both
/// `std::string_view` and C environment APIs.
inline constexpr char HOSTED_WINDOW_ENVIRONMENT_VARIABLE[] = "LUDUS_HOSTED_WINDOW";

/// @brief Why an endpoint refused a connecting window.
enum class HostedRefusal : foundation::uint8
{
    /// No refusal.
    None,
    /// The peer process runs under another user id.
    WrongUser,
    /// The Hello carried a token other than this session's.
    BadToken,
    /// The Hello carried another protocol version or key-vocabulary size.
    IncompatibleVersion,
    /// The first message was not a well-formed Hello.
    MalformedHello,
    /// No complete Hello arrived within the handshake timeout.
    HandshakeTimeout,
};

/// @brief Connection state of a HostedWindowEndpoint.
enum class HostedConnectionState : foundation::uint8
{
    /// Not started, stopped, or Start() failed.
    Stopped,
    /// The socket accepts connections; no window has been admitted yet.
    Listening,
    /// One authenticated window is connected. The socket no longer accepts connections.
    Connected,
    /// The admitted window disconnected or violated the protocol. The session is over.
    Disconnected,
};

/// @brief Result of a HostedWindowEndpoint operation.
enum class HostedEndpointStatus : foundation::uint8
{
    /// Done. For a Send call, the message is queued for the connected window.
    Ok,
    /// No window is connected. A state change (extent, visibility, focus, close) is
    /// still recorded and reaches the window when it is admitted; a key is dropped.
    NotConnected,
    /// The window has not drained the bounded send queue, so this message was
    /// dropped. Once the queue drains, the endpoint sends the current extent and
    /// visibility, an Overflow reset and the current focus baseline.
    Overflow,
    /// An argument is out of range: a zero or overlong path, an extent above the
    /// protocol limit, a non-finite or non-positive scale, an invalid key or
    /// visibility.
    InvalidArgument,
    /// The call does not apply now: the endpoint is stopped or already started, a key
    /// was sent without focus, or a message followed SendClose().
    InvalidState,
    /// The target has no hosted-window support (browser, Windows) or the operating
    /// system refused a directory or socket operation.
    Unavailable,
    /// The endpoint could not allocate its session state, or system entropy for the
    /// token was unavailable.
    OutOfMemory,
};

/// @brief Editor-side endpoint of one hosted-window session.
///
/// Start() creates a private directory (mode 0700) inside a caller-chosen parent,
/// listens on a Unix stream socket in it and draws a 32-byte random token. Pass
/// GetChannelAddress() to the game process as HOSTED_WINDOW_ENVIRONMENT_VARIABLE.
/// Pump() admits at most one window: the peer must run under this process's user
/// id and present the token and protocol version in its first message. Once a
/// window is admitted the socket file is removed, so no further process can
/// connect. Refused peers are disconnected and counted; the endpoint keeps
/// listening for the real window.
///
/// The Send calls mirror the native window vocabulary: extent in backing pixels
/// plus scale, visibility, focus enter (with the held-key baseline) and loss,
/// physical key transitions and close. Stop() or destruction closes the channel,
/// which the window reports as a close.
///
/// All I/O is nonblocking and bounded; no call waits for the game. The endpoint
/// owns no toolkit objects and has no thread affinity, but it is not
/// synchronized: use one instance from one thread at a time. Available on macOS
/// and Linux; elsewhere Start() returns Unavailable.
class HostedWindowEndpoint final
{
public:
    /// Session parameters for Start().
    struct StartInfo final
    {
        /// Existing absolute directory in which Start() creates the private session
        /// directory. Borrowed for the call. The resulting socket path must fit in
        /// 103 bytes, so prefer a short temporary directory.
        std::string_view ParentDirectory;
        /// Initial extent in backing pixels; at most 32768 per axis. Zero is allowed.
        foundation::uint32 Width = 0;
        /// Initial extent in backing pixels; at most 32768 per axis. Zero is allowed.
        foundation::uint32 Height = 0;
        /// Initial backing scale (backing pixels per logical point), in (0, 16].
        foundation::float32 Scale = 1.0F;
        /// Initial visibility.
        WindowVisibility Visibility = WindowVisibility::Visible;
    };

public:
    /// Construct a stopped endpoint. Does not allocate.
    HostedWindowEndpoint() noexcept;
    /// Stop the session, closing the channel and removing its directory.
    ~HostedWindowEndpoint() noexcept;
    HostedWindowEndpoint(const HostedWindowEndpoint&) = delete;
    HostedWindowEndpoint& operator=(const HostedWindowEndpoint&) = delete;
    HostedWindowEndpoint(HostedWindowEndpoint&&) = delete;
    HostedWindowEndpoint& operator=(HostedWindowEndpoint&&) = delete;

    /// @brief Create the session directory, socket and token, and start listening.
    /// @param info Session parameters; the initial state reaches the window in its Welcome.
    /// @return Ok and state Listening; InvalidState when already started; InvalidArgument
    /// for a relative, empty or overlong parent or an invalid initial state; OutOfMemory
    /// or Unavailable on resource failure. On failure the endpoint stays Stopped and
    /// leaves no directory behind.
    [[nodiscard]] HostedEndpointStatus Start(const StartInfo& info) noexcept;

    /// @brief End the session: close the channel, remove the socket and directory.
    ///
    /// A connected window observes channel loss and reports a close. Safe to call
    /// when stopped. The endpoint can be started again afterwards.
    void Stop() noexcept;

    /// @brief Channel address to publish in HOSTED_WINDOW_ENVIRONMENT_VARIABLE.
    ///
    /// Format "1:<64 hexadecimal token digits>:<absolute socket path>". The view
    /// points into the endpoint and stays valid until Stop() or destruction. It is
    /// a secret for the session: pass it only to the child's environment.
    /// @return The address, or an empty view when stopped.
    [[nodiscard]] std::string_view GetChannelAddress() const noexcept;

    /// @brief Absolute path of the private session directory, or empty when stopped.
    ///
    /// Valid until Stop() or destruction. Provided so callers can report or verify
    /// the location; the endpoint removes the directory itself.
    [[nodiscard]] std::string_view GetDirectory() const noexcept;

    /// Current connection state.
    [[nodiscard]] HostedConnectionState GetState() const noexcept;
    /// Reason for the most recent refusal, or None.
    [[nodiscard]] HostedRefusal GetLastRefusal() const noexcept;
    /// Number of peers refused since Start().
    [[nodiscard]] foundation::uint32 GetRefusalCount() const noexcept;

    /// @brief Admit a pending window, progress its handshake, flush queued messages
    /// and detect disconnection.
    ///
    /// Call regularly, for example once per editor frame. A handshake that does not
    /// complete within 5 seconds of the connection is refused as HandshakeTimeout.
    /// @return Ok while the session runs (Listening or Connected); NotConnected after
    /// the window disconnected; InvalidState when stopped.
    HostedEndpointStatus Pump() noexcept;

    /// @brief Publish a new extent in backing pixels and scale.
    /// @return Ok, NotConnected (recorded for the Welcome), Overflow, InvalidArgument or InvalidState.
    HostedEndpointStatus
    SendResize(foundation::uint32 width, foundation::uint32 height, foundation::float32 scale) noexcept;
    /// @brief Publish a visibility change; Minimized makes the window publish a zero extent.
    /// @return Ok, NotConnected (recorded for the Welcome), Overflow, InvalidArgument or InvalidState.
    HostedEndpointStatus SendVisibility(WindowVisibility visibility) noexcept;
    /// @brief Give the window keyboard focus with the keys currently held.
    ///
    /// The window delivers a FocusEntered reset carrying this baseline without
    /// press edges, as Cocoa and Wayland do. Calling it again while focused replaces
    /// the baseline. Invalid key slots in `baseline` are ignored.
    /// @return Ok, NotConnected (recorded for the Welcome), Overflow or InvalidState.
    HostedEndpointStatus SendFocusEntered(const input::FocusBaseline& baseline) noexcept;
    /// @brief Take keyboard focus away; the window delivers a FocusLost reset. No-op when unfocused.
    /// @return Ok, NotConnected (recorded for the Welcome), Overflow or InvalidState.
    HostedEndpointStatus SendFocusLost() noexcept;
    /// @brief Deliver one physical key transition to the focused window.
    /// @param key A valid physical key, usually from MapCocoaKeyCode() or MapEvdevKeyCode().
    /// @param transition Down or Up.
    /// @param isRepeat True for an operating-system auto-repeat Down.
    /// @return Ok; NotConnected (dropped); InvalidState without focus; InvalidArgument for
    /// an invalid key; Overflow.
    HostedEndpointStatus SendKey(input::Key key, input::KeyTransition transition, bool isRepeat) noexcept;
    /// @brief Ask the window to close, as clicking a native close button would.
    ///
    /// The window reports the close from its next HandleEvent(). Later Send calls
    /// return InvalidState.
    /// @return Ok, NotConnected (recorded for the Welcome), Overflow or InvalidState.
    HostedEndpointStatus SendClose() noexcept;

private:
    struct State;
    foundation::core::UniquePtr<State> mState;
};
} // namespace ludus::platform
