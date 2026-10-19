#pragma once

// Producer bookkeeping of the hosted surface, shared by the Metal and Vulkan
// backends: generations, per-image ownership, and the order in which completed
// frames are announced. It allocates nothing and touches no GPU object; the
// backends keep their native images and completion objects in arrays indexed
// by the same generation slot and image. The contract is owned by
// docs/architecture/editor-gui-systems.md, "The hosted surface and frame ownership".

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/platform/hosted_surface.h>

namespace ludus::graphics::rhi::internal
{
/// Images in one hosted surface generation.
inline constexpr foundation::usize HOSTED_IMAGE_COUNT = platform::HOSTED_SURFACE_IMAGE_COUNT;
/// Generations alive at once: the current one plus those waiting for their images to return.
/// A resize while every slot is still held skips frames until the consumer releases.
inline constexpr foundation::usize MAX_HOSTED_GENERATIONS = 3;
/// Submitted frames whose completion has not been announced yet.
inline constexpr foundation::usize MAX_HOSTED_SUBMISSIONS = MAX_HOSTED_GENERATIONS * HOSTED_IMAGE_COUNT;

/// Who owns one hosted image.
enum class HostedImageState : foundation::uint8
{
    /// The producer may acquire it.
    Free,
    /// Acquired by BeginFrame; the frame is being recorded.
    Acquired,
    /// Submitted by EndFrame; the GPU may still be writing it.
    Submitted,
    /// Announced with Present; the consumer holds it until its Release.
    Presented,
};

/// One submitted frame, in submission order.
struct HostedSubmission final
{
    foundation::usize Slot = 0;
    foundation::uint32 Image = 0;
    foundation::uint64 Sequence = 0;
};

/// Result of applying a consumer release.
enum class HostedReleaseResult : foundation::uint8
{
    /// The image is free again.
    Ok,
    /// No live generation has that image presented; the release is ignored.
    Unknown,
};

class HostedRing final
{
public:
    /// Forget every generation and submission; the backend destroys its images first.
    LUDUS_INLINE void Reset() noexcept
    {
        *this = HostedRing{};
    }

    /// True when a current generation exists.
    [[nodiscard]] LUDUS_INLINE bool HasCurrent() const noexcept
    {
        return mHasCurrent;
    }
    /// Slot of the current generation; meaningful only when HasCurrent().
    [[nodiscard]] LUDUS_INLINE foundation::usize GetCurrentSlot() const noexcept
    {
        return mCurrentSlot;
    }
    /// True when the current generation exists and has this extent.
    [[nodiscard]] bool IsCurrentExtent(foundation::uint32 width, foundation::uint32 height) const noexcept
    {
        return mHasCurrent == true && mSlots[mCurrentSlot].Width == width && mSlots[mCurrentSlot].Height == height;
    }

    /// @brief Start a new generation of the given extent, retiring the current one.
    /// @return false when every slot still holds a generation; nothing changes then.
    [[nodiscard]] bool
    TryBeginGeneration(foundation::uint32 width, foundation::uint32 height, foundation::usize& outSlot) noexcept
    {
        for (foundation::usize slot = 0; slot < MAX_HOSTED_GENERATIONS; ++slot)
        {
            if (mSlots[slot].IsLive == true)
            {
                continue;
            }
            ++mLastGeneration;
            mSlots[slot] = {};
            mSlots[slot].IsLive = true;
            mSlots[slot].Generation = mLastGeneration;
            mSlots[slot].Width = width;
            mSlots[slot].Height = height;
            mHasCurrent = true;
            mCurrentSlot = slot;
            outSlot = slot;
            return true;
        }
        return false;
    }

    /// The new generation's images could not be created; free its slot.
    void AbandonGeneration(foundation::usize slot) noexcept
    {
        mSlots[slot] = {};
        if (mHasCurrent == true && mCurrentSlot == slot)
        {
            mHasCurrent = false;
        }
    }

    /// The consumer has been sent this generation's images.
    LUDUS_INLINE void MarkPublished(foundation::usize slot) noexcept
    {
        mSlots[slot].IsPublished = true;
    }
    [[nodiscard]] LUDUS_INLINE bool IsPublished(foundation::usize slot) const noexcept
    {
        return mSlots[slot].IsPublished;
    }
    [[nodiscard]] LUDUS_INLINE bool IsLive(foundation::usize slot) const noexcept
    {
        return mSlots[slot].IsLive;
    }
    [[nodiscard]] LUDUS_INLINE foundation::uint32 GetGeneration(foundation::usize slot) const noexcept
    {
        return mSlots[slot].Generation;
    }
    [[nodiscard]] LUDUS_INLINE foundation::uint32 GetWidth(foundation::usize slot) const noexcept
    {
        return mSlots[slot].Width;
    }
    [[nodiscard]] LUDUS_INLINE foundation::uint32 GetHeight(foundation::usize slot) const noexcept
    {
        return mSlots[slot].Height;
    }
    [[nodiscard]] LUDUS_INLINE HostedImageState GetState(foundation::usize slot,
                                                         foundation::uint32 image) const noexcept
    {
        return mSlots[slot].Images[image];
    }
    /// Live generations, current and retiring.
    [[nodiscard]] foundation::usize GetLiveGenerationCount() const noexcept
    {
        foundation::usize count = 0;
        for (const Slot& slot : mSlots)
        {
            count += slot.IsLive == true ? 1 : 0;
        }
        return count;
    }

    /// @brief Acquire a free image of the current, published generation.
    /// @return false when there is none: the frame is skipped.
    [[nodiscard]] bool TryAcquire(foundation::uint32& outImage) noexcept
    {
        if (mHasCurrent == false || mSlots[mCurrentSlot].IsPublished == false ||
            mSubmissionCount == MAX_HOSTED_SUBMISSIONS)
        {
            return false;
        }
        for (foundation::uint32 image = 0; image < HOSTED_IMAGE_COUNT; ++image)
        {
            if (mSlots[mCurrentSlot].Images[image] == HostedImageState::Free)
            {
                mSlots[mCurrentSlot].Images[image] = HostedImageState::Acquired;
                outImage = image;
                return true;
            }
        }
        return false;
    }

    /// An acquired frame was abandoned before submission.
    LUDUS_INLINE void CancelAcquire(foundation::uint32 image) noexcept
    {
        mSlots[mCurrentSlot].Images[image] = HostedImageState::Free;
    }

    /// @brief Record the submission of the acquired image of the current generation.
    /// @return The submission, with the next frame sequence number.
    HostedSubmission Submit(foundation::uint32 image) noexcept
    {
        mSlots[mCurrentSlot].Images[image] = HostedImageState::Submitted;
        ++mLastSequence;
        const HostedSubmission submission{ .Slot = mCurrentSlot, .Image = image, .Sequence = mLastSequence };
        mSubmissions[(mSubmissionFirst + mSubmissionCount) % MAX_HOSTED_SUBMISSIONS] = submission;
        ++mSubmissionCount;
        return submission;
    }

    /// @brief Oldest submission whose completion has not been handled.
    /// @return false when none is outstanding.
    [[nodiscard]] bool TryPeekSubmission(HostedSubmission& outSubmission) const noexcept
    {
        if (mSubmissionCount == 0)
        {
            return false;
        }
        outSubmission = mSubmissions[mSubmissionFirst];
        return true;
    }

    /// @brief Handle the completion of the oldest submission.
    /// @param isPresented true when its Present was sent; false frees the image unannounced
    /// (a retired generation's frame, or a channel that can no longer present).
    void CompleteSubmission(bool isPresented) noexcept
    {
        const HostedSubmission submission = mSubmissions[mSubmissionFirst];
        mSlots[submission.Slot].Images[submission.Image] =
            isPresented == true ? HostedImageState::Presented : HostedImageState::Free;
        mSubmissionFirst = (mSubmissionFirst + 1) % MAX_HOSTED_SUBMISSIONS;
        --mSubmissionCount;
    }

    /// True when this submission's generation is still current, so its frame is worth showing.
    [[nodiscard]] LUDUS_INLINE bool IsCurrentSubmission(const HostedSubmission& submission) const noexcept
    {
        return mHasCurrent == true && submission.Slot == mCurrentSlot;
    }

    /// @brief Apply a consumer release.
    HostedReleaseResult Release(foundation::uint32 generation, foundation::uint32 image) noexcept
    {
        if (image >= HOSTED_IMAGE_COUNT)
        {
            return HostedReleaseResult::Unknown;
        }
        for (Slot& slot : mSlots)
        {
            if (slot.IsLive == true && slot.Generation == generation &&
                slot.Images[image] == HostedImageState::Presented)
            {
                slot.Images[image] = HostedImageState::Free;
                return HostedReleaseResult::Ok;
            }
        }
        return HostedReleaseResult::Unknown;
    }

    /// @brief Take a retired generation whose images have all come back.
    ///
    /// The slot is freed; the backend destroys its images.
    /// @return false when no retired generation is idle.
    [[nodiscard]] bool TryTakeRetired(foundation::usize& outSlot) noexcept
    {
        for (foundation::usize slot = 0; slot < MAX_HOSTED_GENERATIONS; ++slot)
        {
            if (mSlots[slot].IsLive == false || (mHasCurrent == true && slot == mCurrentSlot))
            {
                continue;
            }
            bool isIdle = true;
            for (const HostedImageState state : mSlots[slot].Images)
            {
                isIdle = isIdle == true && state == HostedImageState::Free;
            }
            if (isIdle == true)
            {
                mSlots[slot] = {};
                outSlot = slot;
                return true;
            }
        }
        return false;
    }

private:
    struct Slot final
    {
        bool IsLive = false;
        bool IsPublished = false;
        foundation::uint32 Generation = 0;
        foundation::uint32 Width = 0;
        foundation::uint32 Height = 0;
        foundation::StaticArray<HostedImageState, HOSTED_IMAGE_COUNT> Images{};
    };

    foundation::StaticArray<Slot, MAX_HOSTED_GENERATIONS> mSlots{};
    bool mHasCurrent = false;
    foundation::usize mCurrentSlot = 0;
    foundation::uint32 mLastGeneration = 0;
    foundation::uint64 mLastSequence = 0;
    foundation::StaticArray<HostedSubmission, MAX_HOSTED_SUBMISSIONS> mSubmissions{};
    foundation::usize mSubmissionFirst = 0;
    foundation::usize mSubmissionCount = 0;
};
} // namespace ludus::graphics::rhi::internal
