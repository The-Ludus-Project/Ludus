// The World inspector's read-only Material rows describe the decoded material a scene object uses.

#include "internal/material_inspection.h"
#include "internal/render_scene.h"

#include <ludus/foundation/base/types.h>

#include <ludus/content/scene_format/luscn.h>
#include <ludus/content/scene_format/material.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/memory/shared_ptr.hpp>
#include <ludus/runtime/resources/scene.h>

#include <QLatin1StringView>
#include <QString>
#include <QVector>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::editor;

namespace
{
namespace format = ludus::content::scene_format;
using ludus::foundation::usize;

constexpr QLatin1StringView CONDUCTOR_ID{"00000000-0000-4000-8000-000000000001"};
constexpr QLatin1StringView COATED_ID{"00000000-0000-4000-8000-000000000002"};
constexpr QLatin1StringView LEGACY_ID{"00000000-0000-4000-8000-000000000003"};
/// Objects, and materials, in MakeScene.
constexpr usize OBJECT_COUNT = 3;

/// Value of the row labelled label, or a null string when rows has none.
QString FindValue(const QVector<MaterialPropertyRow>& rows, const QString& label)
{
    for (const MaterialPropertyRow& row : rows)
    {
        if (row.Label == label)
        {
            return row.Value;
        }
    }
    return {};
}
/// A cooked scene of three objects: a named textured conductor, an emissive masked coated diffuse
/// with a surface map, and an untextured legacy material.
RenderScene MakeScene()
{
    using namespace ludus::foundation;
    SharedPtr<ludus::resources::SceneDefinition> definition;
    REQUIRE(TryMakeShared<ludus::resources::SceneDefinition>(GetSystemAllocationDomain(), definition) ==
            OwnershipStatus::Ok);
    REQUIRE(definition->Textures.TryResize(2) == true);
    definition->Textures[0].Width = 2;
    definition->Textures[0].Height = 1;
    definition->Textures[1].Width = 4;
    definition->Textures[1].Height = 4;
    definition->Textures[1].IsLinear = true;
    REQUIRE(definition->Materials.TryResize(OBJECT_COUNT) == true);

    ludus::resources::SceneMaterial& conductor = definition->Materials[0];
    REQUIRE(conductor.Name.Set("brushed_copper") == true);
    conductor.HasSourceMaterial = true;
    conductor.Reflectance = {.5F, .25F, 1};
    conductor.Texture = 0;
    conductor.Repeat = true;
    conductor.Metallic = 1;
    conductor.Roughness = .3F;
    conductor.Scattering.Model = format::MaterialModel::Conductor;
    conductor.Scattering.Eta = {{.2F, .9F, 1.1F}};
    conductor.Scattering.K = {{3.9F, 2.4F, 2.2F}};
    conductor.Scattering.AlphaU = .09F;
    conductor.Scattering.AlphaV = .09F;

    ludus::resources::SceneMaterial& coated = definition->Materials[1];
    coated.HasSourceMaterial = true;
    coated.Scattering.Model = format::MaterialModel::CoatedDiffuse;
    coated.Scattering.CoatingEta = 1.5F;
    coated.Scattering.CoatingThickness = .01F;
    coated.Scattering.AlphaU = .001F;
    coated.Scattering.AlphaV = .001F;
    coated.SurfaceTexture = 1;
    coated.NormalScale = 2;
    coated.IsAlphaMasked = true;
    coated.AlphaCutoff = .5F;
    coated.Emission = {10, 20, 0};
    coated.IsEmissionTwoSided = false;
    coated.HasEmissionPower = true;
    coated.EmissionPower = 800;

    ludus::resources::SceneMaterial& legacy = definition->Materials[2];
    legacy.Texture = format::NO_TEXTURE;

    RenderScene scene;
    scene.Definition = definition;
    const StaticArray<QLatin1StringView, OBJECT_COUNT> ids{{CONDUCTOR_ID, COATED_ID, LEGACY_ID}};
    for (usize index = 0; index < OBJECT_COUNT; ++index)
    {
        PreviewObject object;
        object.Id = QString(ids[index]);
        object.Surface = static_cast<ludus::foundation::int32>(index);
        scene.Objects.append(object);
    }
    return scene;
}
} // namespace

TEST_CASE("Material inspection follows the scattering model of the selected object", "[editor][world][material]")
{
    const RenderScene scene = MakeScene();
    QVector<MaterialPropertyRow> rows;
    REQUIRE(DescribeSceneObjectMaterial(scene, QString(CONDUCTOR_ID), rows) == true);
    CHECK(FindValue(rows, QStringLiteral("Name")) == QStringLiteral("brushed_copper"));
    CHECK(FindValue(rows, QStringLiteral("Model")) == QStringLiteral("Conductor"));
    CHECK(FindValue(rows, QStringLiteral("Reflectance (linear RGB)")) == QStringLiteral("0.5, 0.25, 1"));
    CHECK(FindValue(rows, QStringLiteral("Base color map")) == QStringLiteral("Texture 0 · 2 × 1 · sRGB"));
    CHECK(FindValue(rows, QStringLiteral("Addressing")) == QStringLiteral("Repeat"));
    CHECK(FindValue(rows, QStringLiteral("Metallic")) == QStringLiteral("1"));
    CHECK(FindValue(rows, QStringLiteral("Roughness (perceptual)")) == QStringLiteral("0.3"));
    CHECK(FindValue(rows, QStringLiteral("IOR η (RGB)")) == QStringLiteral("0.2, 0.9, 1.1"));
    CHECK(FindValue(rows, QStringLiteral("Extinction k (RGB)")) == QStringLiteral("3.9, 2.4, 2.2"));
    CHECK(FindValue(rows, QStringLiteral("GGX α (U, V)")) == QStringLiteral("0.09, 0.09"));
    CHECK(FindValue(rows, QStringLiteral("Coating IOR")).isNull() == true);
    CHECK(FindValue(rows, QStringLiteral("Surface map")) == QStringLiteral("None"));
    CHECK(FindValue(rows, QStringLiteral("Normal scale")).isNull() == true);
    CHECK(FindValue(rows, QStringLiteral("Alpha")) == QStringLiteral("Opaque"));
    CHECK(FindValue(rows, QStringLiteral("Emission (linear RGB)")) == QStringLiteral("0, 0, 0"));
    CHECK(FindValue(rows, QStringLiteral("Emitting sides")).isNull() == true);

    REQUIRE(DescribeSceneObjectMaterial(scene, QString(COATED_ID), rows) == true);
    CHECK(FindValue(rows, QStringLiteral("Name")) == QStringLiteral("(unnamed)"));
    CHECK(FindValue(rows, QStringLiteral("Model")) == QStringLiteral("Coated diffuse"));
    CHECK(FindValue(rows, QStringLiteral("Base color map")) == QStringLiteral("None"));
    CHECK(FindValue(rows, QStringLiteral("Addressing")).isNull() == true);
    CHECK(FindValue(rows, QStringLiteral("IOR η (RGB)")).isNull() == true);
    CHECK(FindValue(rows, QStringLiteral("Coating IOR")) == QStringLiteral("1.5"));
    CHECK(FindValue(rows, QStringLiteral("Coating thickness")) == QStringLiteral("0.01"));
    CHECK(FindValue(rows, QStringLiteral("Coating GGX α (U, V)")) == QStringLiteral("0.001, 0.001"));
    CHECK(FindValue(rows, QStringLiteral("Surface map")) == QStringLiteral("Texture 1 · 4 × 4 · linear"));
    CHECK(FindValue(rows, QStringLiteral("Normal scale")) == QStringLiteral("2"));
    CHECK(FindValue(rows, QStringLiteral("Alpha")) == QStringLiteral("Masked below 0.5"));
    CHECK(FindValue(rows, QStringLiteral("Emission (linear RGB)")) == QStringLiteral("10, 20, 0"));
    CHECK(FindValue(rows, QStringLiteral("Emitting sides")) == QStringLiteral("Front"));
    CHECK(FindValue(rows, QStringLiteral("Emission power (lm)")) == QStringLiteral("800"));
    CHECK(FindValue(rows, QStringLiteral("Emission map")) == QStringLiteral("None"));

    // A legacy material without source parameters renders black when it has no texture.
    REQUIRE(DescribeSceneObjectMaterial(scene, QString(LEGACY_ID), rows) == true);
    CHECK(FindValue(rows, QStringLiteral("Model")) == QStringLiteral("Metallic-roughness"));
    CHECK(FindValue(rows, QStringLiteral("Reflectance (linear RGB)")) == QStringLiteral("0, 0, 0"));
}

TEST_CASE("Material inspection preserves output without a cooked definition or object", "[editor][world][material]")
{
    const QVector<MaterialPropertyRow> previous = {{QStringLiteral("Model"), QStringLiteral("Diffuse")}};
    QVector<MaterialPropertyRow> rows = previous;
    RenderScene scene = MakeScene();
    CHECK(DescribeSceneObjectMaterial(scene, QStringLiteral("missing"), rows) == false);
    CHECK(rows.size() == previous.size());
    CHECK(rows[0].Value == previous[0].Value);
    scene.Objects[0].Surface = 7;
    CHECK(DescribeSceneObjectMaterial(scene, QString(CONDUCTOR_ID), rows) == false);
    scene.Definition = {};
    CHECK(DescribeSceneObjectMaterial(scene, QString(COATED_ID), rows) == false);
    CHECK(rows[0].Value == previous[0].Value);
}
