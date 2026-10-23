// Thanks to Pete Isensee, "Bit Packing: A Network Compression Technique",
// Game Programming Gems 4, section 6.5 (PDF pp. 557-564), and Glenn Fiedler,
// "Serialization Strategies" (https://www.gafferongames.com/post/serialization_strategies/),
// for bounded field packing and checked decoding. This original implementation
// keeps checks in every build and uses shared FoundationBase endian codecs.
// Review: docs/architecture/networking-reference-review.md.
#include <ludus/network/core/wire.h>

#include <ludus/foundation/base/byte_order.hpp>

#include <cstring>

namespace ludus::network
{
namespace
{
/// Message header layout after the four-byte "LNET" magic: version and lane bytes,
/// then little-endian kind, session, sequence, payload length and a reserved word.
constexpr usize HEADER_VERSION_OFFSET = 4;
constexpr usize HEADER_LANE_OFFSET = 5;
constexpr usize HEADER_KIND_OFFSET = 6;
constexpr usize HEADER_SESSION_OFFSET = 8;
constexpr usize HEADER_SEQUENCE_OFFSET = 16;
constexpr usize HEADER_PAYLOAD_BYTES_OFFSET = 20;
constexpr usize HEADER_RESERVED_OFFSET = 22;
bool HasBits(std::span<const uint8> bytes, usize cursor, uint8 count) noexcept
{
    const usize byte = cursor / 8;
    if (byte > bytes.size())
    {
        return false;
    }
    const usize needed = (cursor % 8 + count + 7) / 8;
    return needed <= bytes.size() - byte;
}
} // namespace

WireStatus BitWriter::WriteBits(uint32 value, uint8 count) noexcept
{
    if (count > 32 || (count < 32 && (value >> count) != 0))
    {
        return WireStatus::InvalidArgument;
    }
    if (count > ~usize{0} - mBitCount)
    {
        return WireStatus::InvalidArgument;
    }
    if (HasBits(mBytes, mBitCount, count) == false)
    {
        return WireStatus::BufferTooSmall;
    }
    for (usize i = 0; i < count; ++i)
    {
        const usize bit = mBitCount + i;
        const auto mask = static_cast<uint8>(1U << (bit % 8));
        mBytes[bit / 8] =
            static_cast<uint8>((mBytes[bit / 8] & static_cast<uint8>(~mask)) | (((value >> i) & 1U) != 0 ? mask : 0));
    }
    mBitCount += count;
    return WireStatus::Ok;
}
WireStatus BitWriter::AlignToByte() noexcept
{
    return WriteBits(0, static_cast<uint8>((8 - mBitCount % 8) % 8));
}
WireStatus BitWriter::WriteBytes(std::span<const uint8> bytes) noexcept
{
    if (mBitCount % 8 != 0)
    {
        return WireStatus::InvalidArgument;
    }
    const usize offset = mBitCount / 8;
    if (bytes.size() > mBytes.size() - offset)
    {
        return WireStatus::BufferTooSmall;
    }
    // Keep the bit cursor representable, even for fabricated giant spans.
    if (bytes.size() > (~usize{0} - mBitCount) / 8)
    {
        return WireStatus::InvalidArgument;
    }
    if (bytes.empty() == false)
    {
        std::memcpy(mBytes.data() + offset, bytes.data(), bytes.size());
    }
    mBitCount += bytes.size() * 8;
    return WireStatus::Ok;
}
WireStatus BitReader::ReadBits(uint8 count, uint32& value) noexcept
{
    if (count > 32)
    {
        return WireStatus::InvalidArgument;
    }
    if (count > ~usize{0} - mBitCount)
    {
        return WireStatus::InvalidArgument;
    }
    if (HasBits(mBytes, mBitCount, count) == false)
    {
        return WireStatus::BufferTooSmall;
    }
    uint32 result = 0;
    for (usize i = 0; i < count; ++i)
    {
        const usize bit = mBitCount + i;
        result |= static_cast<uint32>((mBytes[bit / 8] >> (bit % 8)) & 1U) << i;
    }
    mBitCount += count;
    value = result;
    return WireStatus::Ok;
}
WireStatus BitReader::AlignToByte() noexcept
{
    const auto padding = static_cast<uint8>((8 - mBitCount % 8) % 8);
    BitReader candidate = *this;
    uint32 value = 0;
    const auto status = candidate.ReadBits(padding, value);
    if (status != WireStatus::Ok)
    {
        return status;
    }
    if (value != 0)
    {
        return WireStatus::MalformedMessage;
    }
    *this = candidate;
    return WireStatus::Ok;
}
WireStatus BitReader::ReadBytes(std::span<uint8> bytes) noexcept
{
    if (mBitCount % 8 != 0)
    {
        return WireStatus::InvalidArgument;
    }
    const usize offset = mBitCount / 8;
    if (bytes.size() > mBytes.size() - offset)
    {
        return WireStatus::BufferTooSmall;
    }
    if (bytes.size() > (~usize{0} - mBitCount) / 8)
    {
        return WireStatus::InvalidArgument;
    }
    if (bytes.empty() == false)
    {
        std::memcpy(bytes.data(), mBytes.data() + offset, bytes.size());
    }
    mBitCount += bytes.size() * 8;
    return WireStatus::Ok;
}
WireStatus EncodeMessage(const MessageHeader& header,
                         std::span<const uint8> payload,
                         std::span<uint8> destination,
                         usize& written) noexcept
{
    if (static_cast<usize>(header.Lane) >= CHANNEL_COUNT || header.Kind == 0 || header.Session == 0 ||
        payload.size() > MAX_MESSAGE_BYTES - MESSAGE_HEADER_BYTES)
    {
        return WireStatus::InvalidArgument;
    }
    const usize size = MESSAGE_HEADER_BYTES + payload.size();
    if (destination.size() < size)
    {
        return WireStatus::BufferTooSmall;
    }
    destination[0] = 'L';
    destination[1] = 'N';
    destination[2] = 'E';
    destination[3] = 'T';
    destination[HEADER_VERSION_OFFSET] = WIRE_VERSION;
    destination[HEADER_LANE_OFFSET] = static_cast<uint8>(header.Lane);
    (void)ludus::foundation::TryWriteLittleEndian(header.Kind,
                                                  destination.subspan(HEADER_KIND_OFFSET, sizeof(header.Kind)));
    (void)ludus::foundation::TryWriteLittleEndian(header.Session,
                                                  destination.subspan(HEADER_SESSION_OFFSET, sizeof(header.Session)));
    (void)ludus::foundation::TryWriteLittleEndian(header.Sequence,
                                                  destination.subspan(HEADER_SEQUENCE_OFFSET, sizeof(header.Sequence)));
    (void)ludus::foundation::TryWriteLittleEndian(static_cast<uint16>(payload.size()),
                                                  destination.subspan(HEADER_PAYLOAD_BYTES_OFFSET, sizeof(uint16)));
    (void)ludus::foundation::TryWriteLittleEndian(uint16{0},
                                                  destination.subspan(HEADER_RESERVED_OFFSET, sizeof(uint16)));
    if (payload.empty() == false)
    {
        std::memcpy(destination.data() + MESSAGE_HEADER_BYTES, payload.data(), payload.size());
    }
    written = size;
    return WireStatus::Ok;
}
WireStatus DecodeMessage(std::span<const uint8> bytes, MessageView& view) noexcept
{
    if (bytes.size() < MESSAGE_HEADER_BYTES || bytes.size() > MAX_MESSAGE_BYTES)
    {
        return WireStatus::MalformedMessage;
    }
    if (bytes[0] != 'L' || bytes[1] != 'N' || bytes[2] != 'E' || bytes[3] != 'T')
    {
        return WireStatus::MalformedMessage;
    }
    if (bytes[HEADER_VERSION_OFFSET] != WIRE_VERSION)
    {
        return WireStatus::UnsupportedVersion;
    }
    MessageHeader header;
    header.Lane = static_cast<Channel>(bytes[HEADER_LANE_OFFSET]);
    uint16 payloadBytes = 0;
    uint16 reserved = 0;
    if (ludus::foundation::TryReadLittleEndian(bytes.subspan(HEADER_KIND_OFFSET, sizeof(header.Kind)), header.Kind) ==
            false ||
        ludus::foundation::TryReadLittleEndian(bytes.subspan(HEADER_SESSION_OFFSET, sizeof(header.Session)),
                                               header.Session) == false ||
        ludus::foundation::TryReadLittleEndian(bytes.subspan(HEADER_SEQUENCE_OFFSET, sizeof(header.Sequence)),
                                               header.Sequence) == false ||
        ludus::foundation::TryReadLittleEndian(bytes.subspan(HEADER_PAYLOAD_BYTES_OFFSET, sizeof(payloadBytes)),
                                               payloadBytes) == false ||
        ludus::foundation::TryReadLittleEndian(bytes.subspan(HEADER_RESERVED_OFFSET, sizeof(reserved)), reserved) ==
            false ||
        bytes[HEADER_LANE_OFFSET] >= CHANNEL_COUNT || header.Kind == 0 || header.Session == 0 || reserved != 0 ||
        payloadBytes != bytes.size() - MESSAGE_HEADER_BYTES)
    {
        return WireStatus::MalformedMessage;
    }
    view = MessageView{ .Header = header, .Payload = bytes.subspan(MESSAGE_HEADER_BYTES) };
    return WireStatus::Ok;
}
} // namespace ludus::network
