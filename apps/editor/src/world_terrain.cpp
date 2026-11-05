#include "internal/world_terrain.h"

#include "internal/render_scene.h"
#include "internal/world_document.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/addressed_random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <QJsonArray>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace math = foundation::math;

constexpr int32 TERRAIN_RECIPE_VERSION = 1;
constexpr uint32 TERRAIN_RANDOM_DOMAIN = 1;
constexpr uint32 TERRAIN_OCTAVES = 3;
constexpr uint32 INDICES_PER_CELL = 6;
constexpr uint32 COORDINATE_BITS = 32;
/// V1 lattice addresses use signed 32-bit coordinates and sample the next lattice point too.
constexpr float64 MAX_LATTICE_POSITION = 2147483646;
/// Absolute allocation ceiling of one generated mesh. The schema exposes at most its square root.
constexpr uint32 MAX_TERRAIN_MESH_VERTICES = 4096;

struct TerrainShape final
{
    float64 Minimum = 0;
    float64 Maximum = 0;
    float64 FeatureSize = 1;
    float64 Roughness = 0;
    bool IsFlat = false;
    bool IsRidged = false;
};

// Pack the two signed lattice coordinates as their unsigned bit patterns.
// The octave gets its own address, so resolution and traversal never change a sample.
float64 ComputeGradient(math::RandomKey key, uint32 octave, int32 x, int32 z, float64 offsetX, float64 offsetZ)
{
    const uint64 scope = (static_cast<uint64>(static_cast<uint32>(x)) << COORDINATE_BITS) | static_cast<uint32>(z);
    const uint32 gradient = math::SampleUInt32(key, { .Scope = scope, .Event = octave });
    return ((gradient & 1U) == 0 ? offsetX : -offsetX) + ((gradient & 2U) == 0 ? offsetZ : -offsetZ);
}

// This bounded editor recipe uses the global lattice and detail octaves from
// docs/architecture/terrain-generation.md. The lattice is independent of the mesh grid.
float64 SampleTerrainNoise(math::RandomKey key, uint32 octave, float64 positionX, float64 positionZ)
{
    const int32 x = static_cast<int32>(math::Floor(positionX));
    const int32 z = static_cast<int32>(math::Floor(positionZ));
    const float64 offsetX = positionX - x;
    const float64 offsetZ = positionZ - z;
    const float64 fadeX = math::SmoothStep01(offsetX);
    const float64 fadeZ = math::SmoothStep01(offsetZ);
    const float64 near = math::Lerp(ComputeGradient(key, octave, x, z, offsetX, offsetZ),
                                    ComputeGradient(key, octave, x + 1, z, offsetX - 1, offsetZ),
                                    fadeX);
    const float64 far = math::Lerp(ComputeGradient(key, octave, x, z + 1, offsetX, offsetZ - 1),
                                   ComputeGradient(key, octave, x + 1, z + 1, offsetX - 1, offsetZ - 1),
                                   fadeX);
    return math::Clamp(math::Lerp(near, far, fadeZ), -1.0, 1.0);
}

float64 ComputeElevation(const TerrainShape& shape, math::RandomKey key, float64 positionX, float64 positionZ)
{
    if (shape.IsFlat == true || shape.Minimum == shape.Maximum)
    {
        return shape.Minimum;
    }
    float64 frequency = 1 / shape.FeatureSize;
    float64 amplitude = 1;
    float64 totalWeight = 0;
    float64 elevation = 0;
    for (uint32 octave = 0; octave < TERRAIN_OCTAVES; ++octave)
    {
        const float64 noise = SampleTerrainNoise(key, octave, positionX * frequency, positionZ * frequency);
        elevation += amplitude * (shape.IsRidged == true ? 1 - math::Abs(noise) : (noise + 1) * 0.5);
        totalWeight += amplitude;
        amplitude *= shape.Roughness;
        frequency *= 2;
    }
    return math::Lerp(shape.Minimum, shape.Maximum, elevation / totalWeight);
}

void BuildTerrainNormals(uint32 samples, PreviewMesh& inoutMesh)
{
    for (uint32 z = 0; z < samples; ++z)
    {
        const uint32 previousZ = z == 0 ? z : z - 1;
        const uint32 nextZ = math::Min(z + 1, samples - 1);
        for (uint32 x = 0; x < samples; ++x)
        {
            const uint32 previousX = x == 0 ? x : x - 1;
            const uint32 nextX = math::Min(x + 1, samples - 1);
            const math::Vector3 tangentX =
                inoutMesh.Positions[z * samples + nextX] - inoutMesh.Positions[z * samples + previousX];
            const math::Vector3 tangentZ =
                inoutMesh.Positions[nextZ * samples + x] - inoutMesh.Positions[previousZ * samples + x];
            math::Vector3 normal{0, 1, 0};
            (void)math::TryNormalize(math::Cross(tangentZ, tangentX), normal);
            inoutMesh.Normals.append(normal);
        }
    }
}
} // namespace

QString GetTerrainComponentId()
{
    return QStringLiteral("terrain");
}

bool ValidateTerrainRecipe(const QJsonObject& recipe, QString& outError)
{
    if (recipe.value(QStringLiteral("version")).toInt() != TERRAIN_RECIPE_VERSION)
    {
        outError = QStringLiteral("This terrain recipe version is unavailable. Open it in a compatible editor.");
        return false;
    }
    if (recipe.value(QStringLiteral("minimumElevation")).toDouble() >
        recipe.value(QStringLiteral("maximumElevation")).toDouble())
    {
        outError = QStringLiteral("Set the maximum elevation at or above the minimum elevation.");
        return false;
    }
    return true;
}

bool BuildTerrainMesh(const QJsonObject& recipe, PreviewMesh& outMesh, QString& outError)
{
    if (ValidateTerrainRecipe(recipe, outError) == false)
    {
        return false;
    }
    // The built-in World schema has already checked all field types and ranges.
    const uint32 samples = static_cast<uint32>(recipe.value(QStringLiteral("samples")).toInt());
    const float64 width = recipe.value(QStringLiteral("width")).toDouble();
    const float64 depth = recipe.value(QStringLiteral("depth")).toDouble();
    const QString preset = recipe.value(QStringLiteral("shape")).toString();
    const TerrainShape shape
    {
        .Minimum = recipe.value(QStringLiteral("minimumElevation")).toDouble(),
        .Maximum = recipe.value(QStringLiteral("maximumElevation")).toDouble(),
        .FeatureSize = recipe.value(QStringLiteral("featureSize")).toDouble(),
        .Roughness = recipe.value(QStringLiteral("roughness")).toDouble(),
        .IsFlat = preset == QStringLiteral("Flat"),
        .IsRidged = preset == QStringLiteral("Ridges"),
    };
    if (samples < 2 || samples > MAX_TERRAIN_MESH_VERTICES / samples)
    {
        outError = QStringLiteral("Choose Samples per side within the supported terrain mesh size.");
        return false;
    }
    constexpr uint32 HIGHEST_OCTAVE_MULTIPLIER = 1U << (TERRAIN_OCTAVES - 1);
    if (width <= 0 || depth <= 0 || shape.FeatureSize <= 0 ||
        math::Max(width, depth) * 0.5 / shape.FeatureSize * HIGHEST_OCTAVE_MULTIPLIER > MAX_LATTICE_POSITION)
    {
        outError = QStringLiteral("Use positive terrain dimensions and increase Feature size for this extent.");
        return false;
    }
    const uint32 cells = samples - 1;
    math::RandomKey key;
    if (math::TryMakeRandomKey(static_cast<uint64>(recipe.value(QStringLiteral("seed")).toInt()),
                               TERRAIN_RANDOM_DOMAIN,
                               key) != math::MathStatus::Success)
    {
        outError = QStringLiteral("Choose a valid terrain seed.");
        return false;
    }

    PreviewMesh mesh;
    mesh.Id = outMesh.Id;
    mesh.Positions.reserve(samples * samples);
    mesh.Uvs.reserve(samples * samples);
    mesh.Normals.reserve(samples * samples);
    mesh.Indices.reserve(INDICES_PER_CELL * cells * cells);
    for (uint32 z = 0; z < samples; ++z)
    {
        const float64 v = static_cast<float64>(z) / cells;
        const float64 positionZ = (v - 0.5) * depth;
        for (uint32 x = 0; x < samples; ++x)
        {
            const float64 u = static_cast<float64>(x) / cells;
            const float64 positionX = (u - 0.5) * width;
            const float64 elevation = ComputeElevation(shape, key, positionX, positionZ);
            mesh.Positions.append(
                {static_cast<float32>(positionX), static_cast<float32>(elevation), static_cast<float32>(positionZ)});
            mesh.Uvs.append({static_cast<float32>(u), static_cast<float32>(v)});
            if (x < cells && z < cells)
            {
                const uint32 corner = z * samples + x;
                const uint32 nextRow = corner + samples;
                mesh.Indices.append(corner);
                mesh.Indices.append(nextRow);
                mesh.Indices.append(corner + 1);
                mesh.Indices.append(corner + 1);
                mesh.Indices.append(nextRow);
                mesh.Indices.append(nextRow + 1);
            }
        }
    }
    BuildTerrainNormals(samples, mesh);
    outMesh = Move(mesh);
    return true;
}

bool EditWorldTerrain(WorldDocument& inoutWorld,
                      const identity::EntropySource& entropy,
                      const QString& key,
                      const QJsonObject& recipe,
                      QString& outKey,
                      QString& outError)
{
    const QString component = GetTerrainComponentId();
    if (inoutWorld.HasBuiltInTerrain() == false)
    {
        outError = QStringLiteral("Open a default World to generate terrain.");
        return false;
    }
    if (ValidateTerrainRecipe(recipe, outError) == false)
    {
        return false;
    }
    if (key.isEmpty() == true)
    {
        QJsonObject transform;
        const QJsonObject definition = inoutWorld.Component(QStringLiteral("transform"));
        for (const QJsonValue value : definition.value(QStringLiteral("properties")).toArray())
        {
            const QJsonObject property = value.toObject();
            transform.insert(property.value(QStringLiteral("id")).toString(),
                             property.value(QStringLiteral("default")));
        }
        return inoutWorld.CreateEntity(entropy,
                                       outError,
                                       QStringLiteral("Terrain"),
                                       outKey,
                                       {{QStringLiteral("transform"), transform}, {component, recipe}});
    }
    const QJsonObject entity = inoutWorld.Entity(key);
    QJsonObject components = entity.value(QStringLiteral("components")).toObject();
    if (components.contains(component) == false)
    {
        outError = QStringLiteral("Select a terrain entity before editing its generation settings.");
        return false;
    }
    components.insert(component, recipe);
    if (inoutWorld.Edit(key, components, entity.value(QStringLiteral("name")).toString(), outError) == false)
    {
        return false;
    }
    outKey = key;
    return true;
}
} // namespace ludus::editor
