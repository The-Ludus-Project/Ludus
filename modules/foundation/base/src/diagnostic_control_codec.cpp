#include <ludus/foundation/base/diagnostic_output.hpp>

#include <ludus/foundation/base/byte_order.hpp>

#include <span>

namespace ludus::foundation::diagnostics
{
// Thanks to Jason Hughes, "What to Look for When Evaluating Middleware for
// Integration", Game Engine Gems, section 1.13 "Platform Portability", p. 12:
// the endian audit motivates shared bounded codecs. Original implementation;
// retain the existing 16-byte wire format and independent emergency path.
// Review: docs/architecture/primitive-types.md, "Boundary adoption audit".
usize EncodeControlHeader(char* buffer,
                          usize capacity,
                          ControlMessageType kind,
                          uint32 incidentId,
                          uint32 length) noexcept
{
    if (buffer == nullptr || capacity < CONTROL_HEADER_SIZE || length > CONTROL_MAX_PAYLOAD)
    {
        return 0;
    }
    const std::span<uint8> bytes{reinterpret_cast<uint8*>(buffer), CONTROL_HEADER_SIZE};
    if (TryWriteLittleEndian(CONTROL_PROTOCOL_MAGIC, bytes) == false ||
        TryWriteLittleEndian(CONTROL_PROTOCOL_VERSION, bytes.subspan(4)) == false ||
        TryWriteLittleEndian(static_cast<uint16>(kind), bytes.subspan(6)) == false ||
        TryWriteLittleEndian(incidentId, bytes.subspan(8)) == false ||
        TryWriteLittleEndian(length, bytes.subspan(12)) == false)
    {
        return 0;
    }
    return CONTROL_HEADER_SIZE;
}

bool DecodeControlHeader(const char* buffer, usize size, ControlHeader& header) noexcept
{
    if (buffer == nullptr || size < CONTROL_HEADER_SIZE)
    {
        return false;
    }
    const std::span<const uint8> bytes{reinterpret_cast<const uint8*>(buffer), CONTROL_HEADER_SIZE};
    uint32 magic{};
    uint16 version{};
    ControlHeader next;
    if (TryReadLittleEndian(bytes, magic) == false || TryReadLittleEndian(bytes.subspan(4), version) == false ||
        TryReadLittleEndian(bytes.subspan(6), next.Kind) == false ||
        TryReadLittleEndian(bytes.subspan(8), next.IncidentId) == false ||
        TryReadLittleEndian(bytes.subspan(12), next.Length) == false || magic != CONTROL_PROTOCOL_MAGIC ||
        version != CONTROL_PROTOCOL_VERSION || next.Kind < static_cast<uint16>(ControlMessageType::Hello) ||
        next.Kind > static_cast<uint16>(ControlMessageType::DecisionReply) || next.Length > CONTROL_MAX_PAYLOAD ||
        next.Length > size - CONTROL_HEADER_SIZE)
    {
        return false;
    }
    header = next;
    return true;
}
} // namespace ludus::foundation::diagnostics
