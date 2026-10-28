#include "../src/internal/reference_data.h"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace ludus::foundation;
namespace internal = ludus::graphics::renderer::internal;
namespace rhi = ludus::graphics::rhi;
namespace
{
constexpr usize SURFACE_COUNT = 3;
// Surface 0 is a bright candle flame, 1 a dim ceiling panel and 2 is not emissive.
void BuildScene(StaticArray<internal::ReferenceSurfaceData, SURFACE_COUNT>& outSurfaces,
                internal::ReferenceSceneData& outScene)
{
    outSurfaces[0].Emission = {1000, 1000, 1000};
    outSurfaces[1].Emission = {4, 4, 4};
    REQUIRE(outScene.Instances.TryResize(SURFACE_COUNT) == true);
    for (usize i = 0; i < SURFACE_COUNT; ++i)
    {
        outScene.Instances[i].Surface = &outSurfaces[i];
    }
    outScene.InstanceCount = SURFACE_COUNT;
    // Triangle areas (instance): 0.001 (flame), 2 (unlit), 0.001 (flame), 2 (panel), 2 (panel).
    constexpr usize TRIANGLE_COUNT = 5;
    const StaticArray<float32, TRIANGLE_COUNT> areas{{.001F, 2, .001F, 2, 2}};
    const StaticArray<uint32, TRIANGLE_COUNT> instances{{0, 2, 0, 1, 1}};
    REQUIRE(outScene.Triangles.TryResize(TRIANGLE_COUNT) == true);
    for (usize i = 0; i < TRIANGLE_COUNT; ++i)
    {
        outScene.Triangles[i].Area = areas[i];
        outScene.Triangles[i].Instance = instances[i];
    }
    for (const uint32 triangle : {uint32{0}, uint32{2}, uint32{3}, uint32{4}})
    {
        REQUIRE(outScene.Emitters.TryAdd(triangle) == true);
    }
}
} // namespace

TEST_CASE("Reference emitter selection is proportional to emitted luminance times area", "[renderer][reference]")
{
    StaticArray<internal::ReferenceSurfaceData, SURFACE_COUNT> surfaces;
    internal::ReferenceSceneData scene;
    BuildScene(surfaces, scene);
    REQUIRE(internal::BuildReferenceEmitterSelection(scene) == rhi::RasterStatus::Ready);
    REQUIRE(scene.EmitterCdf.GetSize() == scene.Emitters.GetSize());

    // Flames carry 1000 * 0.001 = 1 unit each, panel triangles 4 * 2 = 8 units each.
    constexpr float32 TOTAL_WEIGHT = 18;
    const StaticArray<float32, 4> expected{{1 / TOTAL_WEIGHT, 1 / TOTAL_WEIGHT, 8 / TOTAL_WEIGHT, 8 / TOTAL_WEIGHT}};
    float32 sum = 0;
    for (usize emitter = 0; emitter < expected.GetSize(); ++emitter)
    {
        const float32 probability = internal::GetReferenceEmitterProbability(scene, emitter);
        CHECK(probability == Catch::Approx(expected[emitter]).epsilon(1e-6));
        sum += probability;
    }
    CHECK(sum == Catch::Approx(1).epsilon(1e-6));
    CHECK(internal::GetReferenceEmitterProbability(scene, expected.GetSize()) == 0);

    // Every integer below the total maps into exactly the interval that owns it.
    const uint32 total = scene.EmitterCdf[scene.EmitterCdf.GetSize() - 1];
    // Rounding moves each emitter by at most one unit from its share of the quantized total.
    CHECK(total <= internal::REFERENCE_EMITTER_WEIGHT_TOTAL + scene.Emitters.GetSize());
    CHECK(total + scene.Emitters.GetSize() >= internal::REFERENCE_EMITTER_WEIGHT_TOTAL);
    CHECK(internal::SelectReferenceEmitter(scene, 0) == 0);
    for (usize emitter = 0; emitter < scene.EmitterCdf.GetSize(); ++emitter)
    {
        const uint32 first = emitter == 0 ? 0 : scene.EmitterCdf[emitter - 1];
        const uint32 last = scene.EmitterCdf[emitter] - 1;
        CHECK(internal::SelectReferenceEmitter(scene, first) == emitter);
        CHECK(internal::SelectReferenceEmitter(scene, last) == emitter);
    }

    usize emitter = 99;
    REQUIRE(internal::FindReferenceEmitter(scene, 3, emitter) == true);
    CHECK(emitter == 2);
    REQUIRE(internal::FindReferenceEmitter(scene, 0, emitter) == true);
    CHECK(emitter == 0);
    emitter = 99;
    CHECK(internal::FindReferenceEmitter(scene, 1, emitter) == false);
    CHECK(internal::FindReferenceEmitter(scene, 5, emitter) == false);
    CHECK(emitter == 99);
}

TEST_CASE("Reference emitter selection keeps faint emitters reachable and skips powerless ones",
          "[renderer][reference]")
{
    StaticArray<internal::ReferenceSurfaceData, SURFACE_COUNT> surfaces;
    internal::ReferenceSceneData scene;
    BuildScene(surfaces, scene);
    // The first flame carries 1e-9 of the power, far below one quantization unit:
    // rounding must not drop it from the CDF.
    scene.Triangles[2].Area = 1e6F;
    // A negative channel carries no power; a zero-area triangle has no surface to sample.
    surfaces[1].Emission = {-4, 0, 0};
    scene.Triangles[4].Area = 0;
    REQUIRE(internal::BuildReferenceEmitterSelection(scene) == rhi::RasterStatus::Ready);
    constexpr uint32 BRIGHT_END = internal::REFERENCE_EMITTER_WEIGHT_TOTAL + 1;
    CHECK(scene.EmitterCdf[0] == 1);
    CHECK(scene.EmitterCdf[1] == BRIGHT_END);
    CHECK(scene.EmitterCdf[2] == BRIGHT_END);
    CHECK(scene.EmitterCdf[3] == BRIGHT_END);
    CHECK(internal::GetReferenceEmitterProbability(scene, 0) > 0);
    CHECK(internal::GetReferenceEmitterProbability(scene, 2) == 0);
    CHECK(internal::GetReferenceEmitterProbability(scene, 3) == 0);
    CHECK(internal::SelectReferenceEmitter(scene, 0) == 0);
    CHECK(internal::SelectReferenceEmitter(scene, 1) == 1);
    CHECK(internal::SelectReferenceEmitter(scene, BRIGHT_END - 1) == 1);

    surfaces[0].Emission = {};
    REQUIRE(internal::BuildReferenceEmitterSelection(scene) == rhi::RasterStatus::Ready);
    CHECK(scene.EmitterCdf[3] == 0);
    CHECK(internal::GetReferenceEmitterProbability(scene, 0) == 0);
}

TEST_CASE("Reference emitter selection rejects unordered emitter lists without changing the CDF",
          "[renderer][reference]")
{
    StaticArray<internal::ReferenceSurfaceData, SURFACE_COUNT> surfaces;
    internal::ReferenceSceneData scene;
    BuildScene(surfaces, scene);
    REQUIRE(internal::BuildReferenceEmitterSelection(scene) == rhi::RasterStatus::Ready);
    const uint32 total = scene.EmitterCdf[scene.EmitterCdf.GetSize() - 1];

    scene.Emitters[2] = 2; // Duplicates its predecessor.
    CHECK(internal::BuildReferenceEmitterSelection(scene) == rhi::RasterStatus::InvalidDescription);
    scene.Emitters[2] = 9; // Outside the triangle list.
    CHECK(internal::BuildReferenceEmitterSelection(scene) == rhi::RasterStatus::InvalidDescription);
    REQUIRE(scene.EmitterCdf.GetSize() == scene.Emitters.GetSize());
    CHECK(scene.EmitterCdf[scene.EmitterCdf.GetSize() - 1] == total);

    internal::ReferenceSceneData empty;
    CHECK(internal::BuildReferenceEmitterSelection(empty) == rhi::RasterStatus::Ready);
    CHECK(empty.EmitterCdf.GetSize() == 0);
    CHECK(internal::GetReferenceEmitterProbability(empty, 0) == 0);
}

TEST_CASE("Reference emissive triangle accessors draw and evaluate the power CDF by scene triangle",
          "[renderer][reference]")
{
    StaticArray<internal::ReferenceSurfaceData, SURFACE_COUNT> surfaces;
    internal::ReferenceSceneData scene;
    BuildScene(surfaces, scene);
    REQUIRE(internal::BuildReferenceEmitterSelection(scene) == rhi::RasterStatus::Ready);

    // Probabilities are keyed by scene triangle; triangle 1 is not emissive and 5 is outside the scene.
    CHECK(internal::EvaluateReferenceEmissiveTriangleSelectionPmf(scene, 3) ==
          internal::GetReferenceEmitterProbability(scene, 2));
    CHECK(internal::EvaluateReferenceEmissiveTriangleSelectionPmf(scene, 1) == 0);
    CHECK(internal::EvaluateReferenceEmissiveTriangleSelectionPmf(scene, 5) == 0);

    // Each draw reproduces RT1's bounded ticket over the integer total, then its interval search.
    const math::RandomKey key{0x5eedULL};
    const uint32 total = scene.EmitterCdf[scene.EmitterCdf.GetSize() - 1];
    StaticArray<uint32, 5> histogram{};
    constexpr uint16 DRAW_COUNT = 4096;
    for (uint16 dimension = 0; dimension < DRAW_COUNT; ++dimension)
    {
        const math::RandomAddress address{ .Scope = 7, .Event = 2, .Dimension = dimension };
        uint32 value = 0;
        REQUIRE(math::TrySampleBounded(key, address, total, value) == math::MathStatus::Success);
        const internal::ReferenceEmissiveTriangleSelection selection =
            internal::SelectReferenceEmissiveTriangle(scene, key, address);
        REQUIRE(selection.HasSample == true);
        CHECK(selection.TriangleIndex == scene.Emitters[internal::SelectReferenceEmitter(scene, value)]);
        CHECK(selection.SelectionPmf ==
              internal::EvaluateReferenceEmissiveTriangleSelectionPmf(scene, selection.TriangleIndex));
        ++histogram[selection.TriangleIndex];
    }
    CHECK(histogram[1] == 0);
    // Panels carry 8/18 of the power each; flames 1/18. Margins exceed six standard deviations.
    CHECK(histogram[3] > 1600);
    CHECK(histogram[4] > 1600);
    CHECK(histogram[0] + histogram[2] < 650);

    // The packer writes exactly the words that GPU kernels search.
    StaticArray<math::Vector4, 4> words{};
    REQUIRE(internal::PackReferenceEmitterSelection(scene, {words.GetData(), words.GetSize()}) == true);
    for (usize emitter = 0; emitter < words.GetSize(); ++emitter)
    {
        uint32 triangle = 0;
        uint32 cumulative = 0;
        std::memcpy(&triangle, &words[emitter].X, sizeof(triangle));
        std::memcpy(&cumulative, &words[emitter].Y, sizeof(cumulative));
        CHECK(triangle == scene.Emitters[emitter]);
        CHECK(cumulative == scene.EmitterCdf[emitter]);
    }
    words[0] = {1, 2, 3, 4};
    CHECK(internal::PackReferenceEmitterSelection(scene, {words.GetData(), words.GetSize() - 1}) == false);
    CHECK(words[0].X == 1);

    // Zero total power, an empty list and a stale CDF select nothing.
    surfaces[0].Emission = {};
    surfaces[1].Emission = {};
    REQUIRE(internal::BuildReferenceEmitterSelection(scene) == rhi::RasterStatus::Ready);
    const internal::ReferenceEmissiveTriangleSelection powerless =
        internal::SelectReferenceEmissiveTriangle(scene, key, {});
    CHECK(powerless.HasSample == false);
    CHECK(powerless.TriangleIndex == LUDUS_GPU_PREVIEW_INVALID_TRIANGLE);
    CHECK(powerless.SelectionPmf == 0);
    CHECK(internal::EvaluateReferenceEmissiveTriangleSelectionPmf(scene, 3) == 0);
    REQUIRE(scene.Emitters.TryAdd(5) == true);
    CHECK(internal::SelectReferenceEmissiveTriangle(scene, key, {}).HasSample == false);
    CHECK(internal::EvaluateReferenceEmissiveTriangleSelectionPmf(scene, 0) == 0);
    CHECK(internal::PackReferenceEmitterSelection(scene, {words.GetData(), words.GetSize()}) == false);
    internal::ReferenceSceneData empty;
    CHECK(internal::SelectReferenceEmissiveTriangle(empty, key, {}).HasSample == false);
}
