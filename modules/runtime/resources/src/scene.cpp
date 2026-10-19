#include <ludus/runtime/resources/scene.h>

#include <ludus/content/scene_format/luscn.h>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <atomic>
#include <cstring>
#include <new>

namespace ludus::resources
{
// Thanks to James Boer, "Resource and Memory Management", Game Programming Gems 1,
// section 1.7, pp. 80-87, for retaining metadata independently of payload residency
// and explicit lifetime holds. See docs/architecture/resource-management-reference-review.md.
// This original aggregate CPU adapter owns no GPU objects. Project-level exact
// artifact/dependency contracts: docs/architecture/resource-management.md.
namespace
{
namespace math = foundation::math;
namespace format = content::scene_format;
// The decoded in-memory types must hold exactly the LUSCN wire components.
static_assert(sizeof(format::VertexRecord) == 2 * sizeof(math::Vector3) + sizeof(math::Vector2));
static_assert(sizeof(format::ObjectRecord::Linear) == sizeof(math::Affine3::Columns) &&
              sizeof(format::ObjectRecord::Translation) == sizeof(math::Affine3::Translation));
static_assert(sizeof(format::Header::Eye) == sizeof(math::Vector3));
class Reader final
{
public:
    LUDUS_INLINE Reader(const uint8* bytes, usize count) noexcept : mBytes(bytes), mRemaining(count) {}
    const uint8* Take(usize count) noexcept
    {
        if (count > mRemaining)
        {
            return nullptr;
        }
        const uint8* result = mBytes;
        mBytes += count;
        mRemaining -= count;
        return result;
    }
    bool Read(uint32& outValue) noexcept
    {
        const uint8* bytesOrNull = Take(sizeof(uint32));
        return bytesOrNull != nullptr &&
               foundation::core::TryReadLittleEndian<uint32>({bytesOrNull, sizeof(uint32)}, outValue) == true;
    }
    bool Read(uint64& outValue) noexcept
    {
        const uint8* bytesOrNull = Take(sizeof(uint64));
        return bytesOrNull != nullptr &&
               foundation::core::TryReadLittleEndian<uint64>({bytesOrNull, sizeof(uint64)}, outValue) == true;
    }
    bool Read(float32& outValue) noexcept
    {
        uint32 bits = 0;
        if (Read(bits) == false)
        {
            return false;
        }
        std::memcpy(&outValue, &bits, sizeof(outValue));
        return math::IsFinite(outValue);
    }
    bool Read(math::Vector3& outValue) noexcept
    {
        return Read(outValue.X) == true && Read(outValue.Y) == true && Read(outValue.Z) == true;
    }
    bool Read(math::Vector2& outValue) noexcept
    {
        return Read(outValue.X) == true && Read(outValue.Y) == true;
    }
    [[nodiscard]] LUDUS_INLINE usize Remaining() const noexcept
    {
        return mRemaining;
    }

private:
    const uint8* mBytes;
    usize mRemaining;
};
bool IsNonnegative(const math::Vector3& value, float32 maximum) noexcept
{
    return value.X >= 0 && value.Y >= 0 && value.Z >= 0 && value.X <= maximum && value.Y <= maximum &&
           value.Z <= maximum;
}
bool IsNonzero(const math::Vector3& value) noexcept
{
    return value.X != 0 || value.Y != 0 || value.Z != 0;
}
Status ReadLighting(Reader& inoutReader, SceneDefinition& inoutScene) noexcept
{
    uint32 profile = 0;
    uint32 lightCount = 0;
    SceneEnvironment& environment = inoutScene.Environment;
    if (inoutReader.Read(profile) == false || profile != format::LIGHTING_PROFILE_REC709_PHOTOMETRIC ||
        inoutReader.Read(lightCount) == false || lightCount > format::MAX_LIGHTS ||
        inoutReader.Read(environment.Width) == false || inoutReader.Read(environment.Height) == false ||
        environment.Width > format::MAX_ENVIRONMENT_DIMENSION ||
        environment.Height > format::MAX_ENVIRONMENT_DIMENSION ||
        ((environment.Width == 0) != (environment.Height == 0)) || inoutReader.Read(environment.Rgb) == false ||
        IsNonnegative(environment.Rgb, format::MAX_EMISSION) == false)
    {
        return Status::Failed;
    }
    const usize pixelCount = usize(environment.Width) * environment.Height;
    const usize payloadBytes =
        usize(lightCount) * sizeof(format::LightRecord) + pixelCount * format::ENVIRONMENT_TEXEL_BYTES;
    if (payloadBytes != inoutReader.Remaining())
    {
        return Status::Failed;
    }
    if (inoutScene.Lights.TryResize(lightCount) == false || environment.Pixels.TryResize(pixelCount) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < lightCount; ++i)
    {
        SceneLight& light = inoutScene.Lights[i];
        uint32 kind = 0;
        uint32 flags = 0;
        if (inoutReader.Read(light.SourceId) == false || light.SourceId == 0 || inoutReader.Read(kind) == false ||
            kind > format::LIGHT_SPOT || inoutReader.Read(flags) == false || (flags & ~format::LIGHT_ENABLED) != 0 ||
            inoutReader.Read(light.IntensityRgb) == false ||
            IsNonnegative(light.IntensityRgb, format::MAX_LIGHT_INTENSITY) == false ||
            inoutReader.Read(light.PositionWorld) == false || math::Abs(light.PositionWorld.X) > 1000000 ||
            math::Abs(light.PositionWorld.Y) > 1000000 || math::Abs(light.PositionWorld.Z) > 1000000 ||
            inoutReader.Read(light.DirectionWorld) == false || IsNonzero(light.DirectionWorld) == false ||
            inoutReader.Read(light.Range) == false || light.Range < .01F || light.Range > format::MAX_LIGHT_INTENSITY ||
            inoutReader.Read(light.InnerCos) == false || inoutReader.Read(light.OuterCos) == false ||
            light.InnerCos <= -1 || light.InnerCos > 1 || light.OuterCos <= -1 || light.OuterCos >= light.InnerCos)
        {
            return Status::Failed;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (inoutScene.Lights[j].SourceId == light.SourceId)
            {
                return Status::Failed;
            }
        }
        light.Kind = static_cast<SceneLightKind>(kind);
        light.IsEnabled = (flags & format::LIGHT_ENABLED) != 0;
    }
    for (math::Vector3& pixel : environment.Pixels)
    {
        if (inoutReader.Read(pixel) == false || IsNonnegative(pixel, format::MAX_EMISSION) == false)
        {
            return Status::Failed;
        }
    }
    return Status::Ok;
}
std::atomic<uint64> NextEpoch{1};
uint64 ReserveEpoch() noexcept
{
    auto next = NextEpoch.load(std::memory_order_relaxed);
    while (next != 0)
    {
        const uint64 following = next == ~uint64{0} ? 0 : next + 1;
        if (NextEpoch.compare_exchange_weak(next, following, std::memory_order_relaxed) == true)
        {
            return next;
        }
    }
    return 0;
}
} // namespace
Status SceneDefinition::Decode(const uint8* bytesOrNull, usize count, SceneDefinition& outScene) noexcept
{
    if (bytesOrNull == nullptr || count < sizeof(format::Header) || count > format::MAX_BUNDLE_BYTES)
    {
        return Status::Failed;
    }
    Reader reader(bytesOrNull, count);
    const uint8* magic = reader.Take(format::MAGIC.size());
    uint32 version = 0;
    uint32 meshes = 0;
    uint32 materials = 0;
    uint32 textures = 0;
    uint32 objects = 0;
    SceneDefinition scene;
    if (std::memcmp(magic, format::MAGIC.data(), format::MAGIC.size()) != 0 || reader.Read(version) == false ||
        format::IsSupportedVersion(version) == false || reader.Read(meshes) == false || meshes == 0 ||
        meshes > format::MAX_MESHES || reader.Read(materials) == false || materials == 0 ||
        materials > format::MAX_MATERIALS || reader.Read(textures) == false || textures > format::MAX_TEXTURES ||
        reader.Read(objects) == false || objects == 0 || objects > format::MAX_OBJECTS ||
        reader.Read(scene.Eye) == false || reader.Read(scene.Target) == false || reader.Read(scene.Up) == false ||
        reader.Read(scene.VerticalFov) == false)
    {
        return Status::Failed;
    }
    math::Matrix4 view;
    math::Matrix4 projection;
    if (math::TryLookAt(scene.Eye, scene.Target, scene.Up, view) != math::MathStatus::Success ||
        math::TryPerspectiveReverseZ(scene.VerticalFov, 1, .05F, 100, projection) != math::MathStatus::Success)
    {
        return Status::Failed;
    }
    if (scene.Textures.TryResize(textures) == false || scene.Materials.TryResize(materials) == false ||
        scene.Meshes.TryResize(meshes) == false || scene.Objects.TryResize(objects) == false)
    {
        return Status::OutOfMemory;
    }
    usize textureBytes = 0;
    for (SceneTexture& texture : scene.Textures)
    {
        uint32 size = 0;
        if (reader.Read(texture.Width) == false || reader.Read(texture.Height) == false || reader.Read(size) == false ||
            texture.Width == 0 || texture.Height == 0 || texture.Width > format::MAX_TEXTURE_DIMENSION ||
            texture.Height > format::MAX_TEXTURE_DIMENSION ||
            size != usize(texture.Width) * texture.Height * format::TEXEL_BYTES || size > format::MAX_TEXTURE_BYTES ||
            textureBytes + size > format::MAX_TOTAL_TEXTURE_BYTES)
        {
            return Status::Failed;
        }
        const uint8* pixelsOrNull = reader.Take(size);
        if (pixelsOrNull == nullptr)
        {
            return Status::Failed;
        }
        if (texture.Pixels.TryResize(size) == false)
        {
            return Status::OutOfMemory;
        }
        std::memcpy(texture.Pixels.GetData(), pixelsOrNull, size);
        textureBytes += size;
    }
    for (usize materialIndex = 0; materialIndex < scene.Materials.GetSize(); ++materialIndex)
    {
        SceneMaterial& material = scene.Materials[materialIndex];
        uint32 address = 0;
        if (reader.Read(material.Texture) == false ||
            (version >= format::VERSION_2 && (reader.Read(address) == false || address > format::ADDRESS_REPEAT)))
        {
            return Status::Failed;
        }
        material.Repeat = version >= format::VERSION_2 ? address == format::ADDRESS_REPEAT
                                                       : material.Texture > 0 && material.Texture < textures;
        if (reader.Read(material.Metallic) == false || reader.Read(material.Roughness) == false ||
            reader.Read(material.Emission) == false ||
            (material.Texture != format::NO_TEXTURE && material.Texture >= textures) || material.Metallic < 0 ||
            material.Metallic > 1 || material.Roughness < format::MIN_ROUGHNESS || material.Roughness > 1 ||
            IsNonnegative(material.Emission, format::MAX_EMISSION) == false)
        {
            return Status::Failed;
        }
        material.AreaSourceId = IsNonzero(material.Emission) == true ? materialIndex + 1 : 0;
        if (format::HasLightingRecords(version) == true)
        {
            uint32 flags = 0;
            uint32 reserved = 0;
            if (reader.Read(flags) == false || (flags & ~format::EMISSION_FLAGS) != 0 ||
                reader.Read(material.AreaSourceId) == false || reader.Read(material.EmissionPower) == false ||
                reader.Read(reserved) == false || reserved != 0 || material.EmissionPower < 0 ||
                material.EmissionPower > format::MAX_LIGHT_INTENSITY)
            {
                return Status::Failed;
            }
            material.IsEmissionTwoSided = (flags & format::EMISSION_TWO_SIDED) != 0;
            material.HasEmissionPower = (flags & format::EMISSION_HAS_POWER) != 0;
            if ((material.HasEmissionPower == false && material.EmissionPower != 0) ||
                (IsNonzero(material.Emission) == true && material.AreaSourceId == 0))
            {
                return Status::Failed;
            }
        }
    }
    usize vertexTotal = 0;
    usize indexTotal = 0;
    const usize vertexBytes = format::GetVertexRecordBytes(version);
    const bool hasBakeCoordinates = format::HasStoredBakeCoordinates(version);
    for (SceneMesh& mesh : scene.Meshes)
    {
        uint32 vertices = 0;
        uint32 indices = 0;
        if (reader.Read(vertices) == false || reader.Read(indices) == false || vertices == 0 ||
            vertices > format::MAX_VERTICES || indices == 0 || indices > format::MAX_INDICES || indices % 3 != 0 ||
            vertexTotal + vertices > format::MAX_VERTICES ||
            (version == format::VERSION_4 && indexTotal + indices > format::MAX_TOTAL_INDICES) ||
            usize(vertices) * vertexBytes + usize(indices) * sizeof(uint32) > reader.Remaining())
        {
            return Status::Failed;
        }
        if (mesh.Positions.TryResize(vertices) == false || mesh.Normals.TryResize(vertices) == false ||
            mesh.Texcoords.TryResize(vertices) == false || mesh.Indices.TryResize(indices) == false ||
            (hasBakeCoordinates == true && mesh.BakeTexcoords.TryResize(vertices) == false))
        {
            return Status::OutOfMemory;
        }
        for (usize i = 0; i < vertices; ++i)
        {
            if (reader.Read(mesh.Positions[i]) == false || reader.Read(mesh.Normals[i]) == false ||
                reader.Read(mesh.Texcoords[i]) == false || IsNonzero(mesh.Normals[i]) == false)
            {
                return Status::Failed;
            }
            if (hasBakeCoordinates == true &&
                (reader.Read(mesh.BakeTexcoords[i]) == false || mesh.BakeTexcoords[i].X < 0 ||
                 mesh.BakeTexcoords[i].X > 1 || mesh.BakeTexcoords[i].Y < 0 || mesh.BakeTexcoords[i].Y > 1))
            {
                return Status::Failed;
            }
        }
        for (uint32& index : mesh.Indices)
        {
            if (reader.Read(index) == false || index >= vertices)
            {
                return Status::Failed;
            }
        }
        vertexTotal += vertices;
        indexTotal += indices;
    }
    for (usize i = 0; i < scene.Objects.GetSize(); ++i)
    {
        SceneObject& object = scene.Objects[i];
        object.SourceId = i + 1;
        if (reader.Read(object.Mesh) == false || object.Mesh >= meshes || reader.Read(object.Material) == false ||
            object.Material >= materials)
        {
            return Status::Failed;
        }
        for (math::Vector3& column : object.Transform.Columns)
        {
            if (reader.Read(column) == false)
            {
                return Status::Failed;
            }
        }
        math::Affine3 inverse;
        if (reader.Read(object.Transform.Translation) == false ||
            math::TryInverse(object.Transform, inverse) != math::MathStatus::Success)
        {
            return Status::Failed;
        }
        if (format::HasLightingRecords(version) == true)
        {
            if (reader.Read(object.SourceId) == false || object.SourceId == 0)
            {
                return Status::Failed;
            }
            for (usize j = 0; j < i; ++j)
            {
                if (scene.Objects[j].SourceId == object.SourceId)
                {
                    return Status::Failed;
                }
            }
        }
    }
    if (format::HasLightingRecords(version) == true)
    {
        const Status lightingStatus = ReadLighting(reader, scene);
        if (lightingStatus != Status::Ok)
        {
            return lightingStatus;
        }
    }
    if (reader.Remaining() != 0)
    {
        return Status::Failed;
    }
    outScene = foundation::Move(scene);
    return Status::Ok;
}
uint64 SceneDefinition::Bytes() const noexcept
{
    uint64 bytes = Textures.GetSize() * sizeof(SceneTexture) + Materials.GetSize() * sizeof(SceneMaterial) +
                   Meshes.GetSize() * sizeof(SceneMesh) + Objects.GetSize() * sizeof(SceneObject) +
                   Lights.GetSize() * sizeof(SceneLight) + Environment.Pixels.GetSize() * sizeof(math::Vector3);
    for (const SceneTexture& texture : Textures)
    {
        bytes += texture.Pixels.GetSize();
    }
    for (const SceneMesh& mesh : Meshes)
    {
        bytes += mesh.Positions.GetSize() * sizeof(math::Vector3) + mesh.Normals.GetSize() * sizeof(math::Vector3) +
                 mesh.Texcoords.GetSize() * sizeof(math::Vector2) +
                 mesh.BakeTexcoords.GetSize() * sizeof(math::Vector2) + mesh.Indices.GetSize() * sizeof(uint32);
    }
    return bytes;
}
struct SceneDefinitionAdapter::State final
{
    struct Job final
    {
        bool Used = false;
        bool Cancelled = false;
    };
    content::Resource Resource;
    const SceneDefinition* Scene = nullptr;
    uint64 Epoch = 0;
    Job Jobs[16];
};
SceneDefinitionAdapter::~SceneDefinitionAdapter() noexcept
{
    if (mState != nullptr)
    {
        for (const auto& job : mState->Jobs)
        {
            LUDUS_REQUIRE(job.Used == false);
        }
    }
    delete mState;
}
Status SceneDefinitionAdapter::Initialize(const content::Resource& resource, const SceneDefinition& scene) noexcept
{
    if (mState != nullptr || resource.Type != content::Kind::CookedScene ||
        content::ValidId(resource.Id.View()) == false || content::ValidPath(resource.Path.View()) == false ||
        scene.Meshes.IsEmpty() == true || scene.Objects.IsEmpty() == true || scene.Bytes() > uint64{64} * 1024 * 1024)
    {
        return Status::InvalidState;
    }
    const auto epoch = ReserveEpoch();
    if (epoch == 0)
    {
        return Status::LimitExceeded;
    }
    auto* state = new (std::nothrow) State;
    if (state == nullptr)
    {
        return Status::OutOfMemory;
    }
    state->Resource = resource;
    state->Scene = &scene;
    state->Epoch = epoch;
    mState = state;
    return Status::Ok;
}
Adapter SceneDefinitionAdapter::Descriptor() noexcept
{
    Adapter adapter;
    adapter.Type = content::Kind::CookedScene;
    adapter.Context = this;
    adapter.Epoch = Epoch;
    adapter.Begin = Begin;
    adapter.Poll = Poll;
    adapter.Cancel = Cancel;
    adapter.Retire = Retire;
    adapter.Validate = Validate;
    return adapter;
}
uint64 SceneDefinitionAdapter::Epoch(void* context) noexcept
{
    const auto* state = static_cast<SceneDefinitionAdapter*>(context)->mState;
    return state == nullptr ? 0 : state->Epoch;
}
Status SceneDefinitionAdapter::Begin(void* context, const content::Resource& resource, void*& candidate) noexcept
{
    auto* state = static_cast<SceneDefinitionAdapter*>(context)->mState;
    if (state == nullptr || resource.Id.View() != state->Resource.Id.View() ||
        resource.Path.View() != state->Resource.Path.View() || resource.Type != content::Kind::CookedScene)
    {
        return Status::NotFound;
    }
    for (auto& job : state->Jobs)
    {
        if (job.Used == false)
        {
            job = { .Used = true, .Cancelled = false };
            candidate = &job;
            return Status::Ok;
        }
    }
    return Status::QueueFull;
}
Status SceneDefinitionAdapter::Poll(void* context, void* candidate, const void*& payload) noexcept
{
    if (context == candidate)
    {
        return Status::InvalidHandle;
    }
    auto* state = static_cast<SceneDefinitionAdapter*>(context)->mState;
    const auto& job = *static_cast<State::Job*>(candidate);
    if (job.Used == false || job.Cancelled == true)
    {
        return Status::Cancelled;
    }
    payload = state->Scene;
    return Status::Ok;
}
void SceneDefinitionAdapter::Cancel(void*, void* candidate) noexcept
{
    static_cast<State::Job*>(candidate)->Cancelled = true;
}
Status SceneDefinitionAdapter::Retire(void*, void* candidate) noexcept
{
    *static_cast<State::Job*>(candidate) = {};
    return Status::Ok;
}
Status SceneDefinitionAdapter::Validate(void* context, const void* payload) noexcept
{
    if (context == payload)
    {
        return Status::InvalidHandle;
    }
    const auto* state = static_cast<SceneDefinitionAdapter*>(context)->mState;
    return state != nullptr && payload == state->Scene ? Status::Ok : Status::InvalidHandle;
}
} // namespace ludus::resources
