#pragma once

// Shared fixed-view RT2 qualification scene for the in-tree test and installed SDK consumer.
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/scene_shader.hpp>

namespace ludus::tests::hybrid_diffuse
{
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;

inline constexpr usize RECEIVER = 0;
inline constexpr usize REFLECTOR = 1;
inline constexpr usize EMITTER = 2;
inline constexpr usize BLOCKER = 3;
inline constexpr usize QUAD_COUNT = 4;
inline constexpr usize CORNER_COUNT = 4;
inline constexpr usize INDEX_COUNT = 6;
using Corners = StaticArray<math::Vector3, CORNER_COUNT>;
inline constexpr StaticArray<uint32, INDEX_COUNT> FORWARD_INDICES{{0, 1, 2, 0, 2, 3}};
inline constexpr StaticArray<uint32, INDEX_COUNT> BACKWARD_INDICES{{0, 2, 1, 0, 3, 2}};

inline constexpr math::Vector3 RECEIVER_ALBEDO{.75F, .75F, .75F};
inline constexpr math::Vector3 REFLECTOR_ALBEDO{.8F, .3F, .12F};

struct SceneState final
{
    float32 EmitterRadiance;
    float32 BlockerCenterWorld;
};
inline constexpr usize STATE_COUNT = 3;
inline constexpr StaticArray<SceneState, STATE_COUNT> STATES{{{2, 2.3F}, {8, 2.3F}, {8, .7F}}};

LUDUS_INLINE Corners MakeQuad(float32 xMinimum, float32 xMaximum, float32 halfHeight, float32 depthWorld)
{
    return {{{xMinimum, -halfHeight, depthWorld},
             {xMaximum, -halfHeight, depthWorld},
             {xMaximum, halfHeight, depthWorld},
             {xMinimum, halfHeight, depthWorld}}};
}

// The orthographic camera only sees the receiver, which faces -Z. The colored reflector at z < 0 is outside
// its clipping range. The emitter sits to the side of the camera frustum and faces -Z, away from the receiver.
// The black blocker can intercept both legs of paths that reach the receiver via the reflector.
struct Room final
{
    StaticArray<Corners, QUAD_COUNT> Positions{{MakeQuad(-1.1F, 1.1F, 1.1F, .5F),
                                                MakeQuad(-1.6F, 2.1F, 1.6F, -.4F),
                                                MakeQuad(1.25F, 1.95F, .4F, .2F),
                                                MakeQuad(-.19F, .19F, .45F, -.12F)}};
    Corners FrontNormals{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    Corners BackNormals{{{0, 0, -1}, {0, 0, -1}, {0, 0, -1}, {0, 0, -1}}};
    StaticArray<uint32, QUAD_COUNT> Bindings{{RECEIVER, REFLECTOR, EMITTER, BLOCKER}};
    StaticArray<renderer::RenderGraphMesh, QUAD_COUNT> Meshes{};
    StaticArray<renderer::RenderGraphItem, QUAD_COUNT> Items{};
    StaticArray<renderer::SceneShaderMaterial, QUAD_COUNT> Materials{};
    renderer::ViewDescription Camera{};
    renderer::SceneShaderInputs Inputs{};
    renderer::RenderGraphSceneRevisions Revisions{1, 1, 1, 1, 1};

    Room()
    {
        for (usize quad = 0; quad < QUAD_COUNT; ++quad)
        {
            const StaticArray<uint32, INDEX_COUNT>& indices = quad == REFLECTOR ? FORWARD_INDICES : BACKWARD_INDICES;
            Meshes[quad].Geometry = {Positions[quad].GetData(),
                                     Positions[quad].GetSize(),
                                     indices.GetData(),
                                     indices.GetSize()};
            Meshes[quad].Geometry.Normals = quad == REFLECTOR ? FrontNormals.GetData() : BackNormals.GetData();
            Meshes[quad].MaterialBindings = {&Bindings[quad], 1};
            Items[quad].SourceId = quad + 1;
            Items[quad].MeshIndex = static_cast<uint32>(quad);
            Materials[quad].Scattering.Model = ludus::content::scene_format::MaterialModel::Diffuse;
            Materials[quad].BaseColor = {};
            Materials[quad].Metallic = 0;
        }
        Materials[RECEIVER].BaseColor = RECEIVER_ALBEDO;
        Materials[REFLECTOR].BaseColor = REFLECTOR_ALBEDO;
        Materials[EMITTER].IsEmissionTwoSided = false;
        Camera.OrthographicCamera = true;
        Camera.CameraForward = {0, 0, 1};
        Camera.CameraPosition = {0, 0, -1};
        Inputs.Materials = Materials;
    }

    Room(const Room&) = delete;
    Room& operator=(const Room&) = delete;

    void Apply(const SceneState& state)
    {
        if (Materials[EMITTER].Emission.X != state.EmitterRadiance)
        {
            Materials[EMITTER].Emission = {state.EmitterRadiance, state.EmitterRadiance, state.EmitterRadiance};
            ++Revisions.Materials;
        }
        if (Items[BLOCKER].MeshToWorld.Translation.X != state.BlockerCenterWorld)
        {
            Items[BLOCKER].MeshToWorld.Translation.X = state.BlockerCenterWorld;
            ++Revisions.Instances;
        }
    }
};

} // namespace ludus::tests::hybrid_diffuse
