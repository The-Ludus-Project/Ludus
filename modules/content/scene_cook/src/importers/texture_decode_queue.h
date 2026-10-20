#pragma once

// Texture image decoding for the best-effort cook, optionally in parallel on a
// caller-owned FoundationThreading JobSystem (docs/architecture/threading.md: the
// caller budgets workers; there is no hidden pool).
//
// The cook plans its decodes in the order it will consume them. With a job system
// the queue reads a bounded batch of image files on the calling thread, decodes the
// batch in jobs that each write only their own slot, and hands results back in the
// planned order. Decoding is a pure function of the texture, so every worker count
// (and the inline path without a job system) yields the same images, messages and
// digests; a request that leaves the plan's order is decoded inline instead.

#include "image/image_reader.h"
#include "scene_ir.h"

#include <ludus/foundation/base/core.h>

#include <ludus/content/content.h>
#include <ludus/content/scene_cook/scene_cook.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/threading/jobs.hpp>

#include <span>
#include <string_view>

namespace ludus::content::scene_cook::importers
{
/// Most images decoded together. Bounds the decoded images held beyond the serial
/// cook (a 4096 x 4096 RGBA8 image is 64 MiB, plus the decoder's working buffers).
inline constexpr uint32 MAX_TEXTURE_DECODE_BATCH = 8;

/// Everything one read and decode of a texture's image produced.
struct TextureDecode final
{
    /// Reading the encoded bytes: Ok, or the file read failure (OutOfMemory, NotFound, IoError, Limit).
    Status Read = Status::Ok;
    /// True when the bytes came from a file below the import root rather than an embedded image.
    bool IsFileRead = false;
    /// SHA-256 of the file's bytes; set when IsFileRead and the request asked for a digest.
    Digest ReadDigest;
    /// Decoder outcome; Invalid when Read is not Ok.
    ImageResult Decoded = ImageResult::Invalid;
    /// Static description of a read or decode failure.
    std::string_view Message;
    /// The decoder's output, valid when Decoded is Ok.
    DecodedImage Image;
};

/// One planned decode: a texture and whether its file digest is wanted.
struct TextureDecodeRequest final
{
    uint32 Texture = IR_NONE;
    bool ShouldHash = false;
};

/// Decodes the cook's texture images in planned order, in batches on an optional job system.
///
/// Used on one thread. The scene, root, job system and cancellation hook are borrowed
/// and must outlive the queue. Job callbacks touch only their batch slot and the
/// immutable scene; they do no I/O.
class TextureDecodeQueue final
{
public:
    /// @param jobsOrNull Caller-owned system, not shared with another in-flight graph;
    ///        nullptr decodes every image inline on the calling thread when it is taken.
    TextureDecodeQueue(const SceneIr& scene,
                       std::string_view root,
                       foundation::threading::JobSystem* jobsOrNull,
                       const Cancellation* cancelOrNull) noexcept;
    ~TextureDecodeQueue() noexcept = default;
    TextureDecodeQueue(const TextureDecodeQueue&) = delete;
    TextureDecodeQueue& operator=(const TextureDecodeQueue&) = delete;
    TextureDecodeQueue(TextureDecodeQueue&&) = delete;
    TextureDecodeQueue& operator=(TextureDecodeQueue&&) = delete;

    /// Append a decode the cook expects to take next. Ignored without a job system.
    /// @return false when the plan cannot grow (out of memory).
    [[nodiscard]] bool TryPlan(uint32 texture, bool shouldHash) noexcept;

    /// The decode of a texture with an image: the next planned result when it matches
    /// (prefetching a batch as needed), otherwise an inline decode.
    /// @param outDecode Replaced on Ok; per-image read and decode failures are in its fields.
    /// @return Ok; OutOfMemory for queue storage; or Cancelled when the hook asked to stop
    ///         before more image bytes were read.
    [[nodiscard]] Status Take(uint32 texture, bool shouldHash, TextureDecode& outDecode) noexcept;

private:
    /// One image of the current batch.
    struct Slot final
    {
        TextureDecodeRequest Request;
        /// File bytes read on the calling thread; empty for embedded images.
        Array<uint8> Storage;
        std::span<const uint8> Bytes;
        std::string_view DecoderName;
        TextureDecode Decode;
        bool IsDecoded = false;
    };

    static foundation::threading::JobOutcome runDecodeJob(void* context) noexcept;
    static void decodeSlot(Slot& inoutSlot) noexcept;
    void readSlot(TextureDecodeRequest request, Slot& outSlot) const noexcept;
    void decodeInline(TextureDecodeRequest request, TextureDecode& outDecode) const noexcept;
    [[nodiscard]] Status fillBatch(usize first) noexcept;
    void decodeBatchInJobs(usize count) noexcept;

    const SceneIr& mScene;
    std::string_view mRoot;
    foundation::threading::JobSystem* mJobsOrNull = nullptr;
    const Cancellation* mCancelOrNull = nullptr;
    Array<TextureDecodeRequest> mPlan;
    /// Planned requests before this index were taken or skipped.
    usize mCursor = 0;
    /// Plan index of mSlots[0], and the end of the decoded batch.
    usize mBatchBegin = 0;
    usize mBatchEnd = 0;
    /// Images per batch: the job system's workers plus the helping caller, capped.
    usize mBatchCapacity = 1;
    /// Declared before mGraph, whose destructor drains a submission that still references them.
    Array<Slot> mSlots;
    foundation::threading::JobGraph mGraph;
    bool mIsGraphInitialized = false;
};
} // namespace ludus::content::scene_cook::importers
