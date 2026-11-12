#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/span_compatibility.hpp>
#include <ludus/foundation/filesystem/pack.hpp>

#include "internal/pack_codec.hpp"
#include "pack_fixture.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;
namespace
{
ProviderHandle Storage(std::span<const uint8> bytes)
{
    const MemoryEntry entry{"archive", bytes};
    ProviderHandle output;
    REQUIRE(CreateMemoryProvider({&entry, 1}, output).Succeeded());
    return output;
}
ProviderHandle Pack(std::span<const uint8> bytes = pack_fixture::kPack)
{
    auto storage = Storage(bytes);
    ProviderHandle pack;
    REQUIRE(CreatePackProvider(storage, "archive", {}, pack).Succeeded());
    return pack;
}
template <typename T>
void Set(Array<uint8>& bytes, usize at, T value)
{
    REQUIRE(TryWriteLittleEndian(value, MakeSpan(bytes).subspan(at)));
}
uint64 Get64(std::span<const uint8> bytes, usize at)
{
    uint64 value = 0;
    REQUIRE(TryReadLittleEndian(bytes.subspan(at), value));
    return value;
}
uint32 Get32(std::span<const uint8> bytes, usize at)
{
    uint32 value = 0;
    REQUIRE(TryReadLittleEndian(bytes.subspan(at), value));
    return value;
}
void Seal(Array<uint8>& bytes)
{
    const auto length = static_cast<usize>(Get64(bytes, 40));
    Set(bytes, 64, internal::Crc32(MakeSpan(bytes).subspan(80, length)));
    Set(bytes, 68, uint32{0});
    Set(bytes, 68, internal::Crc32(MakeSpan(bytes).first(80)));
}
usize BlockTable(std::span<const uint8> bytes)
{
    usize position = 80;
    for (usize i = 0; i < Get32(bytes, 24); ++i)
    {
        position += 32 + Get32(bytes, position);
    }
    return position;
}
void Rejected(std::span<const uint8> bytes, Status expected = Status::CorruptData)
{
    auto existing = Pack();
    const auto* before = existing.Get();
    const auto storage = Storage(bytes);
    REQUIRE(CreatePackProvider(storage, "archive", {}, existing).Code == expected);
    REQUIRE(existing.Get() == before);
}
} // namespace

TEST_CASE("Packs built by the Python tool resolve raw compressed and empty revisions across block boundaries")
{
    for (const auto bytes :
         {std::span<const uint8>(pack_fixture::kPack), std::span<const uint8>(pack_fixture::kReorderedPack)})
    {
        auto pack = Pack(bytes);
        const Mount mount{Root::Assets, "pack", 0, 1, pack};
        MountSnapshot snapshot;
        REQUIRE(MountSnapshot::Create({&mount, 1}, 7, snapshot).Succeeded());
        VirtualPath key;
        REQUIRE(key.Set(Root::Assets, "pack/compressed").Succeeded());
        VirtualFile file, clone;
        REQUIRE(snapshot.OpenRead(key, file).Outcome.Succeeded());
        REQUIRE(file.Size() == 70013);
        REQUIRE(file.Clone(clone).Succeeded());
        REQUIRE(MountSnapshot::Create({}, 8, snapshot).Succeeded());
        pack = {};
        uint8 read[32]{};
        auto result = clone.ReadAt(65530, read);
        REQUIRE(result.Outcome.Succeeded());
        REQUIRE(result.BytesRead == 32);
        REQUIRE(std::all_of(std::begin(read), std::end(read), [](uint8 value) { return value == 'A'; }));
        result = clone.ReadAt(69996, read);
        REQUIRE(result.BytesRead == 17);
        REQUIRE(read[3] == 'A');
        REQUIRE(read[4] == 't');
        REQUIRE(read[16] == 'l');
        REQUIRE(clone.ReadAt(clone.Size(), read).BytesRead == 0);
        REQUIRE(clone.ReadAt(clone.Size(), read).Outcome.Succeeded());
        REQUIRE(clone.ReadAt(clone.Size() + 1, read).Outcome.Code == Status::InvalidArgument);
        REQUIRE(clone.Generation() == 7);
    }
    auto pack = Pack();
    ProviderFile* raw = nullptr;
    REQUIRE(pack.Get()->OpenRead("raw", raw).Succeeded());
    uint8 read[251]{};
    REQUIRE(raw->ReadAt(0, read).BytesRead == 251);
    for (usize i = 0; i < 251; ++i)
    {
        REQUIRE(read[i] == i);
    }
    delete raw;
    ProviderFile* empty = nullptr;
    REQUIRE(pack.Get()->OpenRead("empty", empty).Succeeded());
    REQUIRE(empty->Size() == 0);
    REQUIRE(empty->ReadAt(0, read).Outcome.Succeeded());
    REQUIRE(empty->ReadAt(0, read).BytesRead == 0);
    auto* previous = empty;
    REQUIRE(pack.Get()->OpenRead("missing", empty).Code == Status::NotFound);
    REQUIRE(pack.Get()->OpenRead("../raw", empty).Code == Status::InvalidArgument);
    REQUIRE(empty == previous);
    delete empty;
    pack = Pack(pack_fixture::kEmptyPack);
    empty = nullptr;
    REQUIRE(pack.Get()->OpenRead("raw", empty).Code == Status::NotFound);
}

TEST_CASE("Pack admission rejects malformed headers features indexes lengths and ownership before publication")
{
    const Array<uint8> good = Array<uint8>::FromRange(BorrowedView<const uint8>(pack_fixture::kPack));
    for (usize end = 0; end < good.GetSize(); end += 19)
    {
        Rejected(MakeSpan(good).first(end));
    }
    auto corrupt = good;
    corrupt[0] ^= 1;
    Rejected(corrupt);
    corrupt = good;
    corrupt[90] ^= 1;
    Rejected(corrupt);
    for (const usize at : {usize{8}, usize{12}, usize{16}})
    {
        corrupt = good;
        Set(corrupt, at, uint32{99});
        Seal(corrupt);
        Rejected(corrupt, Status::Unsupported);
    }
    for (const usize at : {usize{20}, usize{24}, usize{28}, usize{72}})
    {
        corrupt = good;
        Set(corrupt, at, uint32{0xffffffffU});
        Seal(corrupt);
        Rejected(corrupt, at == 24 || at == 28 ? Status::LimitExceeded : Status::CorruptData);
    }
    corrupt = good;
    Set(corrupt, 80, uint32{1025});
    Seal(corrupt);
    Rejected(corrupt);
    corrupt = good;
    Set(corrupt, 84, uint32{1}); // First entry cannot skip a block.
    Seal(corrupt);
    Rejected(corrupt);
    corrupt = good;
    Set(corrupt, 88, uint32{0xffffffffU});
    Seal(corrupt);
    Rejected(corrupt);
    corrupt = good;
    Set(corrupt, 92, uint32{1});
    Seal(corrupt);
    Rejected(corrupt);
    corrupt = good;
    Set(corrupt, 96, ~uint64{0});
    Seal(corrupt);
    Rejected(corrupt);
    corrupt = good;
    Set(corrupt, 104, uint64{1});
    Seal(corrupt);
    Rejected(corrupt);
    for (const uint8 value : {uint8{0}, uint8{'/'}, uint8{0xff}, uint8{'z'}})
    {
        corrupt = good;
        corrupt[112] = value; // Invalid UTF-8/path, or loss of canonical sorted order.
        Seal(corrupt);
        Rejected(corrupt);
    }
    const usize table = BlockTable(good);
    for (const usize at : {table, table + 24})
    {
        corrupt = good;
        Set(corrupt, at, ~uint64{0});
        Seal(corrupt);
        Rejected(corrupt);
    }
    corrupt = good;
    Set(corrupt, table + 24, Get64(good, table)); // Overlapping payload ranges.
    Seal(corrupt);
    Rejected(corrupt);
    for (const usize at : {table + 8, table + 12})
    {
        corrupt = good;
        Set(corrupt, at, uint32{65537});
        Seal(corrupt);
        Rejected(corrupt);
    }
    corrupt = good;
    Set(corrupt, table + 16, uint32{2});
    Seal(corrupt);
    Rejected(corrupt, Status::Unsupported);
    corrupt = good;
    Set(corrupt, table + 16, uint32{0}); // Raw codec must have equal lengths.
    Seal(corrupt);
    Rejected(corrupt);
    corrupt = good;
    corrupt.Add(uint8{0});
    Set(corrupt, 56, static_cast<uint64>(corrupt.GetSize()));
    Seal(corrupt);
    Rejected(corrupt); // No unowned/trailing payload bytes.
}

TEST_CASE("Configured pack budgets reject storage index files blocks and decoded lengths transactionally")
{
    const auto storage = Storage(pack_fixture::kPack);
    auto pack = Pack();
    const auto* previous = pack.Get();
    for (int budget = 0; budget < 7; ++budget)
    {
        PackLimits limits;
        switch (budget)
        {
            case 0:
                limits.MaxPackBytes = 79;
                break;
            case 1:
                limits.MaxIndexBytes = 1;
                break;
            case 2:
                limits.MaxFileBytes = 1;
                break;
            case 3:
                limits.MaxDecodedBytes = 70000;
                break;
            case 4:
                limits.MaxFiles = 1;
                break;
            case 5:
                limits.MaxBlocks = 1;
                break;
            case 6:
                limits.MaxBlockBytes = 256;
                break;
            default:
                break;
        }
        REQUIRE(CreatePackProvider(storage, "archive", limits, pack).Code == Status::LimitExceeded);
        REQUIRE(pack.Get() == previous);
    }
    PackLimits invalid;
    invalid.MaxBlockBytes = 0;
    REQUIRE(CreatePackProvider(storage, "archive", invalid, pack).Code == Status::InvalidArgument);
    REQUIRE(CreatePackProvider({}, "archive", {}, pack).Code == Status::InvalidArgument);
    REQUIRE(CreatePackProvider(storage, "../archive", {}, pack).Code == Status::InvalidArgument);
    REQUIRE(CreatePackProvider(storage, "missing", {}, pack).Code == Status::NotFound);
    REQUIRE(pack.Get() == previous);
}

TEST_CASE("Payload corruption is visible with only previously verified progress and never falls back")
{
    const Array<uint8> good = Array<uint8>::FromRange(BorrowedView<const uint8>(pack_fixture::kPack));
    const usize table = BlockTable(good);
    auto corrupt = good;
    corrupt[static_cast<usize>(Get64(good, table + 24))] ^= 1; // Second compressed block.
    auto pack = Pack(corrupt); // Mount only checks metadata, avoiding an eager full payload scan.
    ProviderFile* file = nullptr;
    REQUIRE(pack.Get()->OpenRead("compressed", file).Succeeded());
    Array<uint8> bytes(70013, 99);
    const auto result = file->ReadAt(0, bytes);
    REQUIRE(result.Outcome.Code == Status::CorruptData);
    REQUIRE(result.BytesRead == 65536);
    REQUIRE(bytes[65535] == 'A');
    REQUIRE(bytes[65536] == 99);
    REQUIRE(file->ReadAt(65536, bytes).BytesRead == 0);
    delete file;
    corrupt = good;
    const usize rawRecord = table + 48;
    corrupt[static_cast<usize>(Get64(good, rawRecord))] ^= 1;
    pack = Pack(corrupt);
    REQUIRE(pack.Get()->OpenRead("raw", file).Succeeded());
    REQUIRE(file->ReadAt(0, bytes).Outcome.Code == Status::CorruptData);
    REQUIRE(file->ReadAt(0, bytes).BytesRead == 0);
    delete file;

    corrupt = good;
    Set(corrupt, table, ~uint64{0});
    Seal(corrupt);
    const auto storage = Storage(corrupt);
    REQUIRE(CreatePackProvider(storage, "archive", {}, pack).Code == Status::CorruptData);
    // The previous provider is preserved. Corruption cannot be disguised as a missing key.
    const uint8 replacement[]{7};
    const MemoryEntry fallbackEntry{"raw", replacement};
    ProviderHandle fallback;
    REQUIRE(CreateMemoryProvider({&fallbackEntry, 1}, fallback).Succeeded());
    const Mount mounts[]{{Root::Assets, {}, 1, 1, pack}, {Root::Assets, {}, 0, 2, fallback}};
    MountSnapshot snapshot;
    REQUIRE(MountSnapshot::Create(mounts, 1, snapshot).Succeeded());
    VirtualPath path;
    REQUIRE(path.Set(Root::Assets, "raw").Succeeded());
    VirtualFile opened;
    REQUIRE(snapshot.OpenRead(path, opened).Outcome.Succeeded());
    REQUIRE(opened.MountId() == 1);
    REQUIRE(opened.ReadAt(0, bytes).Outcome.Code == Status::CorruptData);
}

TEST_CASE("Independent CRC and LZ4 golden vectors enforce bounded lengths offsets and termination")
{
    const uint8 text[]{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    REQUIRE(internal::Crc32(text) == 0xcbf43926U);
    const uint8 repeated[]{0x17, 'A', 1, 0, 0x50, 't', 'a', 'i', 'l', '!'};
    uint8 output[17]{};
    REQUIRE(internal::DecodeLz4(repeated, output));
    REQUIRE(output[11] == 'A');
    REQUIRE(output[12] == 't');
    REQUIRE(output[16] == '!');
    for (usize count = 0; count < sizeof(repeated); ++count)
    {
        REQUIRE_FALSE(internal::DecodeLz4(std::span(repeated).first(count), output));
    }
    Array<uint8> damaged = Array<uint8>::FromRange(BorrowedView<const uint8>(repeated));
    damaged[2] = 0;
    REQUIRE_FALSE(internal::DecodeLz4(damaged, output));
    damaged[2] = 2;
    REQUIRE_FALSE(internal::DecodeLz4(damaged, output));
    damaged[2] = 1;
    REQUIRE_FALSE(internal::DecodeLz4(damaged, std::span(output).first(16)));
    const uint8 overflow[]{0xf0, 255, 255, 255};
    REQUIRE_FALSE(internal::DecodeLz4(overflow, output));
}

TEST_CASE("Minimum-size independent blocks decode varied overlapping matches across short reads")
{
    const auto pack = Pack(pack_fixture::kSmallBlockPack);
    ProviderFile* file = nullptr;
    REQUIRE(pack.Get()->OpenRead("pattern", file).Succeeded());
    REQUIRE(file->Size() == 1600);
    constexpr std::string_view PATTERN = "abcabcde";
    for (uint64 offset = 0; offset < file->Size(); offset += 237)
    {
        uint8 read[300]{};
        const auto result = file->ReadAt(offset, read);
        REQUIRE(result.Outcome.Succeeded());
        REQUIRE(result.BytesRead == std::min(uint64{300}, file->Size() - offset));
        for (usize i = 0; i < result.BytesRead; ++i)
        {
            REQUIRE(read[i] == static_cast<uint8>(PATTERN[(offset + i) % PATTERN.size()]));
        }
    }
    delete file;
}
