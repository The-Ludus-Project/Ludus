// Persistent generation cache store contracts: verified round trips across store
// instances, completion records written last, and corrupt, truncated, missing or
// unreferenced files reported and rebuilt rather than trusted. See
// docs/architecture/procedural-content-generation.md#incremental-cooking-and-cache-keys.

#include <ludus/content/procedural/storage/file_generation_cache.h>

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace
{
using namespace ludus::content::procedural;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;

constexpr usize ENTRY_BUDGET = 1U << 16U;
constexpr uint32 SCHEMA = 3;
constexpr uint8 PAYLOAD[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};

[[nodiscard]] std::string GetName(const FileGenerationCacheName& name)
{
    return std::string(name.GetView());
}

struct TemporaryDirectory final
{
    char Root[64] = "/tmp/ludus-generation-cache-XXXXXX";
    TemporaryDirectory()
    {
        REQUIRE(::mkdtemp(Root) != nullptr);
    }
    // Best-effort cleanup: only std::filesystem::path allocation can throw.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(Root, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] std::filesystem::path GetPath(std::string_view name) const
    {
        return std::filesystem::path(Root) / name;
    }
    /// The one object file of a key.
    [[nodiscard]] std::filesystem::path FindObject(const GenerationDigest& key) const
    {
        const std::string prefix =
            GetName(GetFileGenerationCacheRecordName(key)).substr(0, 2 * GENERATION_DIGEST_BYTES);
        std::filesystem::path found;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(Root))
        {
            const std::string name = entry.path().filename().string();
            if (name.starts_with(prefix + "-") == true && name.ends_with(".object") == true)
            {
                REQUIRE(found.empty() == true);
                found = entry.path();
            }
        }
        REQUIRE(found.empty() == false);
        return found;
    }
};

[[nodiscard]] GenerationDigest MakeKey(uint32 value)
{
    GenerationByteWriter writer;
    writer.WriteUInt32(value);
    return writer.ComputeDigest();
}

void FlipLastByte(const std::filesystem::path& path)
{
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.good() == true);
    file.seekg(-1, std::ios::end);
    char last = 0;
    file.get(last);
    file.seekp(-1, std::ios::end);
    file.put(static_cast<char>(last ^ 1));
    file.close();
    REQUIRE(file.good() == true);
}

void Truncate(const std::filesystem::path& path)
{
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
}

void RequireRead(FileGenerationCacheStore& inoutStore, const GenerationDigest& key, GenerationStoreRead expected)
{
    Array<uint8> bytes;
    REQUIRE(inoutStore.TryRead(key, bytes) == expected);
    if (expected == GenerationStoreRead::Found)
    {
        REQUIRE(bytes.GetSize() == sizeof(PAYLOAD));
        for (usize i = 0; i < bytes.GetSize(); ++i)
        {
            REQUIRE(bytes[i] == PAYLOAD[i]);
        }
    }
}
} // namespace

TEST_CASE("a persistent store reopens to the verified entries it wrote", "[procedural][storage]")
{
    TemporaryDirectory directory;
    const GenerationDigest key = MakeKey(1);
    {
        FileGenerationCacheStore store;
        REQUIRE(store.IsOpen() == false);
        RequireRead(store, key, GenerationStoreRead::Failed);
        REQUIRE(store.TryWrite(key, BorrowedView<const uint8>(PAYLOAD)) == false);
        REQUIRE(store.TryOpen(directory.Root, ENTRY_BUDGET) == true);
        RequireRead(store, key, GenerationStoreRead::Absent);
        REQUIRE(store.TryWrite(key, BorrowedView<const uint8>(PAYLOAD)) == true);
        RequireRead(store, key, GenerationStoreRead::Found);
        // Rewriting the same bytes is idempotent.
        REQUIRE(store.TryWrite(key, BorrowedView<const uint8>(PAYLOAD)) == true);
    }
    FileGenerationCacheStore reopened;
    REQUIRE(reopened.TryOpen(directory.Root, ENTRY_BUDGET) == true);
    RequireRead(reopened, key, GenerationStoreRead::Found);
    RequireRead(reopened, MakeKey(2), GenerationStoreRead::Absent);

    reopened.Remove(key);
    RequireRead(reopened, key, GenerationStoreRead::Absent);
    REQUIRE(reopened.TryWrite(key, BorrowedView<const uint8>(PAYLOAD)) == true);
    RequireRead(reopened, key, GenerationStoreRead::Found);
}

TEST_CASE("a persistent store refuses invalid roots and entries over budget", "[procedural][storage]")
{
    TemporaryDirectory directory;
    FileGenerationCacheStore store;
    REQUIRE(store.TryOpen("", ENTRY_BUDGET) == false);
    REQUIRE(store.TryOpen(directory.Root, MAX_FILE_GENERATION_CACHE_ENTRY_BYTES + 1) == false);
    REQUIRE(store.IsOpen() == false);
    REQUIRE(store.TryOpen(directory.Root, sizeof(PAYLOAD) - 1) == true);
    REQUIRE(store.TryWrite(MakeKey(1), BorrowedView<const uint8>(PAYLOAD)) == false);
    RequireRead(store, MakeKey(1), GenerationStoreRead::Absent);
}

TEST_CASE("a persistent store reports every damaged entry as corrupt, then absent", "[procedural][storage]")
{
    TemporaryDirectory directory;
    FileGenerationCacheStore store;
    REQUIRE(store.TryOpen(directory.Root, ENTRY_BUDGET) == true);
    const GenerationDigest key = MakeKey(7);
    REQUIRE(store.TryWrite(key, BorrowedView<const uint8>(PAYLOAD)) == true);
    const std::filesystem::path record = directory.GetPath(GetName(GetFileGenerationCacheRecordName(key)));

    SECTION("a flipped object byte fails the object digest")
    {
        FlipLastByte(directory.FindObject(key));
    }
    SECTION("a truncated object fails the recorded length")
    {
        Truncate(directory.FindObject(key));
    }
    SECTION("a missing object")
    {
        REQUIRE(std::filesystem::remove(directory.FindObject(key)) == true);
    }
    SECTION("a flipped record byte fails the record digest")
    {
        FlipLastByte(record);
    }
    SECTION("a truncated record")
    {
        Truncate(record);
    }
    SECTION("a record of another key")
    {
        const GenerationDigest other = MakeKey(8);
        REQUIRE(store.TryWrite(other, BorrowedView<const uint8>(PAYLOAD)) == true);
        std::filesystem::copy_file(directory.GetPath(GetName(GetFileGenerationCacheRecordName(other))),
                                   record,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    RequireRead(store, key, GenerationStoreRead::Corrupt);
    RequireRead(store, key, GenerationStoreRead::Absent);
    // The entry is rebuilt and served again.
    REQUIRE(store.TryWrite(key, BorrowedView<const uint8>(PAYLOAD)) == true);
    RequireRead(store, key, GenerationStoreRead::Found);
}

TEST_CASE("an object without a completion record is never served", "[procedural][storage]")
{
    TemporaryDirectory directory;
    FileGenerationCacheStore store;
    REQUIRE(store.TryOpen(directory.Root, ENTRY_BUDGET) == true);
    const GenerationDigest key = MakeKey(3);
    REQUIRE(store.TryWrite(key, BorrowedView<const uint8>(PAYLOAD)) == true);
    // An interrupted write: the object was staged, but its completion record never published.
    REQUIRE(std::filesystem::remove(directory.GetPath(GetName(GetFileGenerationCacheRecordName(key)))) == true);
    RequireRead(store, key, GenerationStoreRead::Absent);
    REQUIRE(store.TryWrite(key, BorrowedView<const uint8>(PAYLOAD)) == true);
    RequireRead(store, key, GenerationStoreRead::Found);
}

TEST_CASE("the validated product cache rebuilds through a persistent store", "[procedural][storage]")
{
    TemporaryDirectory directory;
    FileGenerationCacheStore store;
    REQUIRE(store.TryOpen(directory.Root, ENTRY_BUDGET) == true);
    const GenerationDigest key = MakeKey(5);
    REQUIRE(TryStoreGenerationProduct(store, key, SCHEMA, BorrowedView<const uint8>(PAYLOAD)) == true);
    Array<uint8> payload;
    REQUIRE(TryFindGenerationProduct(store, key, SCHEMA, payload) == GenerationCacheOutcome::Hit);
    REQUIRE(payload.GetSize() == sizeof(PAYLOAD));

    FlipLastByte(directory.FindObject(key));
    REQUIRE(TryFindGenerationProduct(store, key, SCHEMA, payload) == GenerationCacheOutcome::Corrupt);
    REQUIRE(TryFindGenerationProduct(store, key, SCHEMA, payload) == GenerationCacheOutcome::Absent);
}
