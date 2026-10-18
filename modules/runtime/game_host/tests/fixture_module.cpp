// Reference gameplay fixture module (project-live-reload L0-L5 acceptance).
//
// One gameplay implementation compiled into every reload fixture. It links ONLY
// Ludus::GameApi (no engine singleton runtime), exports exactly one unmangled
// entry (LudusGetGameApi) with hidden default visibility, and implements the
// full ABI table: mandatory Create/Update/Destroy, reload (Quiesce/Resume,
// checkpoint, candidate), live properties and one supported asset replacement.
//
// Two build variants (A and B) differ only by LUDUS_FIXTURE_VARIANT so the
// loader test can prove A and B run their own code concurrently, and B can
// migrate A's checkpoint. The identity string is baked in from the SDK variant
// so the host accepts a matching module and rejects a mismatched one.

#include <ludus/foundation/base/types.h>
#include <ludus/runtime/game_api/api.h>
#include <ludus/runtime/game_api/authored.h>
#include <ludus/runtime/game_api/checkpoint.h>
#include <ludus/runtime/game_api/properties.h>
#include <ludus/runtime/game_api/services.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

#include <pthread.h>
#include <unistd.h>

#ifndef LUDUS_FIXTURE_VARIANT
#    define LUDUS_FIXTURE_VARIANT A
#endif
#ifndef LUDUS_FIXTURE_IDENTITY
#    define LUDUS_FIXTURE_IDENTITY "unset-identity"
#endif

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

#define LUDUS_FIXTURE_STRINGIFY_(x) #x
#define LUDUS_FIXTURE_STRINGIFY(x) LUDUS_FIXTURE_STRINGIFY_(x)
constexpr const char* kVariantName = LUDUS_FIXTURE_STRINGIFY(LUDUS_FIXTURE_VARIANT);
constexpr const char* kIdentity = LUDUS_FIXTURE_IDENTITY;

constexpr bool kIsVariantB = (kVariantName[0] == 'B');

// Failure injection for pre-commit reload stages (tasks.md L3). A test sets
// LUDUS_FIXTURE_FAIL to a stage name; the matching callback returns an error so
// the host must preserve A and resume it unchanged. Read once per call; cheap.
[[nodiscard]] bool ShouldFail(const char* stage) noexcept
{
    const char* want = std::getenv("LUDUS_FIXTURE_FAIL");
    return want != nullptr && std::strcmp(want, stage) == 0;
}

// Checkpoint/property schema versions. Variant B bumps the checkpoint schema to
// exercise explicit migration (design 8).
constexpr uint32 kCheckpointSchemaA = 1;
constexpr uint32 kCheckpointSchemaB = 2;
constexpr uint32 kCheckpointSchema = kIsVariantB ? kCheckpointSchemaB : kCheckpointSchemaA;
constexpr uint32 kPropertySchema = 1;

constexpr uint32 kCapabilities = static_cast<uint32>(Capability::Reload) | static_cast<uint32>(Capability::Properties) |
                                 static_cast<uint32>(Capability::AssetReload);

// Stable property IDs (never inferred from ordering).
constexpr uint64 kObjectId = 0x1001;
constexpr uint64 kPropSpeed = 0x2001;   // float32, persistable
constexpr uint64 kPropBounces = 0x2002; // int32, read-only simulation output
constexpr uint64 kPropLabel = 0x2003;   // string, session-only

// Logical asset ID for the supported asset-reload fixture.
constexpr uint64 kAssetTint = 0x3001;

// Live state. Checkpoints below encode tagged fields explicitly.
struct SimState
{
    float32 PositionX = 0.0F;
    float32 Velocity = 0.6F;
    float32 Speed = 1.0F;
    int32 Bounces = 0;
    uint64 Rng = 0x9E3779B97F4A7C15ULL;
    float64 SimTime = 0.0;
    float32 TintR = 0.2F;
    float32 TintG = 0.3F;
    float32 TintB = 0.8F;
    float32 Energy = 0.0F;
    char Label[32] = "fixture";
    uint64 ProjectId = 0;
    uint64 GameId = 0;
    uint64 BuildId = 0;
    uint64 Revision = 1; // Advances when a mutable value changes.
    bool Paused = false;
    bool HostAllocated = false;
    const HostServices* Services = nullptr;
    pthread_t Worker = {};
    std::atomic<bool> WorkerExit{false};
    bool WorkerLive = false;
};

void* DeferredWork(void* argument) noexcept
{
    auto* state = static_cast<SimState*>(argument);
    while (state->WorkerExit.load(std::memory_order_acquire) == false)
    {
        const timespec delay = {0, 1000000};
        (void)::nanosleep(&delay, nullptr);
    }
    return nullptr;
}

bool JoinWork(SimState* state) noexcept
{
    if (state->WorkerLive == false)
    {
        return true;
    }
    state->WorkerExit.store(true, std::memory_order_release);
    if (::pthread_join(state->Worker, nullptr) != 0)
    {
        return false;
    }
    state->WorkerLive = false;
    return state->Services->ReleaseWorkLease(state->Services->Context);
}

// xorshift so the RNG state is deterministic and survives a reload exactly.
uint64 NextRng(uint64& state) noexcept
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// Thanks to Glenn Fowler, Landon Curt Noll and Phong Vo, "FNV Hash",
// "FNV-1a alternate algorithm" and "Parameters of the FNV-1/FNV-1a hash":
// https://www.isthe.com/chongo/tech/comp/fnv/ . Adopt the XOR/multiply sequence
// and published constants for a non-cryptographic hash; collisions remain possible.
[[nodiscard]] uint64 Fnv1a(const uint8* data, usize size) noexcept
{
    uint64 hash = 0xCBF29CE484222325ULL;
    for (usize i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

void HostLog(const SimState* state, LogSeverity severity, const char* text) noexcept
{
    if (state != nullptr && state->Services != nullptr && state->Services->Log != nullptr)
    {
        state->Services->Log(state->Services->Context, severity, text, std::strlen(text));
    }
}

// Each GameInstance/GameCandidate/GameEditPlan is just a SimState or an edit.
SimState* AsState(GameInstance* instance) noexcept
{
    return reinterpret_cast<SimState*>(instance);
}

// --- ABI implementations ---------------------------------------------------

Status FixtureQuery(GameMetadata* out) noexcept
{
    if (out == nullptr || out->StructSize < sizeof(GameMetadata))
    {
        return Status::InvalidArgument;
    }
    out->AbiMajor = kAbiMajor;
    out->AbiMinor = kAbiMinor;
    out->Capabilities = kCapabilities;
    if (ShouldFail("script-debug-null") || ShouldFail("script-debug-truncated") || ShouldFail("script-debug-minor"))
    {
        out->Capabilities |= static_cast<uint32>(Capability::ScriptDebug);
    }
    if (ShouldFail("script-debug-minor") || ShouldFail("script-debug-legacy"))
    {
        out->AbiMinor = 0;
    }
    out->PropertySchemaVersion = kPropertySchema;
    out->CheckpointSchemaVersion = kCheckpointSchema;
    const usize len = std::strlen(kIdentity);
    const usize bounded = len > kIdentityMax ? kIdentityMax : len;
    out->IdentityLength = static_cast<uint32>(bounded);
    std::memset(out->Identity, 0, sizeof(out->Identity));
    std::memcpy(out->Identity, kIdentity, bounded);
    return Status::Ok;
}

SimState* NewState(const CreateInfo* info) noexcept
{
    SimState* state = nullptr;
    if (info != nullptr && info->Services != nullptr && info->Services->AllocateBytes != nullptr)
    {
        void* storage = info->Services->AllocateBytes(info->Services->Context, sizeof(SimState), alignof(SimState));
        if (storage != nullptr)
        {
            state = new (storage) SimState();
            state->HostAllocated = true;
        }
    }
    else
    {
        state = new (std::nothrow) SimState();
    }
    if (state == nullptr)
    {
        return nullptr;
    }
    if (info != nullptr)
    {
        state->Services = info->Services;
        state->ProjectId = info->ProjectId;
        state->GameId = info->GameId;
        state->BuildId = info->ModuleGeneration;
    }
    return state;
}

void DestroyState(SimState* state) noexcept
{
    if (state == nullptr)
    {
        return;
    }
    if (JoinWork(state) == false)
    {
        return;
    }
    if (state->HostAllocated)
    {
        const HostServices* services = state->Services;
        state->~SimState();
        services->FreeBytes(services->Context, state);
    }
    else
    {
        delete state;
    }
}

Status FixtureCreate(const CreateInfo* info, GameInstance** outInstance) noexcept
{
    if (info == nullptr || outInstance == nullptr)
    {
        return Status::InvalidArgument;
    }
    SimState* state = NewState(info);
    if (state == nullptr)
    {
        return Status::Internal;
    }
    if (info->AuthoredDocument.Size != 0)
    {
        AuthoredReader reader;
        AuthoredRecord record;
        bool seen = false;
        bool valid = reader.Start(info->AuthoredDocument, info->GameId);
        while (valid && reader.Next(record))
        {
            valid = seen == false && record.Object == kObjectId && record.Property == kPropSpeed &&
                    record.Kind == static_cast<uint32>(PropertyKind::Float32) && record.Value.Size == 4;
            if (valid)
            {
                const auto bits = static_cast<uint32>(ReadCheckpointUint(record.Value.Data, record.Value.Size));
                std::memcpy(&state->Speed, &bits, sizeof(bits));
                valid = std::isfinite(state->Speed) && state->Speed >= 0.0F && state->Speed <= 10.0F;
                seen = true;
            }
        }
        if (valid == false || reader.Complete() == false)
        {
            DestroyState(state);
            return Status::InvalidArgument;
        }
    }
    HostLog(state, LogSeverity::Info, "fixture create");
    *outInstance = reinterpret_cast<GameInstance*>(state);
    return Status::Ok;
}

void FixtureDestroy(GameInstance* instance) noexcept
{
    if (kVariantName[0] == 'D' || (ShouldFail("work_pending") && AsState(instance)->WorkerLive))
    {
        std::abort(); // Native destructor failure / unsafe worker destruction probe.
    }
    HostLog(AsState(instance), LogSeverity::Debug, "fixture destroy");
    if (ShouldFail("retire") == false)
    {
        DestroyState(AsState(instance));
    }
}

Status FixtureUpdate(GameInstance* instance, const FrameInput* input, RenderParams* outRender) noexcept
{
    if (kVariantName[0] == 'C')
    {
        std::abort();
    }
    if (kVariantName[0] == 'H')
    {
        for (;;)
        {
            (void)::pause();
        }
    }
    SimState* state = AsState(instance);
    if (state == nullptr || input == nullptr || outRender == nullptr)
    {
        return Status::InvalidArgument;
    }
    if (ShouldFail("work_pending") && state->WorkerLive == false)
    {
        const HostServices* services = state->Services;
        if (services == nullptr || services->StructSize < sizeof(HostServices) ||
            services->AcquireWorkLease == nullptr || services->ReleaseWorkLease == nullptr ||
            services->AcquireWorkLease(services->Context) == false)
        {
            return Status::Internal;
        }
        state->WorkerExit.store(false, std::memory_order_release);
        if (::pthread_create(&state->Worker, nullptr, &DeferredWork, state) != 0)
        {
            (void)services->ReleaseWorkLease(services->Context);
            return Status::Internal;
        }
        state->WorkerLive = true;
    }
    // Advance iff the host says this frame is not paused. Pause/Step is driven
    // by input.Paused alone so a single Step advances exactly one tick (design 8).
    if (input->Paused == 0)
    {
        state->SimTime = input->ElapsedSeconds;
        state->PositionX += state->Velocity * state->Speed * static_cast<float32>(input->DeltaSeconds);
        if (state->PositionX > 1.0F)
        {
            state->PositionX = 1.0F;
            state->Velocity = -state->Velocity;
            state->Bounces += 1;
            state->Energy += 0.1F;
            (void)NextRng(state->Rng);
            state->Revision += 1;
        }
        else if (state->PositionX < 0.0F)
        {
            state->PositionX = 0.0F;
            state->Velocity = -state->Velocity;
            state->Bounces += 1;
            state->Energy += 0.1F;
            (void)NextRng(state->Rng);
            state->Revision += 1;
        }
    }
    // Variant B renders a distinctly brighter frame so a rendered change is
    // observable; both derive the clear color from the asset-backed tint.
    const float32 variantBoost = kIsVariantB ? 0.4F : 0.0F;
    outRender->ClearRed = state->TintR + variantBoost;
    outRender->ClearGreen = state->TintG;
    outRender->ClearBlue = state->TintB;
    outRender->ClearAlpha = 1.0F;
    outRender->Uniform0 = state->PositionX;
    outRender->Uniform1 = static_cast<float32>(state->Bounces);
    outRender->Uniform2 = state->Energy;
    outRender->Uniform3 = kIsVariantB ? 1.0F : 0.0F;
    return Status::Ok;
}

Status FixtureQuiesce(GameInstance* instance) noexcept
{
    SimState* state = AsState(instance);
    if (state == nullptr)
    {
        return Status::InvalidArgument;
    }
    if (ShouldFail("quiesce"))
    {
        return Status::Internal; // must leave A resumable (not mutated here)
    }
    if (ShouldFail("work_pending") == false && JoinWork(state) == false)
    {
        return Status::Internal;
    }
    state->Paused = true;
    return Status::Ok;
}

Status FixtureResume(GameInstance* instance) noexcept
{
    SimState* state = AsState(instance);
    if (state == nullptr)
    {
        return Status::InvalidArgument;
    }
    state->Paused = false;
    return Status::Ok;
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

constexpr usize kCheckpointBytes = 128;

Status FixtureCheckpointSize(GameInstance* instance, usize* outBodySize) noexcept
{
    if (instance == nullptr || outBodySize == nullptr)
    {
        return Status::InvalidArgument;
    }
    if (ShouldFail("checkpoint"))
    {
        return Status::Internal;
    }
    *outBodySize = kCheckpointBytes + (kIsVariantB ? 8 : 0);
    return Status::Ok;
}

Status FixtureWriteCheckpoint(GameInstance* instance,
                              CheckpointHeader* outHeader,
                              ByteSpan body,
                              usize* outBytesWritten) noexcept
{
    SimState* state = AsState(instance);
    if (state == nullptr || outHeader == nullptr || outBytesWritten == nullptr)
    {
        return Status::InvalidArgument;
    }
    CheckpointWriter writer{body};
    bool ok = true;
    ok = WriteScalar(writer, 1, state->PositionX) && ok;
    ok = WriteScalar(writer, 2, state->Velocity) && ok;
    ok = WriteScalar(writer, 3, state->Speed) && ok;
    ok = WriteScalar(writer, 4, state->Bounces) && ok;
    ok = WriteScalar(writer, 5, state->Rng) && ok;
    ok = WriteScalar(writer, 6, state->SimTime) && ok;
    ok = WriteScalar(writer, 7, state->TintR) && ok;
    ok = WriteScalar(writer, 8, state->TintG) && ok;
    ok = WriteScalar(writer, 9, state->TintB) && ok;
    ok = writer.Field(10, {reinterpret_cast<const uint8*>(state->Label), sizeof(state->Label)}) && ok;
    ok = WriteScalar(writer, 11, state->Revision) && ok;
    if (kIsVariantB)
    {
        ok = WriteScalar(writer, 12, state->Energy) && ok;
    }
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

Status FixtureCreateCandidate(const CreateInfo* info,
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
    if (ShouldFail("stage"))
    {
        return Status::Internal;
    }
    if (header->SchemaVersion != 1 && header->SchemaVersion != 2)
    {
        return Status::MigrationUnsupported;
    }
    auto* state = NewState(info);
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
        if (reader.Next(tag, payload) == false || tag > 12 || (seen & (1U << tag)) != 0)
        {
            ok = false;
            break;
        }
        seen |= 1U << tag;
        switch (tag)
        {
            case 1:
                ok = ReadScalar(payload, state->PositionX);
                break;
            case 2:
                ok = ReadScalar(payload, state->Velocity);
                break;
            case 3:
                ok = ReadScalar(payload, state->Speed);
                break;
            case 4:
                ok = ReadScalar(payload, state->Bounces);
                break;
            case 5:
                ok = ReadScalar(payload, state->Rng);
                break;
            case 6:
                ok = ReadScalar(payload, state->SimTime);
                break;
            case 7:
                ok = ReadScalar(payload, state->TintR);
                break;
            case 8:
                ok = ReadScalar(payload, state->TintG);
                break;
            case 9:
                ok = ReadScalar(payload, state->TintB);
                break;
            case 10:
                ok = payload.Size == sizeof(state->Label);
                if (ok)
                {
                    std::memcpy(state->Label, payload.Data, payload.Size);
                    ok = std::memchr(state->Label, '\0', sizeof(state->Label)) != nullptr;
                }
                break;
            case 11:
                ok = ReadScalar(payload, state->Revision);
                break;
            case 12:
                // Schema 2 adds Energy; schema 1 migration defaults it to zero.
                // The downgrade explicitly accepts then discards Energy.
                ok = header->SchemaVersion == 2 && ReadScalar(payload, state->Energy);
                break;
            default:
                ok = false;
                break;
        }
    }
    const uint32 required = 4094U | (header->SchemaVersion == 2 ? 4096U : 0U);
    const bool valuesValid =
        std::isfinite(state->PositionX) && std::isfinite(state->Velocity) && std::isfinite(state->Speed) &&
        std::isfinite(state->SimTime) && std::isfinite(state->TintR) && std::isfinite(state->TintG) &&
        std::isfinite(state->TintB) && std::isfinite(state->Energy) && state->Speed >= 0.0F && state->Speed <= 10.0F &&
        state->Bounces >= 0 && state->PositionX >= 0.0F && state->PositionX <= 1.0F && state->Revision != 0;
    if (ok == false || reader.Offset != body.Size || seen != required || valuesValid == false)
    {
        DestroyState(state);
        return Status::InvalidArgument;
    }
    state->Paused = true;
    *outCandidate = reinterpret_cast<GameCandidate*>(state);
    return Status::Ok;
}

Status FixtureValidateCandidate(GameCandidate* candidate) noexcept
{
    auto* state = reinterpret_cast<SimState*>(candidate);
    if (state == nullptr)
    {
        return Status::InvalidArgument;
    }
    if (ShouldFail("validate"))
    {
        return Status::OutOfRange;
    }
    // Reject impossible migrated state (design 8: validate before commit).
    if (state->Speed < 0.0F || state->Bounces < 0)
    {
        return Status::OutOfRange;
    }
    return Status::Ok;
}

Status FixtureCommitCandidate(GameCandidate* candidate, GameInstance** outInstance) noexcept
{
    if (candidate == nullptr || outInstance == nullptr)
    {
        return Status::InvalidArgument;
    }
    // The candidate SimState becomes the live instance; no allocation here.
    *outInstance = reinterpret_cast<GameInstance*>(candidate);
    return Status::Ok;
}

void FixtureDiscardCandidate(GameCandidate* candidate) noexcept
{
    if (ShouldFail("discard") == false)
    {
        DestroyState(reinterpret_cast<SimState*>(candidate));
    }
}

// --- Properties ------------------------------------------------------------

usize PropertyDescribeBytes() noexcept
{
    return sizeof(PropertyDescriptor) * 3;
}

Status FixtureDescribePropertiesSize(GameInstance* instance, usize* outByteSize) noexcept
{
    if (instance == nullptr || outByteSize == nullptr)
    {
        return Status::InvalidArgument;
    }
    *outByteSize = PropertyDescribeBytes();
    return Status::Ok;
}

PropertyDescriptor
MakeDescriptor(uint64 propId, PropertyKind kind, PropertyScope scope, bool writable, const char* label) noexcept
{
    PropertyDescriptor d;
    d.ObjectId = kObjectId;
    d.PropertyId = propId;
    d.Kind = static_cast<uint32>(kind);
    d.Scope = static_cast<uint32>(scope);
    d.Writable = writable ? 1 : 0;
    const usize len = std::strlen(label);
    d.LabelLength = static_cast<uint8>(len > kMaxLabelBytes ? kMaxLabelBytes : len);
    std::memcpy(d.Label, label, d.LabelLength);
    d.MinFloat = 0.0F;
    d.MaxFloat = 10.0F;
    d.MinInt = 0;
    d.MaxInt = 1000000;
    return d;
}

Status FixtureDescribeProperties(GameInstance* instance, ByteSpan out, usize* outBytesWritten) noexcept
{
    if (instance == nullptr || outBytesWritten == nullptr)
    {
        return Status::InvalidArgument;
    }
    const usize needed = PropertyDescribeBytes();
    if (out.Data == nullptr || out.Capacity < needed)
    {
        return Status::BufferTooSmall;
    }
    PropertyDescriptor descs[3] = {
        MakeDescriptor(kPropSpeed, PropertyKind::Float32, PropertyScope::Persistable, true, "Speed"),
        MakeDescriptor(kPropBounces, PropertyKind::Int32, PropertyScope::ReadOnly, false, "Bounces"),
        MakeDescriptor(kPropLabel, PropertyKind::String, PropertyScope::SessionOnly, true, "Label"),
    };
    std::memcpy(out.Data, descs, needed);
    *outBytesWritten = needed;
    return Status::Ok;
}

Status FixtureReadProperties(GameInstance* instance, ByteSpan out, usize* outBytesWritten) noexcept
{
    SimState* state = AsState(instance);
    if (state == nullptr || outBytesWritten == nullptr)
    {
        return Status::InvalidArgument;
    }
    const usize needed = sizeof(PropertyValue) * 3;
    if (out.Data == nullptr || out.Capacity < needed)
    {
        return Status::BufferTooSmall;
    }
    PropertyValue values[3] = {};
    values[0].ObjectId = kObjectId;
    values[0].PropertyId = kPropSpeed;
    values[0].ObjectRevision = state->Revision;
    values[0].Kind = static_cast<uint32>(PropertyKind::Float32);
    values[0].Float = state->Speed;

    values[1].ObjectId = kObjectId;
    values[1].PropertyId = kPropBounces;
    values[1].ObjectRevision = state->Revision;
    values[1].Kind = static_cast<uint32>(PropertyKind::Int32);
    std::memcpy(&values[1].IntOrEnum, &state->Bounces, sizeof(int32));

    values[2].ObjectId = kObjectId;
    values[2].PropertyId = kPropLabel;
    values[2].ObjectRevision = state->Revision;
    values[2].Kind = static_cast<uint32>(PropertyKind::String);
    const usize labelLen = std::strlen(state->Label);
    values[2].StringLength = static_cast<uint32>(labelLen);
    std::memcpy(values[2].String, state->Label, labelLen);

    std::memcpy(out.Data, values, needed);
    *outBytesWritten = needed;
    return Status::Ok;
}

// Edit plan: validated edits copied into a heap record applied at CommitEdits.
struct EditPlan
{
    bool HasSpeed = false;
    float32 Speed = 0.0F;
    bool HasLabel = false;
    char Label[32] = {};
};

Status FixturePrepareEdits(GameInstance* instance, ByteView edits, GameEditPlan** outPlan) noexcept
{
    SimState* state = AsState(instance);
    if (state == nullptr || outPlan == nullptr || edits.Data == nullptr)
    {
        return Status::InvalidArgument;
    }
    if (edits.Size < sizeof(EditBatchHeader))
    {
        return Status::InvalidArgument;
    }
    EditBatchHeader header;
    std::memcpy(&header, edits.Data, sizeof(header));
    if (header.Count == 0 || header.Count > kMaxEditBatch)
    {
        return Status::OutOfRange;
    }
    if (header.SchemaVersion != kPropertySchema)
    {
        return Status::SchemaChanged;
    }
    const usize expected = sizeof(EditBatchHeader) + sizeof(PropertyEdit) * header.Count;
    if (edits.Size < expected)
    {
        return Status::InvalidArgument;
    }

    auto* plan = new (std::nothrow) EditPlan();
    if (plan == nullptr)
    {
        return Status::Internal;
    }
    const auto* editArray = reinterpret_cast<const PropertyEdit*>(edits.Data + sizeof(EditBatchHeader));
    for (uint32 i = 0; i < header.Count; ++i)
    {
        const PropertyEdit& e = editArray[i];
        if (e.ObjectId != kObjectId || e.ExpectedRevision != state->Revision)
        {
            delete plan;
            return Status::StaleRevision;
        }
        if (e.PropertyId == kPropSpeed && e.Kind == static_cast<uint32>(PropertyKind::Float32))
        {
            if (e.Float < 0.0F || e.Float > 10.0F)
            {
                delete plan;
                return Status::OutOfRange;
            }
            plan->HasSpeed = true;
            plan->Speed = e.Float;
        }
        else if (e.PropertyId == kPropLabel && e.Kind == static_cast<uint32>(PropertyKind::String))
        {
            if (e.StringLength >= sizeof(plan->Label))
            {
                delete plan;
                return Status::OutOfRange;
            }
            plan->HasLabel = true;
            std::memcpy(plan->Label, e.String, e.StringLength);
            plan->Label[e.StringLength] = '\0';
        }
        else
        {
            delete plan; // read-only or unknown property rejected
            return Status::InvalidArgument;
        }
    }
    *outPlan = reinterpret_cast<GameEditPlan*>(plan);
    return Status::Ok;
}

Status FixtureCommitEdits(GameInstance* instance, GameEditPlan* planHandle) noexcept
{
    SimState* state = AsState(instance);
    auto* plan = reinterpret_cast<EditPlan*>(planHandle);
    if (state == nullptr || plan == nullptr)
    {
        return Status::InvalidArgument;
    }
    if (plan->HasSpeed)
    {
        state->Speed = plan->Speed;
    }
    if (plan->HasLabel)
    {
        std::memcpy(state->Label, plan->Label, sizeof(state->Label));
    }
    state->Revision += 1;
    delete plan;
    return Status::Ok;
}

void FixtureDiscardEdits(GameInstance* instance, GameEditPlan* planHandle) noexcept
{
    (void)instance;
    delete reinterpret_cast<EditPlan*>(planHandle);
}

// --- Asset reload ----------------------------------------------------------

Status FixtureReloadAsset(GameInstance* instance,
                          uint64 logicalAssetId,
                          ByteView cookedArtifact,
                          uint64 artifactDigest) noexcept
{
    SimState* state = AsState(instance);
    if (state == nullptr || cookedArtifact.Data == nullptr)
    {
        return Status::InvalidArgument;
    }
    if (logicalAssetId != kAssetTint)
    {
        return Status::Unsupported; // explicit unsupported asset kind
    }
    // Cooked artifact is exactly three float32 RGB tint channels.
    if (cookedArtifact.Size != sizeof(float32) * 3)
    {
        return Status::InvalidArgument;
    }
    if (Fnv1a(cookedArtifact.Data, cookedArtifact.Size) != artifactDigest)
    {
        return Status::InvalidArgument; // validation failed -> old asset retained
    }
    float32 rgb[3];
    std::memcpy(rgb, cookedArtifact.Data, sizeof(rgb));
    state->TintR = rgb[0];
    state->TintG = rgb[1];
    state->TintB = rgb[2];
    state->Revision += 1;
    return Status::Ok;
}

const GameApiTable& FixtureTable() noexcept
{
    static const GameApiTable table = [] {
        GameApiTable t = {};
        t.StructSize = static_cast<uint32>(sizeof(GameApiTable));
        t.AbiMajor = kAbiMajor;
        t.AbiMinor = kAbiMinor;
        t.Query = &FixtureQuery;
        t.Create = &FixtureCreate;
        t.Destroy = &FixtureDestroy;
        t.Update = &FixtureUpdate;
        t.Quiesce = &FixtureQuiesce;
        t.Resume = &FixtureResume;
        t.CheckpointSize = &FixtureCheckpointSize;
        t.WriteCheckpoint = &FixtureWriteCheckpoint;
        t.CreateCandidate = &FixtureCreateCandidate;
        t.ValidateCandidate = &FixtureValidateCandidate;
        t.CommitCandidate = &FixtureCommitCandidate;
        t.DiscardCandidate = &FixtureDiscardCandidate;
        t.DescribePropertiesSize = &FixtureDescribePropertiesSize;
        t.DescribeProperties = &FixtureDescribeProperties;
        t.ReadProperties = &FixtureReadProperties;
        t.PrepareEdits = &FixturePrepareEdits;
        t.CommitEdits = &FixtureCommitEdits;
        t.DiscardEdits = &FixtureDiscardEdits;
        t.ReloadAsset = &FixtureReloadAsset;
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
    // Negotiate the common table size: copy only up to the host-provided size.
    const uint32 hostSize = outTable->StructSize;
    GameApiTable full = FixtureTable();
    if (ShouldFail("script-debug-truncated"))
    {
        full.StructSize = offsetof(GameApiTable, ProcessScriptDebug);
    }
    if (ShouldFail("script-debug-minor") || ShouldFail("script-debug-legacy"))
    {
        full.AbiMinor = 0;
    }
    if (ShouldFail("script-debug-minor"))
    {
        full.ProcessScriptDebug = [](GameInstance*, ByteView, ByteSpan, usize*, uint32*) noexcept {
            return Status::Unsupported;
        };
    }
    if (ShouldFail("script-debug-legacy"))
    {
        full.StructSize = offsetof(GameApiTable, ProcessScriptDebug);
    }
    const uint32 common =
        hostSize < static_cast<uint32>(sizeof(GameApiTable)) ? hostSize : static_cast<uint32>(sizeof(GameApiTable));
    std::memcpy(outTable, &full, common);
    outTable->StructSize = full.StructSize < common ? full.StructSize : common;
    return Status::Ok;
}
