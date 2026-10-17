// Thanks to Celes/de Figueiredo/Ierusalimschy, "Binding C/C++ Objects to Lua",
// Game Programming Gems 6, §4.2, pp.341–355: copied identities and scoped
// facades separate host state from VM object ownership. Thanks to Roblox,
// "Embedding a sandboxed Luau virtual machine", https://luau.org/sandbox/:
// only owner-admitted paired-compiler bytecode enters the interpreter.
// This original generic adapter reuses S1's POD bridge and reviewed loader.
// See docs/architecture/behavior-s4.md for the installed contract and limits.
#include <ludus/runtime/behavior/behavior.h>

#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>

#include <cstring>
#include <new>
#include <type_traits>

#include <lua.h>
#include <lualib.h>

#include "internal/runtime.h"

namespace ludus::runtime::behavior
{
namespace
{
// Bounded contract/value admission shared by native and Luau execution.
bool Name(const char* text) noexcept
{
    if (text == nullptr)
    {
        return false;
    }
    for (usize i = 0; i < 64; ++i)
    {
        const char c = text[i];
        if (c == 0)
        {
            return i != 0;
        }
        if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z') && (i == 0 || ((c < '0' || c > '9') && c != '_')))
        {
            return false;
        }
    }
    return false;
}
bool Hex(std::string_view text, uint8* output) noexcept
{
    if (text.size() != 64)
    {
        return false;
    }
    for (usize i = 0; i < 64; ++i)
    {
        const char c = text[i];
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f'))
        {
            return false;
        }
        const auto digit = static_cast<uint8>(c <= '9' ? c - '0' : c - 'a' + 10);
        if (i % 2 == 0)
        {
            output[i / 2] = static_cast<uint8>(digit << 4U);
        }
        else
        {
            output[i / 2] |= digit;
        }
    }
    return true;
}
bool Specs(std::span<const FieldSpec> specs, bool entity) noexcept
{
    if (specs.size() > 16)
    {
        return false;
    }
    for (usize i = 0; i < specs.size(); ++i)
    {
        const auto& field = specs[i];
        if (field.Id == 0 || !Name(field.Name) || field.Min > field.Max || field.Default < field.Min ||
            field.Default > field.Max)
        {
            return false;
        }
        if (field.Type == Kind::Entity)
        {
            if (!entity || field.Min != 0 || field.Max != 0)
            {
                return false;
            }
        }
        else if ((field.Type != Kind::Uint32 && field.Type != Kind::Boolean) ||
                 (field.Type == Kind::Boolean && field.Max > 1))
        {
            return false;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (field.Id == specs[j].Id || std::strcmp(field.Name, specs[j].Name) == 0)
            {
                return false;
            }
        }
    }
    return true;
}
bool ValidContract(const Contract& contract) noexcept
{
    uint8 hash[32]{};
    if (contract.Digest == nullptr || std::strlen(contract.Digest) != 64 || !Hex(contract.Digest, hash) ||
        contract.State.empty() || !Specs(contract.Config, false) || !Specs(contract.State, false) ||
        !Specs(contract.Event, true) || contract.Operations.size() > 16)
    {
        return false;
    }
    for (usize i = 0; i < contract.Operations.size(); ++i)
    {
        const auto& op = contract.Operations[i];
        if (op.Id == 0 || !Name(op.Name) || op.Phase == 0 || op.Phase > 255 || op.Capability == 0 ||
            op.Arguments.size() > 4 || !Specs(op.Arguments, true))
        {
            return false;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (op.Id == contract.Operations[j].Id || std::strcmp(op.Name, contract.Operations[j].Name) == 0)
            {
                return false;
            }
        }
    }
    return true;
}
bool Empty(EntityRef ref) noexcept
{
    return ref.World == 0 && ref.Session == 0 && ref.Execution == 0 && ref.Slot == 0 && ref.Generation == 0;
}
bool ValidValue(Value value, const FieldSpec& spec, const Invocation& input, Services services) noexcept
{
    if (value.Type != spec.Type)
    {
        return false;
    }
    if (spec.Type != Kind::Entity)
    {
        return Empty(value.Entity) && value.Scalar >= spec.Min && value.Scalar <= spec.Max;
    }
    const auto ref = value.Entity;
    return value.Scalar == 0 && ref.World == input.World && ref.Session == input.Session &&
           ref.Execution == input.Execution && ref.Generation != 0 && services.IsAlive != nullptr &&
           services.IsAlive(services.User, ref);
}
bool ValidRecord(const Record& record,
                 std::span<const FieldSpec> specs,
                 const Invocation& input,
                 Services services) noexcept
{
    if (record.Count != specs.size())
    {
        return false;
    }
    for (usize i = 0; i < specs.size(); ++i)
    {
        uint32 found = 0;
        for (uint32 j = 0; j < record.Count; ++j)
        {
            if (record.Items[j].Id == specs[i].Id)
            {
                ++found;
                if (!ValidValue(record.Items[j].Data, specs[i], input, services))
                {
                    return false;
                }
            }
        }
        if (found != 1)
        {
            return false;
        }
    }
    return true;
}
bool ValidInput(const Contract& contract, const Invocation& input, Services services) noexcept
{
    return input.Asset != 0 && input.Revision != 0 && input.Instance != 0 && input.World != 0 && input.Session != 0 &&
           input.Execution != 0 && input.Phase != 0 && input.Phase <= 255 &&
           ValidRecord(input.Config, contract.Config, input, services) &&
           ValidRecord(input.State, contract.State, input, services) &&
           ValidRecord(input.Event, contract.Event, input, services);
}
const Operation* Find(const Contract& contract, uint32 id) noexcept
{
    for (const auto& operation : contract.Operations)
    {
        if (operation.Id == id)
        {
            return &operation;
        }
    }
    return nullptr;
}
CommandStatus Admit(const Contract& contract,
                    const Invocation& input,
                    Services services,
                    uint32 operation,
                    std::span<const Value> arguments) noexcept
{
    const auto* op = Find(contract, operation);
    if (op == nullptr || op->Arguments.size() != arguments.size())
    {
        return CommandStatus::InvalidValue;
    }
    if (input.Phase != op->Phase)
    {
        return CommandStatus::InvalidPhase;
    }
    if ((input.Capabilities & op->Capability) != op->Capability)
    {
        return CommandStatus::MissingCapability;
    }
    for (usize i = 0; i < arguments.size(); ++i)
    {
        if (!ValidValue(arguments[i], op->Arguments[i], input, services))
        {
            return op->Arguments[i].Type == Kind::Entity ? CommandStatus::InvalidEntity : CommandStatus::InvalidValue;
        }
    }
    return CommandStatus::Accepted;
}
Value FieldValue(const Record& record, uint32 id) noexcept
{
    for (uint32 i = 0; i < record.Count; ++i)
    {
        if (record.Items[i].Id == id)
        {
            return record.Items[i].Data;
        }
    }
    return {};
}
Status Convert(scripting::Status code) noexcept
{
    switch (code)
    {
        case scripting::Status::Completed:
            return Status::Completed;
        case scripting::Status::InvalidArtifact:
            return Status::InvalidPackage;
        case scripting::Status::NotReady:
            return Status::NotReady;
        case scripting::Status::Reentrant:
            return Status::Reentrant;
        case scripting::Status::AllocationFailure:
            return Status::AllocationFailure;
        case scripting::Status::Interrupted:
            return Status::Interrupted;
        case scripting::Status::Paused:
            return Status::Paused;
        default:
            return Status::ScriptFault;
    }
}
} // namespace

Transaction::Transaction(const Contract& contract, const Invocation& input, Services services) noexcept
    : mContract(contract), mInput(input), mServices(services), mCandidate{ .State = input.State }
{
}
Record& Transaction::State() noexcept
{
    return mCandidate.State;
}
const Invocation& Transaction::Input() const noexcept
{
    return mInput;
}
CommandResult Transaction::Request(uint32 operation, std::span<const Value> arguments) noexcept
{
    auto status = Admit(mContract, mInput, mServices, operation, arguments);
    if (status == CommandStatus::Accepted && mCandidate.Count == 16)
    {
        status = CommandStatus::CapacityExceeded;
    }
    mOperation = operation;
    mNativeStatus = static_cast<uint32>(status);
    if (status != CommandStatus::Accepted)
    {
        return { .Status = status };
    }
    auto& command = mCandidate.Commands[mCandidate.Count++];
    command.Operation = operation;
    command.Token = mCandidate.Count;
    command.Count = static_cast<uint32>(arguments.size());
    for (usize i = 0; i < arguments.size(); ++i)
    {
        command.Arguments[i] = arguments[i];
    }
    return { .Status = status, .Token = command.Token };
}
Status ExecuteNative(const Contract& contract,
                     const Invocation& input,
                     Services services,
                     NativeHandler handler,
                     void* user,
                     Outcome& output) noexcept
{
    if (!ValidContract(contract))
    {
        return Status::InvalidContract;
    }
    if (handler == nullptr || !ValidInput(contract, input, services))
    {
        return Status::InvalidInput;
    }
    Transaction candidate(contract, input, services);
    const auto status = handler(candidate, user);
    if (status != Status::Completed)
    {
        return status;
    }
    if (!ValidRecord(candidate.mCandidate.State, contract.State, input, services))
    {
        return Status::InvalidInput;
    }
    output = candidate.mCandidate;
    return Status::Completed;
}
std::string_view LuauProfile() noexcept
{
    return LUDUS_BEHAVIOR_PROFILE;
}

// Declared scalar state only; wire records never serialize native object layout.
bool EncodeState(const Contract& contract, const Record& state, std::span<uint8> output, usize& written) noexcept
{
    if (!ValidContract(contract) || !ValidRecord(state, contract.State, {}, {}))
    {
        return false;
    }
    const usize size = 44 + static_cast<usize>(state.Count) * 12;
    if (output.size() < size)
    {
        return false;
    }
    uint8 bytes[236]{};
    auto data = std::span<uint8>{bytes};
    (void)TryWriteLittleEndian(uint32{0x34534842}, data);
    (void)TryWriteLittleEndian(uint32{1}, data.subspan(4));
    (void)Hex(contract.Digest, bytes + 8);
    (void)TryWriteLittleEndian(state.Count, data.subspan(40));
    // Descriptor order makes equivalent record permutations serialize identically.
    for (usize i = 0; i < contract.State.size(); ++i)
    {
        const auto field = contract.State[i];
        const auto value = FieldValue(state, field.Id);
        auto destination = data.subspan(44 + i * 12, 12);
        (void)TryWriteLittleEndian(field.Id, destination);
        (void)TryWriteLittleEndian(static_cast<uint32>(value.Type), destination.subspan(4));
        (void)TryWriteLittleEndian(value.Scalar, destination.subspan(8));
    }
    std::memcpy(output.data(), bytes, size);
    written = size;
    return true;
}
bool DecodeState(const Contract& contract, std::span<const uint8> bytes, Record& output) noexcept
{
    if (!ValidContract(contract) || bytes.size() < 44)
    {
        return false;
    }
    uint32 magic = 0, version = 0;
    uint8 digest[32]{};
    Record candidate;
    (void)TryReadLittleEndian(bytes, magic);
    (void)TryReadLittleEndian(bytes.subspan(4), version);
    (void)TryReadLittleEndian(bytes.subspan(40), candidate.Count);
    (void)Hex(contract.Digest, digest);
    if (magic != 0x34534842 || version != 1 || std::memcmp(bytes.data() + 8, digest, 32) != 0 || candidate.Count > 16 ||
        bytes.size() != 44 + static_cast<usize>(candidate.Count) * 12)
    {
        return false;
    }
    for (uint32 i = 0; i < candidate.Count; ++i)
    {
        const auto data = bytes.subspan(44 + static_cast<usize>(i) * 12, 12);
        uint32 kind = 0;
        (void)TryReadLittleEndian(data, candidate.Items[i].Id);
        (void)TryReadLittleEndian(data.subspan(4), kind);
        if (kind != 1 && kind != 2)
        {
            return false;
        }
        candidate.Items[i].Data.Type = static_cast<Kind>(kind);
        (void)TryReadLittleEndian(data.subspan(8), candidate.Items[i].Data.Scalar);
    }
    if (!ValidRecord(candidate, contract.State, {}, {}))
    {
        return false;
    }
    output = candidate;
    return true;
}
bool MigrateState(const Record& source, const Contract& target, Record& output) noexcept
{
    if (!ValidContract(target) || source.Count > 16)
    {
        return false;
    }
    Record candidate;
    candidate.Count = static_cast<uint32>(target.State.size());
    for (usize i = 0; i < target.State.size(); ++i)
    {
        const auto spec = target.State[i];
        candidate.Items[i] = { .Id = spec.Id, .Data = { .Type = spec.Type, .Scalar = spec.Default } };
        for (uint32 j = 0; j < source.Count; ++j)
        {
            if (source.Items[j].Id == spec.Id)
            {
                candidate.Items[i] = source.Items[j];
            }
        }
    }
    for (uint32 i = 0; i < source.Count; ++i)
    {
        bool found = false;
        for (const auto& spec : target.State)
        {
            found |= source.Items[i].Id == spec.Id;
        }
        if (!found)
        {
            return false;
        }
        for (uint32 j = 0; j < i; ++j)
        {
            if (source.Items[j].Id == source.Items[i].Id)
            {
                return false;
            }
        }
    }
    if (!ValidRecord(candidate, target.State, {}, {}))
    {
        return false;
    }
    output = candidate;
    return true;
}

// Protected VM calls keep POD-only bridge values and invocation-scoped facades.
struct LuauProvider::Impl
{
    struct Context
    {
        Impl* Owner = nullptr;
        Transaction* Candidate = nullptr;
    };
    struct StateFacade
    {
        uint64 Epoch = 0;
    };
    static_assert(std::is_trivially_destructible_v<Context>);
    static_assert(std::is_trivially_destructible_v<StateFacade>);
    scripting::Runtime Vm;
    Contract Schema;
    scripting::Program Programs[8];
    char Names[8][32]{};
    bool Busy = false;
    Invocation DebugInput;
    Transaction* Pending = nullptr;
    Context DebugContext;
    uint64 DebugStop = 0;
    ~Impl() noexcept
    {
        Vm.Close(); // Retire all callbacks before releasing their candidate/input storage.
        delete Pending;
    }
    static constexpr int32 ENTITY_TAG = 1;
    static constexpr int32 STATE_TAG = 2;

    static Context& Current(lua_State* state) noexcept
    {
        auto& call = scripting::GetCallContext(state);
        if (!call.Active || call.User == nullptr)
        {
            scripting::Reject(state, 1, "operation outside active invocation");
        }
        return *static_cast<Context*>(call.User);
    }
    static Value Read(lua_State* state, int32 index, Kind kind) noexcept
    {
        Value value{ .Type = kind };
        if (kind == Kind::Entity)
        {
            const auto* ref = static_cast<const EntityRef*>(lua_touserdatatagged(state, index, ENTITY_TAG));
            if (ref == nullptr)
            {
                scripting::Reject(state, 1, "opaque EntityRef required");
            }
            value.Entity = *ref;
        }
        else if (kind == Kind::Boolean)
        {
            if (lua_type(state, index) != LUA_TBOOLEAN)
            {
                scripting::Reject(state, 1, "boolean required");
            }
            value.Scalar = lua_toboolean(state, index) != 0 ? 1U : 0U;
        }
        else
        {
            if (lua_type(state, index) != LUA_TNUMBER)
            {
                scripting::Reject(state, 1, "uint32 required");
            }
            const float64 number = lua_tonumber(state, index);
            if (!foundation::math::IsFinite(number) || number < 0 || number > 4294967295.0)
            {
                scripting::Reject(state, 1, "uint32 range");
            }
            value.Scalar = static_cast<uint32>(number);
            if (static_cast<float64>(value.Scalar) != number)
            {
                scripting::Reject(state, 1, "exact integer required");
            }
        }
        return value;
    }
    static void Push(lua_State* state, Value value) noexcept
    {
        if (value.Type == Kind::Boolean)
        {
            lua_pushboolean(state, value.Scalar != 0);
        }
        else if (value.Type == Kind::Uint32)
        {
            lua_pushnumber(state, value.Scalar);
        }
        else
        {
            auto* ref = static_cast<EntityRef*>(lua_newuserdatatagged(state, sizeof(EntityRef), ENTITY_TAG));
            new (ref) EntityRef(value.Entity);
        }
    }
    static void PushRecord(lua_State* state, const Record& record, std::span<const FieldSpec> specs) noexcept
    {
        lua_createtable(state, 0, static_cast<int32>(specs.size()));
        for (const auto& field : specs)
        {
            Push(state, FieldValue(record, field.Id));
            lua_setfield(state, -2, field.Name);
        }
        lua_setreadonly(state, -1, 1);
    }
    static const FieldSpec& StateField(lua_State* state) noexcept
    {
        auto& current = Current(state);
        const auto* facade = static_cast<const StateFacade*>(lua_touserdatatagged(state, 1, STATE_TAG));
        if (facade == nullptr || facade->Epoch != scripting::GetCallContext(state).Epoch)
        {
            scripting::Reject(state, 1, "expired state facade");
        }
        if (lua_type(state, 2) != LUA_TSTRING)
        {
            scripting::Reject(state, 1, "field name required");
        }
        usize length = 0;
        const char* name = lua_tolstring(state, 2, &length);
        for (const auto& field : current.Owner->Schema.State)
        {
            if (length == std::strlen(field.Name) && std::memcmp(name, field.Name, length) == 0)
            {
                return field;
            }
        }
        scripting::Reject(state, 1, "unknown state field");
        return current.Owner->Schema.State[0];
    }
    static int32 ReadState(lua_State* state) noexcept
    {
        const auto& field = StateField(state);
        Push(state, FieldValue(Current(state).Candidate->State(), field.Id));
        return 1;
    }
    static int32 WriteState(lua_State* state) noexcept
    {
        const auto& field = StateField(state);
        const auto value = Read(state, 3, field.Type);
        if (value.Scalar < field.Min || value.Scalar > field.Max)
        {
            scripting::Reject(state, 1, "declared state range");
        }
        auto& record = Current(state).Candidate->State();
        for (uint32 i = 0; i < record.Count; ++i)
        {
            if (record.Items[i].Id == field.Id)
            {
                record.Items[i].Data = value;
            }
        }
        return 0;
    }
    static int32 Request(lua_State* state) noexcept
    {
        auto& current = Current(state);
        const auto index = static_cast<usize>(lua_tointeger(state, lua_upvalueindex(1)));
        const auto& op = current.Owner->Schema.Operations[index];
        scripting::GetCallContext(state).Operation = op.Id;
        if (lua_gettop(state) != static_cast<int32>(op.Arguments.size()))
        {
            scripting::Reject(state, 1, "operation arity");
        }
        // Value::Type defaults to Kind::Uint32; LLVM 23 overlooks that initializer
        // when checking value-initialized arrays. The provider regression checks it.
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
        Value values[4]{};
        for (usize i = 0; i < op.Arguments.size(); ++i)
        {
            values[i] = Read(state, static_cast<int32>(i + 1), op.Arguments[i].Type);
        }
        const auto result = current.Candidate->Request(op.Id, {values, op.Arguments.size()});
        scripting::GetCallContext(state).NativeStatus = static_cast<uint32>(result.Status);
        constexpr const char* NAMES[] =
            {"Accepted", "InvalidValue", "InvalidEntity", "InvalidPhase", "MissingCapability", "CapacityExceeded"};
        lua_createtable(state, 0, 2);
        lua_pushstring(state, NAMES[static_cast<uint8>(result.Status)]);
        lua_setfield(state, -2, "Status");
        lua_pushnumber(state, result.Token);
        lua_setfield(state, -2, "Token");
        lua_setreadonly(state, -1, 1);
        return 1;
    }
    static int32 Install(lua_State* state) noexcept
    {
        const auto* owner = static_cast<const Impl*>(scripting::GetCallContext(state).User);
        lua_createtable(state, 0, 3);
        lua_pushcfunction(state, ReadState, "State.__index");
        lua_setfield(state, -2, "__index");
        lua_pushcfunction(state, WriteState, "State.__newindex");
        lua_setfield(state, -2, "__newindex");
        lua_pushstring(state, "locked");
        lua_setfield(state, -2, "__metatable");
        lua_setreadonly(state, -1, 1);
        lua_setuserdatametatable(state, STATE_TAG);
        lua_createtable(state, 0, static_cast<int32>(owner->Schema.Operations.size()));
        for (usize i = 0; i < owner->Schema.Operations.size(); ++i)
        {
            lua_pushinteger(state, static_cast<int32>(i));
            lua_pushcclosure(state, Request, owner->Schema.Operations[i].Name, 1);
            lua_setfield(state, -2, owner->Schema.Operations[i].Name);
        }
        lua_setreadonly(state, -1, 1);
        lua_setglobal(state, "__ludus_api");
        return 0;
    }
    static int32 Arguments(lua_State* state) noexcept
    {
        const auto& current = Current(state);
        const auto& input = current.Candidate->Input();
        PushRecord(state, input.Config, current.Owner->Schema.Config);
        auto* facade =
            static_cast<StateFacade*>(lua_newuserdatataggedwithmetatable(state, sizeof(StateFacade), STATE_TAG));
        new (facade) StateFacade{scripting::GetCallContext(state).Epoch};
        PushRecord(state, input.Event, current.Owner->Schema.Event);
        lua_getglobal(state, "__ludus_api");
        return 4;
    }
    bool Package(std::span<const uint8> bytes, usize& count) noexcept
    {
        constexpr uint8 MAGIC[] = {'L', 'U', 'D', 'S', '4', 'P', 'K', 0};
        uint32 version = 0, total = 0, programs = 0, reserved = 0;
        uint8 profile[32]{}, contract[32]{};
        if (bytes.size() < 120 || bytes.size() > usize{2} * 1024 * 1024 || std::memcmp(bytes.data(), MAGIC, 8) != 0 ||
            !TryReadLittleEndian(bytes.subspan(8), version) || version != 1 ||
            !TryReadLittleEndian(bytes.subspan(12), total) || total != bytes.size() ||
            !TryReadLittleEndian(bytes.subspan(112), programs) || programs == 0 || programs > 8 ||
            !TryReadLittleEndian(bytes.subspan(116), reserved) || reserved != 0 || !Hex(LuauProfile(), profile) ||
            !Hex(Schema.Digest, contract) || std::memcmp(bytes.data() + 16, profile, 32) != 0 ||
            std::memcmp(bytes.data() + 48, contract, 32) != 0)
        {
            return false;
        }
        usize cursor = 120;
        bool entrypoint = false;
        for (uint32 i = 0; i < programs; ++i)
        {
            if (bytes.size() - cursor < 88)
            {
                return false;
            }
            auto data = bytes.subspan(cursor, 88);
            auto& program = Programs[i];
            uint32 size = 0, flags = 0;
            (void)TryReadLittleEndian(data, program.Asset);
            (void)TryReadLittleEndian(data.subspan(8), program.Revision);
            (void)TryReadLittleEndian(data.subspan(16), size);
            (void)TryReadLittleEndian(data.subspan(20), flags);
            if (program.Asset == 0 || program.Revision == 0 || size == 0 || size > 262144 ||
                (flags & ~uint32{0x10f}) != 0 || (flags & 15U) > 8)
            {
                return false;
            }
            for (uint32 j = 0; j < i; ++j)
            {
                if (program.Asset == Programs[j].Asset)
                {
                    return false;
                }
            }
            program.DependencyCount = static_cast<uint8>(flags & 15U);
            program.Entrypoint = (flags & 0x100U) != 0;
            entrypoint = entrypoint || program.Entrypoint;
            for (uint32 j = 0; j < 8; ++j)
            {
                (void)TryReadLittleEndian(data.subspan(24 + static_cast<usize>(j) * 8), program.Dependencies[j]);
                if ((j < program.DependencyCount) == (program.Dependencies[j] == 0))
                {
                    return false;
                }
                if (j < program.DependencyCount)
                {
                    for (uint32 k = 0; k < j; ++k)
                    {
                        if (program.Dependencies[j] == program.Dependencies[k])
                        {
                            return false;
                        }
                    }
                }
            }
            cursor += 88;
            if (bytes.size() - cursor < size || bytes[cursor] == 0)
            {
                return false;
            }
            program.Code = bytes.data() + cursor;
            program.Bytes = size;
            constexpr char HEX[] = "0123456789abcdef";
            std::memcpy(Names[i], "@asset/", 7);
            for (usize j = 0; j < 16; ++j)
            {
                Names[i][22 - j] = HEX[(program.Asset >> (j * 4U)) & 15U];
            }
            program.Source = Names[i];
            cursor += size;
        }
        count = programs;
        return entrypoint && cursor == bytes.size();
    }
};
static_assert(std::is_trivially_destructible_v<Value>);
static_assert(std::is_trivially_destructible_v<Outcome>);
static_assert(std::is_trivially_destructible_v<EntityRef>);
static_assert(std::is_trivially_destructible_v<Record>);
static_assert(std::is_trivially_destructible_v<Services>);
static_assert(std::is_trivially_destructible_v<CommandResult>);
static_assert(std::is_trivially_destructible_v<Transaction>);

// Owner-thread acquisition, candidate publication and invocation retirement.
LuauProvider::~LuauProvider() noexcept
{
    LUDUS_REQUIRE(Close() != Status::Reentrant);
}
Status LuauProvider::Load(const Contract& contract,
                          std::span<const uint8> trusted_package,
                          usize heap_limit,
                          uint32 safepoints) noexcept
{
    if (mImpl != nullptr && mImpl->Busy)
    {
        return Status::Reentrant;
    }
    if (mImpl != nullptr && mImpl->Pending != nullptr)
    {
        return Status::Paused;
    }
    if (!ValidContract(contract))
    {
        return Status::InvalidContract;
    }
    if (heap_limit == 0 || safepoints == 0)
    {
        return Status::InvalidInput;
    }
    auto* candidate = new (std::nothrow) Impl;
    if (candidate == nullptr)
    {
        return Status::AllocationFailure;
    }
    candidate->Schema = contract;
    usize count = 0;
    Status status = Status::InvalidPackage;
    if (candidate->Package(trusted_package, count))
    {
        candidate->Vm.SetHeapLimit(heap_limit);
        candidate->Vm.SetSafepointLimit(safepoints);
        status = Convert(candidate->Vm.LoadPrograms(candidate->Programs, count, Impl::Install, candidate));
    }
    if (status != Status::Completed)
    {
        delete candidate;
        return status;
    }
    delete mImpl;
    mImpl = candidate;
    return Status::Completed;
}
Status
LuauProvider::Invoke(const Invocation& input, Services services, Outcome& output, Diagnostic& diagnostic) noexcept
{
    if (mImpl == nullptr)
    {
        diagnostic = { .Code = Status::NotReady };
        return Status::NotReady;
    }
    if (mImpl->Busy)
    {
        diagnostic = { .Code = Status::Reentrant };
        return Status::Reentrant;
    }
    if (mImpl->Pending != nullptr)
    {
        diagnostic = { .Code = Status::Paused };
        return Status::Paused;
    }
    mImpl->Vm.EnableDebugger(false);
    mImpl->Busy = true;
    if (!ValidInput(mImpl->Schema, input, services))
    {
        mImpl->Busy = false;
        diagnostic = { .Code = Status::InvalidInput };
        return Status::InvalidInput;
    }
    bool found = false;
    for (const auto& program : mImpl->Programs)
    {
        found |= program.Entrypoint && program.Asset == input.Asset && program.Revision == input.Revision;
    }
    if (!found)
    {
        mImpl->Busy = false;
        diagnostic = { .Code = Status::InvalidInput };
        return Status::InvalidInput;
    }
    Transaction candidate(mImpl->Schema, input, services);
    Impl::Context context{ .Owner = mImpl, .Candidate = &candidate };
    scripting::Diagnostic detail;
    mImpl->Busy = true;
    const auto result = mImpl->Vm.Invoke(
        {
            .Asset = input.Asset,
            .Revision = input.Revision,
            .Execution = input.Execution,
            .Instance = input.Instance,
            .World = input.World,
            .Session = input.Session,
            .Tick = input.Tick,
            .Phase = static_cast<uint8>(input.Phase),
        },
        &context,
        Impl::Arguments,
        detail);
    mImpl->Busy = false;
    diagnostic = { .Code = Convert(result), .Operation = detail.Operation, .NativeStatus = detail.NativeStatus };
    std::memcpy(diagnostic.Message, detail.Message, sizeof(diagnostic.Message));
    if (diagnostic.Code == Status::Completed)
    {
        output = candidate.mCandidate;
    }
    return diagnostic.Code;
}
Status LuauProvider::FinishDebug(Diagnostic& diagnostic, Outcome& output) noexcept
{
    mImpl->Busy = false;
    if (diagnostic.Code == Status::Paused)
    {
        if (mNextStop == ~uint64{0})
        {
            mImpl->Vm.Close();
            diagnostic.Code = Status::Interrupted;
        }
        else
        {
            mImpl->DebugStop = ++mNextStop;
        }
    }
    if (diagnostic.Code == Status::Completed)
    {
        output = mImpl->Pending->mCandidate;
    }
    if (diagnostic.Code != Status::Paused)
    {
        delete mImpl->Pending;
        mImpl->Pending = nullptr;
        mImpl->DebugContext = {};
    }
    return diagnostic.Code;
}
Status
LuauProvider::BeginDebug(const Invocation& input, Services services, Outcome& output, Diagnostic& diagnostic) noexcept
{
    if (mImpl == nullptr || mImpl->Busy || mImpl->Pending != nullptr)
    {
        diagnostic =
        {
            .Code = mImpl == nullptr ? Status::NotReady : (mImpl->Busy ? Status::Reentrant : Status::Paused),
        };
        return diagnostic.Code;
    }
    mImpl->Busy = true;
    bool found = false;
    for (const auto& program : mImpl->Programs)
    {
        found |= program.Entrypoint && program.Asset == input.Asset && program.Revision == input.Revision;
    }
    if (!found || !ValidInput(mImpl->Schema, input, services))
    {
        mImpl->Busy = false;
        diagnostic = { .Code = Status::InvalidInput };
        return diagnostic.Code;
    }
    mImpl->DebugInput = input;
    mImpl->Pending = new (std::nothrow) Transaction(mImpl->Schema, mImpl->DebugInput, services);
    if (mImpl->Pending == nullptr)
    {
        mImpl->Busy = false;
        diagnostic = { .Code = Status::AllocationFailure };
        return diagnostic.Code;
    }
    mImpl->DebugContext = { .Owner = mImpl, .Candidate = mImpl->Pending };
    mImpl->Vm.EnableDebugger(true);
    scripting::Diagnostic detail;
    const auto status = mImpl->Vm.Invoke(
        {
            .Asset = input.Asset,
            .Revision = input.Revision,
            .Execution = input.Execution,
            .Instance = input.Instance,
            .World = input.World,
            .Session = input.Session,
            .Tick = input.Tick,
            .Phase = static_cast<uint8>(input.Phase),
        },
        &mImpl->DebugContext,
        Impl::Arguments,
        detail);
    diagnostic = { .Code = Convert(status), .Operation = detail.Operation, .NativeStatus = detail.NativeStatus };
    std::memcpy(diagnostic.Message, detail.Message, sizeof(diagnostic.Message));
    return FinishDebug(diagnostic, output);
}
Status LuauProvider::ResumeDebug(uint64 expected_stop, DebugMode mode, Outcome& output, Diagnostic& diagnostic) noexcept
{
    if (mImpl == nullptr || mImpl->Busy)
    {
        diagnostic = { .Code = mImpl == nullptr ? Status::NotReady : Status::Reentrant };
        return diagnostic.Code;
    }
    if (mImpl->Pending == nullptr || !mImpl->Vm.IsPaused() || expected_stop == 0 || expected_stop != mImpl->DebugStop ||
        static_cast<uint8>(mode) > static_cast<uint8>(DebugMode::Out))
    {
        diagnostic = { .Code = Status::InvalidInput };
        return diagnostic.Code;
    }
    mImpl->Busy = true;
    scripting::Diagnostic detail;
    const auto status = mImpl->Vm.Resume(static_cast<scripting::ResumeMode>(mode), detail);
    diagnostic = { .Code = Convert(status), .Operation = detail.Operation, .NativeStatus = detail.NativeStatus };
    std::memcpy(diagnostic.Message, detail.Message, sizeof(diagnostic.Message));
    return FinishDebug(diagnostic, output);
}
int32 LuauProvider::Breakpoint(uint64 asset, int32 compiled_line, bool enabled) noexcept
{
    if (mImpl == nullptr || mImpl->Busy || compiled_line <= 0 || compiled_line > 65536)
    {
        return -1;
    }
    mImpl->Busy = true;
    mImpl->Vm.EnableDebugger(true);
    const auto result = mImpl->Vm.Breakpoint({ .Asset = asset, .Line = compiled_line, .Enabled = enabled });
    if (result < 0 && mImpl->Vm.GetMemory().Live == 0)
    {
        delete mImpl->Pending;
        mImpl->Pending = nullptr;
        mImpl->DebugStop = 0;
    }
    mImpl->Busy = false;
    return result;
}
bool LuauProvider::Inspect(DebugSnapshot& output) const noexcept
{
    if (mImpl == nullptr || mImpl->Busy)
    {
        return false;
    }
    const auto& source = mImpl->Vm.Inspect();
    DebugSnapshot candidate;
    candidate.Paused = mImpl->Vm.IsPaused();
    if (candidate.Paused)
    {
        candidate.Stop = mImpl->DebugStop;
        candidate.FrameCount = source.FrameCount;
        candidate.LocalCount = source.LocalCount;
        candidate.Truncated = source.Truncated;
        for (uint32 i = 0; i < source.FrameCount; ++i)
        {
            std::memcpy(candidate.Frames[i].Source, source.Frames[i].Source, sizeof(candidate.Frames[i].Source));
            std::memcpy(candidate.Frames[i].Function, source.Frames[i].Function, sizeof(candidate.Frames[i].Function));
            candidate.Frames[i].Line = source.Frames[i].Line;
        }
        for (uint32 i = 0; i < source.LocalCount; ++i)
        {
            std::memcpy(candidate.Locals[i].Name, source.Locals[i].Name, sizeof(candidate.Locals[i].Name));
            candidate.Locals[i].Kind = static_cast<uint8>(source.Locals[i].Kind);
            candidate.Locals[i].Number = source.Locals[i].Number;
            candidate.Locals[i].Boolean = source.Locals[i].Boolean;
            std::memcpy(candidate.Locals[i].Bytes, source.Locals[i].Bytes, sizeof(candidate.Locals[i].Bytes));
            candidate.Locals[i].Truncated = source.Locals[i].Truncated;
        }
    }
    output = candidate;
    return true;
}
Status LuauProvider::Close() noexcept
{
    if (mImpl != nullptr && mImpl->Busy)
    {
        return Status::Reentrant;
    }
    delete mImpl;
    mImpl = nullptr;
    return Status::Completed;
}
usize LuauProvider::LiveBytes() const noexcept
{
    return mImpl == nullptr ? 0 : mImpl->Vm.GetMemory().Live;
}
} // namespace ludus::runtime::behavior
