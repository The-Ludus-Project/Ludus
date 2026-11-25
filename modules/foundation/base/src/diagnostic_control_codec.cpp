#include <ludus/foundation/base/diagnostic_output.hpp>

#include <ludus/foundation/base/byte_order.hpp>

namespace ludus::foundation::diagnostics
{
namespace
{
/// Field offsets in the 16-byte control header: magic (uint32), version (uint16),
/// kind (uint16), incident (uint32) and payload length (uint32).
constexpr usize CONTROL_VERSION_OFFSET = 4;
constexpr usize CONTROL_KIND_OFFSET = 6;
constexpr usize CONTROL_INCIDENT_OFFSET = 8;
constexpr usize CONTROL_LENGTH_OFFSET = 12;
} // namespace

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
    uint8* bytes = reinterpret_cast<uint8*>(buffer);
    if (TryWriteLittleEndian(CONTROL_PROTOCOL_MAGIC, {bytes, CONTROL_HEADER_SIZE}) == false ||
        TryWriteLittleEndian(CONTROL_PROTOCOL_VERSION,
                             {bytes + CONTROL_VERSION_OFFSET, CONTROL_HEADER_SIZE - CONTROL_VERSION_OFFSET}) == false ||
        TryWriteLittleEndian(static_cast<uint16>(kind),
                             {bytes + CONTROL_KIND_OFFSET, CONTROL_HEADER_SIZE - CONTROL_KIND_OFFSET}) == false ||
        TryWriteLittleEndian(incidentId,
                             {bytes + CONTROL_INCIDENT_OFFSET, CONTROL_HEADER_SIZE - CONTROL_INCIDENT_OFFSET}) ==
            false ||
        TryWriteLittleEndian(length, {bytes + CONTROL_LENGTH_OFFSET, CONTROL_HEADER_SIZE - CONTROL_LENGTH_OFFSET}) ==
            false)
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
    const uint8* bytes = reinterpret_cast<const uint8*>(buffer);
    uint32 magic{};
    uint16 version{};
    ControlHeader next;
    if (TryReadLittleEndian({bytes, CONTROL_HEADER_SIZE}, magic) == false ||
        TryReadLittleEndian({bytes + CONTROL_VERSION_OFFSET, CONTROL_HEADER_SIZE - CONTROL_VERSION_OFFSET}, version) ==
            false ||
        TryReadLittleEndian({bytes + CONTROL_KIND_OFFSET, CONTROL_HEADER_SIZE - CONTROL_KIND_OFFSET}, next.Kind) ==
            false ||
        TryReadLittleEndian({bytes + CONTROL_INCIDENT_OFFSET, CONTROL_HEADER_SIZE - CONTROL_INCIDENT_OFFSET},
                            next.IncidentId) == false ||
        TryReadLittleEndian({bytes + CONTROL_LENGTH_OFFSET, CONTROL_HEADER_SIZE - CONTROL_LENGTH_OFFSET},
                            next.Length) == false ||
        magic != CONTROL_PROTOCOL_MAGIC || version != CONTROL_PROTOCOL_VERSION ||
        next.Kind < static_cast<uint16>(ControlMessageType::Hello) ||
        next.Kind > static_cast<uint16>(ControlMessageType::DecisionReply) || next.Length > CONTROL_MAX_PAYLOAD ||
        next.Length > size - CONTROL_HEADER_SIZE)
    {
        return false;
    }
    header = next;
    return true;
}
} // namespace ludus::foundation::diagnostics
