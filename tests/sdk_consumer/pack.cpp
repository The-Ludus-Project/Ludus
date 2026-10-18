#include <ludus/foundation/filesystem/pack.hpp>

#include <span>

// Cross-platform installed-SDK contract. This small v1 golden pack was emitted
// by scripts/python/pack_builder.py from [("entry", b"A" * 20)]. No producer
// source headers, native path I/O or third-party codecs enter the consumer.
namespace
{
using namespace ludus::foundation;
constexpr uint8 kPack[] = {
    76, 85, 68, 80, 65,  67, 75, 0,   1,   0,   0,   0,   80,  0,   0,  0,  0,   0,   0,  0,   0,  0,  1,   0, 1, 0,
    0,  0,  1,  0,  0,   0,  80, 0,   0,   0,   0,   0,   0,   0,   61, 0,  0,   0,   0,  0,   0,  0,  141, 0, 0, 0,
    0,  0,  0,  0,  151, 0,  0,  0,   0,   0,   0,   0,   164, 93,  31, 2,  183, 202, 63, 126, 0,  0,  0,   0, 0, 0,
    0,  0,  5,  0,  0,   0,  0,  0,   0,   0,   1,   0,   0,   0,   0,  0,  0,   0,   20, 0,   0,  0,  0,   0, 0, 0,
    10, 0,  0,  0,  0,   0,  0,  0,   101, 110, 116, 114, 121, 141, 0,  0,  0,   0,   0,  0,   0,  10, 0,   0, 0, 20,
    0,  0,  0,  1,  0,   0,  0,  197, 24,  32,  29,  26,  65,  1,   0,  80, 65,  65,  65, 65,  65,
};
} // namespace

int ExerciseInstalledPack() noexcept
{
    using namespace ludus::foundation;
    using namespace ludus::foundation::filesystem;
    ProviderHandle storage, pack;
    const MemoryEntry entry{"archive", kPack};
    if (CreateMemoryProvider({&entry, 1}, storage).Succeeded() == false ||
        CreatePackProvider(storage, "archive", {}, pack).Succeeded() == false)
    {
        return 11;
    }
    const Mount mount{Root::Assets, {}, 0, 1, pack};
    MountSnapshot snapshot;
    VirtualPath key;
    VirtualFile file, clone;
    if (MountSnapshot::Create({&mount, 1}, 1, snapshot).Succeeded() == false ||
        key.Set(Root::Assets, "entry").Succeeded() == false ||
        snapshot.OpenRead(key, file).Outcome.Succeeded() == false || file.Clone(clone).Succeeded() == false ||
        MountSnapshot::Create({}, 2, snapshot).Succeeded() == false)
    {
        return 11;
    }
    pack = {};
    storage = {};
    file.Close();
    uint8 bytes[20]{};
    const auto read = clone.ReadAt(0, bytes);
    return read.Outcome.Succeeded() && read.BytesRead == 20 && bytes[19] == 'A' && clone.Generation() == 1 ? 0 : 11;
}
