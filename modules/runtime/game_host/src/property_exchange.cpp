#include "internal/host_session.h"

#include <ludus/runtime/game_api/properties.h>

#include <ludus/foundation/containers/array.hpp>

#include <array>
#include <cstring>
#include <utility>

namespace ludus::runtime::game_host
{
namespace
{
using namespace game_api;
using ludus::foundation::Array;
using protocol::CommandStatus;
using protocol::Message;
/// Range of an Int32 property edit, and the largest Enum value (a non-negative int32).
constexpr protocol::int64 MIN_INT32_VALUE = -2147483648LL;
constexpr protocol::int64 MAX_INT32_VALUE = 2147483647LL;
constexpr uint64 MAX_ENUM_VALUE = 2147483647ULL;

[[nodiscard]] uint32 FloatBits(float32 value) noexcept
{
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
[[nodiscard]] bool Finite(float32 value) noexcept
{
    return (FloatBits(value) & 0x7F800000U) != 0x7F800000U;
}

struct Snapshot
{
    Array<PropertyDescriptor> Descriptors;
    Array<PropertyValue> Values;
};

[[nodiscard]] bool ValidValue(const PropertyDescriptor& descriptor, const PropertyValue& value) noexcept
{
    if (value.ObjectId != descriptor.ObjectId || value.PropertyId != descriptor.PropertyId ||
        value.Kind != descriptor.Kind)
    {
        return false;
    }
    switch (static_cast<PropertyKind>(value.Kind))
    {
        case PropertyKind::Bool:
            return value.IntOrEnum <= 1;
        case PropertyKind::Int32: {
            int32 number = 0;
            std::memcpy(&number, &value.IntOrEnum, sizeof(number));
            return descriptor.Writable == 0 || (number >= descriptor.MinInt && number <= descriptor.MaxInt);
        }
        case PropertyKind::Enum:
            return descriptor.MinInt >= 0 && descriptor.MaxInt >= descriptor.MinInt &&
                   value.IntOrEnum >= static_cast<uint32>(descriptor.MinInt) &&
                   value.IntOrEnum <= static_cast<uint32>(descriptor.MaxInt);
        case PropertyKind::Float32:
            return Finite(value.Float) && (descriptor.Writable == 0 ||
                                           (value.Float >= descriptor.MinFloat && value.Float <= descriptor.MaxFloat));
        case PropertyKind::String:
            return value.StringLength <= sizeof(value.String) &&
                   protocol::ValidUtf8(std::string_view(value.String, value.StringLength));
    }
    return false;
}

// False reports an invalid game response or snapshot allocation failure; the
// caller treats both as a host failure.
[[nodiscard]] bool ReadSnapshot(const GameApiTable& table, GameInstance* instance, Snapshot& out) noexcept
{
    usize size = 0;
    if (table.DescribePropertiesSize(instance, &size) != Status::Ok || size == 0 ||
        size > protocol::kMaxSchemaTransferBytes || size % sizeof(PropertyDescriptor) != 0 ||
        size / sizeof(PropertyDescriptor) > kMaxProperties)
    {
        return false;
    }
    if (out.Descriptors.TryResize(size / sizeof(PropertyDescriptor)) == false)
    {
        return false;
    }
    usize written = 0;
    if (table.DescribeProperties(instance, {reinterpret_cast<uint8*>(out.Descriptors.GetData()), size}, &written) !=
            Status::Ok ||
        written != size)
    {
        return false;
    }
    if (out.Values.TryResize(out.Descriptors.GetSize()) == false)
    {
        return false;
    }
    const usize valueBytes = out.Values.GetSize() * sizeof(PropertyValue);
    if (table.ReadProperties(instance, {reinterpret_cast<uint8*>(out.Values.GetData()), valueBytes}, &written) !=
            Status::Ok ||
        written != valueBytes)
    {
        return false;
    }
    Array<uint64> objects;
    for (usize i = 0; i < out.Descriptors.GetSize(); ++i)
    {
        const auto& descriptor = out.Descriptors[i];
        if (descriptor.ObjectId == 0 || descriptor.PropertyId == 0 || descriptor.Writable > 1 ||
            descriptor.Scope > static_cast<uint32>(PropertyScope::ReadOnly) || descriptor.Reserved0 != 0 ||
            descriptor.Reserved1 != 0 || descriptor.LabelLength > sizeof(descriptor.Label) ||
            protocol::ValidUtf8(std::string_view(descriptor.Label, descriptor.LabelLength)) == false ||
            (descriptor.Scope == static_cast<uint32>(PropertyScope::ReadOnly) && descriptor.Writable != 0) ||
            Finite(descriptor.MinFloat) == false || Finite(descriptor.MaxFloat) == false ||
            descriptor.MinFloat > descriptor.MaxFloat || descriptor.MinInt > descriptor.MaxInt)
        {
            return false;
        }
        bool seenObject = false;
        for (const auto object : objects)
        {
            seenObject = seenObject || object == descriptor.ObjectId;
        }
        if (seenObject == false)
        {
            if (objects.TryAdd(descriptor.ObjectId) == false || objects.GetSize() > kMaxObjects)
            {
                return false;
            }
        }
        for (usize j = 0; j < i; ++j)
        {
            if (out.Descriptors[j].ObjectId == descriptor.ObjectId &&
                out.Descriptors[j].PropertyId == descriptor.PropertyId)
            {
                return false;
            }
        }
        // Accessors may return values in a different order; copy into schema order.
        usize match = i;
        while (match < out.Values.GetSize() && (out.Values[match].ObjectId != descriptor.ObjectId ||
                                                out.Values[match].PropertyId != descriptor.PropertyId))
        {
            ++match;
        }
        if (match == out.Values.GetSize())
        {
            return false;
        }
        std::swap(out.Values[i], out.Values[match]);
        if (ValidValue(descriptor, out.Values[i]) == false)
        {
            return false;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (out.Values[j].ObjectId == descriptor.ObjectId &&
                out.Values[j].ObjectRevision != out.Values[i].ObjectRevision)
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool DecodeEdit(const Message& record, PropertyEdit& edit) noexcept
{
    uint64 kind = 0;
    if (record.HasOnly({"object", "property", "revision", "kind", "value", "bits"}) == false ||
        record.GetHexId("object", edit.ObjectId) == false || record.GetHexId("property", edit.PropertyId) == false ||
        record.GetHexId("revision", edit.ExpectedRevision) == false || record.GetUint("kind", kind) == false ||
        kind > static_cast<uint32>(PropertyKind::String))
    {
        return false;
    }
    edit.Kind = static_cast<uint32>(kind);
    if (edit.Kind == static_cast<uint32>(PropertyKind::Float32))
    {
        uint64 bits = 0;
        if (record.Has("value") || record.GetUint("bits", bits) == false || bits > 0xFFFFFFFFULL)
        {
            return false;
        }
        const uint32 narrowed = static_cast<uint32>(bits);
        std::memcpy(&edit.Float, &narrowed, sizeof(edit.Float));
        return Finite(edit.Float);
    }
    if (record.Has("bits"))
    {
        return false;
    }
    switch (static_cast<PropertyKind>(edit.Kind))
    {
        case PropertyKind::Bool: {
            bool value = false;
            if (record.GetBool("value", value) == false)
            {
                return false;
            }
            edit.IntOrEnum = value ? 1 : 0;
            return true;
        }
        case PropertyKind::Int32: {
            protocol::int64 value = 0;
            if (record.GetInt("value", value) == false || value < MIN_INT32_VALUE || value > MAX_INT32_VALUE)
            {
                return false;
            }
            const int32 narrowed = static_cast<int32>(value);
            std::memcpy(&edit.IntOrEnum, &narrowed, sizeof(narrowed));
            return true;
        }
        case PropertyKind::Enum: {
            uint64 value = 0;
            if (record.GetUint("value", value) == false || value > MAX_ENUM_VALUE)
            {
                return false;
            }
            edit.IntOrEnum = static_cast<uint32>(value);
            return true;
        }
        case PropertyKind::String: {
            std::string value;
            if (record.GetString("value", value) == false || value.size() > sizeof(edit.String) ||
                protocol::ValidUtf8(value) == false)
            {
                return false;
            }
            edit.StringLength = static_cast<uint32>(value.size());
            std::memcpy(edit.String, value.data(), value.size());
            return true;
        }
        case PropertyKind::Float32:
            return false;
    }
    return false;
}

void EncodeValue(Message& record, const PropertyValue& value)
{
    switch (static_cast<PropertyKind>(value.Kind))
    {
        case PropertyKind::Float32:
            record.SetUint("bits", FloatBits(value.Float));
            break;
        case PropertyKind::String:
            record.SetString("value", std::string_view(value.String, value.StringLength));
            break;
        case PropertyKind::Bool:
            record.SetBool("value", value.IntOrEnum != 0);
            break;
        case PropertyKind::Int32: {
            int32 integer = 0;
            std::memcpy(&integer, &value.IntOrEnum, sizeof(integer));
            record.SetInt("value", integer);
            break;
        }
        case PropertyKind::Enum:
            record.SetUint("value", value.IntOrEnum);
            break;
    }
}
} // namespace

protocol::CommandStatus HostSession::PublishProperties(uint64 requestId) noexcept
{
    if (Instance_ == nullptr ||
        game_api::HasCapability(Active_.Metadata().Capabilities, Capability::Properties) == false ||
        (State_ != PlayState::Running && State_ != PlayState::Paused))
    {
        return CommandStatus::InvalidRequest;
    }
    Snapshot snapshot;
    if (ReadSnapshot(Active_.Table(), Instance_, snapshot) == false)
    {
        return CommandStatus::HostFailed;
    }
    // Stage every frame before emitting so allocation failure publishes nothing.
    Array<Message> frames;
    usize total = 0;
    for (usize begin = 0; begin < snapshot.Values.GetSize(); begin += protocol::kMaxPropertyBatch)
    {
        Array<Message> records;
        for (usize i = begin; i < snapshot.Values.GetSize() && i - begin < protocol::kMaxPropertyBatch; ++i)
        {
            const auto& descriptor = snapshot.Descriptors[i];
            const auto& value = snapshot.Values[i];
            Message record;
            record.SetHexId("object", value.ObjectId);
            record.SetHexId("property", value.PropertyId);
            record.SetHexId("revision", value.ObjectRevision);
            record.SetUint("kind", value.Kind);
            record.SetUint("scope", descriptor.Scope);
            record.SetBool("writable", descriptor.Writable != 0);
            record.SetString("label", std::string_view(descriptor.Label, descriptor.LabelLength));
            record.SetInt("min_int", descriptor.MinInt);
            record.SetInt("max_int", descriptor.MaxInt);
            record.SetUint("min_bits", FloatBits(descriptor.MinFloat));
            record.SetUint("max_bits", FloatBits(descriptor.MaxFloat));
            EncodeValue(record, value);
            if (records.TryAdd(std::move(record)) == false)
            {
                return CommandStatus::HostFailed;
            }
        }
        Message frame;
        frame.SetString("event", protocol::EventKindName(protocol::EventKind::PropertiesChanged));
        frame.SetUint("protocol", protocol::kProtocolVersion);
        frame.SetHexId("session", SessionId_);
        frame.SetHexId("epoch", ProjectEpoch_);
        frame.SetHexId("request", requestId);
        frame.SetHexId("generation", ActiveGeneration_);
        frame.SetUint("schema_epoch", SchemaEpoch_);
        frame.SetUint("schema_version", Active_.Metadata().PropertySchemaVersion);
        frame.SetUint("offset", begin);
        frame.SetUint("count", snapshot.Values.GetSize());
        frame.SetRecords("properties", records);
        const usize bytes = frame.Serialize().size();
        if (bytes > protocol::kMaxControlFrameBytes || total + bytes > protocol::kMaxSchemaTransferBytes)
        {
            return CommandStatus::InvalidRequest;
        }
        total += bytes;
        if (frames.TryAdd(std::move(frame)) == false)
        {
            return CommandStatus::HostFailed;
        }
    }
    if (OutputBytes_ + total + frames.GetSize() * 4 + 4096 > protocol::kMaxCommandQueueBytes ||
        Output_.GetSize() + frames.GetSize() + 1 > protocol::kMaxPendingCommands)
    {
        return CommandStatus::Busy;
    }
    for (const auto& frame : frames)
    {
        Emit(frame);
    }
    return CommandStatus::Ok;
}

protocol::CommandStatus HostSession::ApplyPropertyEdits(const Message& command) noexcept
{
    uint64 epoch = 0;
    uint64 schema = 0;
    if (command.GetUint("schema_epoch", epoch) == false || epoch != SchemaEpoch_ ||
        command.GetUint("schema_version", schema) == false || schema != Active_.Metadata().PropertySchemaVersion)
    {
        return CommandStatus::SchemaChanged;
    }
    if (Instance_ == nullptr ||
        game_api::HasCapability(Active_.Metadata().Capabilities, Capability::Properties) == false ||
        (State_ != PlayState::Running && State_ != PlayState::Paused))
    {
        return CommandStatus::InvalidRequest;
    }
    Snapshot snapshot;
    if (ReadSnapshot(Active_.Table(), Instance_, snapshot) == false)
    {
        return CommandStatus::HostFailed;
    }
    Array<Message> records;
    if (command.GetRecords("edits", records) == false || records.IsEmpty())
    {
        return CommandStatus::InvalidRequest;
    }
    struct Batch
    {
        EditBatchHeader Header;
        std::array<PropertyEdit, kMaxEditBatch> Edits;
    } batch = {};
    static_assert(sizeof(EditBatchHeader) == 16 && alignof(PropertyEdit) == 8);
    batch.Header.StructSize = sizeof(EditBatchHeader);
    batch.Header.Count = static_cast<uint32>(records.GetSize());
    batch.Header.SchemaVersion = static_cast<uint32>(schema);
    for (usize i = 0; i < records.GetSize(); ++i)
    {
        auto& edit = batch.Edits[i];
        if (DecodeEdit(records[i], edit) == false)
        {
            return CommandStatus::InvalidRequest;
        }
        const PropertyDescriptor* descriptor = nullptr;
        const PropertyValue* current = nullptr;
        for (usize j = 0; j < snapshot.Descriptors.GetSize(); ++j)
        {
            if (snapshot.Descriptors[j].ObjectId == edit.ObjectId &&
                snapshot.Descriptors[j].PropertyId == edit.PropertyId)
            {
                descriptor = &snapshot.Descriptors[j];
                current = &snapshot.Values[j];
                break;
            }
        }
        if (descriptor == nullptr || descriptor->Writable == 0 || descriptor->Kind != edit.Kind)
        {
            return CommandStatus::InvalidRequest;
        }
        if (current->ObjectRevision != edit.ExpectedRevision)
        {
            return CommandStatus::StaleRevision;
        }
        PropertyValue proposed = {};
        proposed.ObjectId = edit.ObjectId;
        proposed.PropertyId = edit.PropertyId;
        proposed.Kind = edit.Kind;
        proposed.IntOrEnum = edit.IntOrEnum;
        proposed.Float = edit.Float;
        proposed.StringLength = edit.StringLength;
        std::memcpy(proposed.String, edit.String, sizeof(edit.String));
        if (ValidValue(*descriptor, proposed) == false)
        {
            return CommandStatus::InvalidRequest;
        }
        for (usize j = 0; j < i; ++j)
        {
            if (batch.Edits[j].ObjectId == edit.ObjectId && batch.Edits[j].PropertyId == edit.PropertyId)
            {
                return CommandStatus::InvalidRequest;
            }
        }
    }
    GameEditPlan* plan = nullptr;
    const auto& table = Active_.Table();
    const Status prepared = table.PrepareEdits(
        Instance_,
        {reinterpret_cast<const uint8*>(&batch), sizeof(EditBatchHeader) + records.GetSize() * sizeof(PropertyEdit)},
        &plan);
    if (prepared != Status::Ok || plan == nullptr)
    {
        if (plan != nullptr)
        {
            table.DiscardEdits(Instance_, plan);
        }
        return prepared == Status::StaleRevision   ? CommandStatus::StaleRevision
               : prepared == Status::SchemaChanged ? CommandStatus::SchemaChanged
                                                   : CommandStatus::InvalidRequest;
    }
    if (table.CommitEdits(Instance_, plan) != Status::Ok)
    {
        table.DiscardEdits(Instance_, plan);
        State_ = PlayState::CleanupUnknown; // A non-failing commit contract was violated.
        return CommandStatus::RestartRequired;
    }
    // Copy post-commit revisions before another simulation tick. Undo must
    // compare against this outcome, never a later refreshed value that would
    // accidentally overwrite a simulation/external edit.
    uint64 request = 0;
    if (command.GetHexId("request", request))
    {
        (void)PublishProperties(request);
    }
    return CommandStatus::Ok;
}
} // namespace ludus::runtime::game_host
