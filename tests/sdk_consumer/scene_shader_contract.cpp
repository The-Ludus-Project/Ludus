#include "rg2_line_visibility_loop.h"
#include "rg2_scene_compute.h"
#include "rg2_scene_ray.h"

#include <ludus/graphics/renderer/scene_shader.hpp>

namespace renderer = ludus::graphics::renderer;

int main()
{
    // Compile/link through installed exports and verify the generated runtime profile.
    const renderer::RenderGraphPrograms programs = ludus::render_graph_packages::rg2_scene_ray::MakePrograms({}, {});
    // A scene compute pass cooks into the same native ray kernel, always with the persistent wrapper.
    const renderer::RenderGraphPrograms compute = ludus::render_graph_packages::rg2_scene_compute::MakePrograms({}, {});
    // A triangle-loop kernel calling the line visibility query cooks against the installed scene helper.
    const renderer::RenderGraphPrograms lines =
        ludus::render_graph_packages::rg2_line_visibility_loop::MakePrograms({}, {});
    static_assert(renderer::SCENE_LINE_INTERVAL_CAPACITY == LUDUS_SCENE_LINE_INTERVAL_CAPACITY);
    static_assert(renderer::SCENE_LINE_INTERVAL_MERGE_TOLERANCE > 0 &&
                  renderer::SCENE_LINE_INTERVAL_MERGE_TOLERANCE < 1);
    return programs.HasSceneShaderInterface == true && lines.HasSceneShaderInterface == true &&
                   lines.HasPersistentRayBuffers == true &&
                   lines.RayCompute.Artifact.Stage == ludus::graphics::rhi::ShaderStage::Compute &&
                   programs.RayCompute.Artifact.Stage == ludus::graphics::rhi::ShaderStage::Compute &&
                   compute.HasSceneShaderInterface == true && compute.HasPersistentRayBuffers == true &&
                   compute.RayCompute.Artifact.Stage == ludus::graphics::rhi::ShaderStage::Compute
               ? 0
               : 1;
}
