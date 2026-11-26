// Installed-SDK run of the renderer_composite recipe fixture (LUSL L5d wave 2, issue 1736). The
// installed helper compiles the LUSL recipe and generates renderer_composite_slots.h; this program
// binds the texture and sampler by those constants, draws the fixture and requires the pixels
// derived from its inputs. The renderer GPU suite runs the same fixture in the source tree against
// the Slang original, and both print the hash of the image they drew.
#include "composite_recipe_fixture.h"
#include "renderer_composite.h"
#include "renderer_composite_slots.h"

#include <cstdio>

namespace
{
using namespace ludus::foundation;
namespace fixture = ludus::qa::composite_recipe;
namespace rhi = ludus::graphics::rhi;

const char* GetKindName(rhi::RasterBindingKind kind) noexcept
{
    switch (kind)
    {
        case rhi::RasterBindingKind::Texture2D:
            return "Texture2D";
        case rhi::RasterBindingKind::Sampler:
            return "Sampler";
        default:
            return "other";
    }
}

/// Print the reflected fragment bindings beside the generated slots, and check that they agree.
bool CheckReflection(const rhi::RasterShaderDescription& fragment) noexcept
{
    namespace slots = ludus::shaders::renderer_composite::slots;
    const bool isExpected = fragment.Bindings.GetSize() == 2 && fragment.Bindings[0].Binding == slots::IMAGE &&
                            fragment.Bindings[0].Kind == rhi::RasterBindingKind::Texture2D &&
                            fragment.Bindings[1].Binding == slots::IMAGE_SAMPLER &&
                            fragment.Bindings[1].Kind == rhi::RasterBindingKind::Sampler;
    for (usize i = 0; i < fragment.Bindings.GetSize(); ++i)
    {
        std::printf("reflected binding %u: %s\n",
                    static_cast<unsigned>(fragment.Bindings[i].Binding),
                    GetKindName(fragment.Bindings[i].Kind));
    }
    std::printf("generated slots: IMAGE=%u IMAGE_SAMPLER=%u\n",
                static_cast<unsigned>(slots::IMAGE),
                static_cast<unsigned>(slots::IMAGE_SAMPLER));
    return isExpected;
}

int32 Run() noexcept
{
    rhi::DeviceDescription description;
    description.Required.PortableRaster = true;
    rhi::DeviceHandle device;
    rhi::SurfaceHandle surface;
    rhi::StartupInfo failure;
    const rhi::DeviceStatus started =
        rhi::CreateDevice({},
                          { .Width = fixture::TARGET_EXTENT, .Height = fixture::TARGET_EXTENT },
                          description,
                          device,
                          surface,
                          &failure);
    if (started != rhi::DeviceStatus::Ready)
    {
        if (failure.Error == rhi::StartupError::AdapterUnavailable)
        {
            std::printf("No GPU adapter on this host; composite recipe skipped\n");
            return 0;
        }
        std::fprintf(stderr, "Could not create the device\n");
        return 1;
    }
    const fixture::CompositeStages stages
    {
        .Vertex = ludus::shaders::renderer_composite::Vertex(),
        .Fragment = ludus::shaders::renderer_composite::Fragment(),
        .ImageSlot = ludus::shaders::renderer_composite::slots::IMAGE,
        .SamplerSlot = ludus::shaders::renderer_composite::slots::IMAGE_SAMPLER,
        .PositionUvLocation = ludus::shaders::renderer_composite::locations::INPUT_POSITION_UV,
    };
    if (CheckReflection(stages.Fragment) == false)
    {
        std::fprintf(stderr, "The reflected bindings differ from renderer_composite_slots.h\n");
        return 1;
    }
    fixture::Pixels expected{};
    fixture::ComputeExpectedPixels(expected);
    fixture::Pixels nearest{};
    fixture::Pixels linear{};
    const char* step = "";
    if (fixture::RenderComposite(device, surface, stages, rhi::RasterFilter::Nearest, nearest, step) == false ||
        fixture::RenderComposite(device, surface, stages, rhi::RasterFilter::Linear, linear, step) == false)
    {
        std::fprintf(stderr, "Composite recipe fixture could not %s\n", step);
        return 1;
    }
    const usize wrongPixels = fixture::CountDifferingPixels(nearest, expected);
    const usize linearPixels = fixture::CountDifferingPixels(linear, expected);
    std::printf("COMPOSITE_RECIPE_FIXTURE,installed-sdk,%016llx\n",
                static_cast<unsigned long long>(fixture::HashPixels(nearest)));
    std::printf("nearest sampler: %zu of %zu pixels differ from the derived image\n",
                wrongPixels,
                fixture::TARGET_BYTES / fixture::CHANNELS);
    std::printf("linear sampler (control): %zu pixels differ\n", linearPixels);
    if (wrongPixels != 0 || linearPixels == 0)
    {
        std::fprintf(stderr, "Composite recipe fixture pixels are wrong\n");
        return 1;
    }
    std::printf("composite recipe passed\n");
    return 0;
}
} // namespace

int main()
{
    const int32 result = Run();
    rhi::Shutdown();
    return result;
}
