#include <ludus/graphics/renderer/world_feature.hpp>

#include <ludus/graphics/renderer/render_graph.hpp>

namespace ludus::graphics::renderer
{
using namespace foundation;
WorldFeatureSupport GetWorldFeatureSupport(const RenderGraphPrograms& programs, WorldFeature feature) noexcept
{
    // The clustered deferred resolve composes the whole Atmosphere (render_graph_atmosphere.cpp). A ray kernel draws a
    // feature only when reflection kept the table its scene function reads, so the flags follow the kernel's calls.
    if (programs.HasClusteredDeferred == true)
    {
        return WorldFeatureSupport::Supported;
    }
    switch (feature)
    {
        case WorldFeature::AtmosphereSky:
            return programs.HasAtmosphereSky == true ? WorldFeatureSupport::Approximated
                                                     : WorldFeatureSupport::NotRendered;
        case WorldFeature::AerialPerspective:
            return programs.HasAerialPerspective == true ? WorldFeatureSupport::Approximated
                                                         : WorldFeatureSupport::NotRendered;
        case WorldFeature::Aurora:
            return WorldFeatureSupport::NotRendered;
    }
    return WorldFeatureSupport::NotRendered;
}
} // namespace ludus::graphics::renderer
