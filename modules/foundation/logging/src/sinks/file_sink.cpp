#include "sinks/file_sink.hpp"
#include <ludus/foundation/base/config.h>

#include "internal/format_engine.hpp"
#include "internal/formatter.hpp"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
#    include <io.h>
#    include <process.h>
#    define LUDUS_GETPID _getpid
#else
#    include <fcntl.h>
#    include <unistd.h>
#    define LUDUS_GETPID ::getpid
#endif

namespace ludus::foundation::logging::internal
{

namespace
{

// Process-wide nonce so two session files created in the same second by the same
// PID (rapid reinit) never collide on a name (requirements R42; fixes F7).
std::atomic<uint32> gSessionNonce{0};

// Build "YYYY-MM-DD_HH-MM-SS_pid-NNNNN_nMM.log" using UTC time. UTC avoids
// ambiguity across DST/zone changes; the nonce + exclusive creation are the
// real collision guards.
std::string sessionFileName(uint32 nonce)
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm broken{};
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    gmtime_s(&broken, &now);
#else
    gmtime_r(&now, &broken);
#endif

    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &broken);

    const auto pid = static_cast<long>(LUDUS_GETPID());
    char name[96];
    const FormatArg args[] = {std::string_view(stamp), static_cast<int64>(pid), nonce};
    const FormatOutcome result = FormatInto(name, "{}_pid-{}_n{}.log", args);
    if (result.Truncated || result.FormatError)
    {
        return {};
    }
    return {name, result.BytesWritten};
}

// The base name of a session file WITHOUT the rotation suffix, used to group all
// segments of one session together. Session files this logger creates end in
// "_pid-<pid>_n<nonce>.log" (segment 0) or ".<k>.log" after the base for k>0.

// Retention limits grouped so the pruning API is not a row of same-typed args.
struct RetentionLimits
{
    uint32 RetainedSessions = 0;  // 0 = keep all
    uint64 MaxTotalBytes = 0;     // 0 = no total cap
    uint32 MaxSessionAgeDays = 0; // 0 = no age cap
};

// Extract the session group key from a filename. Returns empty if the file is
// not one this logger created (narrow ownership check; never touches unrelated
// files, requirements R43; fixes F7).
std::string sessionBaseKey(const std::string& filename)
{
    // Must end in ".log".
    if (filename.size() < 4 || filename.substr(filename.size() - 4) != ".log")
    {
        return {};
    }
    // Must contain the "_pid-...._n<nonce>" session marker this logger emits.
    const auto pid_pos = filename.find("_pid-");
    const auto nonce_pos = filename.find("_n");
    if (pid_pos == std::string::npos || nonce_pos == std::string::npos || nonce_pos < pid_pos)
    {
        return {};
    }
    // Strip a trailing ".<k>" rotation suffix (before ".log") so all segments
    // of one session share a key. Base segment key is the name minus ".log".
    std::string stem = filename.substr(0, filename.size() - 4); // drop ".log"
    // If stem ends in ".<digits>" AND the part before also matches the session
    // shape, treat the ".<digits>" as a rotation index.
    const auto dot = stem.rfind('.');
    if (dot != std::string::npos && dot > nonce_pos)
    {
        bool all_digits = dot + 1 < stem.size();
        for (usize i = dot + 1; i < stem.size(); ++i)
        {
            if (stem[i] < '0' || stem[i] > '9')
            {
                all_digits = false;
                break;
            }
        }
        if (all_digits)
        {
            stem = stem.substr(0, dot);
        }
    }
    return stem;
}

// Prune whole inactive session groups so at most `retained` sessions (including
// the one about to be created) remain, and (optionally) enforce a total-byte cap
// and age cap. Never deletes the active session or unrelated files. Metadata is
// gathered BEFORE sorting; the comparator is a strict weak ordering over an
// immutable snapshot (requirements R43/R44; fixes F7/F9).
void pruneSessions(const std::filesystem::path& directory,
                   std::string_view activeKey,
                   const RetentionLimits& limits) noexcept
{
    const uint32 retained = limits.RetainedSessions;
    const uint64 maxTotalBytes = limits.MaxTotalBytes;
    const uint32 maxAgeDays = limits.MaxSessionAgeDays;

    std::error_code ec;
    std::filesystem::directory_iterator iter(directory, ec);
    if (ec)
    {
        return;
    }

    const std::string_view active_key = activeKey;

    // Group segments by session key; collect immutable metadata up front. The
    // owning arrays use Ludus::Array; the element types (std::filesystem::path,
    // std::string) remain STL — they belong to <filesystem>/String, which are
    // separate slated facilities, not container replacements.
    struct Group
    {
        std::string Key;
        foundation::Array<std::filesystem::path> Segments;
        std::filesystem::file_time_type NewestTime;
        bool TimeValid = false;
        uint64 TotalBytes = 0;
        bool IsActive = false;
    };
    foundation::Array<Group> groups;

    std::filesystem::directory_iterator end;
    for (; iter != end; iter.increment(ec))
    {
        if (ec)
        {
            break;
        }
        const std::filesystem::directory_entry& entry = *iter;
        std::error_code fec;
        if (entry.is_regular_file(fec) == false || fec)
        {
            continue;
        }
        const std::string name = entry.path().filename().string();
        const std::string key = sessionBaseKey(name);
        if (key.empty())
        {
            continue; // not our file — leave it alone
        }

        std::error_code tec;
        const auto wtime = entry.last_write_time(tec);
        std::error_code sec;
        const auto size = std::filesystem::file_size(entry.path(), sec);

        Group* group = nullptr;
        for (auto& g : groups)
        {
            if (g.Key == key)
            {
                group = &g;
                break;
            }
        }
        if (group == nullptr)
        {
            groups.Add(Group{key, {}, {}, false, 0, key == active_key});
            group = &groups.GetLast();
        }
        group->Segments.Add(entry.path());
        if (sec.value() == 0)
        {
            group->TotalBytes += size;
        }
        if (tec.value() == 0 && (group->TimeValid == false || wtime > group->NewestTime))
        {
            group->NewestTime = wtime;
            group->TimeValid = true;
        }
    }

    // Age cap: delete inactive groups older than maxAgeDays.
    if (maxAgeDays > 0)
    {
        const auto now = std::filesystem::file_time_type::clock::now();
        const auto max_age = std::chrono::hours(24) * static_cast<int>(maxAgeDays);
        for (auto& g : groups)
        {
            if (g.IsActive || g.TimeValid == false)
            {
                continue;
            }
            if (now - g.NewestTime > max_age)
            {
                for (const auto& seg : g.Segments)
                {
                    std::error_code rec;
                    std::filesystem::remove(seg, rec);
                }
                g.Segments.Clear();
            }
        }
    }

    // Build the list of surviving inactive groups, sorted oldest-first over an
    // immutable snapshot (strict weak ordering; entries without a valid time
    // sort last deterministically). `groups` is not mutated after this point, so
    // the Group* pointers stay valid.
    foundation::Array<Group*> inactive;
    for (auto& g : groups)
    {
        if (g.IsActive == false && g.Segments.IsEmpty() == false)
        {
            inactive.Add(&g);
        }
    }
    foundation::Sort(inactive.GetData(), inactive.GetSize(), [](const Group* a, const Group* b) {
        if (a->TimeValid != b->TimeValid)
        {
            return a->TimeValid; // valid times sort before invalid
        }
        if (a->TimeValid == false)
        {
            return a->Key < b->Key; // stable tiebreak
        }
        if (a->NewestTime != b->NewestTime)
        {
            return a->NewestTime < b->NewestTime;
        }
        return a->Key < b->Key;
    });

    // Session-count cap: keep (retained - 1) most-recent inactive groups so the
    // new active session lands at exactly `retained`.
    usize active_count = 0;
    for (const auto& g : groups)
    {
        if (g.IsActive && g.Segments.IsEmpty() == false)
        {
            active_count = 1;
        }
    }
    if (retained > 0)
    {
        const usize keep_inactive = retained > active_count ? (retained - active_count) : 0;
        if (inactive.GetSize() > keep_inactive)
        {
            const usize remove_count = inactive.GetSize() - keep_inactive;
            for (usize i = 0; i < remove_count; ++i)
            {
                for (const auto& seg : inactive[i]->Segments)
                {
                    std::error_code rec;
                    std::filesystem::remove(seg, rec);
                }
                inactive[i]->Segments.Clear();
            }
        }
    }

    // Total-byte cap: remove oldest inactive groups until under the cap.
    if (maxTotalBytes > 0)
    {
        uint64 total = 0;
        for (const auto& g : groups)
        {
            total += g.TotalBytes;
        }
        for (usize i = 0; i < inactive.GetSize() && total > maxTotalBytes; ++i)
        {
            if (inactive[i]->Segments.IsEmpty())
            {
                continue;
            }
            for (const auto& seg : inactive[i]->Segments)
            {
                std::error_code rec;
                std::filesystem::remove(seg, rec);
            }
            total = total > inactive[i]->TotalBytes ? total - inactive[i]->TotalBytes : 0;
            inactive[i]->Segments.Clear();
        }
    }
}

// Exclusive create: "wbx" fails if the file already exists (C11), so we never
// truncate someone else's or a prior same-name session (requirements R42; F7).
std::FILE* exclusiveCreate(const std::filesystem::path& path) noexcept
{
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    std::FILE* file = nullptr;
    // "wbx" is honored by the UCRT; fall back if unavailable.
    if (::fopen_s(&file, path.string().c_str(), "wbx") != 0)
    {
        return nullptr;
    }
    return file;
#else
#    if LUDUS_TARGET_OS == LUDUS_OS_MACOS
    // Darwin stdio does not implement C11's exclusive-create mode. Preserve
    // the no-overwrite contract at the descriptor boundary instead.
    const int descriptor = ::open(path.string().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (descriptor < 0)
    {
        return nullptr;
    }
    std::FILE* file = ::fdopen(descriptor, "wb");
    if (file == nullptr)
    {
        (void)::close(descriptor);
    }
    return file;
#    else
    return std::fopen(path.string().c_str(), "wbx");
#    endif
#endif
}

} // namespace

FileSink* FileSink::Create(const FileSinkConfig& config)
{
    if (config.Directory.empty() || config.Directory.find('\0') != std::string_view::npos)
    {
        return nullptr;
    }

    std::filesystem::path directory{std::string(config.Directory)};

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && std::filesystem::exists(directory) == false)
    {
        return nullptr;
    }

    // Create the session file by exclusive creation with bounded retries on a
    // fresh nonce. Exclusive creation — not a merely-unique-looking name — is
    // the correctness mechanism.
    std::filesystem::path base_path;
    std::FILE* file = nullptr;
    for (int attempt = 0; attempt < 64 && file == nullptr; ++attempt)
    {
        const uint32 nonce = gSessionNonce.fetch_add(1, std::memory_order_relaxed);
        const std::string name = sessionFileName(nonce);
        if (name.empty())
        {
            return nullptr;
        }
        base_path = directory / name;
        file = exclusiveCreate(base_path);
    }
    if (file == nullptr)
    {
        return nullptr;
    }

    // Prune old sessions AFTER creating the active file so it is protected as
    // the active session (never pruned as "old").
    const RetentionLimits limits{config.RetainedSessions, config.MaxTotalBytes, config.MaxSessionAgeDays};
    const std::string active_key = sessionBaseKey(base_path.filename().string());
    pruneSessions(directory, active_key, limits);

    // Store the paths as UTF-8 strings; the header carries no <filesystem>.
    SharedString base;
    if (CreateShared(base_path.string(), GetSystemAllocationDomain(), base) != StringStatus::Ok)
    {
        std::fclose(file);
        return nullptr;
    }
    auto* sink = new (std::nothrow) FileSink(file, std::move(base), config);
    if (sink == nullptr)
    {
        std::fclose(file);
    }
    return sink;
}

FileSink::FileSink(std::FILE* file, SharedString basePathUtf8, const FileSinkConfig& config)
    : mFile(file), mBasePathUtf8(std::move(basePathUtf8)), mActivePathUtf8(mBasePathUtf8),
      mMaxFileSizeBytes(config.MaxFileSizeBytes)
{
    mScratch.reserve(256);
}

FileSink::~FileSink()
{
    if (mFile != nullptr)
    {
        std::fflush(mFile);
        std::fclose(mFile);
        mFile = nullptr;
    }
}

void FileSink::RotateIfNeeded(usize incomingBytes) noexcept
{
    if (mMaxFileSizeBytes == 0 || mFile == nullptr)
    {
        return;
    }
    if (mBytesWritten + incomingBytes <= mMaxFileSizeBytes)
    {
        return;
    }

    ++mRotationIndex;
    // Reconstruct the base path locally to compute the next numbered segment.
    // mBasePathUtf8 ends in ".log"; insert ".<n>" before the extension.
    std::filesystem::path rotated{mBasePathUtf8.GetView()};
    rotated.replace_extension(); // drop ".log"
    char suffix[32];
    const FormatArg args[] = {mRotationIndex};
    const FormatOutcome result = FormatInto(suffix, ".{}.log", args);
    if (result.Truncated || result.FormatError)
    {
        mHealthy = false;
        return;
    }
    rotated += std::string_view(suffix, result.BytesWritten);
    const std::filesystem::path& next = rotated;

    // Open the replacement BEFORE relinquishing the old handle so a failed
    // rotation leaves the current segment usable (requirements R44).
    SharedString nextPath;
    if (CreateShared(next.string(), GetSystemAllocationDomain(), nextPath) != StringStatus::Ok)
    {
        mHealthy = false;
        return;
    }
    std::FILE* newFile = exclusiveCreate(next);
    if (newFile == nullptr)
    {
        mHealthy = false;
        return;
    }

    std::fflush(mFile);
    std::fclose(mFile);
    mFile = newFile;
    mActivePathUtf8 = std::move(nextPath);
    mBytesWritten = 0;
}

SinkStatus FileSink::Write(const LogRecordView& record) noexcept
{
    if (mFile == nullptr)
    {
        return SinkStatus::Failed;
    }
    FormatConsoleLine(record, /*use_color=*/false, mScratch);
    mScratch.push_back('\n');

    RotateIfNeeded(mScratch.size());
    if (mFile == nullptr)
    {
        mHealthy = false;
        return SinkStatus::Failed;
    }

    const usize n = std::fwrite(mScratch.data(), 1, mScratch.size(), mFile);
    mBytesWritten += n;
    mSessionBytesWritten += n;
    if (n != mScratch.size())
    {
        mHealthy = false;
        return SinkStatus::Failed;
    }
    return SinkStatus::Ok;
}

SinkStatus FileSink::Flush() noexcept
{
    if (mFile == nullptr)
    {
        return SinkStatus::Failed;
    }
    return std::fflush(mFile) == 0 ? SinkStatus::Ok : SinkStatus::Failed;
}

SinkStatus FileSink::FlushDurable() noexcept
{
    if (mFile == nullptr)
    {
        return SinkStatus::Failed;
    }
    if (std::fflush(mFile) != 0)
    {
        return SinkStatus::Failed;
    }
    // fflush drains C-library buffering only; a durable flush needs an OS sync of
    // the file's storage (requirements R46). This can stall and is only issued on
    // an explicit Durable flush request.
#if LUDUS_TARGET_OS == LUDUS_OS_WINDOWS
    return SinkStatus::Ok; // FlushFileBuffers wiring is a platform-adapter task
#else
    const int fd = ::fileno(mFile);
    if (fd < 0)
    {
        return SinkStatus::Failed;
    }
#    if LUDUS_TARGET_OS == LUDUS_OS_MACOS
    return ::fsync(fd) == 0 ? SinkStatus::Ok : SinkStatus::Failed;
#    else
    return ::fdatasync(fd) == 0 ? SinkStatus::Ok : SinkStatus::Failed;
#    endif
#endif
}

} // namespace ludus::foundation::logging::internal
