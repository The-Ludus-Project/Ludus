#pragma once

// In-process A/B GPU timing of a ported LUSL recipe against its Slang original (LUSL L5d wave 2), for the
// renderer GPU suites' LUDUS_RENDERER_BENCHMARK mode. Clocks drift between runs, so both programs draw on one
// device in alternating blocks that swap their order each block. Test-only.

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/threading/thread.hpp>
#include <ludus/graphics/rhi/measurement.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>

namespace ludus::graphics::renderer::testing
{
/// Alternating blocks of the in-process A/B timing.
inline constexpr foundation::uint32 RECIPE_AB_BLOCKS = 12;
/// Untimed frames per variant at the start of each block.
inline constexpr foundation::uint32 RECIPE_AB_WARMUP_FRAMES = 3;
/// Timed frames per variant in each block.
inline constexpr foundation::uint32 RECIPE_AB_FRAMES = 20;
/// Bound on the polls of a pending GPU timing sample after its frame completed.
inline constexpr foundation::usize RECIPE_AB_POLL_LIMIT = 1000000;
/// Variant index of the Slang original; the LUSL recipe is 1.
inline constexpr foundation::usize RECIPE_AB_SLANG = 0;
/// Variant index of the LUSL recipe.
inline constexpr foundation::usize RECIPE_AB_LUSL = 1;
/// Number of variants.
inline constexpr foundation::usize RECIPE_AB_VARIANTS = 2;

/// True when LUDUS_RENDERER_BENCHMARK is set: the suites draw 1080p frames and time the two programs.
inline bool IsRecipeBenchmark() noexcept
{
    return std::getenv("LUDUS_RENDERER_BENCHMARK") != nullptr;
}

/// Median of `count` values, which it sorts in place.
inline foundation::uint64 GetMedian(foundation::uint64* inoutValues, foundation::usize count) noexcept
{
    for (foundation::usize i = 1; i < count; ++i)
    {
        for (foundation::usize j = i; j > 0 && inoutValues[j - 1] > inoutValues[j]; --j)
        {
            const foundation::uint64 swapped = inoutValues[j - 1];
            inoutValues[j - 1] = inoutValues[j];
            inoutValues[j] = swapped;
        }
    }
    const foundation::uint64 low = inoutValues[(count - 1) / 2];
    // Halve the gap rather than the sum, which can overflow for offset signed values.
    return low + (inoutValues[count / 2] - low) / 2;
}

/// Time the two variants of `recipe` A/B on `device`, created with DeviceDescription::EnableGpuTiming, and print one
/// RECIPEAB line. drawFrame(variant) submits one frame of RECIPE_AB_SLANG or RECIPE_AB_LUSL and waits for it, for
/// example by reading its pixels back. Each sample is the device-clock interval of the whole frame
/// (rhi::ArmGpuTiming), without the readback that follows it.
template <typename DrawFrame>
void RunRecipeAbTiming(rhi::DeviceHandle device,
                       const char* recipe,
                       foundation::uint32 width,
                       foundation::uint32 height,
                       DrawFrame&& drawFrame)
{
    foundation::uint64 blockMeans[RECIPE_AB_VARIANTS][RECIPE_AB_BLOCKS]{};
    foundation::uint64 differences[RECIPE_AB_BLOCKS]{};
    for (foundation::uint32 block = 0; block < RECIPE_AB_BLOCKS; ++block)
    {
        for (foundation::uint32 order = 0; order < RECIPE_AB_VARIANTS; ++order)
        {
            const foundation::usize variant = (block + order) % RECIPE_AB_VARIANTS;
            foundation::uint64 total = 0;
            for (foundation::uint32 frame = 0; frame < RECIPE_AB_WARMUP_FRAMES + RECIPE_AB_FRAMES; ++frame)
            {
                REQUIRE(rhi::ArmGpuTiming(device) == rhi::RasterStatus::Ready);
                drawFrame(variant);
                rhi::GpuTiming gpu;
                REQUIRE(rhi::GetGpuTiming(device, gpu) == rhi::RasterStatus::Ready);
                for (foundation::usize poll = 0;
                     poll < RECIPE_AB_POLL_LIMIT && gpu.State == rhi::GpuTimingState::Pending;
                     ++poll)
                {
                    foundation::threading::YieldThread();
                    REQUIRE(rhi::GetGpuTiming(device, gpu) == rhi::RasterStatus::Ready);
                }
                REQUIRE(gpu.State == rhi::GpuTimingState::Available);
                if (frame >= RECIPE_AB_WARMUP_FRAMES)
                {
                    total += static_cast<foundation::uint64>(gpu.DurationNanoseconds);
                }
            }
            blockMeans[variant][block] = total / RECIPE_AB_FRAMES;
        }
        differences[block] = blockMeans[RECIPE_AB_LUSL][block] - blockMeans[RECIPE_AB_SLANG][block];
    }
    // Unsigned differences wrap; recentre them so the median orders signed values correctly.
    constexpr foundation::uint64 SIGN_OFFSET = foundation::uint64{1} << 63U;
    for (foundation::uint64& difference : differences)
    {
        difference += SIGN_OFFSET;
    }
    const foundation::uint64 slang = GetMedian(blockMeans[RECIPE_AB_SLANG], RECIPE_AB_BLOCKS);
    const foundation::uint64 lusl = GetMedian(blockMeans[RECIPE_AB_LUSL], RECIPE_AB_BLOCKS);
    const foundation::int64 difference =
        static_cast<foundation::int64>(GetMedian(differences, RECIPE_AB_BLOCKS) - SIGN_OFFSET);
    std::printf(
        "RECIPEAB,%s,%ux%u,%u blocks x %u frames,GPU frame slang=%llu ns,lusl=%llu ns,median difference=%lld ns\n",
        recipe,
        width,
        height,
        RECIPE_AB_BLOCKS,
        RECIPE_AB_FRAMES,
        static_cast<unsigned long long>(slang),
        static_cast<unsigned long long>(lusl),
        static_cast<long long>(difference));
}
} // namespace ludus::graphics::renderer::testing
