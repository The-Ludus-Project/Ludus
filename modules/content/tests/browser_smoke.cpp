#include <ludus/content/content.h>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/watch.hpp>

#include <cassert>
#include <string_view>

#include <sys/stat.h>
#include <unistd.h>

// Execute against the production Emscripten MEMFS backend, not NODEFS or a mock.
int main()
{
    using namespace ludus;
    using foundation::uint8;
    namespace filesystem = foundation::filesystem;
    constexpr char ROOT_PATH[] = "/browser-content";
    constexpr std::string_view ROOT = ROOT_PATH;
    constexpr std::string_view PATH = "catalog.json";
    constexpr uint8 ORIGINAL[] = {'o', 'l', 'd'};
    constexpr uint8 REPLACEMENT[] = {'n', 'e', 'w'};
    constexpr foundation::usize READ_CAP = 16;
    assert(mkdir(ROOT_PATH, S_IRWXU) == 0);
    assert(content::SaveFile(ROOT, PATH, ORIGINAL, nullptr) == content::Status::Ok);
    assert(content::SaveFile(ROOT, PATH, REPLACEMENT, nullptr) == content::Status::Conflict);
    filesystem::Watcher watcher;
    filesystem::WatchHandle watched;
    assert(watcher.Init(ROOT, {1, 1, 0}).Succeeded() == true);
    assert(watcher.Add(PATH, 1, watched).Succeeded() == true);

    filesystem::Directory directory;
    filesystem::File original;
    filesystem::File clone;
    assert(directory.Open(ROOT).Succeeded() == true);
    assert(directory.OpenRead(PATH, original).Succeeded() == true);
    assert(original.Clone(clone).Succeeded() == true);
    content::Bytes bytes;
    assert(content::ReadFile(ROOT, PATH, READ_CAP, bytes) == content::Status::Ok);
    const content::Digest digest = content::Hash(bytes.Data());
    assert(content::SaveFile(ROOT, PATH, REPLACEMENT, &digest) == content::Status::Ok);
    assert(watcher.Advance(1, 1).Succeeded() == true);
    filesystem::WatchHint hint;
    assert(watcher.Poll(hint).Available == true);
    assert(hint.Handle == watched);
    assert(content::ReadFile(ROOT, PATH, READ_CAP, bytes) == content::Status::Ok);
    assert(bytes.String() == "new");
    assert(content::SaveFile(ROOT, PATH, ORIGINAL, &digest) == content::Status::Conflict);
    assert(content::ReadFile(ROOT, PATH, READ_CAP, bytes) == content::Status::Ok);
    assert(bytes.String() == "new");
    uint8 retained[sizeof(ORIGINAL)]{};
    assert(clone.ReadAt(0, retained).Outcome.Succeeded() == true);
    assert(std::string_view(reinterpret_cast<const char*>(retained), sizeof(retained)) == "old");

    // Admission failure preserves outputs and cannot write outside the root.
    assert(content::ReadFile(ROOT, PATH, 1, bytes) == content::Status::Limit);
    assert(bytes.String() == "new");
    assert(content::SaveFile(ROOT, "../escape", ORIGINAL, nullptr) == content::Status::Invalid);
    assert(symlink("catalog.json", "/browser-content/link") == 0);
    assert(content::ReadFile(ROOT, "link", READ_CAP, bytes) == content::Status::IoError);
    assert(bytes.String() == "new");
    assert(content::SaveFile(ROOT, "link", ORIGINAL, nullptr) != content::Status::Ok);
    assert(content::SaveFile(ROOT, "empty", {}, nullptr) == content::Status::Ok);
    assert(content::ReadFile(ROOT, "empty", READ_CAP, bytes) == content::Status::Ok);
    assert(bytes.Data().empty() == true);
    return 0;
}
