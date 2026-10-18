// Example gameplay module for live editing during play (project-live-reload
// design 2/9/12). ONE implementation: ludus_add_game builds it as a reloadable
// module, a project host, and a static shipping executable. The module uses
// Ludus::GameApi and stateless reflection helpers. It exposes a tunable "Speed"
// property, a read-only "Bounces" output, and a tint-colored fullscreen clear
// so a live edit or a code reload produces an observable rendered change.

#include <ludus/foundation/base/types.h>
#include <ludus/foundation/reflection/schema.hpp>
#include <ludus/runtime/game_api/api.h>
#include <ludus/runtime/game_api/authored.h>
#include <ludus/runtime/game_api/checkpoint.h>
#include <ludus/runtime/game_api/properties.h>
#include <ludus/runtime/game_api/services.h>
#include <ludus/runtime/property_binding/scalars.hpp>

#include "body.h"
#include "sample_game_reflection_schema.hpp"

#include <cstring>
#include <new>

namespace
{
using namespace ludus::runtime::game_api;
using ludus::foundation::float32;
using ludus::foundation::float64;
using ludus::foundation::int32;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;

constexpr uint32 kCheckpointSchema = 1;
constexpr uint32 kPropertySchema = ludus::sample::kBodySchemaVersion;
using ludus::sample::kBodyFieldCount;
constexpr uint32 kCapabilities = static_cast<uint32>(Capability::Reload) | static_cast<uint32>(Capability::Properties);

constexpr uint64 kObjectId = 0xA001;
constexpr uint64 kPropSpeed = 0xB001;

#ifndef LUDUS_EXAMPLE_IDENTITY
#    define LUDUS_EXAMPLE_IDENTITY "example-identity-unset"
#endif
constexpr const char* kIdentity = LUDUS_EXAMPLE_IDENTITY;

using ludus::sample::Body;
namespace reflection = ludus::foundation::reflection;
namespace property_binding = ludus::runtime::property_binding;

struct State
{
    Body Sim;
    uint64 ProjectId = 0;
    uint64 GameId = 0;
    uint64 BuildId = 0;
    uint64 Revision = 1;
    bool Paused = false;
};

// Thanks to Glenn Fowler, Landon Curt Noll and Phong Vo, "FNV Hash",
// "FNV-1a alternate algorithm" and "Parameters of the FNV-1/FNV-1a hash":
// https://www.isthe.com/chongo/tech/comp/fnv/ . Adopt the XOR/multiply sequence
// and published constants for a non-cryptographic hash; collisions remain possible.
uint64 Fnv1a(const uint8* data, usize size) noexcept
{
    uint64 hash = 0xCBF29CE484222325ULL;
    for (usize i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

State* AsState(GameInstance* instance) noexcept
{
    return reinterpret_cast<State*>(instance);
}

Status GameQuery(GameMetadata* out) noexcept
{
    if (out == nullptr || out->StructSize < sizeof(GameMetadata))
    {
        return Status::InvalidArgument;
    }
    out->AbiMajor = kAbiMajor;
    out->AbiMinor = kAbiMinor;
    out->Capabilities = kCapabilities;
    out->PropertySchemaVersion = kPropertySchema;
    out->CheckpointSchemaVersion = kCheckpointSchema;
    const usize len = std::strlen(kIdentity);
    const usize bounded = len > kIdentityMax ? kIdentityMax : len;
    out->IdentityLength = static_cast<uint32>(bounded);
    std::memset(out->Identity, 0, sizeof(out->Identity));
    std::memcpy(out->Identity, kIdentity, bounded);
    return Status::Ok;
}

Status GameCreate(const CreateInfo* info, GameInstance** outInstance) noexcept
{
    if (info == nullptr || outInstance == nullptr)
    {
        return Status::InvalidArgument;
    }
    auto* state = new (std::nothrow) State();
    if (state == nullptr)
    {
        return Status::Internal;
    }
    state->ProjectId = info->ProjectId;
    state->GameId = info->GameId;
    state->BuildId = info->ModuleGeneration;
    if (info->AuthoredDocument.Size != 0)
    {
        AuthoredReader reader;
        AuthoredRecord record;
        bool hasSpeed = false;
        if (reader.Start(info->AuthoredDocument, info->GameId) == false)
        {
            delete state;
            return Status::InvalidArgument;
        }
        while (reader.Next(record))
        {
            if (record.Object != kObjectId || record.Property != kPropSpeed || hasSpeed ||
                record.Kind != static_cast<uint32>(PropertyKind::Float32) || record.Value.Size != 4)
            {
                delete state;
                return Status::InvalidArgument;
            }
            const uint32 bits = static_cast<uint32>(ReadCheckpointUint(record.Value.Data, 4));
            std::memcpy(&state->Sim.Speed, &bits, sizeof(bits));
            reflection::Value snapshot[kBodyFieldCount]{};
            reflection::Diagnostic error;
            if (ludus::sample::ReadBody(state->Sim, snapshot, error) != reflection::Status::Ok)
            {
                delete state;
                return Status::OutOfRange;
            }
            hasSpeed = true;
        }
        if (reader.Complete() == false)
        {
            delete state;
            return Status::InvalidArgument;
        }
    }
    *outInstance = reinterpret_cast<GameInstance*>(state);
    return Status::Ok;
}

void GameDestroy(GameInstance* instance) noexcept
{
    delete AsState(instance);
}

Status GameUpdate(GameInstance* instance, const FrameInput* input, RenderParams* outRender) noexcept
{
    State* state = AsState(instance);
    if (state == nullptr || input == nullptr || outRender == nullptr)
    {
        return Status::InvalidArgument;
    }
    if (input->Paused == 0)
    {
        state->Sim.SimTime = input->ElapsedSeconds;
        state->Sim.PositionX += state->Sim.Velocity * state->Sim.Speed * static_cast<float32>(input->DeltaSeconds);
        if (state->Sim.PositionX > 1.0F || state->Sim.PositionX < 0.0F)
        {
            state->Sim.PositionX = state->Sim.PositionX > 1.0F ? 1.0F : 0.0F;
            state->Sim.Velocity = -state->Sim.Velocity;
            state->Sim.Bounces += 1;
            state->Revision += 1;
        }
    }
    outRender->ClearRed = state->Sim.PositionX;
    outRender->ClearGreen = 0.1F;
    outRender->ClearBlue = 1.0F - state->Sim.PositionX;
    outRender->ClearAlpha = 1.0F;
    outRender->Uniform0 = state->Sim.PositionX;
    outRender->Uniform1 = static_cast<float32>(state->Sim.Bounces);
    return Status::Ok;
}

Status GameQuiesce(GameInstance* instance) noexcept
{
    State* s = AsState(instance);
    if (s == nullptr)
    {
        return Status::InvalidArgument;
    }
    s->Paused = true;
    return Status::Ok;
}

Status GameResume(GameInstance* instance) noexcept
{
    State* s = AsState(instance);
    if (s == nullptr)
    {
        return Status::InvalidArgument;
    }
    s->Paused = false;
    return Status::Ok;
}

bool IsFinite(float32 value) noexcept
{
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7F800000U) != 0x7F800000U;
}

bool IsFinite(float64 value) noexcept
{
    uint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL;
}

template <typename T>
bool WriteScalar(CheckpointWriter& writer, uint32 id, T value) noexcept
{
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    uint64 bits = 0;
    if constexpr (sizeof(T) == 4)
    {
        uint32 narrow = 0;
        std::memcpy(&narrow, &value, sizeof(value));
        bits = narrow;
    }
    else
    {
        std::memcpy(&bits, &value, sizeof(value));
    }
    uint8 bytes[8] = {};
    if (WriteCheckpointUint({bytes, sizeof(value)}, bits) == false)
    {
        return false;
    }
    return writer.Field(id, {bytes, sizeof(value)});
}

template <typename T>
bool ReadScalar(ByteView payload, T& value) noexcept
{
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    if (payload.Size != sizeof(value))
    {
        return false;
    }
    const uint64 bits = ReadCheckpointUint(payload.Data, payload.Size);
    if constexpr (sizeof(T) == 4)
    {
        const uint32 narrow = static_cast<uint32>(bits);
        std::memcpy(&value, &narrow, sizeof(value));
    }
    else
    {
        std::memcpy(&value, &bits, sizeof(value));
    }
    return true;
}

constexpr usize kCheckpointBytes = 68;

Status GameCheckpointSize(GameInstance* instance, usize* outBodySize) noexcept
{
    if (instance == nullptr || outBodySize == nullptr)
    {
        return Status::InvalidArgument;
    }
    *outBodySize = kCheckpointBytes;
    return Status::Ok;
}

Status
GameWriteCheckpoint(GameInstance* instance, CheckpointHeader* outHeader, ByteSpan body, usize* outBytesWritten) noexcept
{
    State* state = AsState(instance);
    if (state == nullptr || outHeader == nullptr || outBytesWritten == nullptr)
    {
        return Status::InvalidArgument;
    }
    CheckpointWriter writer{body};
    bool ok = true;
    ok = WriteScalar(writer, 1, state->Sim.PositionX) && ok;
    ok = WriteScalar(writer, 2, state->Sim.Velocity) && ok;
    ok = WriteScalar(writer, 3, state->Sim.Speed) && ok;
    ok = WriteScalar(writer, 4, state->Sim.Bounces) && ok;
    ok = WriteScalar(writer, 5, state->Sim.Rng) && ok;
    ok = WriteScalar(writer, 6, state->Sim.SimTime) && ok;
    ok = WriteScalar(writer, 7, state->Revision) && ok;
    if (ok == false)
    {
        return Status::BufferTooSmall;
    }
    outHeader->FormatVersion = 1;
    outHeader->SchemaVersion = kCheckpointSchema;
    outHeader->ProjectId = state->ProjectId;
    outHeader->GameId = state->GameId;
    outHeader->ModuleBuildId = state->BuildId;
    outHeader->BodyLength = writer.Offset;
    outHeader->BodyDigest = Fnv1a(body.Data, writer.Offset);
    *outBytesWritten = writer.Offset;
    return Status::Ok;
}

Status GameCreateCandidate(const CreateInfo* info,
                           const CheckpointHeader* header,
                           ByteView body,
                           GameCandidate** outCandidate) noexcept
{
    if (info == nullptr || header == nullptr || outCandidate == nullptr || body.Data == nullptr ||
        header->FormatVersion != 1 || header->ProjectId != info->ProjectId || header->GameId != info->GameId ||
        header->BodyLength != body.Size || Fnv1a(body.Data, body.Size) != header->BodyDigest)
    {
        return Status::InvalidArgument;
    }
    if (header->SchemaVersion != kCheckpointSchema)
    {
        return Status::MigrationUnsupported;
    }
    auto* state = new (std::nothrow) State();
    if (state == nullptr)
    {
        return Status::Internal;
    }
    state->ProjectId = info->ProjectId;
    state->GameId = info->GameId;
    state->BuildId = info->ModuleGeneration;
    CheckpointReader reader{body};
    uint32 seen = 0;
    bool ok = true;
    while (reader.Offset < body.Size && ok)
    {
        uint32 tag = 0;
        ByteView payload = {};
        if (reader.Next(tag, payload) == false || tag > 7 || (seen & (1U << tag)) != 0)
        {
            ok = false;
            break;
        }
        seen |= 1U << tag;
        switch (tag)
        {
            case 1:
                ok = ReadScalar(payload, state->Sim.PositionX);
                break;
            case 2:
                ok = ReadScalar(payload, state->Sim.Velocity);
                break;
            case 3:
                ok = ReadScalar(payload, state->Sim.Speed);
                break;
            case 4:
                ok = ReadScalar(payload, state->Sim.Bounces);
                break;
            case 5:
                ok = ReadScalar(payload, state->Sim.Rng);
                break;
            case 6:
                ok = ReadScalar(payload, state->Sim.SimTime);
                break;
            case 7:
                ok = ReadScalar(payload, state->Revision);
                break;
            default:
                ok = false;
                break;
        }
    }
    const uint32 required = 254U;
    reflection::Value snapshot[kBodyFieldCount]{};
    reflection::Diagnostic error;
    if (ok == false || reader.Offset != body.Size || seen != required ||
        ludus::sample::ReadBody(state->Sim, snapshot, error) != reflection::Status::Ok ||
        IsFinite(state->Sim.PositionX) == false || IsFinite(state->Sim.Velocity) == false ||
        IsFinite(state->Sim.SimTime) == false || state->Sim.PositionX < 0.0F || state->Sim.PositionX > 1.0F ||
        state->Revision == 0)
    {
        delete state;
        return Status::InvalidArgument;
    }
    state->Paused = true;
    *outCandidate = reinterpret_cast<GameCandidate*>(state);
    return Status::Ok;
}

Status GameValidateCandidate(GameCandidate* candidate) noexcept
{
    auto* s = reinterpret_cast<State*>(candidate);
    reflection::Value snapshot[kBodyFieldCount]{};
    reflection::Diagnostic error;
    return s != nullptr && ludus::sample::ReadBody(s->Sim, snapshot, error) == reflection::Status::Ok
               ? Status::Ok
               : Status::OutOfRange;
}

Status GameCommitCandidate(GameCandidate* candidate, GameInstance** outInstance) noexcept
{
    if (candidate == nullptr || outInstance == nullptr)
    {
        return Status::InvalidArgument;
    }
    *outInstance = reinterpret_cast<GameInstance*>(candidate);
    return Status::Ok;
}

void GameDiscardCandidate(GameCandidate* candidate) noexcept
{
    delete reinterpret_cast<State*>(candidate);
}

Status GameDescribePropertiesSize(GameInstance* instance, usize* outByteSize) noexcept
{
    if (instance == nullptr || outByteSize == nullptr)
    {
        return Status::InvalidArgument;
    }
    *outByteSize = sizeof(PropertyDescriptor) * kBodyFieldCount;
    return Status::Ok;
}

Status GameDescribeProperties(GameInstance* instance, ByteSpan out, usize* outBytesWritten) noexcept
{
    if (instance == nullptr || outBytesWritten == nullptr)
    {
        return Status::InvalidArgument;
    }
    const usize needed = sizeof(PropertyDescriptor) * kBodyFieldCount;
    if (out.Data == nullptr || out.Capacity < needed)
    {
        return Status::BufferTooSmall;
    }
    PropertyDescriptor descs[kBodyFieldCount]{};
    reflection::Diagnostic error;
    if (property_binding::Describe(ludus::sample::BodySchema(), kObjectId, descs, error) != reflection::Status::Ok)
    {
        return Status::Internal;
    }

    std::memcpy(out.Data, descs, needed);
    *outBytesWritten = needed;
    return Status::Ok;
}

Status GameReadProperties(GameInstance* instance, ByteSpan out, usize* outBytesWritten) noexcept
{
    State* s = AsState(instance);
    if (s == nullptr || outBytesWritten == nullptr)
    {
        return Status::InvalidArgument;
    }
    const usize needed = sizeof(PropertyValue) * kBodyFieldCount;
    if (out.Data == nullptr || out.Capacity < needed)
    {
        return Status::BufferTooSmall;
    }
    reflection::Value snapshot[kBodyFieldCount]{};
    reflection::Diagnostic error;
    PropertyValue values[kBodyFieldCount]{};
    if (ludus::sample::ReadBody(s->Sim, snapshot, error) != reflection::Status::Ok ||
        property_binding::Snapshot(ludus::sample::BodySchema(), snapshot, kObjectId, s->Revision, values, error) !=
            reflection::Status::Ok)
    {
        return Status::OutOfRange;
    }
    std::memcpy(out.Data, values, needed);
    *outBytesWritten = needed;
    return Status::Ok;
}

// Thanks to Frederic My, "Using Custom RTTI Properties to Stream and Edit
// Objects", Game Programming Gems 4, section 1.12, pp. 111-124: shared schema
// constraints drive the inspector. This owner stages a candidate and checks the
// revision again at commit; it never retains an editor pointer into native state.
// Review: docs/architecture/reflection-serialization-gems-review.md.
struct Plan
{
    State* Owner = nullptr;
    uint64 ExpectedRevision = 0;
    float32 Speed = 0.0F;
};

Status GamePrepareEdits(GameInstance* instance, ByteView edits, GameEditPlan** outPlan) noexcept
{
    State* s = AsState(instance);
    if (s == nullptr || outPlan == nullptr || edits.Data == nullptr || edits.Size < sizeof(EditBatchHeader))
    {
        return Status::InvalidArgument;
    }
    EditBatchHeader header;
    std::memcpy(&header, edits.Data, sizeof(header));
    if (header.StructSize != sizeof(EditBatchHeader) || header.Count == 0 || header.Count > kMaxEditBatch)
    {
        return Status::InvalidArgument;
    }
    if (header.SchemaVersion != kPropertySchema)
    {
        return Status::SchemaChanged;
    }
    if (edits.Size != sizeof(EditBatchHeader) + sizeof(PropertyEdit) * header.Count)
    {
        return Status::InvalidArgument;
    }
    // IPC bytes need not be aligned for a native PropertyEdit.
    PropertyEdit input[kMaxEditBatch]{};
    std::memcpy(input, edits.Data + sizeof(EditBatchHeader), sizeof(PropertyEdit) * header.Count);
    for (uint32 index = 0; index < header.Count; ++index)
    {
        if (input[index].ObjectId != kObjectId || input[index].ExpectedRevision != s->Revision)
        {
            return Status::StaleRevision;
        }
    }
    reflection::Edit decoded[kMaxEditBatch]{};
    reflection::Diagnostic error;
    const reflection::Status status = property_binding::DecodeEdits(ludus::sample::BodySchema(),
                                                                    {input, header.Count},
                                                                    kObjectId,
                                                                    s->Revision,
                                                                    decoded,
                                                                    error);
    if (status != reflection::Status::Ok)
    {
        return status == reflection::Status::InvalidValue ? Status::OutOfRange : Status::InvalidArgument;
    }
    Body candidate;
    if (ludus::sample::PrepareBody(s->Sim, {decoded, header.Count}, candidate, error) != reflection::Status::Ok)
    {
        return Status::OutOfRange;
    }
    auto* plan = new (std::nothrow) Plan{ .Owner = s, .ExpectedRevision = s->Revision, .Speed = candidate.Speed };
    if (plan == nullptr)
    {
        return Status::Internal;
    }
    *outPlan = reinterpret_cast<GameEditPlan*>(plan);
    return Status::Ok;
}

Status GameCommitEdits(GameInstance* instance, GameEditPlan* planHandle) noexcept
{
    State* s = AsState(instance);
    auto* plan = reinterpret_cast<Plan*>(planHandle);
    if (s == nullptr || plan == nullptr || plan->Owner != s)
    {
        return Status::InvalidArgument;
    }
    if (s->Revision != plan->ExpectedRevision || s->Revision == ~uint64{0})
    {
        return Status::StaleRevision; // Failed plans remain owned until DiscardEdits.
    }
    s->Sim.Speed = plan->Speed;
    s->Revision += 1;
    delete plan;
    return Status::Ok;
}

void GameDiscardEdits(GameInstance* instance, GameEditPlan* planHandle) noexcept
{
    (void)instance;
    delete reinterpret_cast<Plan*>(planHandle);
}

const GameApiTable& Table() noexcept
{
    static const GameApiTable table = [] {
        GameApiTable t = {};
        t.StructSize = static_cast<uint32>(sizeof(GameApiTable));
        t.AbiMajor = kAbiMajor;
        t.AbiMinor = kAbiMinor;
        t.Query = &GameQuery;
        t.Create = &GameCreate;
        t.Destroy = &GameDestroy;
        t.Update = &GameUpdate;
        t.Quiesce = &GameQuiesce;
        t.Resume = &GameResume;
        t.CheckpointSize = &GameCheckpointSize;
        t.WriteCheckpoint = &GameWriteCheckpoint;
        t.CreateCandidate = &GameCreateCandidate;
        t.ValidateCandidate = &GameValidateCandidate;
        t.CommitCandidate = &GameCommitCandidate;
        t.DiscardCandidate = &GameDiscardCandidate;
        t.DescribePropertiesSize = &GameDescribePropertiesSize;
        t.DescribeProperties = &GameDescribeProperties;
        t.ReadProperties = &GameReadProperties;
        t.PrepareEdits = &GamePrepareEdits;
        t.CommitEdits = &GameCommitEdits;
        t.DiscardEdits = &GameDiscardEdits;
        return t;
    }();
    return table;
}
} // namespace

// NOLINTBEGIN(bugprone-easily-swappable-parameters) — fixed ABI entry signature
extern "C" LUDUS_GAME_API_EXPORT ludus::runtime::game_api::Status
LudusGetGameApi(ludus::foundation::uint32 hostAbiMajor,
                ludus::foundation::uint32 hostAbiMinor,
                ludus::runtime::game_api::GameApiTable* outTable) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    using namespace ludus::runtime::game_api;
    (void)hostAbiMinor;
    if (outTable == nullptr)
    {
        return Status::InvalidArgument;
    }
    if (hostAbiMajor != kAbiMajor)
    {
        return Status::IncompatibleAbi;
    }
    const uint32 hostSize = outTable->StructSize;
    const uint32 common =
        hostSize < static_cast<uint32>(sizeof(GameApiTable)) ? hostSize : static_cast<uint32>(sizeof(GameApiTable));
    std::memcpy(outTable, &Table(), common);
    outTable->StructSize = common;
    return Status::Ok;
}
