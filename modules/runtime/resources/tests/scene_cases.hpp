#pragma once

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/runtime/resources/scene.h>

#include <cstring>

namespace
{
Array<uint8> SceneFixture(uint32 version = 1)
{
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(version == 2 ? 268 : 264));
    usize offset = 0;
    auto integer = [&](uint32 value) {
        for (usize i = 0; i < 4; ++i)
        {
            bytes[offset++] = static_cast<uint8>(value >> (i * 8));
        }
    };
    auto number = [&](float32 value) {
        uint32 bits = 0;
        std::memcpy(&bits, &value, 4);
        integer(bits);
    };
    std::memcpy(bytes.GetData(), "LUSCN01\0", 8);
    offset = 8;
    for (const uint32 value : {version, 1U, 1U, 0U, 1U})
    {
        integer(value);
    }
    for (const float32 value : {0.0F, 0.0F, 3.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F})
    {
        number(value);
    }
    integer(~uint32{0});
    if (version == 2)
    {
        integer(1);
    }
    for (const float32 value : {0.0F, .5F, 1.0F, 1.0F, 1.0F})
    {
        number(value);
    }
    integer(3);
    integer(3);
    for (const auto position : {ludus::foundation::math::Vector3{0, 0, 0},
                                ludus::foundation::math::Vector3{1, 0, 0},
                                ludus::foundation::math::Vector3{0, 1, 0}})
    {
        for (const float32 value : {position.X, position.Y, position.Z, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F})
        {
            number(value);
        }
    }
    integer(0);
    integer(1);
    integer(2);
    integer(0);
    integer(0);
    for (const float32 value : {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F})
    {
        number(value);
    }
    REQUIRE(offset == bytes.GetSize());
    return bytes;
}
} // namespace
TEST_CASE("Cooked scene decode owns arrays and malformed input preserves the last good definition",
          "[resources][scene]")
{
    auto bytes = SceneFixture();
    r::SceneDefinition scene;
    REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Ok);
    REQUIRE(scene.Meshes.GetSize() == 1);
    REQUIRE(scene.Meshes[0].Positions[1].X == 1);
    REQUIRE(scene.Materials[0].Emission.X == 1);
    REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize() - 1, scene) == r::Status::Failed);
    REQUIRE(scene.Meshes[0].Positions[1].X == 1);
    bytes[196] = 3; // An out-of-range mesh index.
    REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Failed);
    REQUIRE(scene.Meshes[0].Indices[0] == 0);
    bytes = SceneFixture();
    bytes[232] = 0;
    bytes[233] = 0;
    bytes[234] = 0;
    bytes[235] = 0; // Singular second basis column.
    REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Failed);
    bytes = SceneFixture();
    REQUIRE(bytes.TryAdd(0));
    REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Failed);
}
TEST_CASE("Cooked scene manager requests share a definition with independent leases", "[resources][scene]")
{
    auto bytes = SceneFixture();
    r::SceneDefinition scene;
    REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Ok);
    c::Resource resource;
    resource.Id = Id("scene/immutable");
    REQUIRE(resource.Path.Set("assets/scene.luscn"));
    resource.Type = c::Kind::CookedScene;
    c::Catalog catalog;
    REQUIRE(catalog.Put(resource) == c::Status::Ok);
    r::SceneDefinitionAdapter provider;
    REQUIRE(provider.Initialize(resource, scene) == r::Status::Ok);
    REQUIRE(provider.Initialize(resource, scene) == r::Status::InvalidState);
    const auto adapter = provider.Descriptor();
    r::Manager manager;
    REQUIRE(manager.Initialize(catalog, &adapter, 1) == r::Status::Ok);
    r::AssetHandle asset;
    r::ScopeHandle scope;
    REQUIRE(manager.Resolve(resource.Id, resource.Type, asset) == r::Status::Ok);
    REQUIRE(manager.OpenScope(Id("level"), scope) == r::Status::Ok);
    r::RequestHandle first, second;
    REQUIRE(manager.BeginAcquire(scope, asset, first) == r::Status::Ok);
    REQUIRE(manager.BeginAcquire(scope, asset, second) == r::Status::Ok);
    for (uint32 i = 0; i < 8; ++i)
    {
        manager.Service();
    }
    r::Lease one, two;
    REQUIRE(manager.TakeLease(first, one) == r::Status::Ok);
    REQUIRE(manager.TakeLease(second, two) == r::Status::Ok);
    const void* payload = nullptr;
    REQUIRE(manager.Borrow(one.Token(), resource.Type, payload) == r::Status::Ok);
    REQUIRE(payload == &scene);
    REQUIRE(one.Release() == r::Status::Ok);
    REQUIRE(manager.Borrow(two.Token(), resource.Type, payload) == r::Status::Ok);
    REQUIRE(manager.CloseScope(scope) == r::Status::Pending);
    REQUIRE(two.Release() == r::Status::Ok);
    for (uint32 i = 0; i < 8; ++i)
    {
        manager.Service();
    }
    REQUIRE(manager.Shutdown() == r::Status::Ok);
}

TEST_CASE("Cooked scene v2 preserves explicit sampler addressing and rejects unknown modes", "[resources][scene]")
{
    r::SceneDefinition scene;
    auto bytes = SceneFixture(2);
    REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Ok);
    REQUIRE(scene.Materials[0].Repeat);
    bytes[72] = 2; // Unknown sampler mode in independently encoded v2 input.
    REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Failed);
    REQUIRE(scene.Materials[0].Repeat);
    bytes[72] = 0;
    REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Ok);
    REQUIRE_FALSE(scene.Materials[0].Repeat);
}

namespace
{
namespace sceneFormat = ludus::content::scene_format;
constexpr usize V3_VERTEX_COUNT = 3;
constexpr usize V3_LIGHT_COUNT = 2;
constexpr usize V3_PIXEL_COUNT = 2;
constexpr usize V3_MATERIAL_OFFSET = sizeof(sceneFormat::Header);
constexpr usize V3_VERTEX_OFFSET =
    V3_MATERIAL_OFFSET + sizeof(sceneFormat::MaterialRecordV3) + sizeof(sceneFormat::MeshRecord);
constexpr usize V3_OBJECT_OFFSET =
    V3_VERTEX_OFFSET + V3_VERTEX_COUNT * sizeof(sceneFormat::VertexRecordV3) + V3_VERTEX_COUNT * sizeof(uint32);
constexpr usize V3_LIGHTING_OFFSET = V3_OBJECT_OFFSET + sizeof(sceneFormat::ObjectRecordV3);
constexpr usize V3_LIGHT_OFFSET = V3_LIGHTING_OFFSET + sizeof(sceneFormat::LightingRecord);
constexpr usize V3_ENVIRONMENT_OFFSET = V3_LIGHT_OFFSET + V3_LIGHT_COUNT * sizeof(sceneFormat::LightRecord);
constexpr usize V3_BUNDLE_BYTES = V3_ENVIRONMENT_OFFSET + V3_PIXEL_COUNT * sceneFormat::ENVIRONMENT_TEXEL_BYTES;
/// Version 5 alpha terms written into every fixture material.
struct FixtureAlpha final
{
    uint32 Mode = sceneFormat::ALPHA_OPAQUE;
    float32 Cutoff = 0;
};
/// Version 3 fixture, or the same scene as version 4 (VertexRecord without stored bake UVs)
/// or version 5 (MaterialRecordV5). Each of materialCount materials has one object.
Array<uint8>
MakeLightingFixture(uint32 version = sceneFormat::VERSION_3, uint32 materialCount = 1, FixtureAlpha alpha = {})
{
    const bool hasBakeCoordinates = sceneFormat::HasStoredBakeCoordinates(version);
    const bool hasAlpha = sceneFormat::HasMaterialAlpha(version);
    const usize extraRecords =
        (materialCount - 1) * (sizeof(sceneFormat::MaterialRecordV3) + sizeof(sceneFormat::ObjectRecordV3));
    const usize alphaBytes =
        hasAlpha == true
            ? materialCount * (sizeof(sceneFormat::MaterialRecordV5) - sizeof(sceneFormat::MaterialRecordV3))
            : 0;
    const usize vertexBytes =
        hasBakeCoordinates == true
            ? 0
            : V3_VERTEX_COUNT * (sizeof(sceneFormat::VertexRecordV3) - sizeof(sceneFormat::VertexRecord));
    Array<uint8> bytes;
    REQUIRE(bytes.TryResize(V3_BUNDLE_BYTES + extraRecords + alphaBytes - vertexBytes) == true);
    usize offset = 0;
    auto writeInteger = [&](uint32 value) {
        REQUIRE(
            ludus::foundation::core::TryWriteLittleEndian<uint32>(value, {bytes.GetData() + offset, sizeof(value)}) ==
            true);
        offset += sizeof(value);
    };
    auto writeIdentity = [&](uint64 value) {
        REQUIRE(
            ludus::foundation::core::TryWriteLittleEndian<uint64>(value, {bytes.GetData() + offset, sizeof(value)}) ==
            true);
        offset += sizeof(value);
    };
    auto writeNumber = [&](float32 value) {
        uint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(value));
        writeInteger(bits);
    };
    std::memcpy(bytes.GetData(), sceneFormat::MAGIC.data(), sceneFormat::MAGIC.size());
    offset = sceneFormat::MAGIC.size();
    for (uint32 value : {version, 1U, materialCount, 0U, materialCount})
    {
        writeInteger(value);
    }
    for (float32 value : {0.F, 0.F, 3.F, 0.F, 0.F, 0.F, 0.F, 1.F, 0.F, 1.F})
    {
        writeNumber(value);
    }
    for (uint32 material = 0; material < materialCount; ++material)
    {
        writeInteger(sceneFormat::NO_TEXTURE);
        writeInteger(sceneFormat::ADDRESS_REPEAT);
        for (float32 value : {0.F, .5F, 1.F, 2.F, 3.F})
        {
            writeNumber(value);
        }
        writeInteger(sceneFormat::EMISSION_HAS_POWER);
        writeIdentity(41);
        writeNumber(1200);
        if (hasAlpha == true)
        {
            writeInteger(alpha.Mode);
            writeNumber(alpha.Cutoff);
        }
        writeInteger(0);
    }
    writeInteger(V3_VERTEX_COUNT);
    writeInteger(V3_VERTEX_COUNT);
    for (ludus::foundation::math::Vector3 position : {ludus::foundation::math::Vector3{0, 0, 0},
                                                      ludus::foundation::math::Vector3{1, 0, 0},
                                                      ludus::foundation::math::Vector3{0, 1, 0}})
    {
        for (float32 value : {position.X, position.Y, position.Z, 0.F, 0.F, 1.F, 4.F, 5.F})
        {
            writeNumber(value);
        }
        if (hasBakeCoordinates == true)
        {
            writeNumber(.25F);
            writeNumber(.75F);
        }
    }
    for (uint32 index : {0U, 1U, 2U})
    {
        writeInteger(index);
    }
    for (uint32 object = 0; object < materialCount; ++object)
    {
        writeInteger(0);
        writeInteger(object);
        for (float32 value : {1.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F})
        {
            writeNumber(value);
        }
        writeIdentity(101 + uint64{object});
    }
    writeInteger(sceneFormat::LIGHTING_PROFILE_REC709_PHOTOMETRIC);
    writeInteger(V3_LIGHT_COUNT);
    writeInteger(V3_PIXEL_COUNT);
    writeInteger(1);
    for (float32 value : {2.F, 3.F, 4.F})
    {
        writeNumber(value);
    }
    for (uint64 identity : {201ULL, 202ULL})
    {
        writeIdentity(identity);
        writeInteger(sceneFormat::LIGHT_SPOT);
        writeInteger(identity == 201 ? sceneFormat::LIGHT_ENABLED : 0);
        for (float32 value : {100.F, 200.F, 300.F, 1.F, 2.F, 3.F, 0.F, -1.F, 0.F, 50.F, .9F, .8F})
        {
            writeNumber(value);
        }
    }
    for (float32 value : {16.F, 8.F, 4.F, 1.F, 2.F, 3.F})
    {
        writeNumber(value);
    }
    REQUIRE(offset == bytes.GetSize());
    return bytes;
}
void SetFixtureWord(Array<uint8>& inoutBytes, usize offset, uint32 value)
{
    REQUIRE(
        ludus::foundation::core::TryWriteLittleEndian<uint32>(value, {inoutBytes.GetData() + offset, sizeof(value)}) ==
        true);
}
} // namespace
TEST_CASE("Cooked v3 preserves authored photometric lights, sided emitters, separate bake UVs and HDR",
          "[resources][scene]")
{
    Array<uint8> bytes = MakeLightingFixture();
    r::SceneDefinition scene;
    REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Ok);
    REQUIRE(scene.Materials[0].HasEmissionPower == true);
    REQUIRE(scene.Materials[0].IsEmissionTwoSided == false);
    REQUIRE(scene.Materials[0].AreaSourceId == 41);
    REQUIRE(scene.Materials[0].EmissionPower == 1200);
    REQUIRE(scene.Objects[0].SourceId == 101);
    REQUIRE(scene.Meshes[0].Texcoords[0].X == 4);
    REQUIRE(scene.Meshes[0].BakeTexcoords[0].X == .25F);
    REQUIRE(scene.Meshes[0].BakeTexcoords[0].Y == .75F);
    REQUIRE(scene.Lights.GetSize() == V3_LIGHT_COUNT);
    REQUIRE(scene.Lights[0].SourceId == 201);
    REQUIRE(scene.Lights[0].Kind == r::SceneLightKind::Spot);
    REQUIRE(scene.Lights[0].IntensityRgb.Y == 200);
    REQUIRE(scene.Lights[0].IsEnabled == true);
    REQUIRE(scene.Lights[1].IsEnabled == false);
    REQUIRE(scene.Environment.Width == 2);
    REQUIRE(scene.Environment.Height == 1);
    REQUIRE(scene.Environment.Rgb.Y == 3);
    REQUIRE(scene.Environment.Pixels[0].X == 16);
    REQUIRE(scene.Environment.Pixels[1].Z == 3);
    const uint64 expectedBytes = sizeof(r::SceneMaterial) + sizeof(r::SceneMesh) + sizeof(r::SceneObject) +
                                 V3_VERTEX_COUNT * (2 * sizeof(ludus::foundation::math::Vector3) +
                                                    2 * sizeof(ludus::foundation::math::Vector2) + sizeof(uint32)) +
                                 V3_LIGHT_COUNT * sizeof(r::SceneLight) +
                                 V3_PIXEL_COUNT * sizeof(ludus::foundation::math::Vector3);
    REQUIRE(scene.Bytes() == expectedBytes);
    for (usize count = 0; count < bytes.GetSize(); ++count)
    {
        REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), count, scene) == r::Status::Failed);
        REQUIRE(scene.Environment.Pixels[0].X == 16);
        REQUIRE(scene.Objects[0].SourceId == 101);
    }
}
TEST_CASE("Cooked v3 rejects unknown contracts and invalid transport values atomically", "[resources][scene]")
{
    const Array<uint8> original = MakeLightingFixture();
    r::SceneDefinition scene;
    REQUIRE(r::SceneDefinition::Decode(original.GetData(), original.GetSize(), scene) == r::Status::Ok);
    struct Mutation final
    {
        usize Offset;
        uint32 Word;
    };
    for (const Mutation mutation :
         {Mutation{V3_MATERIAL_OFFSET + offsetof(sceneFormat::MaterialRecordV3, EmissionFlags), 4},
          Mutation{V3_MATERIAL_OFFSET + offsetof(sceneFormat::MaterialRecordV3, AreaSourceId), 0},
          Mutation{V3_MATERIAL_OFFSET + offsetof(sceneFormat::MaterialRecordV3, Reserved), 1},
          Mutation{V3_VERTEX_OFFSET + offsetof(sceneFormat::VertexRecordV3, BakeUv), 0x40000000U},
          Mutation{V3_OBJECT_OFFSET + offsetof(sceneFormat::ObjectRecordV3, SourceId), 0},
          Mutation{V3_LIGHTING_OFFSET + offsetof(sceneFormat::LightingRecord, Profile), 2},
          Mutation{V3_LIGHTING_OFFSET + offsetof(sceneFormat::LightingRecord, LightCount), sceneFormat::MAX_LIGHTS + 1},
          Mutation{V3_LIGHTING_OFFSET + offsetof(sceneFormat::LightingRecord, EnvironmentWidth), 0},
          Mutation{V3_LIGHT_OFFSET + offsetof(sceneFormat::LightRecord, Kind), 3},
          Mutation{V3_LIGHT_OFFSET + offsetof(sceneFormat::LightRecord, Flags), 2},
          Mutation{V3_LIGHT_OFFSET + offsetof(sceneFormat::LightRecord, IntensityRgb), 0xBF800000U},
          Mutation{V3_LIGHT_OFFSET + offsetof(sceneFormat::LightRecord, InnerCos), 0x3F4CCCCDU},
          Mutation{V3_LIGHT_OFFSET + offsetof(sceneFormat::LightRecord, PositionWorld), 0x49800000U},
          Mutation{V3_LIGHT_OFFSET + sizeof(sceneFormat::LightRecord), 201},
          Mutation{V3_ENVIRONMENT_OFFSET, 0x7FC00000U}})
    {
        Array<uint8> bytes;
        REQUIRE(bytes.TryResize(original.GetSize()) == true);
        std::memcpy(bytes.GetData(), original.GetData(), original.GetSize());
        SetFixtureWord(bytes, mutation.Offset, mutation.Word);
        CAPTURE(mutation.Offset, mutation.Word);
        REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Failed);
        REQUIRE(scene.Materials[0].AreaSourceId == 41);
        REQUIRE(scene.Environment.Pixels[0].X == 16);
    }
}
TEST_CASE("Cooked v4 keeps v3 lighting records with indexed meshes and implicit bake charts", "[resources][scene]")
{
    const Array<uint8> bytes = MakeLightingFixture(sceneFormat::VERSION_4);
    r::SceneDefinition scene;
    REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Ok);
    REQUIRE(scene.Materials[0].AreaSourceId == 41);
    REQUIRE(scene.Materials[0].EmissionPower == 1200);
    REQUIRE(scene.Objects[0].SourceId == 101);
    REQUIRE(scene.Meshes[0].Texcoords[0].X == 4);
    REQUIRE(scene.Meshes[0].Indices.GetSize() == V3_VERTEX_COUNT);
    // No stored chart coordinates: consumers derive them with TryGetBakeChartUv.
    REQUIRE(scene.Meshes[0].BakeTexcoords.IsEmpty() == true);
    REQUIRE(scene.Lights.GetSize() == V3_LIGHT_COUNT);
    REQUIRE(scene.Environment.Pixels[1].Z == 3);
    for (usize count = 0; count < bytes.GetSize(); ++count)
    {
        REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), count, scene) == r::Status::Failed);
        REQUIRE(scene.Objects[0].SourceId == 101);
    }
    Array<uint8> unknown;
    REQUIRE(unknown.TryResize(bytes.GetSize()) == true);
    std::memcpy(unknown.GetData(), bytes.GetData(), bytes.GetSize());
    SetFixtureWord(unknown, sceneFormat::MAGIC.size(), sceneFormat::VERSION_5 + 1);
    REQUIRE(r::SceneDefinition::Decode(unknown.GetData(), unknown.GetSize(), scene) == r::Status::Failed);
}
TEST_CASE("Legacy cooked lighting retains two-sided emitters and deterministic scene identities", "[resources][scene]")
{
    for (uint32 version : {sceneFormat::VERSION_1, sceneFormat::VERSION_2})
    {
        const Array<uint8> bytes = SceneFixture(version);
        r::SceneDefinition scene;
        REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Ok);
        REQUIRE(scene.Materials[0].IsEmissionTwoSided == true);
        REQUIRE(scene.Materials[0].HasEmissionPower == false);
        REQUIRE(scene.Materials[0].AreaSourceId == 1);
        REQUIRE(scene.Objects[0].SourceId == 1);
        REQUIRE(scene.Lights.IsEmpty() == true);
        REQUIRE(scene.Environment.Pixels.IsEmpty() == true);
        REQUIRE(scene.Environment.Rgb.X == 0);
        REQUIRE(scene.Meshes[0].BakeTexcoords.IsEmpty() == true);
    }
}
TEST_CASE("Cooked v5 carries material alpha masks and admits the import profile's counts", "[resources][scene]")
{
    constexpr float32 CUTOFF = .25F;
    // More materials and objects than the resident reference profile admits.
    constexpr uint32 MATERIAL_COUNT = sceneFormat::MAX_MATERIALS + 1;
    const Array<uint8> bytes = MakeLightingFixture(sceneFormat::VERSION_5,
                                                   MATERIAL_COUNT,
                                                   { .Mode = sceneFormat::ALPHA_MASKED, .Cutoff = CUTOFF });
    r::SceneDefinition scene;
    REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Ok);
    REQUIRE(scene.Materials.GetSize() == MATERIAL_COUNT);
    REQUIRE(scene.Objects.GetSize() == MATERIAL_COUNT);
    REQUIRE(scene.Materials[MATERIAL_COUNT - 1].IsAlphaMasked == true);
    REQUIRE(scene.Materials[MATERIAL_COUNT - 1].AlphaCutoff == CUTOFF);
    REQUIRE(scene.Objects[MATERIAL_COUNT - 1].Material == MATERIAL_COUNT - 1);
    REQUIRE(scene.Materials[0].EmissionPower == 1200);
    REQUIRE(scene.Meshes[0].BakeTexcoords.IsEmpty() == true);
    for (usize count = 0; count < bytes.GetSize(); ++count)
    {
        REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), count, scene) == r::Status::Failed);
        REQUIRE(scene.Materials[0].AlphaCutoff == CUTOFF);
    }

    // The same counts exceed the resident reference profile of version 4.
    const Array<uint8> resident = MakeLightingFixture(sceneFormat::VERSION_4, MATERIAL_COUNT);
    r::SceneDefinition rejected;
    REQUIRE(r::SceneDefinition::Decode(resident.GetData(), resident.GetSize(), rejected) == r::Status::Failed);

    const Array<uint8> opaque = MakeLightingFixture(sceneFormat::VERSION_5);
    REQUIRE(r::SceneDefinition::Decode(opaque.GetData(), opaque.GetSize(), scene) == r::Status::Ok);
    REQUIRE(scene.Materials[0].IsAlphaMasked == false);
    REQUIRE(scene.Materials[0].AlphaCutoff == 0);

    // Unknown alpha modes, an opaque cutoff and out-of-range thresholds are rejected.
    for (const FixtureAlpha invalid : {FixtureAlpha{ .Mode = sceneFormat::ALPHA_MASKED + 1, .Cutoff = 0 },
                                       FixtureAlpha{ .Mode = sceneFormat::ALPHA_OPAQUE, .Cutoff = CUTOFF },
                                       FixtureAlpha{ .Mode = sceneFormat::ALPHA_MASKED, .Cutoff = -CUTOFF },
                                       FixtureAlpha{ .Mode = sceneFormat::ALPHA_MASKED, .Cutoff = 1 + CUTOFF }})
    {
        const Array<uint8> malformed = MakeLightingFixture(sceneFormat::VERSION_5, 1, invalid);
        REQUIRE(r::SceneDefinition::Decode(malformed.GetData(), malformed.GetSize(), scene) == r::Status::Failed);
        REQUIRE(scene.Materials[0].IsAlphaMasked == false);
    }
}
namespace
{
/// Version 6 material map terms and one vertex tangent of the surface-map fixture.
struct SurfaceFixture final
{
    uint32 Texture = 0;
    uint32 SurfaceTexture = 1;
    uint32 EmissionTexture = 0;
    float32 NormalScale = 2;
    float32 TangentW = 1;
    float32 TangentX = 1;
    float32 TangentZ = 0;
};
/// Two 1x1 textures, one material using them and one tangent-framed triangle (version 6).
Array<uint8> MakeSurfaceFixture(const SurfaceFixture& fixture = {})
{
    Array<uint8> bytes;
    usize offset = 0;
    auto writeInteger = [&](uint32 value) {
        REQUIRE(bytes.TryResize(offset + sizeof(value)) == true);
        REQUIRE(
            ludus::foundation::core::TryWriteLittleEndian<uint32>(value, {bytes.GetData() + offset, sizeof(value)}) ==
            true);
        offset += sizeof(value);
    };
    auto writeNumber = [&](float32 value) {
        uint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(value));
        writeInteger(bits);
    };
    REQUIRE(bytes.TryResize(sceneFormat::MAGIC.size()) == true);
    std::memcpy(bytes.GetData(), sceneFormat::MAGIC.data(), sceneFormat::MAGIC.size());
    offset = sceneFormat::MAGIC.size();
    for (uint32 value : {sceneFormat::VERSION_6, 1U, 1U, 2U, 1U})
    {
        writeInteger(value);
    }
    for (float32 value : {0.F, 0.F, 3.F, 0.F, 0.F, 0.F, 0.F, 1.F, 0.F, 1.F})
    {
        writeNumber(value);
    }
    for (uint32 texel : {0xff8040c0U, 0x80ff80ffU})
    {
        writeInteger(1);
        writeInteger(1);
        writeInteger(sceneFormat::TEXEL_BYTES);
        writeInteger(texel);
    }
    writeInteger(fixture.Texture);
    writeInteger(sceneFormat::ADDRESS_REPEAT);
    for (float32 value : {.5F, .5F, 0.F, 0.F, 0.F})
    {
        writeNumber(value);
    }
    writeInteger(0);
    writeInteger(0);
    writeInteger(0);
    writeNumber(0);
    writeInteger(sceneFormat::ALPHA_OPAQUE);
    writeNumber(0);
    writeInteger(fixture.SurfaceTexture);
    writeInteger(fixture.EmissionTexture);
    writeNumber(fixture.NormalScale);
    writeInteger(3);
    writeInteger(3);
    for (usize vertex = 0; vertex < 3; ++vertex)
    {
        for (float32 value : {vertex == 1 ? 1.F : 0.F, vertex == 2 ? 1.F : 0.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F})
        {
            writeNumber(value);
        }
        // The last vertex has no tangent (all zero); the others carry the fixture's frame.
        const bool hasTangent = vertex != 2;
        for (float32 value : {hasTangent == true ? fixture.TangentX : 0.F,
                              0.F,
                              hasTangent == true ? fixture.TangentZ : 0.F,
                              hasTangent == true ? fixture.TangentW : 0.F})
        {
            writeNumber(value);
        }
    }
    for (uint32 index : {0U, 1U, 2U})
    {
        writeInteger(index);
    }
    writeInteger(0);
    writeInteger(0);
    for (float32 value : {1.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F})
    {
        writeNumber(value);
    }
    writeInteger(1);
    writeInteger(0);
    writeInteger(sceneFormat::LIGHTING_PROFILE_REC709_PHOTOMETRIC);
    for (uint32 value : {0U, 0U, 0U})
    {
        writeInteger(value);
    }
    for (float32 value : {0.F, 0.F, 0.F})
    {
        writeNumber(value);
    }
    return bytes;
}
} // namespace
TEST_CASE("Cooked v6 carries linear surface maps, emission maps and vertex tangents", "[resources][scene]")
{
    const Array<uint8> bytes = MakeSurfaceFixture();
    r::SceneDefinition scene;
    REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), bytes.GetSize(), scene) == r::Status::Ok);
    REQUIRE(scene.Textures.GetSize() == 2);
    CHECK(scene.Textures[0].IsLinear == false);
    CHECK(scene.Textures[1].IsLinear == true);
    const r::SceneMaterial& material = scene.Materials[0];
    CHECK(material.SurfaceTexture == 1);
    CHECK(material.EmissionTexture == 0);
    CHECK(material.NormalScale == 2);
    const r::SceneMesh& mesh = scene.Meshes[0];
    REQUIRE(mesh.Tangents.GetSize() == 3);
    CHECK(mesh.Tangents[0].X == 1);
    CHECK(mesh.Tangents[0].W == 1);
    CHECK(mesh.Tangents[2].W == 0);
    CHECK(mesh.BakeTexcoords.IsEmpty() == true);
    for (usize count = 0; count < bytes.GetSize(); ++count)
    {
        REQUIRE(r::SceneDefinition::Decode(bytes.GetData(), count, scene) == r::Status::Failed);
        REQUIRE(scene.Materials[0].NormalScale == 2);
    }

    // A surface map is linear data: no material may sample it as colour; maps must exist;
    // the strength is bounded and needs a map; tangents need a sign and a direction.
    for (const SurfaceFixture& invalid : {SurfaceFixture{ .Texture = 1 },
                                          SurfaceFixture{ .EmissionTexture = 1 },
                                          SurfaceFixture{ .SurfaceTexture = 2 },
                                          SurfaceFixture{ .EmissionTexture = 2 },
                                          SurfaceFixture{ .NormalScale = sceneFormat::MAX_NORMAL_SCALE + 1 },
                                          SurfaceFixture{ .NormalScale = -1 },
                                          SurfaceFixture{ .SurfaceTexture = sceneFormat::NO_TEXTURE },
                                          SurfaceFixture{ .TangentW = .5F },
                                          SurfaceFixture{ .TangentW = 0 },
                                          SurfaceFixture{ .TangentX = 0 },
                                          SurfaceFixture{ .TangentX = 0, .TangentZ = 1 }})
    {
        const Array<uint8> malformed = MakeSurfaceFixture(invalid);
        REQUIRE(r::SceneDefinition::Decode(malformed.GetData(), malformed.GetSize(), scene) == r::Status::Failed);
        REQUIRE(scene.Materials[0].SurfaceTexture == 1);
    }
}
