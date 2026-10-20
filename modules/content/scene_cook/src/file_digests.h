#pragma once

// SHA-256 of a list of files, optionally hashed in parallel on a caller-owned
// FoundationThreading JobSystem (docs/architecture/threading.md: the caller budgets
// workers; there is no hidden pool).
//
// With a job system the calling thread reads the files in order into a bounded batch
// buffer, and jobs hash the batch: one job per file segment, each adding its bytes to
// that file's own hasher. A file larger than the buffer continues in the next batch
// with the same hasher, so memory stays bounded for any file size. Callbacks never do
// I/O. The owner takes the results in file order after each Wait, so digests and the
// reported failure are those of the serial path for every worker count.

#include <ludus/foundation/base/core.h>

#include <ludus/content/content.h>
#include <ludus/foundation/threading/jobs.hpp>

#include <span>
#include <string_view>

namespace ludus::content::scene_cook::detail
{
/// Most file bytes held in memory at once while hashing on a job system. Large enough
/// to give several workers a texture-sized file each (Main Sponza's textures reach
/// 43 MB), and in line with the texture decode batch (eight decoded images).
inline constexpr usize FILE_HASH_BATCH_BYTES = usize{256} * 1024 * 1024;

/// SHA-256 of each file in @p paths, as HashFile computes it, stopping at the first
/// file that cannot be hashed.
///
/// Without a job system, or on one with no workers, each file is streamed through
/// HashFile on the calling thread. Otherwise the calling thread reads batches of at
/// most @p batchBytes bytes and helps the system hash them; when the batch storage
/// cannot be allocated the files are hashed serially instead.
/// @param jobsOrNull Caller-owned system with no graph in flight, used from this thread
///        only (a busy or refusing system hashes on the calling thread); not owned.
/// @param paths Native file paths, hashed in order.
/// @param batchBytes Batch bound in bytes (FILE_HASH_BATCH_BYTES outside tests); nonzero.
/// @param outDigests One digest per path; entries before outHashedCount are set.
/// @param outHashedCount Files hashed before the first failure (paths.size() on Ok).
/// @return Ok, or the failure of file outHashedCount: NotFound, IoError, Invalid, Limit
///         or Unsupported, exactly as HashFile reports it for that file.
[[nodiscard]] Status HashFiles(foundation::threading::JobSystem* jobsOrNull,
                               std::span<const std::string_view> paths,
                               usize batchBytes,
                               std::span<Digest> outDigests,
                               usize& outHashedCount) noexcept;
} // namespace ludus::content::scene_cook::detail
