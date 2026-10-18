#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/namespace.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <unistd.h>

int ExerciseInstalledFilesystem() noexcept
{
    using namespace ludus::foundation;
    using namespace ludus::foundation::filesystem;
    char path[] = "/tmp/ludus-installed-fs-XXXXXX";
    if (::mkdtemp(path) == nullptr)
    {
        return 10;
    }
    struct Cleanup final
    {
        const char* Path;
        ~Cleanup()
        {
            std::error_code ignored;
            std::filesystem::remove_all(Path, ignored);
        }
    } cleanup{path};
    {
        std::ofstream file(std::filesystem::path(path) / "asset", std::ios::binary);
        file << "asset";
        file.close();
        if (file.good() == false)
        {
            return 10;
        }
    }
    Directory directory;
    File file, clone;
    uint8 bytes[5]{};
    if (directory.Open(path).Succeeded() == false || directory.OpenRead("asset", file).Succeeded() == false ||
        file.Clone(clone).Succeeded() == false)
    {
        return 10;
    }
    const auto read = clone.ReadAt(1, bytes);
    if (read.Outcome.Succeeded() == false || read.BytesRead != 4 || bytes[0] != 's')
    {
        return 10;
    }
    ProviderHandle memory, native;
    const uint8 memoryBytes[]{1, 2, 3};
    const MemoryEntry entry{"data", memoryBytes};
    if (CreateMemoryProvider({&entry, 1}, memory).Succeeded() == false ||
        CreateDirectoryProvider(path, native).Succeeded() == false)
    {
        return 10;
    }
    const Mount mounts[]{{Root::Assets, {}, 10, 1, memory}, {Root::Assets, {}, 0, 2, native}};
    MountSnapshot snapshot;
    VirtualPath logical;
    VirtualFile opened, retained;
    if (MountSnapshot::Create(mounts, 1, snapshot).Succeeded() == false ||
        logical.Set(Root::Assets, "asset").Succeeded() == false ||
        snapshot.OpenRead(logical, opened).Outcome.Succeeded() == false || opened.MountId() != 2 ||
        opened.Clone(retained).Succeeded() == false || MountSnapshot::Create({}, 2, snapshot).Succeeded() == false)
    {
        return 10;
    }
    const auto retainedRead = retained.ReadAt(0, bytes);
    return retainedRead.Outcome.Succeeded() && retainedRead.BytesRead == 5 && bytes[0] == 'a' &&
                   retained.Generation() == 1
               ? 0
               : 10;
}
