#include "rg2_scene_ray.h"

#include <ludus/graphics/renderer/scene_shader.hpp>

namespace renderer = ludus::graphics::renderer;

int main()
{
    // Compile/link through installed exports and verify the generated runtime profile.
    const renderer::RenderGraphPrograms programs = ludus::render_graph_packages::rg2_scene_ray::MakePrograms({}, {});
    return programs.HasSceneShaderInterface == true &&
                   programs.RayCompute.Artifact.Stage == ludus::graphics::rhi::ShaderStage::Compute
               ? 0
               : 1;
}
