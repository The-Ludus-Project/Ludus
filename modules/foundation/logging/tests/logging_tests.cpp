#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include <catch2/catch_test_macros.hpp>

#include "format_test_name.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

inline constexpr LogCategory LogLifecycleTest{"LifecycleTest"};

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
        std::filesystem::temp_directory_path() / FormatTestName("ludus_log_test_{}_{}", tag, LUDUS_TEST_GETPID());
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

usize count_log_files(const std::filesystem::path& dir)
{
    usize count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".log")
        {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_CASE("logging works before initialization and after shutdown without crashing", "[logging][lifecycle]")
{
    // Not initialized: Warning+ should be accepted (routes to emergency path),
    // lower levels rejected. This must not crash.
    REQUIRE_FALSE(LogSystem::IsInitialized());
    CHECK_FALSE(ShouldLog(LogLevel::Info, LogLifecycleTest));
    CHECK(ShouldLog(LogLevel::Warning, LogLifecycleTest));
    CHECK(ShouldLog(LogLevel::Fatal, LogLifecycleTest));

    // These go to stderr via the emergency path; the point is that they are safe.
    LUDUS_LOG_WARN(LogLifecycleTest, "pre-init warning is safe");
    LUDUS_LOG_INFO(LogLifecycleTest, "pre-init info is dropped");

    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = false;
    LogSystem::Initialize(config);
    REQUIRE(LogSystem::IsInitialized());
    LogSystem::Shutdown();
    REQUIRE_FALSE(LogSystem::IsInitialized());

    // Post-shutdown behaves like pre-init.
    CHECK(ShouldLog(LogLevel::Error, LogLifecycleTest));
    LUDUS_LOG_ERROR(LogLifecycleTest, "post-shutdown error is safe");
}

// Backend tests use raw-text submission so Release's macro stripping does not erase
// their inputs. Macro stripping has separate coverage in category_tests.cpp.
TEST_CASE("synchronous logging makes the record visible before the call returns", "[logging][synchronous]")
{
    const auto dir = make_temp_log_dir("sync");

    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.Mode = LogMode::Synchronous;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    const std::string dir_str = dir.string();
    config.Directory = dir_str;
    LogSystem::Initialize(config);

    LUDUS_LOG_TEXT(LogLifecycleTest, Info, "synchronous visibility marker");
    // In synchronous mode the record must already be in the file sink's stream
    // by the time the log call returns. We flush to defeat OS buffering, then read.
    LogSystem::Flush();

    const std::string contents = read_file(find_log_file(dir));
    CHECK(contents.find("synchronous visibility marker") != std::string::npos);

    LogSystem::Shutdown();
    std::filesystem::remove_all(dir);
}

TEST_CASE("file sink creates a uniquely named session file and writes to it", "[logging][file]")
{
    const auto dir = make_temp_log_dir("file_create");

    LogConfig config{};
    config.GlobalLevel = LogLevel::Info;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    const std::string dir_str = dir.string();
    config.Directory = dir_str;
    LogSystem::Initialize(config);

    const std::filesystem::path path = find_log_file(dir);
    REQUIRE_FALSE(path.empty());
    const std::string name = path.filename().string();
    // Session naming carries the pid (spec section 20) so concurrent processes
    // never collide.
    CHECK(name.find("_pid-") != std::string::npos);

    LUDUS_LOG_TEXT(LogLifecycleTest, Info, "written to session file");
    LogSystem::Flush();
    LogSystem::Shutdown();

    const std::string contents = read_file(path);
    CHECK(contents.find("written to session file") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("shutdown drains and flushes buffered records to the file", "[logging][file][lifecycle]")
{
    const auto dir = make_temp_log_dir("drain");

    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    const std::string dir_str = dir.string();
    config.Directory = dir_str;
    LogSystem::Initialize(config);

    const std::filesystem::path path = find_log_file(dir);
    // Exercise backend flushing even when the preset strips Debug macros.
    // Error would flush before Shutdown and weaken the test.
    LUDUS_LOG_TEXT(LogLifecycleTest, Debug, "record before shutdown");
    // Do NOT flush explicitly; shutdown() must flush on its own (spec section 24).
    LogSystem::Shutdown();

    const std::string contents = read_file(path);
    CHECK(contents.find("record before shutdown") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("file retention prunes old sessions but not unrelated files", "[logging][file][retention]")
{
    const auto dir = make_temp_log_dir("retention");

    // Pre-seed the directory with session files in THIS logger's naming scheme
    // (UTC stamp + pid + nonce marker), beyond the retention limit. Give them an
    // old modification time so they sort as the oldest sessions.
    for (int i = 0; i < 5; ++i)
    {
        const auto stale_path = dir / FormatTestName("2020-01-01_00-00-0{}_pid-100{}_n{}.log", i, i, i);
        {
            std::ofstream stale(stale_path);
            stale << "old session " << i << '\n';
        }
    }
    // An unrelated file that this logger did NOT create must never be pruned
    // (requirements R43; fixes F7).
    {
        std::ofstream unrelated(dir / "important-notes.log");
        unrelated << "do not delete me\n";
    }
    REQUIRE(count_log_files(dir) == 6);

    LogConfig config{};
    config.GlobalLevel = LogLevel::Info;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    const std::string dir_str = dir.string();
    config.Directory = dir_str;
    config.RetainedSessions = 3;
    LogSystem::Initialize(config);
    LogSystem::Shutdown();

    // The unrelated file must survive.
    CHECK(std::filesystem::exists(dir / "important-notes.log"));

    // Session files (matching this logger's scheme) are pruned to at most
    // `RetainedSessions`: 2 kept old sessions + the 1 new active session.
    usize session_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        const std::string name = entry.path().filename().string();
        if (name.find("_pid-") != std::string::npos && name.find("_n") != std::string::npos)
        {
            ++session_files;
        }
    }
    CHECK(session_files <= 3);

    std::filesystem::remove_all(dir);
}

TEST_CASE("statistics count submitted and written records", "[logging][statistics]")
{
    const auto dir = make_temp_log_dir("stats");

    LogConfig config{};
    config.GlobalLevel = LogLevel::Trace;
    config.EnableConsole = false;
    config.EnableDebugger = false;
    config.EnableFile = true;
    const std::string dir_str = dir.string();
    config.Directory = dir_str;
    LogSystem::Initialize(config);

    const LogStatistics before = LogSystem::Statistics();
    LUDUS_LOG_TEXT(LogLifecycleTest, Info, "counted record 1");
    LUDUS_LOG_TEXT(LogLifecycleTest, Info, "counted record 2");
    const LogStatistics after = LogSystem::Statistics();

    CHECK(after.Submitted >= before.Submitted + 2);
    CHECK(after.Written >= before.Written + 2);
    // Synchronous mode never drops.
    CHECK(after.Dropped == 0);

    LogSystem::Shutdown();
    std::filesystem::remove_all(dir);
}
