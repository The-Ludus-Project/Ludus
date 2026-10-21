#include "../src/internal/render_graph_ray_scene.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace internal = renderer::internal;
namespace rhi = ludus::graphics::rhi;
namespace
{
constexpr usize VERTICES = 4;
constexpr usize INDICES = 6;
constexpr usize MATERIALS = 2;
constexpr usize BUDGET = 1024 * 1024;
struct SceneFixture final
{
    StaticArray<math::Vector3, VERTICES> Positions{{{0, 0, .5F}, {1, 0, .5F}, {0, 1, .5F}, {1, 1, .5F}}};
    StaticArray<uint32, INDICES> Indices{{0, 1, 2, 1, 3, 2}};
    StaticArray<uint32, MATERIALS> Slots{{0, 1}};
    StaticArray<uint32, MATERIALS> Bindings{{0, 1}};
    StaticArray<renderer::SceneShaderMaterial, MATERIALS> Materials{};
    renderer::RenderGraphMesh Mesh;
    renderer::RenderGraphItem Item;
    renderer::RenderGraphSceneDescription Scene;
    renderer::ReferenceCaptureDescription Camera;
    SceneFixture()
    {
        Mesh.Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Mesh.MaterialSlots = {Slots.GetData(), Slots.GetSize()};
        Mesh.MaterialBindings = {Bindings.GetData(), Bindings.GetSize()};
        Item.SourceId = 73;
        Materials[0].BaseColor = {1, 0, 0};
        Materials[1].BaseColor = {0, 1, 0};
        Materials[1].Emission = {2, 3, 4};
        Scene.Meshes = {&Mesh, 1};
        Scene.Items = {&Item, 1};
        Scene.Inputs.Materials = {Materials.GetData(), Materials.GetSize()};
        Scene.HasMaterialInterface = true;
        Camera.OrthographicCamera = true;
        Camera.Width = 16;
        Camera.Height = 16;
    }
};
} // namespace
TEST_CASE("Graph ray jobs initialize their random key from the requested seed", "[scene-shader][random]")
{
    SceneFixture fixture;
    internal::ReferenceMeshData mesh;
    for (const math::Vector3 position : fixture.Positions)
    {
        REQUIRE(mesh.Positions.TryAdd(position) == true);
    }
    for (const uint32 index : fixture.Indices)
    {
        REQUIRE(mesh.Indices.TryAdd(index) == true);
    }
    const internal::GraphRaySceneInput input{&mesh, {}, {1, 1, 1}, 1};
    constexpr uint64 SEED_COUNT = 2;
    uint64 previousKey = 0;
    for (uint64 seed = 0; seed < SEED_COUNT; ++seed)
    {
        fixture.Camera.Seed = seed;
        math::RandomKey expectedKey;
        REQUIRE(math::TryMakeRandomKey(seed, internal::REFERENCE_RANDOM_DOMAIN, expectedKey) ==
                math::MathStatus::Success);
        core::UniquePtr<internal::ReferenceJob> job;
        REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
                rhi::RasterStatus::Ready);
        CHECK(job->Key.Value == expectedKey.Value);
        job.Reset();
        REQUIRE(internal::BuildGraphRayScene({&input, 1}, fixture.Camera, BUDGET, job) == rhi::RasterStatus::Ready);
        CHECK(job->Key.Value == expectedKey.Value);
        if (seed != 0)
        {
            CHECK(expectedKey.Value != previousKey);
        }
        previousKey = expectedKey.Value;
    }
}
TEST_CASE("Scene shader triangles resolve independent mesh slots and instance overrides", "[scene-shader]")
{
    SceneFixture fixture;
    core::UniquePtr<internal::ReferenceJob> job;
    REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
            rhi::RasterStatus::Ready);
    REQUIRE(job->Scene.Triangles.GetSize() == 2);
    CHECK(job->Scene.Triangles[0].ShaderMaterial == 0);
    CHECK(job->Scene.Triangles[1].ShaderMaterial == 1);
    CHECK(job->Scene.Triangles[1].Primitive == 1);
    CHECK(job->Scene.Triangles[1].ShaderSourceId == 73);
    REQUIRE(job->ShaderEmitters.GetSize() == 1);
    CHECK(job->ShaderEmitters[0].Z == .5F);
    CHECK(job->Scene.Emitters.GetSize() == 1);
    CHECK(job->FlatSurfaces[1].Emission.Y == 3);
    job.Reset();
    StaticArray<uint32, MATERIALS> overrides{{1, renderer::INHERIT_SCENE_MATERIAL}};
    fixture.Item.MaterialOverrides = {overrides.GetData(), overrides.GetSize()};
    REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
            rhi::RasterStatus::Ready);
    CHECK(job->Scene.Triangles[0].ShaderMaterial == 1);
    CHECK(job->Scene.Triangles[1].ShaderMaterial == 1);
}
TEST_CASE("Missing materials use neutral default while broken bindings require explicit diagnostic admission",
          "[scene-shader]")
{
    SceneFixture fixture;
    fixture.Bindings[0] = renderer::DEFAULT_SCENE_MATERIAL;
    core::UniquePtr<internal::ReferenceJob> job;
    REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
            rhi::RasterStatus::Ready);
    CHECK(job->FlatSurfaces[0].Bsdf.BaseColor.X == .5F);
    CHECK(job->FlatSurfaces[0].Bsdf.Roughness == 1);
    CHECK(job->FlatSurfaces[0].Emission.X == 0);
    CHECK(job->Scene.Triangles[0].ShaderMaterialKind == 1);
    job.Reset();
    fixture.Bindings[0] = 37;
    CHECK(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(job.Get() == nullptr);
    fixture.Scene.AreErrorsAllowed = true;
    fixture.Bindings[1] = 38;
    renderer::SceneShaderDiagnostics diagnostics;
    fixture.Scene.DiagnosticsOrNull = &diagnostics;
    REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
            rhi::RasterStatus::Ready);
    CHECK(job->FlatSurfaces[0].Bsdf.BaseColor.X == 1);
    CHECK(job->FlatSurfaces[0].Bsdf.BaseColor.Y == 0);
    CHECK(job->FlatSurfaces[0].Bsdf.BaseColor.Z == 1);
    CHECK(job->FlatSurfaces[0].Emission.X == 0);
    CHECK(job->Scene.Triangles[0].ShaderMaterialKind == 2);
    CHECK(diagnostics.ErrorBindings == 2);
    CHECK(diagnostics.ErrorMaterial == 37);
    CHECK(diagnostics.ErrorSourceId == fixture.Item.SourceId);
}
TEST_CASE("Scene materials retain texture bytes and analytic lights without geometry", "[scene-shader]")
{
    SceneFixture fixture;
    StaticArray<uint8, 4> pixels{{255, 0, 0, 255}};
    fixture.Materials[0].ColorTexture = {pixels.GetData(), pixels.GetSize(), 4, 1, 1, rhi::RasterFormat::Rgba8Srgb};
    StaticArray<renderer::DirectLight, 3> lights{};
    lights[0].SourceId = 1;
    lights[0].Kind = renderer::LightKind::Point;
    lights[1].SourceId = 2;
    lights[1].Kind = renderer::LightKind::Directional;
    lights[2].SourceId = 3;
    lights[2].Kind = renderer::LightKind::Spot;
    fixture.Scene.Inputs.Lights = {lights.GetData(), lights.GetSize()};
    core::UniquePtr<internal::ReferenceJob> job;
    REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
            rhi::RasterStatus::Ready);
    CHECK(job->Description.LightCount == 3);
    CHECK(job->Lights[0].Kind == renderer::LightKind::Point);
    CHECK(job->Lights[1].Kind == renderer::LightKind::Directional);
    CHECK(job->Lights[2].Kind == renderer::LightKind::Spot);
    CHECK(job->Scene.Instances[0].Texture.Bytes != pixels.GetData());
    pixels[0] = 0;
    CHECK(job->Scene.Instances[0].Texture.Bytes[0] == 255);
}
TEST_CASE("Emitter construction skips collapsed faces and computes transformed area", "[scene-shader]")
{
    SceneFixture fixture;
    fixture.Indices[0] = fixture.Indices[1];
    fixture.Item.MeshToWorld.Columns[0].X = 2;
    core::UniquePtr<internal::ReferenceJob> job;
    REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
            rhi::RasterStatus::Ready);
    REQUIRE(job->Scene.Triangles.GetSize() == 1);
    CHECK(job->Scene.Triangles[0].Primitive == 1);
    CHECK(job->ShaderEmitters[0].Z == 1);
    job.Reset();
    fixture.Item.MeshToWorld.Columns[0].X = -2;
    REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
            rhi::RasterStatus::Ready);
    CHECK(job->Scene.Triangles[0].GeometricNormal.Z == 1);
    CHECK(job->ShaderEmitters[0].Z == 1);
    job.Reset();
    CHECK(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, 1, job) ==
          rhi::RasterStatus::CapacityExceeded);
    CHECK(job.Get() == nullptr);
    fixture.Mesh.MaterialSlots = {fixture.Slots.GetData(), 1};
    CHECK(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
          rhi::RasterStatus::InvalidDescription);
}

TEST_CASE("Ray scene admission reports required and available CPU bytes and clears stale diagnostics", "[scene-shader]")
{
    SceneFixture fixture;
    renderer::SceneShaderDiagnostics diagnostics;
    fixture.Scene.DiagnosticsOrNull = &diagnostics;
    core::UniquePtr<internal::ReferenceJob> job;
    REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, 1, job) ==
            rhi::RasterStatus::CapacityExceeded);
    CHECK(job.Get() == nullptr);
    CHECK(diagnostics.ExceededBudget == renderer::SceneMemoryBudget::CpuScene);
    CHECK(diagnostics.RequiredBytes > sizeof(internal::ReferenceTriangle) * 2);
    CHECK(diagnostics.AvailableBytes == 1);
    const usize requiredBytes = static_cast<usize>(diagnostics.RequiredBytes);
    REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, requiredBytes - 1, job) ==
            rhi::RasterStatus::CapacityExceeded);
    CHECK(diagnostics.RequiredBytes == requiredBytes);
    CHECK(diagnostics.AvailableBytes == requiredBytes - 1);
    REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, requiredBytes, job) ==
            rhi::RasterStatus::Ready);
    CHECK(diagnostics.ExceededBudget == renderer::SceneMemoryBudget::None);
    CHECK(diagnostics.RequiredBytes == 0);
    CHECK(diagnostics.AvailableBytes == 0);
    CHECK(job->Scene.Triangles.GetSize() == 2);
}

TEST_CASE("Ray CPU byte estimation rejects overflow before reading triangle indices", "[scene-shader]")
{
    SceneFixture fixture;
    constexpr usize CORNERS_PER_TRIANGLE = 3;
    fixture.Mesh.Geometry.IndexCount = (~usize{0} / CORNERS_PER_TRIANGLE) * CORNERS_PER_TRIANGLE;
    fixture.Mesh.MaterialSlots = {};
    core::UniquePtr<internal::ReferenceJob> job;
    CHECK(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
          rhi::RasterStatus::CapacityExceeded);
    CHECK(job.Get() == nullptr);
}

TEST_CASE("Unassigned higher mesh slots retain defaults and expose candidate diagnostics", "[scene-shader]")
{
    SceneFixture fixture;
    fixture.Mesh.MaterialBindings = {};
    fixture.Slots[1] = 3;
    renderer::SceneShaderDiagnostics diagnostics;
    fixture.Scene.DiagnosticsOrNull = &diagnostics;
    core::UniquePtr<internal::ReferenceJob> job;
    REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
            rhi::RasterStatus::Ready);
    REQUIRE(job->Scene.Triangles.GetSize() == 2);
    CHECK(job->Scene.Triangles[1].ShaderSlot == 3);
    CHECK(job->Scene.Triangles[1].ShaderMaterialKind == 1);
    CHECK(diagnostics.DefaultBindings == 2);
    CHECK(diagnostics.ErrorBindings == 0);
    CHECK(job->ShaderEmitters.GetSize() == 0);
}
TEST_CASE("HDR environments are copied and mesh power resolves using placed area", "[scene-shader]")
{
    SceneFixture fixture;
    math::Vector3 pixel{1, 2, 3};
    fixture.Scene.Inputs.Environment = {1, 1, 1};
    fixture.Scene.Inputs.EnvironmentWidth = 1;
    fixture.Scene.Inputs.EnvironmentHeight = 1;
    fixture.Scene.Inputs.EnvironmentPixels = {&pixel, 1};
    core::UniquePtr<internal::ReferenceJob> job;
    REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
            rhi::RasterStatus::Ready);
    REQUIRE(job->EnvironmentOrNull != nullptr);
    CHECK(job->EnvironmentOrNull->Pixels[0].Y == 2);
    pixel.Y = 0;
    CHECK(job->EnvironmentOrNull->Pixels[0].Y == 2);
    math::Vector3 emission;
    REQUIRE(renderer::ResolveMeshEmissionPower({1, 1, 1}, static_cast<float32>(math::kPi), 1, false, emission) ==
            rhi::RasterStatus::Ready);
    CHECK(math::Abs(emission.X - 1) < 1e-6F);
    REQUIRE(renderer::ResolveMeshEmissionPower({1, 1, 1}, static_cast<float32>(math::kPi), 1, true, emission) ==
            rhi::RasterStatus::Ready);
    CHECK(math::Abs(emission.X - .5F) < 1e-6F);
    CHECK(renderer::ResolveMeshEmissionPower({1, 1, 1}, 1, 0, false, emission) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(math::Abs(emission.X - .5F) < 1e-6F);
}

TEST_CASE("Scene bundle selection retains geometry and omits unrequested resources", "[scene-shader][bundle]")
{
    SceneFixture fixture;
    StaticArray<uint8, 4> pixels{{255, 0, 0, 255}};
    fixture.Materials[0].ColorTexture = {pixels.GetData(), pixels.GetSize(), 4, 1, 1, rhi::RasterFormat::Rgba8Srgb};
    renderer::DirectLight light;
    light.SourceId = 1;
    light.Kind = renderer::LightKind::Point;
    fixture.Scene.Inputs.Lights = {&light, 1};
    fixture.Scene.Inputs.Environment = {1, 1, 1};
    core::UniquePtr<internal::ReferenceJob> job;
    SECTION("Geometry only ignores invalid material and light inputs")
    {
        fixture.Materials[0].HasError = true;
        fixture.Bindings[0] = 37;
        light.SourceId = 0;
        fixture.Scene.SceneData = ::ludus::content::render_graph_format::SCENE_DATA_GEOMETRY;
        REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
                rhi::RasterStatus::Ready);
        CHECK(job->Scene.Triangles.GetSize() == 2);
        CHECK(job->Scene.Triangles[1].Primitive == 1);
        CHECK(job->FlatSurfaces[0].HasTexture == false);
        CHECK(job->Description.LightCount == 0);
        CHECK(job->Scene.Emitters.GetSize() == 0);
        CHECK(job->Description.Environment.X == 0);
        CHECK(job->SceneData == ::ludus::content::render_graph_format::SCENE_DATA_GEOMETRY);
    }
    SECTION("Materials include texture and emission but no light sampling resources")
    {
        fixture.Scene.SceneData = ::ludus::content::render_graph_format::SCENE_DATA_GEOMETRY |
                                  ::ludus::content::render_graph_format::SCENE_DATA_MATERIALS;
        REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
                rhi::RasterStatus::Ready);
        CHECK(job->FlatSurfaces[0].HasTexture == true);
        CHECK(job->FlatSurfaces[1].Emission.Y == 3);
        CHECK(job->Description.LightCount == 0);
        CHECK(job->Scene.Emitters.GetSize() == 0);
        CHECK(job->Description.Environment.X == 0);
    }
    SECTION("Lights retain emission dependencies without surface maps")
    {
        fixture.Scene.SceneData = ::ludus::content::render_graph_format::SCENE_DATA_GEOMETRY |
                                  ::ludus::content::render_graph_format::SCENE_DATA_LIGHTS;
        REQUIRE(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
                rhi::RasterStatus::Ready);
        CHECK(job->FlatSurfaces[0].HasTexture == false);
        CHECK(job->FlatSurfaces[1].Bsdf.BaseColor.Y == 0);
        CHECK(job->FlatSurfaces[1].Emission.Y == 3);
        CHECK(job->Description.LightCount == 1);
        CHECK(job->Scene.Emitters.GetSize() == 1);
        CHECK(job->Description.Environment.X == 1);
    }
    SECTION("Invalid selection preserves the output")
    {
        fixture.Scene.SceneData = ::ludus::content::render_graph_format::SCENE_DATA_MATERIALS;
        CHECK(internal::BuildMaterialGraphRayScene(fixture.Scene, fixture.Camera, BUDGET, job) ==
              rhi::RasterStatus::InvalidDescription);
        CHECK(job.Get() == nullptr);
    }
}
