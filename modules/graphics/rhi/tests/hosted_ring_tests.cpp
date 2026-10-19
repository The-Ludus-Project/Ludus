// Hosted surface producer bookkeeping, shared by the Metal and Vulkan backends.
// The two-process GPU tests exercise it end to end; these cases pin each
// ownership transition without a GPU.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>

#include "internal/hosted_ring.h"

#include <catch2/catch_test_macros.hpp>

using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::usize;
using ludus::graphics::rhi::internal::HOSTED_IMAGE_COUNT;
using ludus::graphics::rhi::internal::HostedImageState;
using ludus::graphics::rhi::internal::HostedReleaseResult;
using ludus::graphics::rhi::internal::HostedRing;
using ludus::graphics::rhi::internal::HostedSubmission;
using ludus::graphics::rhi::internal::MAX_HOSTED_GENERATIONS;

namespace
{
/// Start and publish a generation; returns its slot.
usize Publish(HostedRing& inoutRing, uint32 width, uint32 height)
{
    usize slot = 0;
    REQUIRE(inoutRing.TryBeginGeneration(width, height, slot));
    inoutRing.MarkPublished(slot);
    return slot;
}

/// Acquire, submit and complete one frame; returns its image.
uint32 PresentFrame(HostedRing& inoutRing)
{
    uint32 image = 0;
    REQUIRE(inoutRing.TryAcquire(image));
    static_cast<void>(inoutRing.Submit(image));
    HostedSubmission submission;
    REQUIRE(inoutRing.TryPeekSubmission(submission));
    REQUIRE(inoutRing.IsCurrentSubmission(submission));
    inoutRing.CompleteSubmission(true);
    return image;
}
} // namespace

TEST_CASE("A hosted generation is acquirable only once published", "[rhi][hosted]")
{
    HostedRing ring;
    uint32 image = 0;
    CHECK_FALSE(ring.TryAcquire(image));
    usize slot = 0;
    REQUIRE(ring.TryBeginGeneration(64, 48, slot));
    CHECK(ring.GetGeneration(slot) == 1);
    CHECK(ring.IsCurrentExtent(64, 48));
    CHECK_FALSE(ring.IsCurrentExtent(64, 49));
    CHECK_FALSE(ring.TryAcquire(image));
    ring.MarkPublished(slot);
    CHECK(ring.TryAcquire(image));
    CHECK(ring.GetState(slot, image) == HostedImageState::Acquired);
    ring.CancelAcquire(image);
    CHECK(ring.GetState(slot, image) == HostedImageState::Free);
}

TEST_CASE("Presented hosted images are reused only after their release", "[rhi][hosted]")
{
    HostedRing ring;
    const usize slot = Publish(ring, 64, 48);
    ludus::foundation::StaticArray<bool, HOSTED_IMAGE_COUNT> isUsed{};
    for (usize i = 0; i < HOSTED_IMAGE_COUNT; ++i)
    {
        const uint32 image = PresentFrame(ring);
        CHECK_FALSE(isUsed[image]);
        isUsed[image] = true;
        CHECK(ring.GetState(slot, image) == HostedImageState::Presented);
    }
    // Every image is with the consumer: the frame is skipped.
    uint32 image = 0;
    CHECK_FALSE(ring.TryAcquire(image));
    CHECK(ring.Release(1, 1) == HostedReleaseResult::Ok);
    // A second release of the same image, or of an unknown generation, is ignored.
    CHECK(ring.Release(1, 1) == HostedReleaseResult::Unknown);
    CHECK(ring.Release(7, 0) == HostedReleaseResult::Unknown);
    CHECK(ring.Release(1, HOSTED_IMAGE_COUNT) == HostedReleaseResult::Unknown);
    REQUIRE(ring.TryAcquire(image));
    CHECK(image == 1);
}

TEST_CASE("Hosted submissions complete in order with increasing sequences", "[rhi][hosted]")
{
    HostedRing ring;
    static_cast<void>(Publish(ring, 8, 8));
    uint32 first = 0;
    uint32 second = 0;
    REQUIRE(ring.TryAcquire(first));
    const HostedSubmission a = ring.Submit(first);
    REQUIRE(ring.TryAcquire(second));
    const HostedSubmission b = ring.Submit(second);
    CHECK(b.Sequence > a.Sequence);
    HostedSubmission oldest;
    REQUIRE(ring.TryPeekSubmission(oldest));
    CHECK(oldest.Image == first);
    ring.CompleteSubmission(true);
    REQUIRE(ring.TryPeekSubmission(oldest));
    CHECK(oldest.Image == second);
    // A frame that cannot be presented frees its image unannounced.
    ring.CompleteSubmission(false);
    CHECK(ring.GetState(0, second) == HostedImageState::Free);
    CHECK_FALSE(ring.TryPeekSubmission(oldest));
}

TEST_CASE("A resized hosted surface retires its old generation once every image is back", "[rhi][hosted]")
{
    HostedRing ring;
    const usize first = Publish(ring, 64, 48);
    const uint32 shown = PresentFrame(ring);
    uint32 inFlight = 0;
    REQUIRE(ring.TryAcquire(inFlight));
    static_cast<void>(ring.Submit(inFlight));

    const usize second = Publish(ring, 32, 16);
    CHECK(second != first);
    CHECK(ring.GetGeneration(second) == 2);
    CHECK(ring.GetLiveGenerationCount() == 2);
    usize retired = 0;
    CHECK_FALSE(ring.TryTakeRetired(retired));

    // The old generation's in-flight frame is no longer worth showing.
    HostedSubmission submission;
    REQUIRE(ring.TryPeekSubmission(submission));
    CHECK_FALSE(ring.IsCurrentSubmission(submission));
    ring.CompleteSubmission(false);
    CHECK_FALSE(ring.TryTakeRetired(retired));
    // The consumer still shows its last old image until it releases it.
    CHECK(ring.Release(1, shown) == HostedReleaseResult::Ok);
    REQUIRE(ring.TryTakeRetired(retired));
    CHECK(retired == first);
    CHECK(ring.GetLiveGenerationCount() == 1);
    CHECK_FALSE(ring.TryTakeRetired(retired));
}

TEST_CASE("A hosted surface skips resizes while every generation slot is held", "[rhi][hosted]")
{
    HostedRing ring;
    for (usize i = 0; i < MAX_HOSTED_GENERATIONS; ++i)
    {
        static_cast<void>(Publish(ring, static_cast<uint32>(16 + i), 16));
        static_cast<void>(PresentFrame(ring));
    }
    usize slot = 0;
    CHECK_FALSE(ring.TryBeginGeneration(99, 99, slot));
    // The current generation stays usable meanwhile.
    CHECK(ring.IsCurrentExtent(16 + MAX_HOSTED_GENERATIONS - 1, 16));
    CHECK(ring.Release(1, 0) == HostedReleaseResult::Ok);
    usize retired = 0;
    REQUIRE(ring.TryTakeRetired(retired));
    CHECK(ring.TryBeginGeneration(99, 99, slot));
    CHECK(slot == retired);
    CHECK(ring.GetGeneration(slot) == MAX_HOSTED_GENERATIONS + 1);
}
