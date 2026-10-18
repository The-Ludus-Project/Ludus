// Host session + protocol end-to-end acceptance (tasks.md L1/L3, design 7/10).
//
// Drives a real HostSession over a socketpair: Hello/ready events, Pause/Step/
// Resume/Status, a full reload transaction (Validate..Done phases) that migrates
// A's checkpoint into B preserving simulation state, and Stop ending the session
// with retired instances and zero leaked host allocations. Reload runs on a
// background thread so the test can inject control frames and read events.

#include "internal/host_session.h"
#include "internal/protocol_codec.h"
#include "internal/timing_telemetry.h"

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/runtime/game_host/host.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace ludus::runtime::game_host;
using ludus::foundation::Array;
using protocol::CommandKind;
using protocol::EncodeFrame;
using protocol::FrameReader;
using protocol::Message;

namespace
{
uint64 CurrentSession = 0;
uint64 CurrentGeneration = 1;
class HostWorker
{
public:
    template <typename Fn>
    HostWorker(int fd, Fn&& fn) : Fd_(fd), Thread_(std::forward<Fn>(fn))
    {
    }
    ~HostWorker()
    {
        if (Thread_.joinable())
        {
            (void)::shutdown(Fd_, SHUT_RDWR);
            Thread_.join();
        }
    }
    void join()
    {
        Thread_.join();
    }

private:
    int Fd_ = -1;
    std::thread Thread_;
};
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void SendCommand(int fd, const Message& m)
{
    Array<ludus::foundation::uint8> frame;
    Message command = m;
    command.SetUint("protocol", protocol::kProtocolVersion);
    command.SetHexId("session", CurrentSession);
    command.SetHexId("epoch", 1);
    std::string type;
    (void)command.GetString("command", type);
    if (type == "Pause" || type == "Resume" || type == "Step" || type == "Reload" || type == "ReadProperties" ||
        type == "ApplyEdits")
    {
        if (command.Has("expected_generation") == false)
        {
            command.SetHexId("expected_generation", CurrentGeneration);
        }
    }
    REQUIRE(EncodeFrame(command.Serialize(), frame));
    std::size_t written = 0;
    while (written < frame.GetSize())
    {
        const ssize_t n = ::write(fd, frame.GetData() + written, frame.GetSize() - written);
        REQUIRE(n > 0);
        written += static_cast<std::size_t>(n);
    }
}

// Collect events for a short window using a caller-owned reader so partial
// frames survive across calls. Uses poll with a timeout so a quiet channel
// never blocks the test.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Array<Message> DrainEvents(int fd, FrameReader& reader, int budgetMs)
{
    Array<Message> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 5);
        if (ready > 0 && (pfd.revents & POLLIN) != 0)
        {
            std::array<ludus::foundation::uint8, 4096> buf = {};
            const ssize_t n = ::read(fd, buf.data(), buf.size());
            if (n > 0)
            {
                reader.Append(buf.data(), static_cast<ludus::foundation::usize>(n));
                std::string payload;
                while (reader.Next(payload))
                {
                    Message m;
                    if (Message::Parse(payload, m))
                    {
                        (void)m.GetHexId("session", CurrentSession);
                        std::string event;
                        std::string phase;
                        (void)m.GetString("event", event);
                        (void)m.GetString("phase", phase);
                        if (event == "ModuleReady" || (event == "ReloadPhase" && phase == "Done"))
                        {
                            (void)m.GetHexId("generation", CurrentGeneration);
                        }
                        events.Add(m);
                    }
                }
            }
        }
    }
    return events;
}

bool HasEvent(const Array<Message>& events, const char* event)
{
    for (const Message& m : events)
    {
        std::string e;
        if (m.GetString("event", e) && e == event)
        {
            return true;
        }
    }
    return false;
}

bool HasReloadPhase(const Array<Message>& events, const char* phase)
{
    for (const Message& m : events)
    {
        std::string e;
        std::string p;
        if (m.GetString("event", e) && e == "ReloadPhase" && m.GetString("phase", p) && p == phase)
        {
            return true;
        }
    }
    return false;
}

bool HasStatus(const Array<Message>& events, uint64 request, const char* status)
{
    for (const auto& event : events)
    {
        uint64 id = 0;
        std::string value;
        if (event.GetHexId("request", id) && id == request && event.GetString("status", value) && value == status)
        {
            return true;
        }
    }
    return false;
}
// Replies are asynchronous: wait for their identity rather than assuming a
// host thread is scheduled within a 50 ms observation window on a CI runner.
template <typename Predicate>
Array<Message> AwaitEvents(int fd, FrameReader& reader, const Predicate& complete)
{
    Array<Message> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (complete(events) == false && std::chrono::steady_clock::now() < deadline)
    {
        const Array<Message> batch = DrainEvents(fd, reader, 20);
        events.AddRange(batch);
    }
    REQUIRE(complete(events));
    return events;
}

Array<Message> AwaitStatus(int fd, FrameReader& reader, uint64 request)
{
    return AwaitEvents(fd, reader, [request](const Array<Message>& events) {
        for (const auto& event : events)
        {
            uint64 id = 0;
            if (event.GetHexId("request", id) && id == request && event.Has("status"))
            {
                return true;
            }
        }
        return false;
    });
}

Array<Message> AwaitEvent(int fd, FrameReader& reader, const char* event)
{
    return AwaitEvents(fd, reader, [event](const Array<Message>& events) { return HasEvent(events, event); });
}
} // namespace

TEST_CASE("initial gameplay waits for Hello and the selected Load", "[session][startup]")
{
    int sockets[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    FrameReader reader;
    HostSession session(0x11, 0x22, sockets[1]);
    REQUIRE(session.PrepareInitialLoad(LUDUS_FIXTURE_A_PATH, 42, {}));
    RunResult result = RunResult::Internal;
    HostWorker worker(sockets[0], [&] { result = session.RunLoop(0); });
    const auto ready = AwaitEvent(sockets[0], reader, "SessionReady");
    REQUIRE(HasEvent(ready, "SessionReady"));
    REQUIRE_FALSE(HasEvent(ready, "ModuleReady"));
    Message load;
    load.SetString("command", "Load");
    load.SetHexId("request", 1);
    load.SetHexId("generation", 42);
    SendCommand(sockets[0], load);
    REQUIRE(HasStatus(AwaitStatus(sockets[0], reader, 1), 1, "InvalidRequest"));
    Message hello;
    hello.SetString("command", "Hello");
    hello.SetHexId("request", 2);
    SendCommand(sockets[0], hello);
    REQUIRE(HasStatus(AwaitStatus(sockets[0], reader, 2), 2, "Ok"));
    load.SetHexId("request", 3);
    load.SetHexId("generation", 43);
    SendCommand(sockets[0], load);
    REQUIRE(HasStatus(AwaitStatus(sockets[0], reader, 3), 3, "InvalidRequest"));
    load.SetHexId("request", 4);
    load.SetHexId("generation", 42);
    SendCommand(sockets[0], load);
    const auto loaded = AwaitStatus(sockets[0], reader, 4);
    REQUIRE(HasEvent(loaded, "ModuleReady"));
    REQUIRE(HasStatus(loaded, 4, "Ok"));
    load.SetHexId("request", 5);
    SendCommand(sockets[0], load);
    REQUIRE(HasStatus(AwaitStatus(sockets[0], reader, 5), 5, "InvalidRequest"));
    Message stop;
    stop.SetString("command", "Stop");
    stop.SetHexId("request", 6);
    SendCommand(sockets[0], stop);
    REQUIRE(HasEvent(AwaitEvent(sockets[0], reader, "SessionEnded"), "SessionEnded"));
    worker.join();
    CHECK(result == RunResult::Ok);
    CHECK(session.ActiveGeneration() == 42);
    (void)::close(sockets[0]);
    (void)::close(sockets[1]);
}

TEST_CASE("session performs a full reload transaction over the protocol", "[session]")
{
    int sv[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const int editorEnd = sv[0];
    const int hostEnd = sv[1];
    FrameReader reader;

    HostSession session(0x11, 0x22, hostEnd);
    REQUIRE(session.LoadInitial(LUDUS_FIXTURE_A_PATH));

    // Run the frame loop on a background thread; the main thread drives control.
    RunResult result = RunResult::Internal;
    HostWorker hostThread(editorEnd, [&] { result = session.RunLoop(0); });

    // Collect the ready events.
    auto startup = AwaitEvent(editorEnd, reader, "ModuleReady");
    REQUIRE(HasEvent(startup, "SessionReady"));
    REQUIRE(HasEvent(startup, "ModuleReady"));

    // Pause, then reload A -> B (migrates checkpoint). Expect all phases to Done.
    {
        Message pause;
        pause.SetString("command", protocol::CommandKindName(CommandKind::Pause));
        pause.SetHexId("request", 1);
        SendCommand(editorEnd, pause);
    }
    (void)AwaitStatus(editorEnd, reader, 1);
    {
        Message reload;
        reload.SetString("command", protocol::CommandKindName(CommandKind::Reload));
        reload.SetHexId("request", 2);
        reload.SetString("module_path", LUDUS_FIXTURE_B_PATH);
        reload.SetHexId("generation", 2);
        SendCommand(editorEnd, reload);
    }
    auto reloadEvents = AwaitStatus(editorEnd, reader, 2);
    REQUIRE(HasReloadPhase(reloadEvents, "Validate"));
    REQUIRE(HasReloadPhase(reloadEvents, "Quiesce"));
    REQUIRE(HasReloadPhase(reloadEvents, "Snapshot"));
    REQUIRE(HasReloadPhase(reloadEvents, "Stage"));
    REQUIRE(HasReloadPhase(reloadEvents, "Commit"));
    REQUIRE(HasReloadPhase(reloadEvents, "Retire"));
    REQUIRE(HasReloadPhase(reloadEvents, "Done"));

    // Stop ends the session.
    {
        Message stop;
        stop.SetString("command", protocol::CommandKindName(CommandKind::Stop));
        stop.SetHexId("request", 3);
        SendCommand(editorEnd, stop);
    }
    auto ending = AwaitEvent(editorEnd, reader, "SessionEnded");
    REQUIRE(HasEvent(ending, "SessionEnded"));

    hostThread.join();
    REQUIRE(result == RunResult::Ok);

    ::close(editorEnd);
    ::close(hostEnd);
}

TEST_CASE("property batches are conditional, atomic and replayable across reload", "[session][properties]")
{
    int sv[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    FrameReader reader;
    HostSession session(0x11, 0x22, sv[1]);
    REQUIRE(session.LoadInitial(LUDUS_FIXTURE_A_PATH));
    RunResult result = RunResult::Internal;
    HostWorker worker(sv[0], [&] { result = session.RunLoop(0); });
    (void)AwaitEvent(sv[0], reader, "ModuleReady");
    auto command = [](std::string_view name, uint64 request) {
        Message message;
        message.SetString("command", name);
        message.SetHexId("request", request);
        return message;
    };
    auto expectStatus = [](const Array<Message>& events, uint64 request, std::string_view expected) {
        bool found = false;
        for (const auto& event : events)
        {
            uint64 id = 0;
            std::string status;
            if (event.GetHexId("request", id) && id == request && event.GetString("status", status))
            {
                CHECK(status == expected);
                found = true;
            }
        }
        REQUIRE(found);
    };
    SendCommand(sv[0], command("Pause", 1));
    expectStatus(AwaitStatus(sv[0], reader, 1), 1, "Ok");
    auto read = [&](uint64 request) {
        SendCommand(sv[0], command("ReadProperties", request));
        auto events = AwaitStatus(sv[0], reader, request);
        expectStatus(events, request, "Ok");
        Array<Message> properties;
        for (const auto& event : events)
        {
            Array<Message> rows;
            uint64 id = 0;
            if (event.GetHexId("request", id) && id == request && event.GetRecords("properties", rows))
            {
                properties.AddRange(rows);
            }
        }
        REQUIRE(properties.GetSize() == 3);
        return properties;
    };
    const auto before = read(2);
    uint64 revision = 0;
    REQUIRE(before[0].GetHexId("revision", revision));
    Message speed;
    speed.SetHexId("object", 0x1001);
    speed.SetHexId("property", 0x2001);
    speed.SetHexId("revision", revision);
    speed.SetUint("kind", 2);
    speed.SetUint("bits", 0x40000000); // Exactly 2.0f.
    Message label;
    label.SetHexId("object", 0x1001);
    label.SetHexId("property", 0x2003);
    label.SetHexId("revision", revision);
    label.SetUint("kind", 4);
    label.SetString("value", "edited é");
    auto batch = [&](uint64 request, const Array<Message>& edits) {
        auto message = command("ApplyEdits", request);
        message.SetUint("schema_epoch", 0);
        message.SetUint("schema_version", 1);
        message.SetRecords("edits", edits);
        return message;
    };
    Message bad = label;
    bad.SetString("value", std::string(32, 'x')); // Module-specific 31-byte bound.
    SendCommand(sv[0], batch(3, {speed, bad}));
    expectStatus(AwaitStatus(sv[0], reader, 3), 3, "InvalidRequest");
    auto unchanged = read(4);
    for (usize i = 0; i < before.GetSize(); ++i)
    {
        CHECK(unchanged[i].Serialize() == before[i].Serialize());
    }
    auto edit = batch(5, {speed, label});
    SendCommand(sv[0], edit);
    expectStatus(AwaitStatus(sv[0], reader, 5), 5, "Ok");
    const auto after = read(6);
    uint64 bits = 0;
    uint64 editedRevision = 0;
    REQUIRE(after[0].GetUint("bits", bits));
    CHECK(bits == 0x40000000);
    REQUIRE(after[0].GetHexId("revision", editedRevision));
    CHECK(editedRevision == revision + 1);
    SendCommand(sv[0], edit); // Lost reply retry must not advance revision twice.
    expectStatus(AwaitStatus(sv[0], reader, 5), 5, "Ok");
    const auto retry = read(7);
    REQUIRE(retry[0].GetHexId("revision", editedRevision));
    CHECK(editedRevision == revision + 1);
    SendCommand(sv[0], batch(8, {speed}));
    expectStatus(AwaitStatus(sv[0], reader, 8), 8, "StaleRevision");
    Message nonfinite = speed;
    nonfinite.SetHexId("revision", editedRevision);
    nonfinite.SetUint("bits", 0x7FC00000);
    SendCommand(sv[0], batch(9, {nonfinite}));
    expectStatus(AwaitStatus(sv[0], reader, 9), 9, "InvalidRequest");
    auto reload = command("Reload", 10);
    reload.SetString("module_path", LUDUS_FIXTURE_B_PATH);
    reload.SetHexId("generation", 2);
    SendCommand(sv[0], reload);
    expectStatus(AwaitStatus(sv[0], reader, 10), 10, "Ok");
    auto staleSchema = batch(11, {speed});
    SendCommand(sv[0], staleSchema);
    expectStatus(AwaitStatus(sv[0], reader, 11), 11, "SchemaChanged");
    auto oldGeneration = command("Pause", 12);
    oldGeneration.SetHexId("expected_generation", 1);
    SendCommand(sv[0], oldGeneration);
    expectStatus(AwaitStatus(sv[0], reader, 12), 12, "SchemaChanged");
    auto unknownField = command("Resume", 13);
    unknownField.SetUint("surprise", 1);
    SendCommand(sv[0], unknownField);
    expectStatus(AwaitStatus(sv[0], reader, 13), 13, "InvalidRequest");
    auto status = command("Status", 14);
    status.SetHexId("reconcile", 5);
    SendCommand(sv[0], status);
    auto events = AwaitStatus(sv[0], reader, 14);
    bool reconciled = false;
    for (const auto& event : events)
    {
        std::string outcome;
        if (event.GetString("reconciled_status", outcome))
        {
            CHECK(outcome == "Ok");
            reconciled = true;
        }
    }
    CHECK(reconciled);
    SendCommand(sv[0], command("Stop", 15));
    (void)AwaitEvent(sv[0], reader, "SessionEnded");
    worker.join();
    CHECK(result == RunResult::Ok);
    ::close(sv[0]);
    ::close(sv[1]);
}

namespace
{
// Pull a uint field out of the newest Status CommandResult in a drained batch.
bool LatestStatusUint(const Array<Message>& events, const char* field, ludus::foundation::uint64& out)
{
    bool found = false;
    for (const Message& m : events)
    {
        std::string e;
        ludus::foundation::uint64 v = 0;
        if (m.GetString("event", e) && e == "CommandResult" && m.GetUint(field, v))
        {
            out = v;
            found = true;
        }
    }
    return found;
}
} // namespace

TEST_CASE("pause then step advances exactly one simulation tick (no catch-up)", "[session][pause]")
{
    int sv[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const int editorEnd = sv[0];
    const int hostEnd = sv[1];
    FrameReader reader;

    HostSession session(0x11, 0x22, hostEnd);
    REQUIRE(session.LoadInitial(LUDUS_FIXTURE_A_PATH));
    RunResult result = RunResult::Internal;
    HostWorker hostThread(editorEnd, [&] { result = session.RunLoop(0); });
    (void)AwaitEvent(editorEnd, reader, "ModuleReady");

    // Pause.
    {
        Message pause;
        pause.SetString("command", protocol::CommandKindName(CommandKind::Pause));
        pause.SetHexId("request", 1);
        SendCommand(editorEnd, pause);
    }
    (void)AwaitStatus(editorEnd, reader, 1);

    // Status: record sim ticks while paused.
    {
        Message status;
        status.SetString("command", protocol::CommandKindName(CommandKind::Status));
        status.SetHexId("request", 2);
        SendCommand(editorEnd, status);
    }
    auto s1 = AwaitStatus(editorEnd, reader, 2);
    ludus::foundation::uint64 ticksPaused = 0;
    REQUIRE(LatestStatusUint(s1, "sim_ticks", ticksPaused));

    // Let the loop spin while paused; ticks must NOT advance (no catch-up).
    (void)DrainEvents(editorEnd, reader, 60);
    {
        Message status;
        status.SetString("command", protocol::CommandKindName(CommandKind::Status));
        status.SetHexId("request", 3);
        SendCommand(editorEnd, status);
    }
    auto s2 = AwaitStatus(editorEnd, reader, 3);
    ludus::foundation::uint64 ticksStillPaused = 0;
    REQUIRE(LatestStatusUint(s2, "sim_ticks", ticksStillPaused));
    REQUIRE(ticksStillPaused == ticksPaused);

    // One Step: exactly one additional simulation tick.
    {
        Message step;
        step.SetString("command", protocol::CommandKindName(CommandKind::Step));
        step.SetHexId("request", 4);
        SendCommand(editorEnd, step);
    }
    (void)AwaitStatus(editorEnd, reader, 4);
    {
        Message status;
        status.SetString("command", protocol::CommandKindName(CommandKind::Status));
        status.SetHexId("request", 5);
        SendCommand(editorEnd, status);
    }
    auto s3 = AwaitStatus(editorEnd, reader, 5);
    ludus::foundation::uint64 ticksAfterStep = 0;
    REQUIRE(LatestStatusUint(s3, "sim_ticks", ticksAfterStep));
    REQUIRE(ticksAfterStep == ticksPaused + 1);

    {
        Message stop;
        stop.SetString("command", protocol::CommandKindName(CommandKind::Stop));
        stop.SetHexId("request", 6);
        SendCommand(editorEnd, stop);
    }
    (void)AwaitEvent(editorEnd, reader, "SessionEnded");
    hostThread.join();
    REQUIRE(result == RunResult::Ok);
    ::close(editorEnd);
    ::close(hostEnd);
}

TEST_CASE("reload to an incompatible module keeps the session alive", "[session]")
{
    int sv[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const int editorEnd = sv[0];
    const int hostEnd = sv[1];
    FrameReader reader;

    HostSession session(0x11, 0x22, hostEnd);
    REQUIRE(session.LoadInitial(LUDUS_FIXTURE_A_PATH));
    RunResult result = RunResult::Internal;
    HostWorker hostThread(editorEnd, [&] { result = session.RunLoop(0); });
    (void)AwaitEvent(editorEnd, reader, "ModuleReady");

    {
        Message reload;
        reload.SetString("command", protocol::CommandKindName(CommandKind::Reload));
        reload.SetHexId("request", 1);
        reload.SetString("module_path", LUDUS_FIXTURE_BADID_PATH);
        reload.SetHexId("generation", 2);
        SendCommand(editorEnd, reload);
    }
    auto events = AwaitStatus(editorEnd, reader, 1);
    REQUIRE(HasReloadPhase(events, "Rejected"));

    // The session is still running: Status returns Ok and Stop ends cleanly.
    {
        Message stop;
        stop.SetString("command", protocol::CommandKindName(CommandKind::Stop));
        stop.SetHexId("request", 2);
        SendCommand(editorEnd, stop);
    }
    (void)AwaitEvent(editorEnd, reader, "SessionEnded");
    hostThread.join();
    REQUIRE(result == RunResult::Ok);
    ::close(editorEnd);
    ::close(hostEnd);
}

TEST_CASE("duplicate Step is applied once and acknowledged IDs are never replayed", "[session][protocol]")
{
    int sv[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const int editorEnd = sv[0];
    HostSession session(1, 1, sv[1]);
    REQUIRE(session.LoadInitial(LUDUS_FIXTURE_A_PATH));
    HostWorker worker(editorEnd, [&] { (void)session.RunLoop(0); });
    FrameReader reader;
    (void)AwaitEvent(editorEnd, reader, "ModuleReady");
    Message pause;
    pause.SetString("command", "Pause");
    pause.SetHexId("request", 1);
    SendCommand(editorEnd, pause);
    (void)AwaitStatus(editorEnd, reader, 1);
    Message status;
    status.SetString("command", "Status");
    status.SetHexId("request", 2);
    SendCommand(editorEnd, status);
    auto before = AwaitStatus(editorEnd, reader, 2);
    uint64 ticks = 0;
    REQUIRE(LatestStatusUint(before, "sim_ticks", ticks));
    Message step;
    step.SetString("command", "Step");
    step.SetHexId("request", 3);
    SendCommand(editorEnd, step);
    SendCommand(editorEnd, step);
    (void)AwaitStatus(editorEnd, reader, 3);
    status.SetHexId("request", 4);
    SendCommand(editorEnd, status);
    auto after = AwaitStatus(editorEnd, reader, 4);
    uint64 advanced = 0;
    REQUIRE(LatestStatusUint(after, "sim_ticks", advanced));
    REQUIRE(advanced == ticks + 1);
    Message mismatch = step;
    mismatch.SetString("command", "Resume");
    SendCommand(editorEnd, mismatch);
    REQUIRE(HasStatus(AwaitStatus(editorEnd, reader, 3), 3, "InvalidRequest"));
    Message ack;
    ack.SetString("command", "Ack");
    ack.SetHexId("request", 5);
    ack.SetHexId("acknowledge", 3);
    SendCommand(editorEnd, ack);
    (void)AwaitStatus(editorEnd, reader, 5);
    SendCommand(editorEnd, step);
    REQUIRE(HasStatus(AwaitStatus(editorEnd, reader, 3), 3, "InvalidRequest"));
    Message stop;
    stop.SetString("command", "Stop");
    stop.SetHexId("request", 6);
    SendCommand(editorEnd, stop);
    (void)AwaitEvent(editorEnd, reader, "SessionEnded");
    worker.join();
    ::close(editorEnd);
    ::close(sv[1]);
}

TEST_CASE("Status reports runtime-computed frame timing", "[session][timing]")
{
    ludus::foundation::StaticArray<ludus::foundation::int32, 2> sockets{-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.GetData()) == 0);
    const ludus::foundation::int32 editorEnd = sockets[0];
    HostSession session(1, 1, sockets[1]);
    REQUIRE(session.LoadInitial(LUDUS_FIXTURE_A_PATH));
    HostWorker worker(editorEnd, [&] { (void)session.RunLoop(0); });
    FrameReader reader;
    (void)AwaitEvent(editorEnd, reader, "ModuleReady");

    // Poll until the paced loop has measured a few frames; real time, so only
    // structural and policy facts are asserted, never host-dependent durations.
    constexpr ludus::foundation::uint64 MEASURED_FRAMES = 5;
    TimingReport report;
    ludus::foundation::uint64 ticks = 0;
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    ludus::foundation::uint64 request = 1;
    while (report.WindowFrames < MEASURED_FRAMES && std::chrono::steady_clock::now() < deadline)
    {
        Message status;
        status.SetString("command", "Status");
        status.SetHexId("request", request);
        SendCommand(editorEnd, status);
        for (const Message& event : AwaitStatus(editorEnd, reader, request))
        {
            uint64 id = 0;
            Array<Message> records;
            if (event.GetHexId("request", id) == true && id == request &&
                event.GetRecords(TIMING_RECORD_KEY, records) == true)
            {
                REQUIRE(records.GetSize() == 1);
                REQUIRE(ReadTimingRecord(records[0], report));
                REQUIRE(event.GetUint("sim_ticks", ticks));
            }
        }
        ++request;
    }
    CHECK(report.WindowFrames >= MEASURED_FRAMES);
    CHECK(report.WindowFrames <= TIMING_WINDOW_FRAMES);
    CHECK(report.Frames >= report.WindowFrames);
    CHECK(report.Baselines == 1);
    CHECK(report.PacingIntervalNanoseconds == 16'666'667);
    CHECK(report.SimulationTicks == ticks);
    CHECK(report.WindowIntervalMinimumNanoseconds <= report.WindowIntervalMeanNanoseconds);
    CHECK(report.WindowIntervalMeanNanoseconds <= report.WindowIntervalMaximumNanoseconds);

    Message stop;
    stop.SetString("command", "Stop");
    stop.SetHexId("request", request);
    SendCommand(editorEnd, stop);
    (void)AwaitEvent(editorEnd, reader, "SessionEnded");
    worker.join();
    ::close(editorEnd);
    ::close(sockets[1]);
}

TEST_CASE("Stop is bounded under output backpressure without corrupting frames", "[session][backpressure]")
{
#if defined(LUDUS_PLATFORM_MACOS)
    SKIP("The Linux socket-buffer saturation fixture has not been ported to macOS");
#endif
    int sv[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int smallBuffer = 1024;
    REQUIRE(::setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &smallBuffer, sizeof(smallBuffer)) == 0);
    const int editorEnd = sv[0];
    HostSession session(1, 1, sv[1]);
    REQUIRE(session.LoadInitial(LUDUS_FIXTURE_A_PATH));
    RunResult result = RunResult::Internal;
    HostWorker worker(editorEnd, [&] { result = session.RunLoop(0); });
    FrameReader reader;
    (void)DrainEvents(editorEnd, reader, 50);
    for (uint64 i = 1; i <= 512; ++i)
    {
        Message status;
        status.SetString("command", "Status");
        status.SetHexId("request", i);
        SendCommand(editorEnd, status);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    Message stop;
    stop.SetString("command", "Stop");
    stop.SetHexId("request", 513);
    SendCommand(editorEnd, stop);
    auto ending = DrainEvents(editorEnd, reader, 1200);
    worker.join();
    CHECK_FALSE(reader.Failed());
    CHECK(HasEvent(ending, "SessionEnded"));
    CHECK(result == RunResult::Ok);
    ::close(editorEnd);
    ::close(sv[1]);
}
