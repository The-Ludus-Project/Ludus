// Serial-oracle tests for HashFiles: file digests hashed in bounded batches on a
// caller-owned JobSystem must equal HashFile's for every worker count and batch
// bound, and must stop at the same failing file.

#include "import_test_support.h"

#include <ludus/content/content.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/threading/jobs.hpp>

#include "file_digests.h"
#include "importers/parallel_tasks.h"
#include "native_files.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace
{
using namespace ludus::scene_import_tests;
namespace threading = ludus::foundation::threading;
namespace detail = ludus::content::scene_cook::detail;
using ludus::content::Digest;
using ludus::content::Status;
using ludus::foundation::Array;
using ludus::foundation::core::uint32;
using ludus::foundation::core::usize;

/// Worker budgets compared with the serial oracle: the zero-worker executor (serial),
/// one worker, and several.
constexpr usize WORKER_BUDGET_COUNT = 3;
const ludus::foundation::StaticArray<uint32, WORKER_BUDGET_COUNT> WORKER_BUDGETS{0, 1, 3};

constexpr usize MEBIBYTE = usize{1024} * 1024;
/// Batch bounds: smaller than most files (every file spans batches), one that grows the
/// buffer past its first allocation and splits the largest file, and the production bound.
constexpr usize BATCH_BOUND_COUNT = 3;
const ludus::foundation::StaticArray<usize, BATCH_BOUND_COUNT> BATCH_BOUNDS{
    4099,
    3 * MEBIBYTE + 1,
    detail::FILE_HASH_BATCH_BYTES,
};

/// Overwrite content with deterministic bytes that differ between seeds and offsets.
void FillContent(uint32 seed, std::string& outContent)
{
    uint32 state = seed * 2654435761U + 1;
    for (char& value : outContent)
    {
        state = state * 1664525U + 1013904223U;
        value = static_cast<char>(state >> 24);
    }
}

/// Files of the given sizes in scratch, named in order.
Array<std::string> WriteFiles(const ScratchDirectory& scratch, const Array<usize>& sizes)
{
    Array<std::string> paths;
    for (usize i = 0; i < sizes.GetSize(); ++i)
    {
        const fs::path path = scratch.Path / ("input-" + std::to_string(i) + ".bin");
        std::string content(sizes[i], '\0');
        FillContent(static_cast<uint32>(i), content);
        WriteFile(path, content);
        paths.Add(path.string());
    }
    return paths;
}

/// A copy of paths with path inserted at index.
Array<std::string> InsertPath(usize index, const std::string& path, const Array<std::string>& paths)
{
    Array<std::string> result;
    for (const std::string& existing : paths)
    {
        result.Add(existing);
    }
    result.InsertAt(index, path);
    return result;
}

struct HashOutcome final
{
    Status Result = Status::Ok;
    usize HashedCount = 0;
    Array<Digest> Digests;
};

/// The oracle: HashFile on each path in turn, stopping at the first failure.
HashOutcome HashOneByOne(const Array<std::string>& paths)
{
    HashOutcome outcome;
    outcome.Digests.Resize(paths.GetSize());
    for (const std::string& path : paths)
    {
        outcome.Result = detail::HashFile(path, outcome.Digests[outcome.HashedCount]);
        if (outcome.Result != Status::Ok)
        {
            break;
        }
        ++outcome.HashedCount;
    }
    return outcome;
}

HashOutcome HashAll(threading::JobSystem* jobsOrNull, const Array<std::string>& paths, usize batchBytes)
{
    Array<std::string_view> views;
    for (const std::string& path : paths)
    {
        views.Add(path);
    }
    HashOutcome outcome;
    outcome.Digests.Resize(paths.GetSize());
    outcome.Result = detail::HashFiles(jobsOrNull,
                                       {views.GetData(), views.GetSize()},
                                       batchBytes,
                                       {outcome.Digests.GetData(), outcome.Digests.GetSize()},
                                       outcome.HashedCount);
    return outcome;
}

void RequireSameOutcome(const HashOutcome& expected, const HashOutcome& actual)
{
    CHECK(actual.Result == expected.Result);
    REQUIRE(actual.HashedCount == expected.HashedCount);
    for (usize i = 0; i < expected.HashedCount; ++i)
    {
        INFO("file " << i);
        CHECK(actual.Digests[i] == expected.Digests[i]);
    }
}

/// HashFiles without a system, and on each worker budget and batch bound, against the oracle.
void RequireSerialOutcome(const Array<std::string>& paths, const HashOutcome& expected)
{
    RequireSameOutcome(expected, HashAll(nullptr, paths, detail::FILE_HASH_BATCH_BYTES));
    for (const uint32 workers : WORKER_BUDGETS)
    {
        threading::JobSystem system;
        REQUIRE(system.Initialize({ .WorkerCount = workers }) == threading::JobStatus::Ok);
        for (const usize batchBytes : BATCH_BOUNDS)
        {
            INFO("workers " << workers << ", batch " << batchBytes);
            RequireSameOutcome(expected, HashAll(&system, paths, batchBytes));
        }
    }
}
} // namespace

TEST_CASE("File digests match HashFile for every worker count and batch bound", "[scene_cook][jobs]")
{
    ScratchDirectory scratch;
    // Empty files, files that end exactly at a batch bound, and one larger than the
    // 3 MiB bound, so files start, continue and end inside batches.
    const Array<usize> sizes{
        0,
        1,
        4099,
        5 * MEBIBYTE + 3,
        0,
        65536,
        MEBIBYTE + 17,
        3 * MEBIBYTE + 1,
        2 * MEBIBYTE,
        12345,
        0,
    };
    const Array<std::string> paths = WriteFiles(scratch, sizes);
    const HashOutcome expected = HashOneByOne(paths);
    REQUIRE(expected.Result == Status::Ok);
    REQUIRE(expected.HashedCount == paths.GetSize());
    CHECK(expected.Digests[0] == ludus::content::Hash({}));
    RequireSerialOutcome(paths, expected);
}

TEST_CASE("File digests cover batches with more files than one graph admits", "[scene_cook][jobs]")
{
    ScratchDirectory scratch;
    Array<usize> sizes;
    sizes.Resize(usize{ludus::content::scene_cook::importers::MAX_PARALLEL_TASKS} * 2 + 3, 7);
    const Array<std::string> paths = WriteFiles(scratch, sizes);
    const HashOutcome expected = HashOneByOne(paths);
    REQUIRE(expected.Result == Status::Ok);
    RequireSerialOutcome(paths, expected);
}

TEST_CASE("File digests stop at the first file the serial path cannot hash", "[scene_cook][jobs]")
{
    ScratchDirectory scratch;
    const Array<usize> sizes{3000, 5 * MEBIBYTE, 10, 2 * MEBIBYTE, 77, 4 * MEBIBYTE};
    const Array<std::string> paths = WriteFiles(scratch, sizes);
    const std::string missingPath = (scratch.Path / "missing.bin").string();

    // A missing file after a file that spans batches: earlier digests are kept.
    const Array<std::string> missing = InsertPath(2, missingPath, paths);
    const HashOutcome notFound = HashOneByOne(missing);
    REQUIRE(notFound.Result == Status::NotFound);
    REQUIRE(notFound.HashedCount == 2);
    RequireSerialOutcome(missing, notFound);

    // A directory opens but cannot be read: the read failure is that file's.
    const Array<std::string> unreadable = InsertPath(4, scratch.Path.string(), paths);
    const HashOutcome readFailure = HashOneByOne(unreadable);
    REQUIRE(readFailure.Result == Status::IoError);
    REQUIRE(readFailure.HashedCount == 4);
    RequireSerialOutcome(unreadable, readFailure);

    // A failing first file hashes nothing.
    const Array<std::string> first = InsertPath(0, missingPath, paths);
    const HashOutcome firstFailure = HashOneByOne(first);
    REQUIRE(firstFailure.HashedCount == 0);
    RequireSerialOutcome(first, firstFailure);
}
