#include "internal/window_hosted.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/input/keyboard_event.h>
#include <ludus/platform/keyboard_sink.h>
#include <ludus/platform/log_categories.h>
#include <ludus/platform/native_window.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <sys/socket.h>
#include <sys/un.h>

// The hosted window reproduces the Cocoa and Wayland event vocabulary from a
// host channel; see docs/architecture/editor-gui-systems.md, "The hosted window".
namespace ludus::platform::hosted
{
namespace
{
/// Receive-and-decode rounds per HandleEvent(), bounding one pump to 64 KiB.
constexpr usize MAX_RECEIVE_ROUNDS = 16;
constexpr foundation::uint64 NANOSECONDS_PER_MILLISECOND = 1'000'000;

class WindowHosted final : public WindowBase
{
public:
    WindowHosted(const CreateInfo& info, SocketDescriptor&& connection) noexcept
        : WindowBase(info), mConnection(foundation::Move(connection))
    {
        mNativeWindowInfo.System = WindowSystem::Hosted;
    }
    ~WindowHosted() noexcept override
    {
        closeWindow();
    }
    WindowHosted(const WindowHosted&) = delete;
    WindowHosted& operator=(const WindowHosted&) = delete;
    WindowHosted(WindowHosted&&) = delete;
    WindowHosted& operator=(WindowHosted&&) = delete;

    bool HandleEvent([[maybe_unused]] const Event& event) noexcept override
    {
        if (mIsClosed == true)
        {
            return false;
        }
        synchronizeSink();
        bool isChannelLost = processBuffered() == false;
        for (usize round = 0; round < MAX_RECEIVE_ROUNDS && isChannelLost == false && mIsClosed == false; ++round)
        {
            usize received = 0;
            const TransferStatus status =
                ReceiveSome(mConnection.Get(),
                            {mReceived.GetData() + mReceivedSize, mReceived.GetSize() - mReceivedSize},
                            received);
            if (status == TransferStatus::WouldBlock)
            {
                break;
            }
            if (status != TransferStatus::Progress)
            {
                isChannelLost = true;
                break;
            }
            mReceivedSize += received;
            isChannelLost = processBuffered() == false;
        }
        if (isChannelLost == true)
        {
            // Losing the channel is a close, exactly as closing a native window would be.
            closeWindow();
        }
        return mIsClosed == false;
    }

    /// Wait for the endpoint's answer to the Hello.
    [[nodiscard]] bool WaitForWelcome(uint32 timeoutMilliseconds, HostedRefusal& outRefusal) noexcept
    {
        const foundation::time::Timestamp start = foundation::time::Now();
        while (true)
        {
            Message message;
            usize consumed = 0;
            const DecodeStatus decoded = TryDecodeMessage({mReceived.GetData(), mReceivedSize}, message, consumed);
            if (decoded == DecodeStatus::Malformed)
            {
                return false;
            }
            if (decoded == DecodeStatus::Ok)
            {
                if (message.Kind == MessageKind::Refused)
                {
                    outRefusal = message.Refusal;
                    return false;
                }
                if (message.Kind != MessageKind::Welcome)
                {
                    return false;
                }
                consume(consumed);
                mHostWidth = message.Width;
                mHostHeight = message.Height;
                mNativeWindowInfo.Scale = message.Scale;
                mVisibility = message.Visibility;
                publishExtent();
                return true;
            }
            const foundation::uint64 limit =
                static_cast<foundation::uint64>(timeoutMilliseconds) * NANOSECONDS_PER_MILLISECOND;
            const foundation::uint64 elapsed = foundation::time::Now().Nanoseconds - start.Nanoseconds;
            if (elapsed >= limit || WaitUntilReadable(mConnection.Get(), {limit - elapsed}) == false)
            {
                return false;
            }
            usize received = 0;
            const TransferStatus status =
                ReceiveSome(mConnection.Get(),
                            {mReceived.GetData() + mReceivedSize, mReceived.GetSize() - mReceivedSize},
                            received);
            if (status == TransferStatus::Closed || status == TransferStatus::Failed)
            {
                return false;
            }
            if (status == TransferStatus::Progress)
            {
                mReceivedSize += received;
            }
        }
    }

private:
    /// Decode and apply every complete buffered message; false on a protocol violation.
    [[nodiscard]] bool processBuffered() noexcept
    {
        while (mIsClosed == false)
        {
            Message message;
            usize consumed = 0;
            const DecodeStatus decoded = TryDecodeMessage({mReceived.GetData(), mReceivedSize}, message, consumed);
            if (decoded == DecodeStatus::NeedMore)
            {
                return true;
            }
            if (decoded == DecodeStatus::Malformed)
            {
                return false;
            }
            consume(consumed);
            if (apply(message) == false)
            {
                return false;
            }
        }
        return true;
    }

    void consume(usize size) noexcept
    {
        std::memmove(mReceived.GetData(), mReceived.GetData() + size, mReceivedSize - size);
        mReceivedSize -= size;
    }

    [[nodiscard]] bool apply(const Message& message) noexcept
    {
        switch (message.Kind)
        {
            case MessageKind::Resize:
                mHostWidth = message.Width;
                mHostHeight = message.Height;
                mNativeWindowInfo.Scale = message.Scale;
                publishExtent();
                return true;
            case MessageKind::Visibility:
                mVisibility = message.Visibility;
                publishExtent();
                return true;
            case MessageKind::Reset:
                applyReset(message.Reason, message.Baseline);
                return true;
            case MessageKind::Key:
                applyKey(message);
                return true;
            case MessageKind::Close:
                closeWindow();
                return true;
            case MessageKind::Hello:
            case MessageKind::Welcome:
            case MessageKind::Refused:
                break;
        }
        // Handshake messages after the handshake violate the protocol.
        return false;
    }

    void applyReset(input::ResetReason reason, const input::FocusBaseline& baseline) noexcept
    {
        if (reason == input::ResetReason::FocusEntered)
        {
            mIsFocused = true;
            mHeldKeys = baseline;
            mHeldKeys.Focused = true;
            deliverReset(reason, mHeldKeys);
            return;
        }
        if (reason == input::ResetReason::FocusLost && mIsFocused == false)
        {
            return;
        }
        // FocusLost and Overflow both cancel every held key without release edges.
        mIsFocused = false;
        mHeldKeys = {};
        deliverReset(reason, mHeldKeys);
    }

    void applyKey(const Message& message) noexcept
    {
        if (mIsFocused == false)
        {
            return;
        }
        mHeldKeys.Keys[input::KeyIndex(message.PhysicalKey)] = message.Transition == input::KeyTransition::Down;
        input::KeyboardRecord record;
        record.Source = input::RecordSource::Native;
        record.PhysicalKey = message.PhysicalKey;
        record.Transition = message.Transition;
        record.Repeat = message.IsRepeat;
        const KeyboardSink sink = GetKeyboardSink();
        if (sink.OnRecord != nullptr)
        {
            sink.OnRecord(sink.UserData, record);
        }
    }

    void deliverReset(input::ResetReason reason, const input::FocusBaseline& baseline) const noexcept
    {
        const KeyboardSink sink = GetKeyboardSink();
        if (sink.OnReset != nullptr)
        {
            sink.OnReset(sink.UserData, reason, baseline);
        }
    }

    /// A newly attached sink receives the current focus state first, as Cocoa does.
    void synchronizeSink() noexcept
    {
        const KeyboardSink sink = GetKeyboardSink();
        if (sink.UserData != mSynchronizedSink.UserData || sink.OnRecord != mSynchronizedSink.OnRecord ||
            sink.OnReset != mSynchronizedSink.OnReset)
        {
            mSynchronizedSink = sink;
            deliverReset(mIsFocused == true ? input::ResetReason::FocusEntered : input::ResetReason::FocusLost,
                         mHeldKeys);
        }
    }

    /// A minimized window publishes a zero extent, as WindowVisibility::Minimized requires.
    void publishExtent() noexcept
    {
        const bool isMinimized = mVisibility == WindowVisibility::Minimized;
        mNativeWindowInfo.Width = isMinimized == true ? 0 : mHostWidth;
        mNativeWindowInfo.Height = isMinimized == true ? 0 : mHostHeight;
    }

    void closeWindow() noexcept
    {
        if (mIsClosed == true)
        {
            return;
        }
        mIsClosed = true;
        mIsFocused = false;
        mHeldKeys = {};
        mConnection.Reset();
        mNativeWindowInfo.Width = 0;
        mNativeWindowInfo.Height = 0;
        mVisibility = WindowVisibility::Occluded;
        deliverReset(input::ResetReason::WindowClosed, mHeldKeys);
    }

private:
    SocketDescriptor mConnection;
    foundation::StaticArray<uint8, RECEIVE_BUFFER_BYTES> mReceived = {};
    usize mReceivedSize = 0;
    uint32 mHostWidth = 0;
    uint32 mHostHeight = 0;
    bool mIsFocused = false;
    input::FocusBaseline mHeldKeys = {};
    KeyboardSink mSynchronizedSink = {};
    bool mIsClosed = false;
};

std::string_view GetRefusalName(HostedRefusal refusal) noexcept
{
    switch (refusal)
    {
        case HostedRefusal::None:
            return "none";
        case HostedRefusal::WrongUser:
            return "wrong user";
        case HostedRefusal::BadToken:
            return "bad token";
        case HostedRefusal::IncompatibleVersion:
            return "incompatible version";
        case HostedRefusal::MalformedHello:
            return "malformed hello";
        case HostedRefusal::HandshakeTimeout:
            return "handshake timeout";
    }
    return "unknown";
}

bool TryReadAddress(Token& outToken, SocketPath& outSocketPath) noexcept
{
    const char* valueOrNull = ::getenv(HOSTED_WINDOW_ENVIRONMENT_VARIABLE);
    return valueOrNull != nullptr && TryParseChannelAddress(valueOrNull, outToken, outSocketPath) == true;
}
} // namespace

bool IsRequested() noexcept
{
    return ::getenv(HOSTED_WINDOW_ENVIRONMENT_VARIABLE) != nullptr;
}

bool Initialize() noexcept
{
    Token token{};
    SocketPath socketPath;
    if (TryReadAddress(token, socketPath) == false)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM,
                        "{} does not hold a valid hosted-window channel address",
                        std::string_view(HOSTED_WINDOW_ENVIRONMENT_VARIABLE));
        return false;
    }
    return true;
}

bool TrySendHello(int32 descriptor, const Token& token) noexcept
{
    Message hello;
    hello.Kind = MessageKind::Hello;
    hello.Version = PROTOCOL_VERSION;
    hello.KeyCount = static_cast<uint16>(input::KEY_COUNT);
    hello.SessionToken = token;
    foundation::StaticArray<uint8, MAX_MESSAGE_BYTES> bytes{};
    usize size = 0;
    usize sent = 0;
    // A fresh Unix socket always has room for one small message, so one send suffices.
    return TryEncodeMessage(hello, {bytes.GetData(), bytes.GetSize()}, size) == true &&
           SendSome(descriptor, {bytes.GetData(), size}, sent) == TransferStatus::Progress && sent == size;
}

foundation::UniquePtr<Window> OpenWindow(SocketDescriptor&& connection,
                                         const Window::CreateInfo& info,
                                         uint32 timeoutMilliseconds,
                                         HostedRefusal& outRefusal) noexcept
{
    SocketDescriptor owned(foundation::Move(connection));
    if (owned.IsOpen() == false)
    {
        return nullptr;
    }
    WindowHosted* allocationOrNull = new (std::nothrow) WindowHosted(info, foundation::Move(owned));
    foundation::UniquePtr<WindowHosted> window(allocationOrNull);
    if (window.Get() == nullptr || window->WaitForWelcome(timeoutMilliseconds, outRefusal) == false)
    {
        return nullptr;
    }
    Window* base = window.Release();
    return foundation::UniquePtr<Window>(base);
}

foundation::UniquePtr<Window> CreateWindow(const Window::CreateInfo& info) noexcept
{
    Token token{};
    SocketPath socketPath;
    if (TryReadAddress(token, socketPath) == false)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM,
                        "{} does not hold a valid hosted-window channel address",
                        std::string_view(HOSTED_WINDOW_ENVIRONMENT_VARIABLE));
        return nullptr;
    }
    sockaddr_un address{};
    static_assert(MAX_SOCKET_PATH_BYTES < sizeof(address.sun_path));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socketPath.GetData(), socketPath.GetSize());
    SocketDescriptor connection(::socket(AF_UNIX, SOCK_STREAM, 0));
    // Connect before switching to nonblocking I/O: local admission completes at once.
    if (connection.IsOpen() == false ||
        ::connect(connection.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        TryConfigureSocket(connection.Get()) == false)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "Cannot connect to the hosted-window endpoint");
        return nullptr;
    }
    uint32 peerUserId = 0;
    if (TryReadPeerUserId(connection.Get(), peerUserId) == false || peerUserId != GetCurrentUserId())
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "The hosted-window endpoint runs under another user; refusing it");
        return nullptr;
    }
    if (TrySendHello(connection.Get(), token) == false)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM, "Cannot send the hosted-window handshake");
        return nullptr;
    }
    HostedRefusal refusal = HostedRefusal::None;
    foundation::UniquePtr<Window> window =
        OpenWindow(foundation::Move(connection), info, WELCOME_TIMEOUT_MILLISECONDS, refusal);
    if (window.Get() == nullptr)
    {
        LUDUS_LOG_ERROR(LOG_PLATFORM,
                        "The hosted-window endpoint did not accept this window ({})",
                        GetRefusalName(refusal));
    }
    return window;
}
} // namespace ludus::platform::hosted
