// Hosted-window channel tests. Most cases connect an EndpointSession and a
// hosted window over a socketpair, so every event, focus baseline, refusal and
// channel loss runs through the production codec, session and window without a
// listening socket. The final cases run WindowManager against a real
// HostedWindowEndpoint.

#include <ludus/foundation/base/pointer.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/input/key.h>
#include <ludus/input/keyboard_event.h>
#include <ludus/platform/base/window.h>
#include <ludus/platform/hosted_window.h>
#include <ludus/platform/keyboard_sink.h>
#include <ludus/platform/native_window.h>

#include "internal/hosted_endpoint_session.hpp"
#include "internal/hosted_protocol.hpp"
#include "internal/hosted_socket.hpp"
#include "internal/window_hosted.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sched.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

using ludus::foundation::float32;
using ludus::foundation::int32;
using ludus::foundation::uint16;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::UniquePtr;
using ludus::foundation::usize;
using ludus::foundation::time::Timestamp;
using ludus::input::FocusBaseline;
using ludus::input::Key;
using ludus::input::KeyboardRecord;
using ludus::input::KeyTransition;
using ludus::input::ResetReason;
using ludus::platform::HostedConnectionState;
using ludus::platform::HostedEndpointStatus;
using ludus::platform::HostedRefusal;
using ludus::platform::HostedWindowEndpoint;
using ludus::platform::KeyboardSink;
using ludus::platform::Window;
using ludus::platform::WindowManager;
using ludus::platform::WindowSystem;
using ludus::platform::WindowVisibility;

namespace hosted = ludus::platform::hosted;

namespace
{
constexpr usize MAX_RECORDED = 16;
constexpr uint32 OPEN_TIMEOUT_MILLISECONDS = 2000;
constexpr uint64 NANOSECONDS_PER_SECOND = 1'000'000'000;
constexpr uint64 PUMP_DEADLINE_NANOSECONDS = 10 * NANOSECONDS_PER_SECOND;
constexpr usize FLOOD_LIMIT = 1'000'000;
constexpr usize DRAIN_ROUNDS = 100'000;

struct ResetEntry final
{
    ResetReason Reason = ResetReason::None;
    FocusBaseline Baseline = {};
};

/// Records what a hosted window delivers to its keyboard sink.
struct SinkRecorder final
{
    ludus::foundation::StaticArray<KeyboardRecord, MAX_RECORDED> Records{};
    usize RecordCount = 0;
    KeyboardRecord LastRecord = {};
    ludus::foundation::StaticArray<ResetEntry, MAX_RECORDED> Resets{};
    usize ResetCount = 0;

    static void OnRecord(void* userData, const KeyboardRecord& record) noexcept
    {
        SinkRecorder& recorder = *static_cast<SinkRecorder*>(userData);
        if (recorder.RecordCount < MAX_RECORDED)
        {
            recorder.Records[recorder.RecordCount] = record;
        }
        recorder.LastRecord = record;
        ++recorder.RecordCount;
    }
    static void OnReset(void* userData, ResetReason reason, const FocusBaseline& baseline) noexcept
    {
        SinkRecorder& recorder = *static_cast<SinkRecorder*>(userData);
        if (recorder.ResetCount < MAX_RECORDED)
        {
            recorder.Resets[recorder.ResetCount] = { .Reason = reason, .Baseline = baseline };
        }
        ++recorder.ResetCount;
    }
    [[nodiscard]] KeyboardSink GetSink() noexcept
    {
        return { .OnRecord = &OnRecord, .OnReset = &OnReset, .UserData = this };
    }
    [[nodiscard]] const ResetEntry& GetLastReset() const noexcept
    {
        return Resets[(ResetCount < MAX_RECORDED ? ResetCount : MAX_RECORDED) - 1];
    }
};

struct SocketPair final
{
    hosted::SocketDescriptor EndpointSide;
    hosted::SocketDescriptor WindowSide;
};

SocketPair MakeSocketPair()
{
    int32 descriptors[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    SocketPair pair;
    pair.EndpointSide.Reset(descriptors[0]);
    pair.WindowSide.Reset(descriptors[1]);
    REQUIRE(hosted::TryConfigureSocket(pair.WindowSide.Get()));
    return pair;
}

hosted::Token MakeToken(uint8 seed)
{
    hosted::Token token{};
    for (usize i = 0; i < hosted::TOKEN_BYTES; ++i)
    {
        token[i] = static_cast<uint8>(seed + i);
    }
    return token;
}

hosted::MirroredWindowState MakeInitialState()
{
    hosted::MirroredWindowState state;
    state.Width = 640;
    state.Height = 360;
    state.Scale = 2.0F;
    state.Visibility = WindowVisibility::Visible;
    return state;
}

Window::CreateInfo MakeCreateInfo()
{
    Window::CreateInfo info;
    info.Name = "hosted test";
    return info;
}

void WriteMessage(int32 descriptor, const hosted::Message& message)
{
    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> bytes{};
    usize size = 0;
    REQUIRE(hosted::TryEncodeMessage(message, {bytes.GetData(), bytes.GetSize()}, size));
    usize sent = 0;
    REQUIRE(hosted::SendSome(descriptor, {bytes.GetData(), size}, sent) == hosted::TransferStatus::Progress);
    REQUIRE(sent == size);
}

FocusBaseline MakeBaseline(Key first, Key second)
{
    FocusBaseline baseline;
    baseline.Focused = true;
    baseline.Keys[ludus::input::KeyIndex(first)] = true;
    baseline.Keys[ludus::input::KeyIndex(second)] = true;
    return baseline;
}

usize CountHeld(const FocusBaseline& baseline)
{
    usize count = 0;
    for (const bool isHeld : baseline.Keys)
    {
        count += isHeld == true ? 1 : 0;
    }
    return count;
}

Timestamp At(uint64 nanoseconds)
{
    return {nanoseconds};
}

/// A session connected to a hosted window over a socketpair.
struct ConnectedPair final
{
    // Declared first so it outlives the window attached to it.
    SinkRecorder Recorder;
    hosted::EndpointSession Session;
    UniquePtr<Window> HostedWindow;
};

/// Admit one end of a socketpair, send the window's Hello and open the window.
void Connect(ConnectedPair& outPair, const hosted::Token& windowToken, HostedRefusal& outRefusal)
{
    SocketPair pair = MakeSocketPair();
    REQUIRE(hosted::TrySendHello(pair.WindowSide.Get(), windowToken));
    outPair.Session.Admit(ludus::foundation::Move(pair.EndpointSide), At(0));
    outPair.Session.Pump(At(1));
    outPair.HostedWindow = hosted::OpenWindow(ludus::foundation::Move(pair.WindowSide),
                                              MakeCreateInfo(),
                                              OPEN_TIMEOUT_MILLISECONDS,
                                              outRefusal);
}

void ConnectAndAttach(ConnectedPair& outPair)
{
    const hosted::Token token = MakeToken(7);
    outPair.Session.Configure(token, hosted::GetCurrentUserId(), MakeInitialState());
    HostedRefusal refusal = HostedRefusal::None;
    Connect(outPair, token, refusal);
    REQUIRE(refusal == HostedRefusal::None);
    REQUIRE(outPair.HostedWindow.Get() != nullptr);
    REQUIRE(outPair.Session.GetPhase() == hosted::SessionPhase::Connected);
    outPair.HostedWindow->AttachKeyboardSink(outPair.Recorder.GetSink());
    // The first pump synchronizes the new sink with the unfocused state.
    REQUIRE(outPair.HostedWindow->HandleEvent({}));
    REQUIRE(outPair.Recorder.ResetCount == 1);
    REQUIRE(outPair.Recorder.Resets[0].Reason == ResetReason::FocusLost);
}
} // namespace

TEST_CASE("Hosted messages round-trip through the codec", "[platform][hosted]")
{
    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> bytes{};
    hosted::Message resize;
    resize.Kind = hosted::MessageKind::Resize;
    resize.Width = 1920;
    resize.Height = 1080;
    resize.Scale = 1.25F;
    usize size = 0;
    REQUIRE(hosted::TryEncodeMessage(resize, {bytes.GetData(), bytes.GetSize()}, size));
    CHECK(size == hosted::HEADER_BYTES + hosted::RESIZE_PAYLOAD_BYTES);

    hosted::Message decoded;
    usize consumed = 0;
    CHECK(hosted::TryDecodeMessage({bytes.GetData(), size - 1}, decoded, consumed) == hosted::DecodeStatus::NeedMore);
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
    CHECK(consumed == size);
    CHECK(decoded.Kind == hosted::MessageKind::Resize);
    CHECK(decoded.Width == 1920);
    CHECK(decoded.Height == 1080);
    CHECK(decoded.Scale == 1.25F);

    hosted::Message reset;
    reset.Kind = hosted::MessageKind::Reset;
    reset.Reason = ResetReason::FocusEntered;
    reset.Baseline = MakeBaseline(Key::KeyW, Key::ShiftRight);
    REQUIRE(hosted::TryEncodeMessage(reset, {bytes.GetData(), bytes.GetSize()}, size));
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
    CHECK(decoded.Reason == ResetReason::FocusEntered);
    CHECK(decoded.Baseline.Focused);
    CHECK(decoded.Baseline.Keys[ludus::input::KeyIndex(Key::KeyW)]);
    CHECK(decoded.Baseline.Keys[ludus::input::KeyIndex(Key::ShiftRight)]);
    CHECK(CountHeld(decoded.Baseline) == 2);

    hosted::Message key;
    key.Kind = hosted::MessageKind::Key;
    key.PhysicalKey = Key::Space;
    key.Transition = KeyTransition::Down;
    key.IsRepeat = true;
    REQUIRE(hosted::TryEncodeMessage(key, {bytes.GetData(), bytes.GetSize()}, size));
    REQUIRE(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Ok);
    CHECK(decoded.PhysicalKey == Key::Space);
    CHECK(decoded.Transition == KeyTransition::Down);
    CHECK(decoded.IsRepeat);
}

TEST_CASE("The hosted codec rejects out-of-range and malformed messages", "[platform][hosted]")
{
    ludus::foundation::StaticArray<uint8, hosted::MAX_MESSAGE_BYTES> bytes{};
    usize size = 0;
    hosted::Message resize;
    resize.Kind = hosted::MessageKind::Resize;
    resize.Width = 100;
    resize.Height = 100;
    for (const float32 scale :
         {0.0F, -1.0F, 17.0F, std::numeric_limits<float32>::quiet_NaN(), std::numeric_limits<float32>::infinity()})
    {
        resize.Scale = scale;
        CHECK_FALSE(hosted::TryEncodeMessage(resize, {bytes.GetData(), bytes.GetSize()}, size));
    }
    resize.Scale = 1.0F;
    resize.Width = hosted::MAX_EXTENT_PIXELS + 1;
    CHECK_FALSE(hosted::TryEncodeMessage(resize, {bytes.GetData(), bytes.GetSize()}, size));

    hosted::Message key;
    key.Kind = hosted::MessageKind::Key;
    key.PhysicalKey = Key::Unknown;
    CHECK_FALSE(hosted::TryEncodeMessage(key, {bytes.GetData(), bytes.GetSize()}, size));

    hosted::Message reset;
    reset.Kind = hosted::MessageKind::Reset;
    reset.Reason = ResetReason::WindowClosed;
    CHECK_FALSE(hosted::TryEncodeMessage(reset, {bytes.GetData(), bytes.GetSize()}, size));

    hosted::Message decoded;
    usize consumed = 0;
    // Unknown kind.
    const uint8 unknownKind[] = {0x63, 0x00, 0x00, 0x00};
    CHECK(hosted::TryDecodeMessage({unknownKind, sizeof(unknownKind)}, decoded, consumed) ==
          hosted::DecodeStatus::Malformed);
    // Close with a payload.
    const uint8 longClose[] = {0x08, 0x00, 0x01, 0x00, 0x00};
    CHECK(hosted::TryDecodeMessage({longClose, sizeof(longClose)}, decoded, consumed) ==
          hosted::DecodeStatus::Malformed);
    // A frame longer than any version may send.
    const uint8 hugeFrame[] = {0x01, 0x00, 0xFF, 0xFF};
    CHECK(hosted::TryDecodeMessage({hugeFrame, sizeof(hugeFrame)}, decoded, consumed) ==
          hosted::DecodeStatus::Malformed);

    // A FocusEntered mask with the reserved Unknown bit, and a FocusLost carrying keys.
    reset.Reason = ResetReason::FocusEntered;
    reset.Baseline = MakeBaseline(Key::KeyA, Key::KeyB);
    REQUIRE(hosted::TryEncodeMessage(reset, {bytes.GetData(), bytes.GetSize()}, size));
    bytes[hosted::HEADER_BYTES + 1] = static_cast<uint8>(bytes[hosted::HEADER_BYTES + 1] | 0x01);
    CHECK(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Malformed);
    bytes[hosted::HEADER_BYTES + 1] = static_cast<uint8>(bytes[hosted::HEADER_BYTES + 1] & 0xFE);
    bytes[hosted::HEADER_BYTES] = static_cast<uint8>(ResetReason::FocusLost);
    CHECK(hosted::TryDecodeMessage({bytes.GetData(), size}, decoded, consumed) == hosted::DecodeStatus::Malformed);
}

TEST_CASE("Hosted channel addresses carry a token and an absolute socket path", "[platform][hosted]")
{
    const hosted::Token token = MakeToken(0xA0);
    hosted::ChannelAddressText address;
    REQUIRE(hosted::TryFormatChannelAddress(token, "/tmp/ludus-hosted-abc/window", address));
    CHECK(address.GetSize() == hosted::ADDRESS_PREFIX.size() + hosted::TOKEN_HEX_BYTES + 1 + 28);
    CHECK(address.GetView().starts_with("1:a0a1a2a3"));

    hosted::Token parsed{};
    hosted::SocketPath path;
    REQUIRE(hosted::TryParseChannelAddress(address.GetView(), parsed, path));
    CHECK(hosted::AreTokensEqual(parsed, token));
    CHECK(path.GetView() == "/tmp/ludus-hosted-abc/window");

    CHECK_FALSE(hosted::TryFormatChannelAddress(token, "relative/window", address));
    CHECK_FALSE(hosted::TryParseChannelAddress("2:00:/tmp/x", parsed, path));
    CHECK_FALSE(hosted::TryParseChannelAddress("1:abc:/tmp/x", parsed, path));
    const std::string_view view = address.GetView();
    const usize pathStart = hosted::ADDRESS_PREFIX.size() + hosted::TOKEN_HEX_BYTES + 1;
    hosted::ChannelAddressText relative;
    REQUIRE(relative.TryAssign(view.substr(0, pathStart)) == ludus::foundation::StringStatus::Ok);
    REQUIRE(relative.TryAppend("tmp/x") == ludus::foundation::StringStatus::Ok);
    CHECK_FALSE(hosted::TryParseChannelAddress(relative.GetView(), parsed, path));
    // Token digits are lowercase only.
    hosted::ChannelAddressText uppercase;
    REQUIRE(uppercase.TryAssign("1:A0") == ludus::foundation::StringStatus::Ok);
    REQUIRE(uppercase.TryAppend(view.substr(4)) == ludus::foundation::StringStatus::Ok);
    CHECK_FALSE(hosted::TryParseChannelAddress(uppercase.GetView(), parsed, path));
}

TEST_CASE("The Welcome publishes the host's extent, scale and visibility", "[platform][hosted]")
{
    ConnectedPair pair;
    ConnectAndAttach(pair);
    const ludus::platform::NativeWindowInfo info = pair.HostedWindow->GetNativeWindowInfo();
    CHECK(info.System == WindowSystem::Hosted);
    CHECK(info.Width == 640);
    CHECK(info.Height == 360);
    CHECK(info.Scale == 2.0F);
    CHECK(info.CocoaWindow == nullptr);
    CHECK(info.Surface == nullptr);
    CHECK(pair.HostedWindow->GetVisibility() == WindowVisibility::Visible);
}

TEST_CASE("Hosted resize publishes backing pixels and scale", "[platform][hosted]")
{
    ConnectedPair pair;
    ConnectAndAttach(pair);
    CHECK(pair.Session.SendResize(1280, 720, 1.5F) == HostedEndpointStatus::Ok);
    REQUIRE(pair.HostedWindow->HandleEvent({}));
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Width == 1280);
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Height == 720);
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Scale == 1.5F);
    CHECK(pair.Session.SendResize(1, 1, 0.0F) == HostedEndpointStatus::InvalidArgument);
    CHECK(pair.Session.SendResize(hosted::MAX_EXTENT_PIXELS + 1, 1, 1.0F) == HostedEndpointStatus::InvalidArgument);
}

TEST_CASE("Hosted visibility follows the host, and minimization publishes a zero extent", "[platform][hosted]")
{
    ConnectedPair pair;
    ConnectAndAttach(pair);
    CHECK(pair.Session.SendVisibility(WindowVisibility::Occluded) == HostedEndpointStatus::Ok);
    REQUIRE(pair.HostedWindow->HandleEvent({}));
    CHECK(pair.HostedWindow->GetVisibility() == WindowVisibility::Occluded);
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Width == 640);

    CHECK(pair.Session.SendVisibility(WindowVisibility::Minimized) == HostedEndpointStatus::Ok);
    REQUIRE(pair.HostedWindow->HandleEvent({}));
    CHECK(pair.HostedWindow->GetVisibility() == WindowVisibility::Minimized);
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Width == 0);
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Height == 0);

    CHECK(pair.Session.SendVisibility(WindowVisibility::Visible) == HostedEndpointStatus::Ok);
    REQUIRE(pair.HostedWindow->HandleEvent({}));
    CHECK(pair.HostedWindow->GetVisibility() == WindowVisibility::Visible);
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Width == 640);
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Height == 360);
    CHECK(pair.Session.SendVisibility(static_cast<WindowVisibility>(9)) == HostedEndpointStatus::InvalidArgument);
}

TEST_CASE("Hosted focus enter carries the held-key baseline without press edges", "[platform][hosted]")
{
    ConnectedPair pair;
    ConnectAndAttach(pair);
    CHECK(pair.Session.SendFocusEntered(MakeBaseline(Key::KeyW, Key::ShiftLeft)) == HostedEndpointStatus::Ok);
    REQUIRE(pair.HostedWindow->HandleEvent({}));
    REQUIRE(pair.Recorder.ResetCount == 2);
    const ResetEntry& entered = pair.Recorder.Resets[1];
    CHECK(entered.Reason == ResetReason::FocusEntered);
    CHECK(entered.Baseline.Focused);
    CHECK(entered.Baseline.Keys[ludus::input::KeyIndex(Key::KeyW)]);
    CHECK(entered.Baseline.Keys[ludus::input::KeyIndex(Key::ShiftLeft)]);
    CHECK(CountHeld(entered.Baseline) == 2);
    CHECK(pair.Recorder.RecordCount == 0);

    CHECK(pair.Session.SendFocusLost() == HostedEndpointStatus::Ok);
    REQUIRE(pair.HostedWindow->HandleEvent({}));
    REQUIRE(pair.Recorder.ResetCount == 3);
    CHECK(pair.Recorder.Resets[2].Reason == ResetReason::FocusLost);
    CHECK_FALSE(pair.Recorder.Resets[2].Baseline.Focused);
    CHECK(CountHeld(pair.Recorder.Resets[2].Baseline) == 0);

    // A repeated loss is a no-op, as for a native window that is already unfocused.
    CHECK(pair.Session.SendFocusLost() == HostedEndpointStatus::Ok);
    REQUIRE(pair.HostedWindow->HandleEvent({}));
    CHECK(pair.Recorder.ResetCount == 3);
}

TEST_CASE("A sink attached later receives the current hosted focus baseline", "[platform][hosted]")
{
    ConnectedPair pair;
    ConnectAndAttach(pair);
    CHECK(pair.Session.SendFocusEntered(MakeBaseline(Key::KeyA, Key::KeyD)) == HostedEndpointStatus::Ok);
    CHECK(pair.Session.SendKey(Key::KeyD, KeyTransition::Up, false) == HostedEndpointStatus::Ok);
    REQUIRE(pair.HostedWindow->HandleEvent({}));

    SinkRecorder later;
    pair.HostedWindow->AttachKeyboardSink(later.GetSink());
    REQUIRE(pair.HostedWindow->HandleEvent({}));
    REQUIRE(later.ResetCount == 1);
    CHECK(later.Resets[0].Reason == ResetReason::FocusEntered);
    CHECK(later.Resets[0].Baseline.Keys[ludus::input::KeyIndex(Key::KeyA)]);
    CHECK_FALSE(later.Resets[0].Baseline.Keys[ludus::input::KeyIndex(Key::KeyD)]);
    pair.HostedWindow->DetachKeyboardSink();
}

TEST_CASE("Hosted key transitions reach the attached sink as native records", "[platform][hosted]")
{
    ConnectedPair pair;
    ConnectAndAttach(pair);
    // Without focus the endpoint refuses keys, as a native window ignores them.
    CHECK(pair.Session.SendKey(Key::KeyW, KeyTransition::Down, false) == HostedEndpointStatus::InvalidState);
    CHECK(pair.Session.SendFocusEntered({}) == HostedEndpointStatus::Ok);
    CHECK(pair.Session.SendKey(Key::KeyW, KeyTransition::Down, false) == HostedEndpointStatus::Ok);
    CHECK(pair.Session.SendKey(Key::KeyW, KeyTransition::Down, true) == HostedEndpointStatus::Ok);
    CHECK(pair.Session.SendKey(Key::KeyW, KeyTransition::Up, false) == HostedEndpointStatus::Ok);
    CHECK(pair.Session.SendKey(Key::Unknown, KeyTransition::Down, false) == HostedEndpointStatus::InvalidArgument);
    REQUIRE(pair.HostedWindow->HandleEvent({}));
    REQUIRE(pair.Recorder.RecordCount == 3);
    for (usize i = 0; i < 3; ++i)
    {
        CHECK(pair.Recorder.Records[i].Source == ludus::input::RecordSource::Native);
        CHECK(pair.Recorder.Records[i].PhysicalKey == Key::KeyW);
        CHECK_FALSE(pair.Recorder.Records[i].HasNativeTime);
    }
    CHECK(pair.Recorder.Records[0].Transition == KeyTransition::Down);
    CHECK_FALSE(pair.Recorder.Records[0].Repeat);
    CHECK(pair.Recorder.Records[1].Repeat);
    CHECK(pair.Recorder.Records[2].Transition == KeyTransition::Up);
}

TEST_CASE("A hosted window ignores keys that arrive without focus", "[platform][hosted]")
{
    SocketPair pair = MakeSocketPair();
    hosted::Message welcome;
    welcome.Kind = hosted::MessageKind::Welcome;
    welcome.Width = 10;
    welcome.Height = 10;
    WriteMessage(pair.EndpointSide.Get(), welcome);
    hosted::Message key;
    key.Kind = hosted::MessageKind::Key;
    key.PhysicalKey = Key::KeyQ;
    WriteMessage(pair.EndpointSide.Get(), key);
    // Declared before the window so it outlives the attached sink.
    SinkRecorder recorder;
    HostedRefusal refusal = HostedRefusal::None;
    UniquePtr<Window> window = hosted::OpenWindow(ludus::foundation::Move(pair.WindowSide),
                                                  MakeCreateInfo(),
                                                  OPEN_TIMEOUT_MILLISECONDS,
                                                  refusal);
    REQUIRE(window.Get() != nullptr);
    window->AttachKeyboardSink(recorder.GetSink());
    REQUIRE(window->HandleEvent({}));
    CHECK(recorder.RecordCount == 0);
}

TEST_CASE("A hosted close is reported like a native close", "[platform][hosted]")
{
    ConnectedPair pair;
    ConnectAndAttach(pair);
    CHECK(pair.Session.SendFocusEntered(MakeBaseline(Key::KeyW, Key::KeyS)) == HostedEndpointStatus::Ok);
    CHECK(pair.Session.SendClose() == HostedEndpointStatus::Ok);
    CHECK(pair.Session.SendResize(1, 1, 1.0F) == HostedEndpointStatus::InvalidState);
    CHECK(pair.Session.SendClose() == HostedEndpointStatus::InvalidState);
    CHECK_FALSE(pair.HostedWindow->HandleEvent({}));
    const ResetEntry& closed = pair.Recorder.GetLastReset();
    CHECK(closed.Reason == ResetReason::WindowClosed);
    CHECK_FALSE(closed.Baseline.Focused);
    CHECK(CountHeld(closed.Baseline) == 0);
    CHECK(pair.HostedWindow->GetVisibility() == WindowVisibility::Occluded);
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Width == 0);
    CHECK_FALSE(pair.HostedWindow->HandleEvent({}));

    // The window released the channel, so the endpoint observes the disconnection.
    pair.Session.Pump(At(2));
    CHECK(pair.Session.GetPhase() == hosted::SessionPhase::Disconnected);
}

TEST_CASE("Losing the hosted channel delivers a close after the queued events", "[platform][hosted]")
{
    ConnectedPair pair;
    ConnectAndAttach(pair);
    CHECK(pair.Session.SendFocusEntered({}) == HostedEndpointStatus::Ok);
    CHECK(pair.Session.SendKey(Key::Enter, KeyTransition::Down, false) == HostedEndpointStatus::Ok);
    pair.Session.Close();
    CHECK(pair.Session.GetPhase() == hosted::SessionPhase::Disconnected);
    CHECK(pair.Session.SendKey(Key::Enter, KeyTransition::Up, false) == HostedEndpointStatus::NotConnected);

    CHECK_FALSE(pair.HostedWindow->HandleEvent({}));
    REQUIRE(pair.Recorder.RecordCount == 1);
    CHECK(pair.Recorder.Records[0].PhysicalKey == Key::Enter);
    CHECK(pair.Recorder.GetLastReset().Reason == ResetReason::WindowClosed);
}

TEST_CASE("A hosted protocol violation closes the window", "[platform][hosted]")
{
    SocketPair pair = MakeSocketPair();
    hosted::Message welcome;
    welcome.Kind = hosted::MessageKind::Welcome;
    WriteMessage(pair.EndpointSide.Get(), welcome);
    // Declared before the window so it outlives the attached sink.
    SinkRecorder recorder;
    HostedRefusal refusal = HostedRefusal::None;
    UniquePtr<Window> window = hosted::OpenWindow(ludus::foundation::Move(pair.WindowSide),
                                                  MakeCreateInfo(),
                                                  OPEN_TIMEOUT_MILLISECONDS,
                                                  refusal);
    REQUIRE(window.Get() != nullptr);
    window->AttachKeyboardSink(recorder.GetSink());
    // A second Welcome after the handshake.
    WriteMessage(pair.EndpointSide.Get(), welcome);
    CHECK_FALSE(window->HandleEvent({}));
    CHECK(recorder.GetLastReset().Reason == ResetReason::WindowClosed);
}

TEST_CASE("The endpoint observes a destroyed hosted window as a disconnection", "[platform][hosted]")
{
    ConnectedPair pair;
    ConnectAndAttach(pair);
    pair.HostedWindow.Reset();
    CHECK(pair.Recorder.GetLastReset().Reason == ResetReason::WindowClosed);
    pair.Session.Pump(At(2));
    CHECK(pair.Session.GetPhase() == hosted::SessionPhase::Disconnected);
    CHECK(pair.Session.SendResize(10, 10, 1.0F) == HostedEndpointStatus::NotConnected);
}

TEST_CASE("State recorded before admission reaches the window in its handshake", "[platform][hosted]")
{
    ConnectedPair pair;
    const hosted::Token token = MakeToken(3);
    pair.Session.Configure(token, hosted::GetCurrentUserId(), MakeInitialState());
    CHECK(pair.Session.SendResize(800, 600, 1.0F) == HostedEndpointStatus::NotConnected);
    CHECK(pair.Session.SendVisibility(WindowVisibility::Occluded) == HostedEndpointStatus::NotConnected);
    CHECK(pair.Session.SendFocusEntered(MakeBaseline(Key::Space, Key::KeyE)) == HostedEndpointStatus::NotConnected);
    CHECK(pair.Session.SendKey(Key::KeyE, KeyTransition::Up, false) == HostedEndpointStatus::NotConnected);
    HostedRefusal refusal = HostedRefusal::None;
    Connect(pair, token, refusal);
    REQUIRE(pair.HostedWindow.Get() != nullptr);
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Width == 800);
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Scale == 1.0F);
    CHECK(pair.HostedWindow->GetVisibility() == WindowVisibility::Occluded);

    pair.HostedWindow->AttachKeyboardSink(pair.Recorder.GetSink());
    REQUIRE(pair.HostedWindow->HandleEvent({}));
    const ResetEntry& entered = pair.Recorder.GetLastReset();
    CHECK(entered.Reason == ResetReason::FocusEntered);
    CHECK(entered.Baseline.Keys[ludus::input::KeyIndex(Key::Space)]);
    CHECK_FALSE(entered.Baseline.Keys[ludus::input::KeyIndex(Key::KeyE)]);
}

TEST_CASE("The endpoint refuses a hosted window with the wrong token", "[platform][hosted]")
{
    ConnectedPair pair;
    pair.Session.Configure(MakeToken(1), hosted::GetCurrentUserId(), MakeInitialState());
    HostedRefusal refusal = HostedRefusal::None;
    Connect(pair, MakeToken(2), refusal);
    CHECK(pair.HostedWindow.Get() == nullptr);
    CHECK(refusal == HostedRefusal::BadToken);
    CHECK(pair.Session.GetPhase() == hosted::SessionPhase::Idle);
    CHECK(pair.Session.GetLastRefusal() == HostedRefusal::BadToken);
    CHECK(pair.Session.GetRefusalCount() == 1);

    // The refusal does not end the session: the real window is still admitted.
    Connect(pair, MakeToken(1), refusal);
    CHECK(pair.HostedWindow.Get() != nullptr);
    CHECK(pair.Session.GetPhase() == hosted::SessionPhase::Connected);
}

TEST_CASE("The endpoint refuses a peer under another user id", "[platform][hosted]")
{
    ConnectedPair pair;
    // The session expects another uid, so this process's own connection is foreign to it.
    pair.Session.Configure(MakeToken(1), hosted::GetCurrentUserId() + 1, MakeInitialState());
    HostedRefusal refusal = HostedRefusal::None;
    Connect(pair, MakeToken(1), refusal);
    CHECK(pair.HostedWindow.Get() == nullptr);
    CHECK(refusal == HostedRefusal::WrongUser);
    CHECK(pair.Session.GetLastRefusal() == HostedRefusal::WrongUser);
    CHECK(pair.Session.GetPhase() == hosted::SessionPhase::Idle);
}

TEST_CASE("The endpoint refuses another protocol version or key vocabulary", "[platform][hosted]")
{
    const hosted::Token token = MakeToken(4);
    for (const uint16 keyCount : {static_cast<uint16>(ludus::input::KEY_COUNT + 1), static_cast<uint16>(0)})
    {
        hosted::EndpointSession session;
        session.Configure(token, hosted::GetCurrentUserId(), MakeInitialState());
        SocketPair pair = MakeSocketPair();
        hosted::Message hello;
        hello.Kind = hosted::MessageKind::Hello;
        hello.KeyCount = keyCount;
        hello.SessionToken = token;
        WriteMessage(pair.WindowSide.Get(), hello);
        session.Admit(ludus::foundation::Move(pair.EndpointSide), At(0));
        session.Pump(At(1));
        CHECK(session.GetLastRefusal() == HostedRefusal::IncompatibleVersion);
    }

    hosted::EndpointSession session;
    session.Configure(token, hosted::GetCurrentUserId(), MakeInitialState());
    SocketPair pair = MakeSocketPair();
    hosted::Message hello;
    hello.Kind = hosted::MessageKind::Hello;
    hello.Version = hosted::PROTOCOL_VERSION + 1;
    hello.KeyCount = static_cast<uint16>(ludus::input::KEY_COUNT);
    WriteMessage(pair.WindowSide.Get(), hello);
    session.Admit(ludus::foundation::Move(pair.EndpointSide), At(0));
    session.Pump(At(1));
    CHECK(session.GetLastRefusal() == HostedRefusal::IncompatibleVersion);
    HostedRefusal refusal = HostedRefusal::None;
    UniquePtr<Window> window = hosted::OpenWindow(ludus::foundation::Move(pair.WindowSide),
                                                  MakeCreateInfo(),
                                                  OPEN_TIMEOUT_MILLISECONDS,
                                                  refusal);
    CHECK(window.Get() == nullptr);
    CHECK(refusal == HostedRefusal::IncompatibleVersion);
}

TEST_CASE("The endpoint refuses a malformed or late Hello", "[platform][hosted]")
{
    const hosted::Token token = MakeToken(5);
    {
        hosted::EndpointSession session;
        session.Configure(token, hosted::GetCurrentUserId(), MakeInitialState());
        SocketPair pair = MakeSocketPair();
        hosted::Message close;
        close.Kind = hosted::MessageKind::Close;
        WriteMessage(pair.WindowSide.Get(), close);
        session.Admit(ludus::foundation::Move(pair.EndpointSide), At(0));
        session.Pump(At(1));
        CHECK(session.GetLastRefusal() == HostedRefusal::MalformedHello);
    }
    {
        hosted::EndpointSession session;
        session.Configure(token, hosted::GetCurrentUserId(), MakeInitialState());
        SocketPair pair = MakeSocketPair();
        // Half a header, then silence.
        const uint8 partial[] = {0x01, 0x00};
        usize sent = 0;
        REQUIRE(hosted::SendSome(pair.WindowSide.Get(), {partial, sizeof(partial)}, sent) ==
                hosted::TransferStatus::Progress);
        session.Admit(ludus::foundation::Move(pair.EndpointSide), At(0));
        session.Pump(At(hosted::HANDSHAKE_TIMEOUT_NANOSECONDS));
        CHECK(session.GetPhase() == hosted::SessionPhase::Handshaking);
        session.Pump(At(hosted::HANDSHAKE_TIMEOUT_NANOSECONDS + 1));
        CHECK(session.GetPhase() == hosted::SessionPhase::Idle);
        CHECK(session.GetLastRefusal() == HostedRefusal::HandshakeTimeout);
    }
}

TEST_CASE("Hosted overflow drops keys, then resynchronizes the state and baseline", "[platform][hosted]")
{
    ConnectedPair pair;
    ConnectAndAttach(pair);
    CHECK(pair.Session.SendFocusEntered({}) == HostedEndpointStatus::Ok);
    usize accepted = 0;
    bool hasOverflowed = false;
    for (usize i = 0; i < FLOOD_LIMIT && hasOverflowed == false; ++i)
    {
        const KeyTransition transition = i % 2 == 0 ? KeyTransition::Down : KeyTransition::Up;
        const HostedEndpointStatus status = pair.Session.SendKey(Key::KeyJ, transition, false);
        hasOverflowed = status == HostedEndpointStatus::Overflow;
        accepted += status == HostedEndpointStatus::Ok ? 1 : 0;
    }
    REQUIRE(hasOverflowed);
    // Keys after the overflow are dropped but still tracked: J and K end up held.
    CHECK(pair.Session.SendKey(Key::KeyJ, KeyTransition::Down, false) == HostedEndpointStatus::Overflow);
    CHECK(pair.Session.SendKey(Key::KeyK, KeyTransition::Down, false) == HostedEndpointStatus::Overflow);
    CHECK(pair.Session.SendResize(320, 200, 1.0F) == HostedEndpointStatus::Overflow);

    // Resets so far: the sink synchronization and the first FocusEntered. The overflow
    // adds an Overflow reset and a fresh FocusEntered baseline.
    constexpr usize RESETS_AFTER_RESYNCHRONIZATION = 4;
    for (usize round = 0; round < DRAIN_ROUNDS && (pair.Recorder.ResetCount < RESETS_AFTER_RESYNCHRONIZATION ||
                                                   pair.Recorder.GetLastReset().Reason != ResetReason::FocusEntered);
         ++round)
    {
        REQUIRE(pair.HostedWindow->HandleEvent({}));
        pair.Session.Pump(At(2));
    }
    CHECK(pair.Recorder.RecordCount == accepted);
    REQUIRE(pair.Recorder.ResetCount == RESETS_AFTER_RESYNCHRONIZATION);
    const usize last = (pair.Recorder.ResetCount < MAX_RECORDED ? pair.Recorder.ResetCount : MAX_RECORDED) - 1;
    CHECK(pair.Recorder.Resets[last - 1].Reason == ResetReason::Overflow);
    CHECK_FALSE(pair.Recorder.Resets[last - 1].Baseline.Focused);
    const ResetEntry& baseline = pair.Recorder.Resets[last];
    CHECK(baseline.Reason == ResetReason::FocusEntered);
    CHECK(baseline.Baseline.Keys[ludus::input::KeyIndex(Key::KeyJ)]);
    CHECK(baseline.Baseline.Keys[ludus::input::KeyIndex(Key::KeyK)]);
    CHECK(CountHeld(baseline.Baseline) == 2);
    CHECK(pair.HostedWindow->GetNativeWindowInfo().Width == 320);
    CHECK(pair.Session.SendKey(Key::KeyK, KeyTransition::Up, false) == HostedEndpointStatus::Ok);
}

namespace
{
/// Restores the hosted-window variable when a test ends.
class EnvironmentGuard final
{
public:
    EnvironmentGuard() = default;
    ~EnvironmentGuard()
    {
        static_cast<void>(::unsetenv(ludus::platform::HOSTED_WINDOW_ENVIRONMENT_VARIABLE));
    }
    EnvironmentGuard(const EnvironmentGuard&) = delete;
    EnvironmentGuard& operator=(const EnvironmentGuard&) = delete;
    EnvironmentGuard(EnvironmentGuard&&) = delete;
    EnvironmentGuard& operator=(EnvironmentGuard&&) = delete;
};

std::string_view GetTemporaryDirectory()
{
    // A short parent keeps the socket path within sockaddr_un on every host.
    return "/tmp";
}

/// Pump an endpoint on another thread while this thread blocks in CreateWindow().
class EndpointPumpThread final
{
public:
    explicit EndpointPumpThread(HostedWindowEndpoint& inoutEndpoint)
        : mThread([this, &inoutEndpoint] {
              const Timestamp start = ludus::foundation::time::Now();
              while (mShouldStop.load() == false &&
                     ludus::foundation::time::Now().Nanoseconds - start.Nanoseconds < PUMP_DEADLINE_NANOSECONDS)
              {
                  static_cast<void>(inoutEndpoint.Pump());
                  static_cast<void>(::sched_yield());
              }
          })
    {
    }
    ~EndpointPumpThread()
    {
        Stop();
    }
    EndpointPumpThread(const EndpointPumpThread&) = delete;
    EndpointPumpThread& operator=(const EndpointPumpThread&) = delete;
    EndpointPumpThread(EndpointPumpThread&&) = delete;
    EndpointPumpThread& operator=(EndpointPumpThread&&) = delete;
    void Stop()
    {
        mShouldStop.store(true);
        if (mThread.joinable() == true)
        {
            mThread.join();
        }
    }

private:
    std::atomic<bool> mShouldStop{false};
    std::thread mThread;
};
} // namespace

TEST_CASE("WindowManager opens a hosted window from the endpoint's environment address", "[platform][hosted]")
{
    EnvironmentGuard guard;
    HostedWindowEndpoint endpoint;
    HostedWindowEndpoint::StartInfo startInfo;
    startInfo.ParentDirectory = GetTemporaryDirectory();
    startInfo.Width = 1024;
    startInfo.Height = 768;
    startInfo.Scale = 2.0F;
    REQUIRE(endpoint.Start(startInfo) == HostedEndpointStatus::Ok);
    CHECK(endpoint.Start(startInfo) == HostedEndpointStatus::InvalidState);
    CHECK(endpoint.GetState() == HostedConnectionState::Listening);

    // The session directory is private to this user.
    ludus::foundation::StaticString<hosted::MAX_SOCKET_PATH_BYTES> directory;
    REQUIRE(directory.TryAssign(endpoint.GetDirectory()) == ludus::foundation::StringStatus::Ok);
    struct stat status{};
    REQUIRE(::stat(directory.GetData(), &status) == 0);
    CHECK(S_ISDIR(status.st_mode));
    CHECK((status.st_mode & 0777) == 0700);
    CHECK(status.st_uid == ::geteuid());

    hosted::Token token{};
    hosted::SocketPath socketPath;
    REQUIRE(hosted::TryParseChannelAddress(endpoint.GetChannelAddress(), token, socketPath));
    CHECK(socketPath.GetView().starts_with(endpoint.GetDirectory()));
    REQUIRE(::stat(socketPath.GetData(), &status) == 0);

    ludus::foundation::StaticString<hosted::MAX_ADDRESS_BYTES> address;
    REQUIRE(address.TryAssign(endpoint.GetChannelAddress()) == ludus::foundation::StringStatus::Ok);
    REQUIRE(::setenv(ludus::platform::HOSTED_WINDOW_ENVIRONMENT_VARIABLE, address.GetData(), 1) == 0);

    SinkRecorder recorder;
    WindowManager manager;
    REQUIRE(manager.Initialize({}));
    UniquePtr<Window> window;
    {
        EndpointPumpThread pump(endpoint);
        REQUIRE(manager.CreateWindow(MakeCreateInfo(), window));
    }
    REQUIRE(window.Get() != nullptr);
    CHECK(window->GetNativeWindowInfo().System == WindowSystem::Hosted);
    CHECK(window->GetNativeWindowInfo().Width == 1024);
    CHECK(window->GetNativeWindowInfo().Scale == 2.0F);
    CHECK(endpoint.Pump() == HostedEndpointStatus::Ok);
    CHECK(endpoint.GetState() == HostedConnectionState::Connected);
    // Once a window is admitted no other process can connect.
    CHECK(::stat(socketPath.GetData(), &status) != 0);

    window->AttachKeyboardSink(recorder.GetSink());
    CHECK(endpoint.SendFocusEntered(MakeBaseline(Key::KeyW, Key::KeyA)) == HostedEndpointStatus::Ok);
    CHECK(endpoint.SendKey(Key::KeyW, KeyTransition::Up, false) == HostedEndpointStatus::Ok);
    REQUIRE(window->HandleEvent({}));
    CHECK(recorder.GetLastReset().Reason == ResetReason::FocusEntered);
    CHECK(recorder.RecordCount == 1);

    // Stopping the endpoint is a channel loss: the game sees its window close.
    endpoint.Stop();
    CHECK(endpoint.GetState() == HostedConnectionState::Stopped);
    CHECK_FALSE(window->HandleEvent({}));
    CHECK(recorder.GetLastReset().Reason == ResetReason::WindowClosed);
    CHECK(::stat(directory.GetData(), &status) != 0);
}

TEST_CASE("WindowManager refuses a hosted address it cannot use, without a native fallback", "[platform][hosted]")
{
    EnvironmentGuard guard;
    REQUIRE(::setenv(ludus::platform::HOSTED_WINDOW_ENVIRONMENT_VARIABLE, "not an address", 1) == 0);
    WindowManager manager;
    CHECK_FALSE(manager.Initialize({}));
    UniquePtr<Window> window;
    CHECK_FALSE(manager.CreateWindow(MakeCreateInfo(), window));
    CHECK(window.Get() == nullptr);

    // A well-formed address whose token the endpoint does not know.
    HostedWindowEndpoint endpoint;
    HostedWindowEndpoint::StartInfo startInfo;
    startInfo.ParentDirectory = GetTemporaryDirectory();
    REQUIRE(endpoint.Start(startInfo) == HostedEndpointStatus::Ok);
    hosted::Token token{};
    hosted::SocketPath socketPath;
    REQUIRE(hosted::TryParseChannelAddress(endpoint.GetChannelAddress(), token, socketPath));
    token[0] = static_cast<uint8>(token[0] ^ 0xFF);
    hosted::ChannelAddressText forged;
    REQUIRE(hosted::TryFormatChannelAddress(token, socketPath.GetView(), forged));
    REQUIRE(::setenv(ludus::platform::HOSTED_WINDOW_ENVIRONMENT_VARIABLE, forged.GetData(), 1) == 0);
    REQUIRE(manager.Initialize({}));
    {
        EndpointPumpThread pump(endpoint);
        CHECK_FALSE(manager.CreateWindow(MakeCreateInfo(), window));
    }
    CHECK(window.Get() == nullptr);
    CHECK(endpoint.GetLastRefusal() == HostedRefusal::BadToken);
    CHECK(endpoint.GetRefusalCount() == 1);
    CHECK(endpoint.GetState() == HostedConnectionState::Listening);
}

TEST_CASE("The hosted endpoint validates its start parameters", "[platform][hosted]")
{
    HostedWindowEndpoint endpoint;
    CHECK(endpoint.GetState() == HostedConnectionState::Stopped);
    CHECK(endpoint.GetChannelAddress().empty());
    CHECK(endpoint.Pump() == HostedEndpointStatus::InvalidState);
    CHECK(endpoint.SendClose() == HostedEndpointStatus::InvalidState);
    HostedWindowEndpoint::StartInfo startInfo;
    startInfo.ParentDirectory = "relative";
    CHECK(endpoint.Start(startInfo) == HostedEndpointStatus::InvalidArgument);
    startInfo.ParentDirectory = "/tmp/this/parent/path/is/far/too/long/for/any/unix/socket/address/to/hold/the/window";
    CHECK(endpoint.Start(startInfo) == HostedEndpointStatus::InvalidArgument);
    startInfo.ParentDirectory = GetTemporaryDirectory();
    startInfo.Scale = 0.0F;
    CHECK(endpoint.Start(startInfo) == HostedEndpointStatus::InvalidArgument);
    startInfo.Scale = 1.0F;
    startInfo.ParentDirectory = "/nonexistent-ludus-parent";
    CHECK(endpoint.Start(startInfo) == HostedEndpointStatus::Unavailable);
    CHECK(endpoint.GetState() == HostedConnectionState::Stopped);
}
