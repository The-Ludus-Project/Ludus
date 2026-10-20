// File digests in bounded batches on an optional job system; see file_digests.h.

#include "file_digests.h"

#include "importers/parallel_tasks.h"
#include "native_files.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::scene_cook::detail
{
namespace
{
namespace threading = foundation::threading;
namespace math = foundation::math;
using foundation::Array;
using importers::MAX_PARALLEL_TASKS;
using importers::ParallelTasks;

/// First batch buffer size (1 MiB of the production bound); the buffer doubles up to
/// the batch bound as files need it, so a small import never touches the whole bound.
constexpr usize INITIAL_BATCH_BUFFER_BYTES = FILE_HASH_BATCH_BYTES / 256;

/// Consecutive bytes of one file in the batch buffer.
struct FileSegment final
{
    /// Index of the file in the request; the job adds the bytes to its hasher only.
    usize File = 0;
    /// Byte offset of the segment in the batch buffer.
    usize Offset = 0;
    /// Segment length in bytes (zero when the batch starts at the file's end).
    usize Size = 0;
    /// True when the file ends with this segment.
    bool IsLast = false;
    /// Written by the job: Hasher::Add accepted the bytes.
    bool IsAdded = false;
};

/// Everything the batch's jobs read; each job writes only its own segment and hasher.
struct HashBatch final
{
    std::span<const uint8> Bytes;
    std::span<FileSegment> Segments;
    std::span<Hasher> Hashers;
};

void HashSegment(void* context, usize task) noexcept
{
    const HashBatch& batch = *static_cast<const HashBatch*>(context);
    FileSegment& segment = batch.Segments[task];
    segment.IsAdded = batch.Hashers[segment.File].Add(batch.Bytes.subspan(segment.Offset, segment.Size));
}

[[nodiscard]] Status
HashFilesSerially(std::span<const std::string_view> paths, std::span<Digest> outDigests, usize& outHashedCount) noexcept
{
    for (usize i = 0; i < paths.size(); ++i)
    {
        const Status status = HashFile(paths[i], outDigests[i]);
        if (status != Status::Ok)
        {
            outHashedCount = i;
            return status;
        }
    }
    outHashedCount = paths.size();
    return Status::Ok;
}

/// Reads files on the owner into a reused batch buffer and hashes each batch in jobs.
class BatchedFileHashing final
{
public:
    BatchedFileHashing(threading::JobSystem& inoutJobs,
                       std::span<const std::string_view> paths,
                       usize batchBytes,
                       std::span<Digest> outDigests) noexcept
        : mJobs(inoutJobs), mPaths(paths), mBatchBytes(batchBytes), mDigests(outDigests)
    {
    }

    /// Allocate the hashers, segment table and first buffer; false leaves hashing to the serial path.
    [[nodiscard]] bool Prepare() noexcept
    {
        return mHashers.TryResize(mPaths.size()) == true && mSegments.TryEnsureCapacity(MAX_PARALLEL_TASKS) == true &&
               mBuffer.TryResize(math::Min(mBatchBytes, INITIAL_BATCH_BUFFER_BYTES)) == true;
    }

    /// Hash every file in order; same contract as HashFiles.
    [[nodiscard]] Status Run(usize& outHashedCount) noexcept
    {
        while (mNextFile < mPaths.size())
        {
            const Status read = readBatch();
            hashBatch();
            // Files finish in request order, so the first failure found here is the serial one.
            for (const FileSegment& segment : mSegments)
            {
                if (segment.IsAdded == false)
                {
                    outHashedCount = segment.File;
                    return Status::Limit;
                }
                if (segment.IsLast == true)
                {
                    mDigests[segment.File] = mHashers[segment.File].Finish();
                }
            }
            if (read != Status::Ok)
            {
                outHashedCount = mNextFile;
                return read;
            }
        }
        outHashedCount = mPaths.size();
        return Status::Ok;
    }

private:
    /// Fill the buffer with the next file bytes in order, up to the batch bound.
    /// @return Ok, or the open/read failure of file mNextFile; the batch then holds only
    ///         the files before it.
    [[nodiscard]] Status readBatch() noexcept
    {
        mSegments.Clear();
        usize used = 0;
        while (mNextFile < mPaths.size() && mSegments.GetSize() < MAX_PARALLEL_TASKS)
        {
            if (used == mBuffer.GetSize() && growBuffer() == false)
            {
                break;
            }
            if (mIsFileOpen == false)
            {
                const Status opened = mFile.Open(mPaths[mNextFile]);
                if (opened != Status::Ok)
                {
                    return opened;
                }
                mIsFileOpen = true;
            }
            // The capacity reserved in Prepare admits every segment of a batch.
            if (mSegments.IsEmpty() == true || mSegments.GetLast().File != mNextFile)
            {
                if (mSegments.TryAdd(FileSegment{ .File = mNextFile, .Offset = used }) == false)
                {
                    break;
                }
            }
            FileSegment& segment = mSegments.GetLast();
            usize count = 0;
            const Status read = mFile.Read(std::span<uint8>(mBuffer.GetData(), mBuffer.GetSize()).subspan(used), count);
            if (read != Status::Ok)
            {
                // The failing file is reported, not hashed.
                mSegments.RemoveLast();
                return read;
            }
            if (count == 0)
            {
                mFile.Close();
                mIsFileOpen = false;
                segment.IsLast = true;
                ++mNextFile;
                continue;
            }
            segment.Size += count;
            used += count;
        }
        return Status::Ok;
    }

    /// Double the buffer up to the batch bound; false when the batch is full.
    [[nodiscard]] bool growBuffer() noexcept
    {
        const usize size = mBuffer.GetSize();
        if (size >= mBatchBytes)
        {
            return false;
        }
        // An allocation failure keeps the current size as the batch bound.
        return mBuffer.TryResize(math::Min(mBatchBytes, size * 2)) == true;
    }

    /// Hash the batch's segments in jobs; any segment the system did not run runs here.
    void hashBatch() noexcept
    {
        HashBatch batch
        {
            .Bytes = std::span<const uint8>(mBuffer.GetData(), mBuffer.GetSize()),
            .Segments = std::span<FileSegment>(mSegments.GetData(), mSegments.GetSize()),
            .Hashers = std::span<Hasher>(mHashers.GetData(), mHashers.GetSize()),
        };
        if (mTasks.Run(&mJobs, mSegments.GetSize(), HashSegment, &batch, "Hash source file") == false)
        {
            // Without task storage nothing ran; hash the batch in order on this thread.
            for (usize i = 0; i < mSegments.GetSize(); ++i)
            {
                HashSegment(&batch, i);
            }
        }
    }

    threading::JobSystem& mJobs;
    std::span<const std::string_view> mPaths;
    usize mBatchBytes = 0;
    std::span<Digest> mDigests;
    /// One hasher per file; a file spanning batches keeps adding to the same hasher.
    Array<Hasher> mHashers;
    Array<FileSegment> mSegments;
    Array<uint8> mBuffer;
    ParallelTasks mTasks;
    SequentialFile mFile;
    bool mIsFileOpen = false;
    /// File being read (the first file not yet read to its end).
    usize mNextFile = 0;
};
} // namespace

Status HashFiles(threading::JobSystem* jobsOrNull,
                 std::span<const std::string_view> paths,
                 usize batchBytes,
                 std::span<Digest> outDigests,
                 usize& outHashedCount) noexcept
{
    LUDUS_ASSERT(outDigests.size() == paths.size());
    LUDUS_ASSERT(batchBytes > 0);
    if (jobsOrNull == nullptr || jobsOrNull->GetWorkerCount() == 0 || paths.size() <= 1)
    {
        return HashFilesSerially(paths, outDigests, outHashedCount);
    }
    BatchedFileHashing hashing(*jobsOrNull, paths, batchBytes, outDigests);
    if (hashing.Prepare() == false)
    {
        return HashFilesSerially(paths, outDigests, outHashedCount);
    }
    return hashing.Run(outHashedCount);
}
} // namespace ludus::content::scene_cook::detail
