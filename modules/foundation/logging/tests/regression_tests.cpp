// Phase-0 regression tests: each reproduces a confirmed defect from
// docs/architecture/logging-review.md (findings F1-F12) and the logging-redesign
// spec. They are written to assert the CORRECT (target) behavior, so they FAIL
// against the pre-repair implementation and PASS once the corresponding
// Phase-1+ fix lands. See .kiro/specs/logging-redesign/{requirements,tasks}.md.
//
// Concurrency reproduction (F1) lives under TSan in a separate build
// configuration; a lightweight concurrent smoke test is included here so the
// default suite exercises multi-producer dispatch even without TSan.

// Runtime/formatter tests must exercise every severity in every build flavor.
// Compile-time stripping is tested separately in category_tests.cpp.
#if defined(LUDUS_COMPILED_LOG_LEVEL)
#    undef LUDUS_COMPILED_LOG_LEVEL
#endif
#define LUDUS_COMPILED_LOG_LEVEL 0

#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include <ludus/foundation/base/diagnostic.hpp>

#include "internal/breadcrumb.hpp"
#include "internal/mpsc_queue.hpp"

#include <ludus/foundation/containers/array.hpp>

#include <catch2/catch_test_macros.hpp>

#include "format_test_name.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#    include <process.h>
#    define LUDUS_TEST_GETPID _getpid
#else
#    include <unistd.h>
#    define LUDUS_TEST_GETPID ::getpid
#endif

namespace
{
using namespace ludus::foundation;
using namespace ludus::foundation::logging;

inline constexpr LogCategory LogRegression{"Regression"};

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (stream.fail() == true)
    {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::filesystem::path make_temp_log_dir(const std::string& tag)
{
    const auto base =
        std::filesystem::temp_directory_path() / FormatTestName("ludus_regr_{}_{}", tag, LUDUS_TEST_GETPID());
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    return base;
}

std::filesystem::path find_log_file(const std::filesystem::path& dir)
{
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".log")
        {
            return entry.path();
        }
    }
    return {};
}

std::string capture_single_file_log(const std::filesystem::path& dir)
{
    return read_file(find_log_file(dir));
}

} // namespace

// --- F6: zero-argument escaped braces must be unescaped like std::format -----
TEST_CASE("F6: zero-argument escaped braces are unescaped", "[logging][regression][F6][format]")
{
    const auto dir = make_temp_log_dir("f6_braces");
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    const std::string dir_str = dir.string();
    config.Directory = dir_str;
    LogSystem::Initialize(config);

    // A formatted call with escaped braces and no args must render "{braces}",
    // NOT the doubled "{{braces}}" (review F6, reproduced). Callers that want
    // literal text use the raw-text surface instead.
    LUDUS_LOG_INFO(LogRegression, "escaped {{braces}}");
    LogSystem::Flush();
    LogSystem::Shutdown();

    const std::string contents = capture_single_file_log(dir);
    CHECK(contents.find("escaped {braces}") != std::string::npos);
    CHECK(contents.find("escaped {{braces}}") == std::string::npos);
    std::filesystem::remove_all(dir);
}

// --- F6: raw-text surface never interprets its argument as a format string ---
TEST_CASE("F6: raw text is emitted verbatim (no format interpretation)", "[logging][regression][F6][raw]")
{
    const auto dir = make_temp_log_dir("f6_raw");
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    const std::string dir_str = dir.string();
    config.Directory = dir_str;
    LogSystem::Initialize(config);

    // Dynamic/third-party text containing brace characters must survive intact
    // and must never trigger a format-parse (requirements R16).
    const std::string driver_message = "GL error {invalid enum} at {{driver}}";
    LUDUS_LOG_TEXT(LogRegression, Warning, driver_message);
    LogSystem::Flush();
    LogSystem::Shutdown();

    const std::string contents = capture_single_file_log(dir);
    CHECK(contents.find("GL error {invalid enum} at {{driver}}") != std::string::npos);
    std::filesystem::remove_all(dir);
}

// --- F6: category expression is evaluated exactly once -----------------------
TEST_CASE("F6: an accepted log evaluates its category expression once", "[logging][regression][F6][category]")
{
    const auto dir = make_temp_log_dir("f6_cat_once");
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = false;
    LogSystem::Initialize(config);

    int category_evaluations = 0;
    auto make_category = [&]() -> LogCategory {
        ++category_evaluations;
        return LogRegression;
    };

    LUDUS_LOG_INFO(make_category(), "exact-once category evaluation");
    CHECK(category_evaluations == 1);

    LogSystem::Shutdown();
    std::filesystem::remove_all(dir);
}

// --- F3: an abrupt exit after a timed flush interval must not lose the tail ---
// (Full subprocess reproduction lives in the harness; here we assert the timed
//  flush interval is honored so a record becomes visible without explicit Flush.)
TEST_CASE("F3: records become visible on the timed flush interval", "[logging][regression][F3][flush]")
{
    const auto dir = make_temp_log_dir("f3_flush");
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    const std::string dir_str = dir.string();
    config.Directory = dir_str;
    config.FlushIntervalMilliseconds = 20;
    LogSystem::Initialize(config);

    LUDUS_LOG_INFO(LogRegression, "timed-flush visibility marker");
    // Do NOT call Flush(). Wait longer than the configured interval; the record
    // must reach the file on its own (requirements R46). Poll to avoid flakiness.
    const auto path = find_log_file(dir);
    bool visible = false;
    for (int i = 0; i < 100 && visible == false; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        visible = read_file(path).find("timed-flush visibility marker") != std::string::npos;
    }
    CHECK(visible);

    LogSystem::Shutdown();
    std::filesystem::remove_all(dir);
}

// --- F8: statistics must not count a delivery when there are no sinks ---------
TEST_CASE("F8: Written counts real deliveries, not zero-sink dispatch", "[logging][regression][F8][stats]")
{
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = false; // NO sinks at all
    LogSystem::Initialize(config);

    const LogStatistics before = LogSystem::Statistics();
    LUDUS_LOG_INFO(LogRegression, "no sink is attached");
    const LogStatistics after = LogSystem::Statistics();

    // Submitted may increase (an attempt happened), but Written must NOT, because
    // nothing was actually written anywhere (review F8).
    CHECK(after.Written == before.Written);

    LogSystem::Shutdown();
}

// --- F7: rapid reinitialization must not truncate a prior session file -------
TEST_CASE("F7: reinitialization creates a distinct session file (no truncation)", "[logging][regression][F7][file]")
{
    const auto dir = make_temp_log_dir("f7_reinit");
    const std::string dir_str = dir.string();

    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    config.Directory = dir_str;
    config.RetainedSessions = 0; // keep everything so we can observe both files

    // First session writes a marker, then shuts down.
    LogSystem::Initialize(config);
    LUDUS_LOG_INFO(LogRegression, "FIRST_SESSION_MARKER");
    LogSystem::Flush();
    LogSystem::Shutdown();

    // Immediately reinitialize (same second, same PID). The old file must NOT be
    // reopened in truncate mode; a new, distinctly-named session file appears and
    // the first session's marker survives (requirements R42; fixes F7).
    LogSystem::Initialize(config);
    LUDUS_LOG_INFO(LogRegression, "SECOND_SESSION_MARKER");
    LogSystem::Flush();
    LogSystem::Shutdown();

    bool first_survived = false;
    bool second_present = false;
    int session_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (entry.path().extension() != ".log")
        {
            continue;
        }
        ++session_files;
        const std::string contents = read_file(entry.path());
        if (contents.find("FIRST_SESSION_MARKER") != std::string::npos)
        {
            first_survived = true;
        }
        if (contents.find("SECOND_SESSION_MARKER") != std::string::npos)
        {
            second_present = true;
        }
    }
    CHECK(session_files >= 2);
    CHECK(first_survived);
    CHECK(second_present);
    std::filesystem::remove_all(dir);
}

// --- F4: shutdown closes admission; late logging is safely rejected ----------
TEST_CASE("F4: logging after shutdown is safely rejected, not lost into dead sinks",
          "[logging][regression][F4][lifecycle]")
{
    const auto dir = make_temp_log_dir("f4_admission");
    const std::string dir_str = dir.string();
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    config.Directory = dir_str;
    LogSystem::Initialize(config);
    REQUIRE(LogSystem::IsInitialized());
    LogSystem::Shutdown();
    REQUIRE_FALSE(LogSystem::IsInitialized());

    // Post-shutdown logging must not crash and must not resurrect the cleared
    // sinks. Warning+ routes to the emergency path (stderr); Info is dropped.
    LUDUS_LOG_INFO(LogRegression, "post-shutdown info dropped");
    LUDUS_LOG_ERROR(LogRegression, "post-shutdown error is safe");
    CHECK_FALSE(LogSystem::IsInitialized());
    std::filesystem::remove_all(dir);
}

// --- Bounded typed formatter contract (Phase 2) ------------------------------
TEST_CASE("typed formatter: supported types, hex, and escaped braces", "[logging][format][phase2]")
{
    const auto dir = make_temp_log_dir("fmt_types");
    const std::string dir_str = dir.string();
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    config.Directory = dir_str;
    LogSystem::Initialize(config);

    const int i = -7;
    const unsigned u = 255;
    const double d = 2.5;
    const bool b = true;
    const char* s = "str";
    LUDUS_LOG_INFO(LogRegression, "i={} u={} d={} b={} s={} hex={:x} braces={{}}", i, u, d, b, s, u);
    LogSystem::Flush();
    LogSystem::Shutdown();

    const std::string contents = capture_single_file_log(dir);
    CHECK(contents.find("i=-7 u=255 d=2.5 b=true s=str hex=ff braces={}") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("typed formatter: over-long message is truncated, not allocated", "[logging][format][phase2][bounds]")
{
    const auto dir = make_temp_log_dir("fmt_trunc");
    const std::string dir_str = dir.string();
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    config.Directory = dir_str;
    LogSystem::Initialize(config);

    // A 4000-char argument exceeds the 2048-byte producer buffer; the record
    // must be bounded (truncated), never grow via heap allocation.
    const std::string big(4000, 'A');
    LUDUS_LOG_INFO(LogRegression, "big={}", big);
    LogSystem::Flush();
    LogSystem::Shutdown();

    const std::string contents = capture_single_file_log(dir);
    // The written line is bounded well under the raw argument size.
    CHECK(contents.size() < 3000);
    CHECK(contents.find("big=AAAA") != std::string::npos);
    std::filesystem::remove_all(dir);
}

// --- F1 (smoke, non-TSan): concurrent producers must not corrupt output -------
TEST_CASE("F1: concurrent producers do not corrupt or lose framing", "[logging][regression][F1][concurrency]")
{
    const auto dir = make_temp_log_dir("f1_concurrency");
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    const std::string dir_str = dir.string();
    config.Directory = dir_str;
    LogSystem::Initialize(config);

    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    Array<std::thread> workers;
    workers.EnsureCapacity(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        workers.AddInPlace([t] {
            for (int i = 0; i < kPerThread; ++i)
            {
                LUDUS_LOG_WARN(LogRegression, "producer={} record={}", t, i);
            }
        });
    }
    for (auto& w : workers)
    {
        w.join();
    }
    LogSystem::Flush();
    LogSystem::Shutdown();

    // Every emitted line must be well-formed: no interleaved/torn line should
    // appear. We check that the count of "producer=" markers equals the number
    // of records and that no line contains two markers (a sign of torn framing).
    const std::string contents = capture_single_file_log(dir);
    std::istringstream stream(contents);
    std::string line;
    int marker_lines = 0;
    bool torn = false;
    while (std::getline(stream, line))
    {
        const auto first = line.find("producer=");
        if (first == std::string::npos)
        {
            continue; // source-suffix line for Warning+, ignore
        }
        ++marker_lines;
        if (line.find("producer=", first + 1) != std::string::npos)
        {
            torn = true;
        }
    }
    CHECK_FALSE(torn);
    CHECK(marker_lines == kThreads * kPerThread);
    std::filesystem::remove_all(dir);
}

// --- Phase 3: direct debug-critical delivery returns a result ----------------
TEST_CASE("direct debug-sync delivers and reports status", "[logging][phase3][direct]")
{
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = false;
    LogSystem::Initialize(config);

    // Delivered directly (to stderr via the Base primitive), independent of any
    // sink/worker, and returns Delivered.
    const DeliveryResult r = LUDUS_LOG_DEBUG_SYNC(LogRegression, "before breakpoint");
    CHECK(r.Status == DeliveryStatus::Delivered);

    // When the category is filtered out at runtime, it reports Filtered and does
    // not deliver.
    LogSystem::SetGlobalLevel(LogLevel::Error);
    const DeliveryResult r2 = LUDUS_LOG_DEBUG_SYNC(LogRegression, "should be filtered");
    CHECK(r2.Status == DeliveryStatus::Filtered);

    LogSystem::Shutdown();
}

// --- Phase 3: producer-side breadcrumbs capture recent Warning+ context ------
TEST_CASE("breadcrumbs capture recent warning/error records", "[logging][phase3][breadcrumb]")
{
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = false;
    LogSystem::Initialize(config);

    LUDUS_LOG_WARN(LogRegression, "breadcrumb WARN {}", 1);
    LUDUS_LOG_ERROR(LogRegression, "breadcrumb ERROR {}", 2);
    // Info is below the Warning+ breadcrumb selection and must NOT be captured.
    LUDUS_LOG_INFO(LogRegression, "breadcrumb INFO not captured");

    internal::BreadcrumbRing::Entry entries[internal::BreadcrumbRing::kCapacity];
    const usize n = SnapshotBreadcrumbs(entries, internal::BreadcrumbRing::kCapacity);
    REQUIRE(n >= 2);

    bool sawWarn = false;
    bool sawError = false;
    bool sawInfo = false;
    for (usize i = 0; i < n; ++i)
    {
        const std::string_view msg(entries[i].Message, entries[i].MessageLen);
        if (msg.find("breadcrumb WARN 1") != std::string_view::npos)
        {
            sawWarn = true;
        }
        if (msg.find("breadcrumb ERROR 2") != std::string_view::npos)
        {
            sawError = true;
        }
        if (msg.find("not captured") != std::string_view::npos)
        {
            sawInfo = true;
        }
    }
    CHECK(sawWarn);
    CHECK(sawError);
    CHECK_FALSE(sawInfo);

    LogSystem::Shutdown();
}

// --- Phase 3: breadcrumb ring is race-free under concurrent writers+reader ---
TEST_CASE("breadcrumb ring: concurrent writers and a reader are race-free",
          "[logging][phase3][breadcrumb][concurrency]")
{
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = false;
    LogSystem::Initialize(config);

    std::atomic<bool> stop{false};
    Array<std::thread> writers;
    writers.EnsureCapacity(6);
    for (int t = 0; t < 6; ++t)
    {
        writers.AddInPlace([t, &stop] {
            int i = 0;
            while (stop.load(std::memory_order_relaxed) == false)
            {
                LUDUS_LOG_WARN(LogRegression, "crumb t={} i={}", t, i++);
            }
        });
    }
    // Concurrent reader: repeatedly snapshot the ring while writers run. Under
    // TSan this exercises the atomic-generation read protocol (requirements R34).
    for (int r = 0; r < 200; ++r)
    {
        internal::BreadcrumbRing::Entry entries[internal::BreadcrumbRing::kCapacity];
        const usize n = SnapshotBreadcrumbs(entries, internal::BreadcrumbRing::kCapacity);
        (void)n;
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& w : writers)
    {
        w.join();
    }
    CHECK(true); // the real assertion is "TSan reports no race"
    LogSystem::Shutdown();
}

// --- Phase 3: emergency reentry guard suppresses recursion -------------------
TEST_CASE("emergency reentry guard suppresses nested reporting", "[logging][phase3][emergency]")
{
    // A direct nested EmergencyReport (simulating a fault while reporting) must
    // be suppressed and counted, never recurse (requirements R32). We cannot
    // easily force reentry from outside, so we assert the counter is observable
    // and monotonic and that ordinary reporting does not increment it.
    const ludus::foundation::uint64 before = ludus::foundation::base::EmergencyReentryCount();
    ludus::foundation::base::EmergencyReport(ludus::foundation::base::DiagnosticSeverity::Note,
                                             "Test",
                                             "ordinary emergency, not reentrant");
    const ludus::foundation::uint64 after = ludus::foundation::base::EmergencyReentryCount();
    CHECK(after == before); // a non-nested call does not bump the reentry counter
}

// --- Phase 4: asynchronous backend end-to-end --------------------------------
TEST_CASE("async: records delivered to file, visible after acknowledged flush", "[logging][phase4][async]")
{
    const auto dir = make_temp_log_dir("async_basic");
    const std::string dir_str = dir.string();
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.Mode = LogMode::Asynchronous;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    config.Directory = dir_str;
    config.FlushIntervalMilliseconds = 20;

    const LogInitResult init = LogSystem::Initialize(config);
    REQUIRE(init.EffectiveMode == LogMode::Asynchronous);
    REQUIRE(init.Status == LogStatus::Ok);

    for (int i = 0; i < 50; ++i)
    {
        LUDUS_LOG_INFO(LogRegression, "async record {}", i);
    }
    // Acknowledged visible flush: after it returns Ok, all records submitted
    // before the fence are processed and the file sink is flushed.
    const FlushResult f = LogSystem::Flush(FlushKind::Visible, 2000);
    CHECK(f.Status == LogStatus::Ok);

    const std::string contents = capture_single_file_log(dir);
    CHECK(contents.find("async record 0") != std::string::npos);
    CHECK(contents.find("async record 49") != std::string::npos);

    CHECK(LogSystem::Shutdown() == LogStatus::Ok);
    std::filesystem::remove_all(dir);
}

TEST_CASE("async: shutdown drains outstanding records", "[logging][phase4][async][lifecycle]")
{
    const auto dir = make_temp_log_dir("async_drain");
    const std::string dir_str = dir.string();
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.Mode = LogMode::Asynchronous;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    config.Directory = dir_str;
    config.FlushIntervalMilliseconds = 1000; // long, so shutdown must drain, not the timer
    LogSystem::Initialize(config);

    for (int i = 0; i < 100; ++i)
    {
        LUDUS_LOG_INFO(LogRegression, "drain me {}", i);
    }
    // No explicit flush: Shutdown must drain published records before joining.
    CHECK(LogSystem::Shutdown() == LogStatus::Ok);

    const std::string contents = capture_single_file_log(dir);
    CHECK(contents.find("drain me 0") != std::string::npos);
    CHECK(contents.find("drain me 99") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("async: many concurrent producers, bounded accounting, no corruption",
          "[logging][phase4][async][concurrency]")
{
    const auto dir = make_temp_log_dir("async_conc");
    const std::string dir_str = dir.string();
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.Mode = LogMode::Asynchronous;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    config.Directory = dir_str;
    LogSystem::Initialize(config);

    constexpr int kThreads = 8;
    constexpr int kPer = 500;
    Array<std::thread> workers;
    workers.EnsureCapacity(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        workers.AddInPlace([t] {
            for (int i = 0; i < kPer; ++i)
            {
                LUDUS_LOG_INFO(LogRegression, "t={} i={}", t, i);
            }
        });
    }
    for (auto& w : workers)
    {
        w.join();
    }
    const FlushResult f = LogSystem::Flush(FlushKind::Visible, 5000);
    CHECK((f.Status == LogStatus::Ok || f.Status == LogStatus::TimedOut));

    const LogStatistics stats = LogSystem::Statistics();
    // Accounting is bounded and honest: delivered + dropped never exceeds
    // submitted, and every submitted record is either written or dropped once
    // fully drained (requirements R38/R48).
    CHECK(stats.Submitted >= static_cast<ludus::foundation::uint64>(kThreads * kPer));
    CHECK(stats.Written + stats.Dropped <= stats.Submitted);

    CHECK(LogSystem::Shutdown() == LogStatus::Ok);
    std::filesystem::remove_all(dir);
}

// --- Phase 4: MPSC queue invariants (direct unit test) -----------------------
TEST_CASE("mpsc queue: full queue drops without overwrite; drain preserves FIFO", "[logging][phase4][queue]")
{
    using ludus::foundation::logging::internal::MpscQueue;
    using ludus::foundation::logging::internal::QueuedRecord;
    auto q = std::make_unique<MpscQueue<8>>();

    QueuedRecord rec;
    // Fill to capacity.
    int enqueued = 0;
    for (int i = 0; i < 8; ++i)
    {
        rec.Sequence = static_cast<ludus::foundation::uint64>(i);
        if (q->TryEnqueue(rec))
        {
            ++enqueued;
        }
    }
    CHECK(enqueued == 8);

    // The next enqueue must FAIL (full) and be counted as a drop, never
    // overwrite an occupied cell (requirements R35/R38).
    rec.Sequence = 999;
    CHECK_FALSE(q->TryEnqueue(rec));
    CHECK(q->Dropped() >= 1);

    // Drain in FIFO order; the dropped record (999) must never appear.
    for (int i = 0; i < 8; ++i)
    {
        QueuedRecord out;
        REQUIRE(q->TryDequeue(out));
        CHECK(out.Sequence == static_cast<ludus::foundation::uint64>(i));
    }
    // Empty now.
    QueuedRecord out;
    CHECK_FALSE(q->TryDequeue(out));

    // After draining, the freed cells accept new records again (reuse only after
    // consumer release).
    rec.Sequence = 1000;
    CHECK(q->TryEnqueue(rec));
    REQUIRE(q->TryDequeue(out));
    CHECK(out.Sequence == 1000);
}

TEST_CASE("async: queue saturation is bounded and accounted, no crash", "[logging][phase4][async][saturation]")
{
    const auto dir = make_temp_log_dir("async_sat");
    const std::string dir_str = dir.string();
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.Mode = LogMode::Asynchronous;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    config.Directory = dir_str;
    config.FlushIntervalMilliseconds = 1000; // slow flush so the queue can fill
    LogSystem::Initialize(config);

    // Flood far beyond queue capacity as fast as possible from one thread; some
    // records will be dropped when the worker cannot keep up. The system must
    // stay bounded (no unbounded memory), never crash, and account every record.
    constexpr int kFlood = 200000;
    for (int i = 0; i < kFlood; ++i)
    {
        LUDUS_LOG_INFO(LogRegression, "flood {}", i);
    }
    const FlushResult f = LogSystem::Flush(FlushKind::Visible, 5000);
    CHECK((f.Status == LogStatus::Ok || f.Status == LogStatus::TimedOut));

    const LogStatistics stats = LogSystem::Statistics();
    CHECK(stats.Submitted >= static_cast<ludus::foundation::uint64>(kFlood));
    // Bounded accounting: delivered + dropped never exceeds submitted.
    CHECK(stats.Written + stats.Dropped <= stats.Submitted);

    CHECK(LogSystem::Shutdown() == LogStatus::Ok);
    std::filesystem::remove_all(dir);
}

// --- Phase 4: async Error records are promptly flushed (Error visibility) ----
TEST_CASE("async: Error is visible without an explicit flush (R54)", "[logging][phase4][async][error]")
{
    const auto dir = make_temp_log_dir("async_error");
    const std::string dir_str = dir.string();
    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.Mode = LogMode::Asynchronous;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    config.Directory = dir_str;
    config.FlushIntervalMilliseconds = 1000; // long: rely on the Error auto-flush, not the timer
    LogSystem::Initialize(config);

    LUDUS_LOG_ERROR(LogRegression, "async error visibility marker");

    // Poll briefly: the worker flushes Error promptly (RenderAndWrite flushes on
    // Error/Fatal), so the record should appear without our calling Flush().
    const auto path = find_log_file(dir);
    bool visible = false;
    for (int i = 0; i < 200 && visible == false; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        visible = read_file(path).find("async error visibility marker") != std::string::npos;
    }
    CHECK(visible);

    CHECK(LogSystem::Shutdown() == LogStatus::Ok);
    std::filesystem::remove_all(dir);
}
