#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/filesystem/watch.hpp>

#include <string_view>
#if defined(LUDUS_PLATFORM_LINUX) || defined(LUDUS_PLATFORM_MACOS)
#    include <cstdlib>
#endif
#if defined(LUDUS_PLATFORM_LINUX) || defined(LUDUS_PLATFORM_MACOS)
#    include <unistd.h>
#endif

int ExerciseInstalledPersistence() noexcept
{
    using namespace ludus::foundation;
    using namespace ludus::foundation::filesystem;
    WriteDirectory writer;
    Watcher watcher;
#if defined(LUDUS_PLATFORM_LINUX) || defined(LUDUS_PLATFORM_MACOS)
    char root[] = "/tmp/ludus-sdk-persistence-XXXXXX";
    if (::mkdtemp(root) == nullptr)
    {
        return 13;
    }
    struct Cleanup final
    {
        const char* Root;
        ~Cleanup() noexcept
        {
            char path[128]{};
            usize length = 0;
            for (char ch : std::string_view(Root))
            {
                path[length++] = ch;
            }
            for (char ch : std::string_view("/data"))
            {
                path[length++] = ch;
            }
            (void)::unlink(path);
            (void)::rmdir(Root);
        }
    } cleanup{root};
    Directory reads;
    File retained;
    WatchHandle handle;
    if (writer.Open(root).Succeeded() == false || reads.Open(root).Succeeded() == false ||
        watcher.Init(root, {1, 1, 0}).Succeeded() == false || watcher.Add("data", 13, handle).Succeeded() == false)
    {
        return 13;
    }
    const uint8 original[]{1, 2}, replacement[]{3};
    WriteOptions options;
    options.Condition = WriteCondition::Missing;
    const auto first = writer.Publish("data", original, options);
    if (first.Published == false || first.Outcome.Succeeded() == false || first.FileSynced == false ||
        first.DirectorySynced == false || first.Cleanup.Succeeded() == false ||
        reads.OpenRead("data", retained).Succeeded() == false || watcher.Advance(1, 1).Succeeded() == false)
    {
        return 13;
    }
    WatchHint hint;
    if (watcher.Poll(hint).Available == false || hint.Handle != handle || hint.PathView() != "data" ||
        hint.Observation.Succeeded() == false || hint.Stamp.Size != 2)
    {
        return 13;
    }
    options.Condition = WriteCondition::MatchingStamp;
    options.Expected = hint.Stamp;
    const auto second = writer.Publish("data", replacement, options);
    uint8 bytes[2]{};
    if (second.Published == false || second.Outcome.Succeeded() == false ||
        writer.Publish("data", original, options).Outcome.Code != Status::Conflict ||
        retained.ReadAt(0, bytes).BytesRead != 2 || bytes[0] != 1 || watcher.BeginRescan().Succeeded() == false)
    {
        return 13;
    }
    return watcher.NextRescan(hint).Available && hint.Kind == WatchHintKind::RescanEntry && hint.Stamp.Size == 1 &&
                   watcher.NextRescan(hint).Available == false && watcher.NeedsRescan() == false
               ? 0
               : 13;
#else
    const uint8 byte[]{1};
    Directory reads;
    FileStamp stamp;
    stamp.Size = 19;
    WatchHint hint;
    hint.Tag = 13;
#    if defined(LUDUS_PLATFORM_WEB)
    constexpr Status UNOPENED_READ_STATUS = Status::InvalidArgument;
    constexpr Status MISSING_WATCH_ROOT_STATUS = Status::NotFound;
#    else
    constexpr Status UNOPENED_READ_STATUS = Status::Unsupported;
    constexpr Status MISSING_WATCH_ROOT_STATUS = Status::Unsupported;
#    endif
    return writer.Open("root").Code == Status::Unsupported &&
                   writer.Publish("data", byte).Outcome.Code == Status::Unsupported && writer.IsOpen() == false &&
                   reads.Observe("data", stamp).Code == UNOPENED_READ_STATUS && stamp.Size == 19 &&
                   watcher.Init("root").Code == MISSING_WATCH_ROOT_STATUS && watcher.Poll(hint).Available == false &&
                   hint.Tag == 13
               ? 0
               : 13;
#endif
}

#if defined(LUDUS_SDK_PERSISTENCE_STANDALONE)
int main()
{
    return ExerciseInstalledPersistence();
}
#endif
