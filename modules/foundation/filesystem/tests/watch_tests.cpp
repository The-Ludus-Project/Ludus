#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/watch.hpp>

#include "persistence_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;
using filesystem_fixture::Fixture;
using filesystem_fixture::Publish;

TEST_CASE("Watch debounce coalesces cooked replacements and leaves opened readers unchanged")
{
    Fixture fixture;
    fixture.Write("nested/data", "old");
    WriteDirectory writer;
    Directory root;
    File old;
    REQUIRE(writer.Open(fixture.Root).Succeeded());
    REQUIRE(root.Open(fixture.Root).Succeeded());
    REQUIRE(root.OpenRead("nested/data", old).Succeeded());
    Watcher watcher;
    REQUIRE(watcher.Init(fixture.Root, {1, 4, 10}).Succeeded());
    WatchHandle handle;
    REQUIRE(watcher.Add("nested/data", 42, handle).Succeeded());
    WatchHint hint;
    REQUIRE(watcher.Advance(0, 1).Succeeded());
    REQUIRE_FALSE(watcher.Poll(hint).Available);
    REQUIRE(Publish(writer, "nested/data", "first").Published);
    REQUIRE(watcher.Advance(1, 1).Succeeded());
    REQUIRE(watcher.Advance(9, 1).Succeeded());
    REQUIRE_FALSE(watcher.Poll(hint).Available);
    REQUIRE(Publish(writer, "nested/data", "final").Published);
    REQUIRE(watcher.Advance(10, 1).Succeeded());
    REQUIRE(watcher.Advance(19, 1).Succeeded());
    REQUIRE_FALSE(watcher.Poll(hint).Available);
    REQUIRE(watcher.Advance(20, 1).Succeeded());
    REQUIRE(watcher.Poll(hint).Available);
    REQUIRE(hint.Kind == WatchHintKind::Changed);
    REQUIRE(hint.Handle == handle);
    REQUIRE(hint.Tag == 42);
    REQUIRE(hint.PathView() == "nested/data");
    REQUIRE(hint.Observation.Succeeded());
    REQUIRE(hint.Stamp.Size == 5);
    REQUIRE_FALSE(watcher.Poll(hint).Available);
    REQUIRE(hint.Tag == 42);
    uint8 bytes[3]{};
    REQUIRE(old.ReadAt(0, bytes).Outcome.Succeeded());
    REQUIRE(bytes[0] == 'o');
    REQUIRE(watcher.Advance(19, 1).Code == Status::InvalidArgument);
    REQUIRE(watcher.Advance(21, 0).Code == Status::InvalidArgument);
    REQUIRE(watcher.Advance(21, 2).Code == Status::InvalidArgument);
}
TEST_CASE("Watch disappearance creation and reverted candidates have explicit observations")
{
    Fixture fixture;
    Watcher watcher;
    REQUIRE(watcher.Init(fixture.Root, {1, 4, 10}).Succeeded());
    WatchHandle handle;
    REQUIRE(watcher.Add("nested/data", 1, handle).Succeeded());
    fixture.Write("nested/data", "candidate");
    REQUIRE(watcher.Advance(1, 1).Succeeded());
    std::filesystem::remove(fixture.Path("nested/data"));
    REQUIRE(watcher.Advance(12, 1).Succeeded());
    WatchHint hint;
    REQUIRE_FALSE(watcher.Poll(hint).Available);
    fixture.Write("nested/data", "created");
    REQUIRE(watcher.Advance(20, 1).Succeeded());
    REQUIRE(watcher.Advance(30, 1).Succeeded());
    REQUIRE(watcher.Poll(hint).Available);
    REQUIRE(hint.Observation.Succeeded());
    std::filesystem::remove(fixture.Path("nested/data"));
    REQUIRE(watcher.Advance(40, 1).Succeeded());
    REQUIRE(watcher.Advance(50, 1).Succeeded());
    REQUIRE(watcher.Poll(hint).Available);
    REQUIRE(hint.Observation.Code == Status::NotFound);
    REQUIRE(hint.Stamp == FileStamp{});
}
TEST_CASE("Watch overflow invalidates hints until a bounded full registered-path rescan")
{
    Fixture fixture;
    Watcher watcher;
    REQUIRE(watcher.Init(fixture.Root, {3, 1, 0}).Succeeded());
    WatchHandle a, b, c;
    REQUIRE(watcher.Add("nested/a", 1, a).Succeeded());
    REQUIRE(watcher.Add("nested/b", 2, b).Succeeded());
    REQUIRE(watcher.Add("nested/c", 3, c).Succeeded());
    fixture.Write("nested/a", "a");
    fixture.Write("nested/b", "b");
    REQUIRE(watcher.Advance(1, 1).Succeeded());
    REQUIRE_FALSE(watcher.NeedsRescan());
    REQUIRE(watcher.Advance(1, 1).Code == Status::LimitExceeded);
    REQUIRE(watcher.NeedsRescan());
    REQUIRE(watcher.DroppedHints() == 2);
    WatchHint hint;
    REQUIRE(watcher.Poll(hint).Available);
    REQUIRE(hint.Kind == WatchHintKind::RescanRequired);
    REQUIRE(hint.PathView().empty());
    REQUIRE_FALSE(watcher.Poll(hint).Available);
    REQUIRE(watcher.Advance(2, 1).Code == Status::Conflict);
    REQUIRE(watcher.BeginRescan().Succeeded());
    REQUIRE(watcher.IsRescanning());
    REQUIRE(watcher.Add("new", 0, a).Code == Status::Conflict);
    REQUIRE(watcher.Remove(a).Code == Status::Conflict);
    REQUIRE(watcher.Poll(hint).Outcome.Code == Status::Conflict);
    REQUIRE(watcher.BeginRescan().Code == Status::Conflict);
    for (uint64 tag = 1; tag <= 3; ++tag)
    {
        REQUIRE(watcher.NextRescan(hint).Available);
        REQUIRE(hint.Kind == WatchHintKind::RescanEntry);
        REQUIRE(hint.Tag == tag);
        REQUIRE(hint.Observation.Code == (tag == 3 ? Status::NotFound : Status::Ok));
    }
    REQUIRE_FALSE(watcher.NextRescan(hint).Available);
    REQUIRE_FALSE(watcher.IsRescanning());
    REQUIRE_FALSE(watcher.NeedsRescan());
    REQUIRE(watcher.NextRescan(hint).Outcome.Code == Status::Conflict);
    fixture.Write("nested/c", "after-rescan");
    REQUIRE(watcher.Advance(2, 3).Succeeded());
    REQUIRE(watcher.Poll(hint).Available);
    REQUIRE(hint.Handle == c);
}
TEST_CASE("Watch observation errors request rescan without interpreting unsafe files as absent")
{
    Fixture fixture;
    fixture.Write("nested/data", "original");
    Watcher watcher;
    REQUIRE(watcher.Init(fixture.Root, {1, 1, 0}).Succeeded());
    WatchHandle handle;
    REQUIRE(watcher.Add("nested/data", 7, handle).Succeeded());
    std::filesystem::remove(fixture.Path("nested/data"));
    std::filesystem::create_symlink("/tmp", fixture.Path("nested/data"));
    REQUIRE(watcher.Advance(1, 1).Code == Status::AccessDenied);
    REQUIRE(watcher.NeedsRescan());
    WatchHint hint;
    REQUIRE(watcher.Poll(hint).Available);
    REQUIRE(watcher.BeginRescan().Succeeded());
    REQUIRE(watcher.NextRescan(hint).Available);
    REQUIRE(hint.Observation.Code == Status::AccessDenied);
    REQUIRE_FALSE(watcher.NextRescan(hint).Available);
    REQUIRE(watcher.NeedsRescan());
    REQUIRE(watcher.Poll(hint).Available);
    REQUIRE(hint.Kind == WatchHintKind::RescanRequired);
    std::filesystem::remove(fixture.Path("nested/data"));
    fixture.Write("nested/data", "fixed");
    REQUIRE(watcher.BeginRescan().Succeeded());
    REQUIRE(watcher.NextRescan(hint).Available);
    REQUIRE(hint.Observation.Succeeded());
    REQUIRE_FALSE(watcher.NextRescan(hint).Available);
    REQUIRE_FALSE(watcher.NeedsRescan());
}
TEST_CASE("Watch registrations enforce capacity retire hints and reject stale or foreign handles")
{
    Fixture fixture;
    Watcher watcher, other;
    WatchHint hint;
    WatchHandle a, b;
    REQUIRE(watcher.Add("data", 0, a).Code == Status::InvalidArgument);
    REQUIRE(watcher.Poll(hint).Outcome.Code == Status::InvalidArgument);
    REQUIRE(watcher.BeginRescan().Code == Status::InvalidArgument);
    REQUIRE(watcher.Init(fixture.Root, {0, 1, 0}).Code == Status::InvalidArgument);
    REQUIRE(watcher.Init(fixture.Root, {1, 0, 0}).Code == Status::InvalidArgument);
    REQUIRE(watcher.Init(fixture.Root, {4097, 1, 0}).Code == Status::InvalidArgument);
    REQUIRE(watcher.Init(fixture.Root, {2, 3, 0}).Succeeded());
    REQUIRE(other.Init(fixture.Root, {1, 1, 0}).Succeeded());
    REQUIRE(watcher.Add("nested/a", 1, a).Succeeded());
    const auto preserved = a;
    REQUIRE(watcher.Add("nested/a", 0, a).Code == Status::InvalidArgument);
    REQUIRE(a == preserved);
    REQUIRE(watcher.Add("../bad", 0, a).Code == Status::InvalidArgument);
    REQUIRE(watcher.Add("nested/b", 2, b).Succeeded());
    REQUIRE(watcher.Add("nested/c", 3, a).Code == Status::LimitExceeded);
    REQUIRE(other.Remove(a).Code == Status::InvalidArgument);
    fixture.Write("nested/a", "a");
    fixture.Write("nested/b", "b");
    REQUIRE(watcher.Advance(1, 2).Succeeded());
    REQUIRE(watcher.Remove(a).Succeeded());
    REQUIRE(watcher.Remove(a).Code == Status::InvalidArgument);
    REQUIRE(watcher.Poll(hint).Available);
    REQUIRE(hint.Handle == b);
    REQUIRE_FALSE(watcher.Poll(hint).Available);
    WatchHandle replacement;
    REQUIRE(watcher.Add("nested/c", 3, replacement).Succeeded());
    REQUIRE(replacement != a);
    REQUIRE(watcher.Remove(a).Code == Status::InvalidArgument);
    REQUIRE(watcher.Init("/ludus-missing-watch-f5").Code == Status::NotFound);
    REQUIRE(watcher.Remove(replacement).Succeeded());
    REQUIRE(watcher.Init(fixture.Root, {1, 1, 0}).Succeeded());
    REQUIRE(watcher.Remove(b).Code == Status::InvalidArgument);
}
TEST_CASE("Watch roots remain pinned after trusted-root pathname moves")
{
    Fixture fixture;
    Watcher watcher;
    const std::string alias = fixture.Path("trusted-root");
    std::filesystem::create_directory_symlink(fixture.Root, alias);
    REQUIRE(watcher.Init(alias, {1, 1, 0}).Succeeded());
    WatchHandle handle;
    REQUIRE(watcher.Add("nested/data", 9, handle).Succeeded());
    const auto moved = std::string(fixture.Root) + "-moved";
    std::filesystem::rename(fixture.Root, moved);
    constexpr std::string_view COMPLETE = "complete";
    REQUIRE(ludus::test_support::WriteFile(moved + "/nested/data",
                                           {reinterpret_cast<const uint8*>(COMPLETE.data()), COMPLETE.size()}) == true);
    REQUIRE(watcher.Advance(1, 1).Succeeded());
    WatchHint hint;
    REQUIRE(watcher.Poll(hint).Available);
    REQUIRE(hint.Stamp.Size == 8);
    std::filesystem::rename(moved, fixture.Root);
}
