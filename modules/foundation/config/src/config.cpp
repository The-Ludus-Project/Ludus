#include <ludus/foundation/config/config.hpp>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/strings/status.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include <atomic>
#include <new>

// Thanks to Peter Dalton, "Registered Variables", Game Programming Gems 8,
// sec.4.2, pp.363-372: skip work for unchanged values. Independent group
// generations replace dirty-bit clearing and variable redirector chains.
// Thanks to Epic Games, "Console Variables and Commands", sections "How to
// Track Console Variable Changes" and "Intended Console Variable Behavior":
// https://dev.epicgames.com/documentation/en-us/unreal-engine/console-variables-cplusplus-in-unreal-engine
// Batch related changes at an owner-selected boundary; keep desired and active
// values separate. No source code copied. Review: engine-configuration.md.
namespace ludus::foundation::config
{
namespace
{
constexpr usize kMaxSettings = 4096;
constexpr usize kGroups = 32;
/// Longest descriptor help text, in bytes.
constexpr usize MAX_HELP_BYTES = 4096;
/// Longest descriptor units label, in bytes.
constexpr usize MAX_UNITS_BYTES = 64;
/// Most choices one enum descriptor may declare.
constexpr usize MAX_ENUM_CHOICES = 32;
/// Values stored per setting beside its layer cells: requested, candidate and active.
constexpr usize VALUES_PER_SETTING = 3;
std::atomic<uint64> gNextContext{1};

Status Fail(Diagnostic& error, Status status, std::string_view name = {}, usize record = 0) noexcept
{
    error = {};
    error.Result = status;
    error.Record = record;
    (void)error.Key.TryAssign(name);
    return status;
}

bool ValidText(std::string_view text, usize bound) noexcept
{
    return text.size() <= bound && text.find('\0') == std::string_view::npos &&
           foundation::ValidateCString(text) == StringStatus::Ok;
}

Status Normalize(const Descriptor& descriptor, const Value& input, Value& output) noexcept
{
    if (input.Kind != descriptor.Default.Kind)
    {
        return Status::InvalidValue;
    }
    Value result = input;
    switch (input.Kind)
    {
        case Type::Bool:
            break;
        case Type::Int32:
        case Type::Int64: {
            int32 narrowed{};
            if (input.Signed < descriptor.MinSigned || input.Signed > descriptor.MaxSigned ||
                (input.Kind == Type::Int32 && TryIntegerCast(input.Signed, narrowed) == false))
            {
                return Status::InvalidValue;
            }
            break;
        }
        case Type::Uint32:
        case Type::Uint64: {
            uint32 narrowed{};
            if (input.Unsigned < descriptor.MinUnsigned || input.Unsigned > descriptor.MaxUnsigned ||
                (input.Kind == Type::Uint32 && TryIntegerCast(input.Unsigned, narrowed) == false))
            {
                return Status::InvalidValue;
            }
            break;
        }
        case Type::Float32:
        case Type::Float64:
            if (math::IsFinite(input.Real) == false || input.Real < descriptor.MinReal ||
                input.Real > descriptor.MaxReal)
            {
                return Status::InvalidValue;
            }
            if (input.Kind == Type::Float32)
            {
                constexpr float64 kMaxFloat32 = 3.4028234663852886e38;
                if (input.Real < -kMaxFloat32 || input.Real > kMaxFloat32)
                {
                    return Status::InvalidValue;
                }
                result.Real = static_cast<float64>(static_cast<float32>(input.Real));
                if (result.Real < descriptor.MinReal || result.Real > descriptor.MaxReal)
                {
                    return Status::InvalidValue;
                }
            }
            if (result.Real == 0)
            {
                result.Real = 0; // Canonical positive zero.
            }
            break;
        case Type::Enum:
        case Type::String:
            if (ValidText(input.Text.GetView(), descriptor.MaxTextBytes) == false)
            {
                return Status::InvalidValue;
            }
            if (input.Kind == Type::Enum)
            {
                bool found = false;
                for (const auto choice : descriptor.Choices)
                {
                    found = found || choice == input.Text.GetView();
                }
                if (found == false)
                {
                    return Status::InvalidValue;
                }
            }
            break;
        default:
            return Status::InvalidValue;
    }
    output = result;
    return Status::Ok;
}

template <typename Element>
Element* Allocate(const AllocationDomain& domain, usize count) noexcept
{
    usize bytes{};
    if (TryMultiply(count, sizeof(Element), bytes) == false)
    {
        return nullptr;
    }
    auto* result = static_cast<Element*>(domain.TryAllocate(bytes, alignof(Element)));
    if (result != nullptr)
    {
        for (usize i = 0; i < count; ++i)
        {
            new (result + i) Element{};
        }
    }
    return result;
}

template <typename Element>
void Release(const AllocationDomain& domain, Element* pointer, usize count) noexcept
{
    if (pointer != nullptr)
    {
        for (usize i = 0; i < count; ++i)
        {
            pointer[i].~Element();
        }
        domain.Free(pointer, count * sizeof(Element), alignof(Element));
    }
}
} // namespace

struct Context::Impl final
{
    struct Cell final
    {
        Value Data;
        Origin From;
        bool Present = false;
    };
    const AllocationDomain* Domain;
    std::span<const Descriptor> Descriptors;
    Limits Bounds;
    Cell* Cells = nullptr;
    Cell* CandidateCells = nullptr;
    Value* Requested = nullptr;
    Value* Candidate = nullptr;
    Value* Active = nullptr;
    uint32* Sorted = nullptr;
    uint64 Generations[kGroups]{};
    uint64 Token = 0;
    uint64 Revision = 1;
    ValidateFunction Validator = nullptr;
    void* ValidatorContext = nullptr;
    bool Sealed = false;
    bool Prepared = false;
    Apply PreparedApply = Apply::Live;

    Impl(const AllocationDomain& domain, std::span<const Descriptor> descriptors, Limits limits) noexcept
        : Domain(&domain), Descriptors(descriptors), Bounds(limits)
    {
    }
    ~Impl() noexcept
    {
        const usize count = Descriptors.size();
        Release(*Domain, Cells, count * LAYER_COUNT);
        Release(*Domain, CandidateCells, count * LAYER_COUNT);
        Release(*Domain, Requested, count);
        Release(*Domain, Candidate, count);
        Release(*Domain, Active, count);
        Release(*Domain, Sorted, count);
    }
    [[nodiscard]] Cell& At(Cell* cells, usize setting, usize layer) const noexcept
    {
        return cells[setting * LAYER_COUNT + layer];
    }
    [[nodiscard]] bool Valid(Binding binding) const noexcept
    {
        return binding.Context == Token && binding.Index < Descriptors.size();
    }
    void Resolve() noexcept
    {
        for (usize i = 0; i < Descriptors.size(); ++i)
        {
            (void)Normalize(Descriptors[i], Descriptors[i].Default, Candidate[i]);
            for (usize layer = 0; layer < LAYER_COUNT; ++layer)
            {
                const auto& cell = At(CandidateCells, i, layer);
                if (cell.Present)
                {
                    Candidate[i] = cell.Data;
                }
            }
        }
    }
};

Value Value::FromBool(bool value) noexcept
{
    Value result;
    result.Boolean = value;
    return result;
}
Value Value::FromInt32(int32 value) noexcept
{
    Value result;
    result.Kind = Type::Int32;
    result.Signed = value;
    return result;
}
Value Value::FromUint32(uint32 value) noexcept
{
    Value result;
    result.Kind = Type::Uint32;
    result.Unsigned = value;
    return result;
}
Value Value::FromInt64(int64 value) noexcept
{
    Value result;
    result.Kind = Type::Int64;
    result.Signed = value;
    return result;
}
Value Value::FromUint64(uint64 value) noexcept
{
    Value result;
    result.Kind = Type::Uint64;
    result.Unsigned = value;
    return result;
}
Value Value::FromFloat32(float32 value) noexcept
{
    Value result;
    result.Kind = Type::Float32;
    result.Real = static_cast<float64>(value);
    return result;
}
Value Value::FromFloat64(float64 value) noexcept
{
    Value result;
    result.Kind = Type::Float64;
    result.Real = value;
    return result;
}
Status Value::FromText(Type kind, std::string_view text, Value& output) noexcept
{
    if ((kind != Type::String && kind != Type::Enum) || ValidText(text, MAX_TEXT_VALUE_BYTES) == false)
    {
        return Status::InvalidValue;
    }
    Value result;
    result.Kind = kind;
    (void)result.Text.TryAssign(text);
    output = result;
    return Status::Ok;
}
bool operator==(const Value& left, const Value& right) noexcept
{
    if (left.Kind != right.Kind)
    {
        return false;
    }
    switch (left.Kind)
    {
        case Type::Bool:
            return left.Boolean == right.Boolean;
        case Type::Int32:
        case Type::Int64:
            return left.Signed == right.Signed;
        case Type::Uint32:
        case Type::Uint64:
            return left.Unsigned == right.Unsigned;
        case Type::Float32:
        case Type::Float64:
            return left.Real == right.Real;
        case Type::Enum:
        case Type::String:
            return left.Text.GetView() == right.Text.GetView();
        default:
            return false;
    }
}

Context::~Context() noexcept
{
    if (mImpl != nullptr)
    {
        const auto* domain = mImpl->Domain;
        mImpl->~Impl();
        domain->Free(mImpl, sizeof(Impl), alignof(Impl));
    }
}

Status Context::Initialize(std::span<const Descriptor> schema,
                           const AllocationDomain& domain,
                           Diagnostic& error,
                           const Limits& limits,
                           ValidateFunction validate,
                           void* validationContext) noexcept
{
    error = {};
    if (mImpl != nullptr)
    {
        return Fail(error, Status::InvalidState);
    }
    if (schema.empty() || schema.size() > limits.MaxSettings || limits.MaxSettings > kMaxSettings ||
        limits.MaxEdits == 0 || limits.MaxEdits > kMaxSettings)
    {
        return Fail(error, Status::LimitExceeded);
    }
    usize bytesPerSetting{};
    usize storageBytes{};
    if (TryMultiply(sizeof(Impl::Cell), LAYER_COUNT * 2, bytesPerSetting) == false ||
        TryAdd(bytesPerSetting, sizeof(Value) * VALUES_PER_SETTING + sizeof(uint32), bytesPerSetting) == false ||
        TryMultiply(bytesPerSetting, schema.size(), storageBytes) == false ||
        TryAdd(storageBytes, sizeof(Impl), storageBytes) == false || storageBytes > limits.MaxStorageBytes)
    {
        return Fail(error, Status::LimitExceeded);
    }
    for (usize i = 0; i < schema.size(); ++i)
    {
        const auto& descriptor = schema[i];
        Value normalized;
        if (ValidName(descriptor.Name) == false || descriptor.Group >= kGroups ||
            descriptor.MaxTextBytes > MAX_TEXT_VALUE_BYTES || descriptor.Application > Apply::NextLaunch ||
            descriptor.AllowedSources == 0 || (descriptor.AllowedSources & ~ALL_SOURCES) != 0 ||
            (descriptor.Persistent && (descriptor.AllowedSources & SourceMask(Layer::Preference)) == 0) ||
            descriptor.MinSigned > descriptor.MaxSigned || descriptor.MinUnsigned > descriptor.MaxUnsigned ||
            math::IsFinite(descriptor.MinReal) == false || math::IsFinite(descriptor.MaxReal) == false ||
            descriptor.MinReal > descriptor.MaxReal || ValidText(descriptor.Help, MAX_HELP_BYTES) == false ||
            ValidText(descriptor.Units, MAX_UNITS_BYTES) == false ||
            (descriptor.Default.Kind != Type::Enum && descriptor.Choices.empty() == false) ||
            descriptor.Choices.size() > MAX_ENUM_CHOICES ||
            Normalize(descriptor, descriptor.Default, normalized) != Status::Ok ||
            (normalized == descriptor.Default) == false)
        {
            return Fail(error, Status::InvalidSchema, descriptor.Name, i);
        }
        for (usize choice = 0; choice < descriptor.Choices.size(); ++choice)
        {
            if (ValidText(descriptor.Choices[choice], descriptor.MaxTextBytes) == false ||
                descriptor.Choices[choice].empty())
            {
                return Fail(error, Status::InvalidSchema, descriptor.Name, i);
            }
            for (usize prior = 0; prior < choice; ++prior)
            {
                if (descriptor.Choices[prior] == descriptor.Choices[choice])
                {
                    return Fail(error, Status::InvalidSchema, descriptor.Name, i);
                }
            }
        }
        for (usize prior = 0; prior < i; ++prior)
        {
            if (schema[prior].Name == descriptor.Name)
            {
                return Fail(error, Status::InvalidSchema, descriptor.Name, i);
            }
        }
    }
    void* memory = domain.TryAllocate(sizeof(Impl), alignof(Impl));
    if (memory == nullptr)
    {
        return Fail(error, Status::OutOfMemory);
    }
    auto* candidate = new (memory) Impl(domain, schema, limits);
    const usize count = schema.size();
    candidate->Cells = Allocate<Impl::Cell>(domain, count * LAYER_COUNT);
    candidate->CandidateCells = Allocate<Impl::Cell>(domain, count * LAYER_COUNT);
    candidate->Requested = Allocate<Value>(domain, count);
    candidate->Candidate = Allocate<Value>(domain, count);
    candidate->Active = Allocate<Value>(domain, count);
    candidate->Sorted = Allocate<uint32>(domain, count);
    Status status = Status::Ok;
    if (candidate->Cells == nullptr || candidate->CandidateCells == nullptr || candidate->Requested == nullptr ||
        candidate->Candidate == nullptr || candidate->Active == nullptr || candidate->Sorted == nullptr)
    {
        status = Status::OutOfMemory;
    }
    else
    {
        for (usize i = 0; i < count; ++i)
        {
            (void)Normalize(schema[i], schema[i].Default, candidate->Requested[i]);
            candidate->Active[i] = candidate->Requested[i];
            candidate->Sorted[i] = static_cast<uint32>(i);
            usize position = i;
            while (position > 0 &&
                   schema[candidate->Sorted[position]].Name < schema[candidate->Sorted[position - 1]].Name)
            {
                const uint32 swap = candidate->Sorted[position];
                candidate->Sorted[position] = candidate->Sorted[position - 1];
                candidate->Sorted[--position] = swap;
            }
        }
        if (validate != nullptr)
        {
            status = validate(validationContext, {candidate->Requested, count}, error);
        }
        uint64 token = gNextContext.load(std::memory_order_relaxed);
        if (status == Status::Ok)
        {
            while (token != ~uint64{0} &&
                   gNextContext.compare_exchange_weak(token, token + 1, std::memory_order_relaxed) == false)
            {
            }
            status = token == ~uint64{0} ? Status::GenerationExhausted : Status::Ok;
            candidate->Token = token;
        }
    }
    if (status != Status::Ok)
    {
        candidate->~Impl();
        domain.Free(candidate, sizeof(Impl), alignof(Impl));
        error.Result = status;
        return status;
    }
    candidate->Validator = validate;
    candidate->ValidatorContext = validationContext;
    mImpl = candidate;
    return Status::Ok;
}

bool Context::IsValid() const noexcept
{
    return mImpl != nullptr;
}
uint64 Context::Revision() const noexcept
{
    return mImpl == nullptr ? 0 : mImpl->Revision;
}
std::span<const Descriptor> Context::Schema() const noexcept
{
    return mImpl == nullptr ? std::span<const Descriptor>{} : mImpl->Descriptors;
}
Status Context::Bind(std::string_view name, Binding& output) const noexcept
{
    if (mImpl == nullptr)
    {
        return Status::InvalidState;
    }
    usize begin = 0;
    usize end = mImpl->Descriptors.size();
    while (begin < end)
    {
        const usize middle = begin + (end - begin) / 2;
        const uint32 index = mImpl->Sorted[middle];
        const int comparison = mImpl->Descriptors[index].Name.compare(name);
        if (comparison == 0)
        {
            output = {mImpl->Token, index};
            return Status::Ok;
        }
        if (comparison < 0)
        {
            begin = middle + 1;
        }
        else
        {
            end = middle;
        }
    }
    return Status::UnknownSetting;
}
Status Context::Explain(Binding binding, Explanation& output) const noexcept
{
    if (mImpl == nullptr || mImpl->Valid(binding) == false)
    {
        return Status::InvalidState;
    }
    Explanation result;
    result.Requested = mImpl->Requested[binding.Index];
    result.Active = mImpl->Active[binding.Index];
    result.Pending = (result.Requested == result.Active) == false;
    result.GroupGeneration = mImpl->Generations[mImpl->Descriptors[binding.Index].Group];
    for (usize layer = 0; layer < LAYER_COUNT; ++layer)
    {
        const auto& cell = mImpl->At(mImpl->Cells, binding.Index, layer);
        if (cell.Present)
        {
            result.Overridden += result.IsDefault ? 0U : 1U;
            result.IsDefault = false;
            result.Winner = static_cast<Layer>(layer);
            result.From = cell.From;
        }
    }
    output = result;
    return Status::Ok;
}
Status Context::ReadLayer(Binding binding, Layer layer, Value& output, Origin& origin) const noexcept
{
    if (mImpl == nullptr || mImpl->Valid(binding) == false || static_cast<usize>(layer) >= LAYER_COUNT)
    {
        return Status::InvalidState;
    }
    const auto& cell = mImpl->At(mImpl->Cells, binding.Index, static_cast<usize>(layer));
    if (cell.Present == false)
    {
        return Status::UnknownSetting;
    }
    output = cell.Data;
    origin = cell.From;
    return Status::Ok;
}
Status Context::Prepare(Layer layer,
                        std::span<const Assignment> edits,
                        bool replace,
                        uint64 expectedRevision,
                        Diagnostic& error) noexcept
{
    error = {};
    if (mImpl == nullptr || mImpl->Prepared || static_cast<usize>(layer) >= LAYER_COUNT)
    {
        return Fail(error, Status::InvalidState);
    }
    if (expectedRevision != mImpl->Revision)
    {
        return Fail(error, Status::Conflict);
    }
    if (edits.size() > mImpl->Bounds.MaxEdits)
    {
        return Fail(error, Status::LimitExceeded);
    }
    const usize count = mImpl->Descriptors.size();
    for (usize i = 0; i < count * LAYER_COUNT; ++i)
    {
        mImpl->CandidateCells[i] = mImpl->Cells[i];
    }
    if (replace)
    {
        for (usize i = 0; i < count; ++i)
        {
            mImpl->At(mImpl->CandidateCells, i, static_cast<usize>(layer)) = {};
        }
    }
    for (usize i = 0; i < edits.size(); ++i)
    {
        const auto& edit = edits[i];
        Binding binding;
        const auto found = Bind(edit.Name, binding);
        if (found != Status::Ok)
        {
            return Fail(error, found, edit.Name, i);
        }
        for (usize prior = 0; prior < i; ++prior)
        {
            if (edits[prior].Name == edit.Name)
            {
                return Fail(error, Status::DuplicateAssignment, edit.Name, i);
            }
        }
        const auto& descriptor = mImpl->Descriptors[binding.Index];
        if ((descriptor.AllowedSources & SourceMask(layer)) == 0 ||
            (layer == Layer::Preference && descriptor.Persistent == false))
        {
            return Fail(error, Status::InvalidSource, edit.Name, i);
        }
        if (ValidText(edit.From.Source.GetView(), MAX_ORIGIN_SOURCE_BYTES) == false)
        {
            return Fail(error, Status::InvalidValue, edit.Name, i);
        }
        auto& cell = mImpl->At(mImpl->CandidateCells, binding.Index, static_cast<usize>(layer));
        if (edit.Remove)
        {
            cell = {};
        }
        else
        {
            Value normalized;
            const auto status = Normalize(descriptor, edit.Data, normalized);
            if (status != Status::Ok)
            {
                return Fail(error, status, edit.Name, i);
            }
            cell.Data = normalized;
            cell.From = edit.From;
            cell.Present = true;
        }
    }
    mImpl->Resolve();
    bool changed = false;
    uint32 group = 0;
    Apply application = Apply::Live;
    for (usize i = 0; i < count; ++i)
    {
        if (mImpl->Candidate[i] == mImpl->Requested[i])
        {
            continue;
        }
        const auto& descriptor = mImpl->Descriptors[i];
        if (mImpl->Sealed && descriptor.Simulation)
        {
            return Fail(error, Status::Frozen, descriptor.Name);
        }
        if (mImpl->Sealed && changed && (group != descriptor.Group || application != descriptor.Application))
        {
            return Fail(error, Status::MixedApplyGroups, descriptor.Name);
        }
        changed = true;
        group = descriptor.Group;
        application = descriptor.Application;
    }
    if (mImpl->Validator != nullptr)
    {
        const auto status = mImpl->Validator(mImpl->ValidatorContext, {mImpl->Candidate, count}, error);
        if (status != Status::Ok)
        {
            error.Result = status;
            return status;
        }
    }
    // A pending restart may make requested constraints differ from the options
    // currently in use. Validate the live publication against those active
    // restart values too, without allocating another snapshot.
    if (mImpl->Sealed && application == Apply::Live && mImpl->Validator != nullptr)
    {
        for (usize i = 0; i < count; ++i)
        {
            if (mImpl->Descriptors[i].Application != Apply::Live)
            {
                mImpl->Candidate[i] = mImpl->Active[i];
            }
        }
        const auto status = mImpl->Validator(mImpl->ValidatorContext, {mImpl->Candidate, count}, error);
        mImpl->Resolve();
        if (status != Status::Ok)
        {
            error.Result = status;
            return status;
        }
    }
    mImpl->PreparedApply = application;
    mImpl->Prepared = true;
    return Status::Ok;
}
Status Context::ReadPrepared(Binding binding, Value& output) const noexcept
{
    if (mImpl == nullptr || mImpl->Valid(binding) == false || mImpl->Prepared == false)
    {
        return Status::InvalidState;
    }
    output = mImpl->Candidate[binding.Index];
    return Status::Ok;
}
Status Context::Commit(uint64 expectedRevision, Diagnostic& error) noexcept
{
    error = {};
    if (mImpl == nullptr || mImpl->Prepared == false)
    {
        return Fail(error, Status::InvalidState);
    }
    if (expectedRevision != mImpl->Revision)
    {
        return Fail(error, Status::Conflict);
    }
    if (mImpl->Revision == ~uint64{0})
    {
        return Fail(error, Status::GenerationExhausted);
    }
    const usize count = mImpl->Descriptors.size();
    bool changed[kGroups]{};
    for (usize i = 0; i < count; ++i)
    {
        const auto& descriptor = mImpl->Descriptors[i];
        if ((mImpl->Sealed == false || descriptor.Application == Apply::Live) &&
            (mImpl->Active[i] == mImpl->Candidate[i]) == false)
        {
            changed[descriptor.Group] = true;
        }
    }
    for (usize group = 0; group < kGroups; ++group)
    {
        if (changed[group] && mImpl->Generations[group] == ~uint64{0})
        {
            return Fail(error, Status::GenerationExhausted);
        }
    }
    for (usize i = 0; i < count * LAYER_COUNT; ++i)
    {
        mImpl->Cells[i] = mImpl->CandidateCells[i];
    }
    for (usize i = 0; i < count; ++i)
    {
        mImpl->Requested[i] = mImpl->Candidate[i];
        if (mImpl->Sealed == false || mImpl->Descriptors[i].Application == Apply::Live)
        {
            mImpl->Active[i] = mImpl->Candidate[i];
        }
    }
    for (usize group = 0; group < kGroups; ++group)
    {
        mImpl->Generations[group] += changed[group] ? 1U : 0U;
    }
    ++mImpl->Revision;
    mImpl->Prepared = false;
    return mImpl->Sealed && mImpl->PreparedApply != Apply::Live ? Status::PendingRestart : Status::Ok;
}
void Context::Discard() noexcept
{
    if (mImpl != nullptr)
    {
        mImpl->Prepared = false;
    }
}
Status Context::SealStartup() noexcept
{
    if (mImpl == nullptr || mImpl->Prepared)
    {
        return Status::InvalidState;
    }
    mImpl->Sealed = true;
    return Status::Ok;
}

bool ValidName(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 128)
    {
        return false;
    }
    bool start = true;
    for (char ch : name)
    {
        if (ch == '.')
        {
            if (start)
            {
                return false;
            }
            start = true;
        }
        else
        {
            if ((ch < 'a' || ch > 'z') && (start || ch < '0' || ch > '9') && (start || ch != '_'))
            {
                return false;
            }
            start = false;
        }
    }
    return start == false;
}
std::string_view StatusName(Status status) noexcept
{
    constexpr std::string_view kNames[]{"Ok",
                                        "PendingRestart",
                                        "InvalidState",
                                        "InvalidSchema",
                                        "InvalidValue",
                                        "UnknownSetting",
                                        "DuplicateAssignment",
                                        "InvalidSource",
                                        "ConstraintFailed",
                                        "Conflict",
                                        "MixedApplyGroups",
                                        "Frozen",
                                        "OutOfMemory",
                                        "LimitExceeded",
                                        "GenerationExhausted",
                                        "InvalidBundle"};
    const usize index = static_cast<usize>(status);
    return index < sizeof(kNames) / sizeof(kNames[0]) ? kNames[index] : "Unknown";
}
std::string_view TypeName(Type type) noexcept
{
    constexpr std::string_view
        kNames[]{"bool", "int32", "uint32", "int64", "uint64", "float32", "float64", "enum", "string"};
    const usize index = static_cast<usize>(type);
    return index < sizeof(kNames) / sizeof(kNames[0]) ? kNames[index] : "unknown";
}
std::string_view LayerName(Layer layer) noexcept
{
    constexpr std::string_view
        kNames[]{"engine", "project", "profile", "device", "quality", "preference", "developer", "launch", "session"};
    const usize index = static_cast<usize>(layer);
    return index < LAYER_COUNT ? kNames[index] : "unknown";
}
std::string_view ApplyName(Apply apply) noexcept
{
    constexpr std::string_view kNames[]{"live", "restart_session", "restart_engine", "next_launch"};
    const usize index = static_cast<usize>(apply);
    return index < sizeof(kNames) / sizeof(kNames[0]) ? kNames[index] : "unknown";
}
} // namespace ludus::foundation::config
