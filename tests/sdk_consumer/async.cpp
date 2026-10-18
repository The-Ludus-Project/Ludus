#include <ludus/foundation/filesystem/async.hpp>

int ExerciseInstalledAsync() noexcept
{
    using namespace ludus::foundation;
    using namespace ludus::foundation::filesystem;
    uint8 bytes[3]{};
    AsyncReader reader;
#if defined(LUDUS_PLATFORM_WEB)
    (void)bytes;
    ReadCompletion completion;
    completion.Tag = 17;
    return reader.Initialize() == AsyncStatus::Unsupported && reader.Poll(completion) == false && completion.Tag == 17
               ? 0
               : 12;
#else
    const uint8 source[]{3, 2, 1};
    const MemoryEntry entry{"bytes", source};
    ProviderHandle provider;
    MountSnapshot mounts;
    VirtualPath path;
    VirtualFile file;
    if (CreateMemoryProvider({&entry, 1}, provider).Succeeded() == false)
    {
        return 12;
    }
    const Mount mount{Root::Assets, {}, 0, 1, provider};
    if (MountSnapshot::Create({&mount, 1}, 1, mounts).Succeeded() == false ||
        path.Set(Root::Assets, "bytes").Succeeded() == false ||
        mounts.OpenRead(path, file).Outcome.Succeeded() == false ||
        reader.Initialize({2, 1, 8, 1, 0}) != AsyncStatus::Ok)
    {
        return 12;
    }
    RequestHandle handle;
    if (reader.Submit(file, 0, bytes, handle) != AsyncStatus::Ok)
    {
        return 12;
    }
    file.Close();
    mounts = {};
    provider = {};
    ReadCompletion completion;
    const uint64 until = AsyncNow() + 5'000'000'000ULL;
    while (reader.Poll(completion) == false && AsyncNow() < until)
    {
    }
    return completion.Handle.Sequence == handle.Sequence && completion.Read.Outcome.Succeeded() &&
                   completion.Read.BytesRead == 3 && bytes[0] == 3 && bytes[2] == 1
               ? 0
               : 12;
#endif
}
