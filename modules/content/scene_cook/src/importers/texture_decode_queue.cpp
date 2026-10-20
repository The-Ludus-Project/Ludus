// Planned, batched texture decoding for the best-effort cook; see texture_decode_queue.h.

#include "texture_decode_queue.h"

#include "../native_files.h"
#include "front_end_support.h"

#include <ludus/content/scene_cook/limits.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/strings/string.hpp>

namespace ludus::content::scene_cook::importers
{
namespace
{
namespace threading = foundation::threading;
namespace math = foundation::math;
using foundation::String;
using foundation::StringStatus;

/// Largest image file read for a texture.
constexpr usize MAX_IMAGE_FILE_BYTES = MAX_SOURCE_IMAGE_BYTES;
/// Planned requests searched past the cursor for a match: the most decodes one material
/// slot's surface and emission maps take (normal, roughness, metallic and emission).
constexpr usize RESYNC_LOOKAHEAD = 4;
/// Message of a texture whose image bytes cannot be read.
constexpr std::string_view UNREADABLE_MESSAGE = "cannot read the image file";
} // namespace

TextureDecodeQueue::TextureDecodeQueue(const SceneIr& scene,
                                       std::string_view root,
                                       threading::JobSystem* jobsOrNull,
                                       const Cancellation* cancelOrNull) noexcept
    : mScene(scene), mRoot(root), mJobsOrNull(jobsOrNull), mCancelOrNull(cancelOrNull)
{
    if (mJobsOrNull != nullptr)
    {
        // Workers plus the caller, which helps in Wait; zero workers decodes one at a time.
        mBatchCapacity =
            math::Min(usize{mJobsOrNull->GetWorkerCount()} + 1, static_cast<usize>(MAX_TEXTURE_DECODE_BATCH));
    }
}

bool TextureDecodeQueue::TryPlan(uint32 texture) noexcept
{
    return mJobsOrNull == nullptr || mPlan.TryAdd(texture);
}

Status TextureDecodeQueue::Take(uint32 texture, TextureDecode& outDecode) noexcept
{
    const Status taken = takeDecode(texture, outDecode);
    if (taken != Status::Ok || outDecode.IsFileRead == false || outDecode.Read != Status::Ok)
    {
        return taken;
    }
    return mFileReads.TryAdd({ .Texture = texture, .FileDigest = outDecode.ReadDigest }) == true ? Status::Ok
                                                                                               : Status::OutOfMemory;
}

std::span<const TextureFileRead> TextureDecodeQueue::GetFileReads() const noexcept
{
    return mFileReads.AsSpan();
}

Status TextureDecodeQueue::takeDecode(uint32 texture, TextureDecode& outDecode) noexcept
{
    if (IsCancelled(mCancelOrNull) == true)
    {
        return Status::Cancelled;
    }
    const usize end = math::Min(mPlan.GetSize(), mCursor + RESYNC_LOOKAHEAD);
    usize found = mPlan.GetSize();
    for (usize i = mCursor; i < end; ++i)
    {
        if (mPlan[i] == texture)
        {
            found = i;
            break;
        }
    }
    if (found == mPlan.GetSize())
    {
        // Off the plan (for example a material slot whose maps no longer fit): the same
        // decode, without prefetching.
        decodeInline(texture, outDecode);
        return Status::Ok;
    }
    // Release the results of skipped requests; they are decoded again if taken later.
    for (usize i = math::Max(mCursor, mBatchBegin); i < found && i < mBatchEnd; ++i)
    {
        mSlots[i - mBatchBegin].Decode = TextureDecode{};
    }
    mCursor = found + 1;
    if (found >= mBatchEnd)
    {
        const Status filled = fillBatch(found);
        if (filled != Status::Ok)
        {
            return filled;
        }
    }
    TextureDecode& decoded = mSlots[found - mBatchBegin].Decode;
    outDecode = static_cast<TextureDecode&&>(decoded);
    decoded = TextureDecode{};
    return Status::Ok;
}

threading::JobOutcome TextureDecodeQueue::runDecodeJob(void* context) noexcept
{
    decodeSlot(*static_cast<Slot*>(context));
    return threading::JobOutcome::Succeeded;
}

void TextureDecodeQueue::decodeSlot(Slot& inoutSlot) noexcept
{
    if (inoutSlot.IsDecoded == true)
    {
        return;
    }
    TextureDecode& decode = inoutSlot.Decode;
    if (decode.IsFileRead == true)
    {
        decode.ReadDigest = Hash(inoutSlot.Bytes);
    }
    decode.Decoded = DecodeImage(inoutSlot.DecoderName, inoutSlot.Bytes, decode.Image, decode.Message);
    inoutSlot.IsDecoded = true;
}

void TextureDecodeQueue::readSlot(uint32 texture, Slot& outSlot) const noexcept
{
    outSlot = Slot{};
    outSlot.Texture = texture;
    outSlot.Decode.Message = UNREADABLE_MESSAGE;
    const IrTexture& source = mScene.Textures[texture];
    if (source.EmbeddedImage != IR_NONE)
    {
        // Embedded images are borrowed from the IR, which no job modifies.
        const IrEmbeddedImage& image = mScene.EmbeddedImages[source.EmbeddedImage];
        outSlot.Bytes = image.Bytes.AsSpan();
        outSlot.DecoderName = image.DecoderName.GetView();
        return;
    }
    String path;
    const std::string_view parts[] = {mRoot, std::string_view("/"), source.File.GetView()};
    const Status read = path.TryAppendMany(parts) != StringStatus::Ok
                            ? Status::OutOfMemory
                            : detail::ReadFileBounded(path.GetView(), MAX_IMAGE_FILE_BYTES, outSlot.Storage);
    if (read != Status::Ok)
    {
        outSlot.Decode.Read = read;
        outSlot.IsDecoded = true;
        return;
    }
    outSlot.Decode.IsFileRead = true;
    outSlot.Bytes = outSlot.Storage.AsSpan();
    outSlot.DecoderName = source.File.GetView();
}

void TextureDecodeQueue::decodeInline(uint32 texture, TextureDecode& outDecode) const noexcept
{
    Slot slot;
    readSlot(texture, slot);
    decodeSlot(slot);
    outDecode = static_cast<TextureDecode&&>(slot.Decode);
}

Status TextureDecodeQueue::fillBatch(usize first) noexcept
{
    const usize count = math::Min(mBatchCapacity, mPlan.GetSize() - first);
    if (mSlots.GetSize() < mBatchCapacity && mSlots.TryResize(mBatchCapacity) == false)
    {
        return Status::OutOfMemory;
    }
    mBatchBegin = first;
    mBatchEnd = first;
    // Every file is read here, on the calling thread: job callbacks do no I/O.
    for (usize i = 0; i < mSlots.GetSize(); ++i)
    {
        if (i < count)
        {
            readSlot(mPlan[first + i], mSlots[i]);
        }
        else
        {
            mSlots[i] = Slot{};
        }
    }
    decodeBatchInJobs(count);
    for (usize i = 0; i < count; ++i)
    {
        // Decodes any image the job system did not run, then drops the encoded bytes.
        decodeSlot(mSlots[i]);
        mSlots[i].Bytes = {};
        mSlots[i].Storage = Array<uint8>{};
    }
    mBatchEnd = first + count;
    return Status::Ok;
}

void TextureDecodeQueue::decodeBatchInJobs(usize count) noexcept
{
    if (mJobsOrNull == nullptr || count == 0)
    {
        return;
    }
    if (mIsGraphInitialized == false)
    {
        if (mGraph.Initialize(
        {
            .MaxJobs = MAX_TEXTURE_DECODE_BATCH,
            .MaxDependencies = 0,
        }) != threading::JobStatus::Ok)
        {
            return;
        }
        mIsGraphInitialized = true;
    }
    if (mGraph.Reset() != threading::JobStatus::Ok)
    {
        return;
    }
    // A job system that refuses the graph (busy with another graph, or called from a
    // job) leaves the batch to the inline decode in fillBatch.
    for (usize i = 0; i < count; ++i)
    {
        threading::JobHandle handle;
        if (mSlots[i].IsDecoded == false &&
            mGraph.Add(runDecodeJob, &mSlots[i], handle, { .NameOrNull = "SceneCook.DecodeTexture" }) !=
                threading::JobStatus::Ok)
        {
            break;
        }
    }
    if (mGraph.Seal() != threading::JobStatus::Ok || mJobsOrNull->Submit(mGraph) != threading::JobStatus::Ok)
    {
        return;
    }
    threading::JobOutcome outcome = threading::JobOutcome::Pending;
    const threading::JobStatus waited = mJobsOrNull->Wait(outcome);
    // Submit already refused a call from a job. NotInitialized or InvalidArgument means an
    // owner Shutdown drained the graph; either way every callback write is now visible.
    LUDUS_REQUIRE(waited != threading::JobStatus::InsideJob);
}
} // namespace ludus::content::scene_cook::importers
