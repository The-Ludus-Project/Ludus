#include "internal/hosted_protocol.hpp"

#include <ludus/foundation/base/byte_order.hpp>

#include <cstring>
#include <span>

namespace ludus::platform::hosted
{
namespace
{
constexpr uint8 HEX_RADIX = 16;
constexpr uint8 HEX_LETTERS_OFFSET = 10;
constexpr std::string_view HEX_DIGITS = "0123456789abcdef";
constexpr usize BITS_PER_BYTE = 8;
constexpr std::string_view ADDRESS_SEPARATOR = ":";

/// Appends little-endian fields to a bounded message buffer.
class MessageWriter final
{
public:
    explicit MessageWriter(foundation::BorrowedView<uint8> bytes) noexcept : mBytes(bytes) {}

    template <typename ValueType>
    void Write(ValueType value) noexcept
    {
        if (mHasFailed == true || mBytes.GetSize() - mSize < sizeof(ValueType))
        {
            mHasFailed = true;
            return;
        }
        const std::span<uint8> destination(mBytes.GetDataOrNull() + mSize, sizeof(ValueType));
        mHasFailed = foundation::TryWriteLittleEndian<ValueType>(value, destination) == false;
        mSize += sizeof(ValueType);
    }

    void WriteBytes(const uint8* bytes, usize size) noexcept
    {
        if (mHasFailed == true || mBytes.GetSize() - mSize < size)
        {
            mHasFailed = true;
            return;
        }
        std::memcpy(mBytes.GetDataOrNull() + mSize, bytes, size);
        mSize += size;
    }

    [[nodiscard]] bool HasFailed() const noexcept
    {
        return mHasFailed;
    }
    [[nodiscard]] usize GetSize() const noexcept
    {
        return mSize;
    }

private:
    foundation::BorrowedView<uint8> mBytes;
    usize mSize = 0;
    bool mHasFailed = false;
};

/// Reads little-endian fields from one received payload.
class MessageReader final
{
public:
    MessageReader(const uint8* bytes, usize size) noexcept : mBytes(bytes), mSize(size) {}

    template <typename ValueType>
    [[nodiscard]] ValueType Read() noexcept
    {
        ValueType value = 0;
        if (mHasFailed == true || mSize - mOffset < sizeof(ValueType))
        {
            mHasFailed = true;
            return value;
        }
        const std::span<const uint8> source(mBytes + mOffset, sizeof(ValueType));
        mHasFailed = foundation::TryReadLittleEndian<ValueType>(source, value) == false;
        mOffset += sizeof(ValueType);
        return value;
    }

    [[nodiscard]] const uint8* ReadBytesOrNull(usize size) noexcept
    {
        if (mHasFailed == true || mSize - mOffset < size)
        {
            mHasFailed = true;
            return nullptr;
        }
        const uint8* bytes = mBytes + mOffset;
        mOffset += size;
        return bytes;
    }

    [[nodiscard]] bool HasFailed() const noexcept
    {
        return mHasFailed;
    }

private:
    const uint8* mBytes;
    usize mSize;
    usize mOffset = 0;
    bool mHasFailed = false;
};

uint32 GetScaleBits(float32 scale) noexcept
{
    static_assert(sizeof(float32) == sizeof(uint32));
    uint32 bits = 0;
    std::memcpy(&bits, &scale, sizeof(bits));
    return bits;
}

float32 GetScale(uint32 bits) noexcept
{
    float32 scale = 0.0F;
    std::memcpy(&scale, &bits, sizeof(scale));
    return scale;
}

bool IsValidExtent(uint32 width, uint32 height) noexcept
{
    return width <= MAX_EXTENT_PIXELS && height <= MAX_EXTENT_PIXELS;
}

bool IsValidVisibility(uint8 value) noexcept
{
    return value <= static_cast<uint8>(WindowVisibility::Minimized);
}

bool IsValidRefusal(uint8 value) noexcept
{
    return value > static_cast<uint8>(HostedRefusal::None) &&
           value <= static_cast<uint8>(HostedRefusal::HandshakeTimeout);
}

bool IsWireResetReason(input::ResetReason reason) noexcept
{
    return reason == input::ResetReason::FocusEntered || reason == input::ResetReason::FocusLost ||
           reason == input::ResetReason::Overflow;
}

void WriteKeyMask(const input::FocusBaseline& baseline, bool isFocused, MessageWriter& inoutWriter) noexcept
{
    foundation::StaticArray<uint8, KEY_MASK_BYTES> mask{};
    if (isFocused == true)
    {
        for (usize i = 0; i < input::KEY_COUNT; ++i)
        {
            const input::Key key = static_cast<input::Key>(i);
            if (input::IsValidKey(key) == true && baseline.Keys[i] == true)
            {
                mask[i / BITS_PER_BYTE] = static_cast<uint8>(mask[i / BITS_PER_BYTE] | (1U << (i % BITS_PER_BYTE)));
            }
        }
    }
    inoutWriter.WriteBytes(mask.GetData(), mask.GetSize());
}

bool TryReadKeyMask(const uint8* mask, bool isFocused, input::FocusBaseline& outBaseline) noexcept
{
    input::FocusBaseline baseline;
    baseline.Focused = isFocused;
    for (usize i = 0; i < KEY_MASK_BYTES * BITS_PER_BYTE; ++i)
    {
        const bool isSet = (mask[i / BITS_PER_BYTE] & (1U << (i % BITS_PER_BYTE))) != 0;
        if (isSet == false)
        {
            continue;
        }
        // Unknown, padding bits and any key without focus are protocol violations.
        if (i >= input::KEY_COUNT || input::IsValidKey(static_cast<input::Key>(i)) == false || isFocused == false)
        {
            return false;
        }
        baseline.Keys[i] = true;
    }
    outBaseline = baseline;
    return true;
}

bool TryEncodePayload(const Message& message, MessageWriter& inoutWriter) noexcept
{
    switch (message.Kind)
    {
        case MessageKind::Hello:
            inoutWriter.Write<uint32>(HELLO_MAGIC);
            inoutWriter.Write<uint16>(message.Version);
            inoutWriter.Write<uint16>(message.KeyCount);
            inoutWriter.WriteBytes(message.SessionToken.GetData(), message.SessionToken.GetSize());
            return true;
        case MessageKind::Welcome:
            if (IsValidExtent(message.Width, message.Height) == false || IsValidScale(message.Scale) == false ||
                IsValidVisibility(static_cast<uint8>(message.Visibility)) == false)
            {
                return false;
            }
            inoutWriter.Write<uint16>(PROTOCOL_VERSION);
            inoutWriter.Write<uint8>(static_cast<uint8>(message.Visibility));
            inoutWriter.Write<uint8>(0);
            inoutWriter.Write<uint32>(message.Width);
            inoutWriter.Write<uint32>(message.Height);
            inoutWriter.Write<uint32>(GetScaleBits(message.Scale));
            return true;
        case MessageKind::Refused:
            if (IsValidRefusal(static_cast<uint8>(message.Refusal)) == false)
            {
                return false;
            }
            inoutWriter.Write<uint8>(static_cast<uint8>(message.Refusal));
            return true;
        case MessageKind::Resize:
            if (IsValidExtent(message.Width, message.Height) == false || IsValidScale(message.Scale) == false)
            {
                return false;
            }
            inoutWriter.Write<uint32>(message.Width);
            inoutWriter.Write<uint32>(message.Height);
            inoutWriter.Write<uint32>(GetScaleBits(message.Scale));
            return true;
        case MessageKind::Visibility:
            if (IsValidVisibility(static_cast<uint8>(message.Visibility)) == false)
            {
                return false;
            }
            inoutWriter.Write<uint8>(static_cast<uint8>(message.Visibility));
            return true;
        case MessageKind::Reset:
            if (IsWireResetReason(message.Reason) == false)
            {
                return false;
            }
            inoutWriter.Write<uint8>(static_cast<uint8>(message.Reason));
            WriteKeyMask(message.Baseline, message.Reason == input::ResetReason::FocusEntered, inoutWriter);
            return true;
        case MessageKind::Key:
            if (input::IsValidKey(message.PhysicalKey) == false ||
                static_cast<uint8>(message.Transition) > static_cast<uint8>(input::KeyTransition::Up))
            {
                return false;
            }
            inoutWriter.Write<uint16>(static_cast<uint16>(message.PhysicalKey));
            inoutWriter.Write<uint8>(static_cast<uint8>(message.Transition));
            inoutWriter.Write<uint8>(message.IsRepeat == true ? KEY_FLAG_REPEAT : 0);
            return true;
        case MessageKind::Close:
            return true;
    }
    return false;
}

usize GetPayloadBytes(MessageKind kind) noexcept
{
    switch (kind)
    {
        case MessageKind::Hello:
            return HELLO_PAYLOAD_BYTES;
        case MessageKind::Welcome:
            return WELCOME_PAYLOAD_BYTES;
        case MessageKind::Refused:
            return REFUSED_PAYLOAD_BYTES;
        case MessageKind::Resize:
            return RESIZE_PAYLOAD_BYTES;
        case MessageKind::Visibility:
            return VISIBILITY_PAYLOAD_BYTES;
        case MessageKind::Reset:
            return RESET_PAYLOAD_BYTES;
        case MessageKind::Key:
            return KEY_PAYLOAD_BYTES;
        case MessageKind::Close:
            return CLOSE_PAYLOAD_BYTES;
    }
    return 0;
}

bool IsKnownKind(uint16 value) noexcept
{
    return value >= static_cast<uint16>(MessageKind::Hello) && value <= static_cast<uint16>(MessageKind::Close);
}

bool TryDecodeHello(MessageReader& inoutReader, usize payloadSize, Message& inoutMessage) noexcept
{
    if (payloadSize < sizeof(uint32) + sizeof(uint16) + sizeof(uint16) || inoutReader.Read<uint32>() != HELLO_MAGIC)
    {
        return false;
    }
    inoutMessage.Version = inoutReader.Read<uint16>();
    inoutMessage.KeyCount = inoutReader.Read<uint16>();
    if (inoutMessage.Version != PROTOCOL_VERSION || inoutMessage.KeyCount != input::KEY_COUNT)
    {
        // Another SDK's Hello: report the mismatch, ignore the rest of its payload.
        return inoutReader.HasFailed() == false;
    }
    if (payloadSize != HELLO_PAYLOAD_BYTES)
    {
        return false;
    }
    const uint8* tokenOrNull = inoutReader.ReadBytesOrNull(TOKEN_BYTES);
    if (tokenOrNull == nullptr)
    {
        return false;
    }
    std::memcpy(inoutMessage.SessionToken.GetData(), tokenOrNull, TOKEN_BYTES);
    return true;
}

bool TryDecodePayload(MessageReader& inoutReader, usize payloadSize, Message& inoutMessage) noexcept
{
    if (inoutMessage.Kind == MessageKind::Hello)
    {
        return TryDecodeHello(inoutReader, payloadSize, inoutMessage);
    }
    if (payloadSize != GetPayloadBytes(inoutMessage.Kind))
    {
        return false;
    }
    switch (inoutMessage.Kind)
    {
        case MessageKind::Hello:
            return false;
        case MessageKind::Welcome: {
            inoutMessage.Version = inoutReader.Read<uint16>();
            const uint8 visibility = inoutReader.Read<uint8>();
            const uint8 reserved = inoutReader.Read<uint8>();
            inoutMessage.Width = inoutReader.Read<uint32>();
            inoutMessage.Height = inoutReader.Read<uint32>();
            inoutMessage.Scale = GetScale(inoutReader.Read<uint32>());
            inoutMessage.Visibility = static_cast<WindowVisibility>(visibility);
            return inoutMessage.Version == PROTOCOL_VERSION && reserved == 0 && IsValidVisibility(visibility) == true &&
                   IsValidExtent(inoutMessage.Width, inoutMessage.Height) == true &&
                   IsValidScale(inoutMessage.Scale) == true;
        }
        case MessageKind::Refused: {
            const uint8 refusal = inoutReader.Read<uint8>();
            inoutMessage.Refusal = static_cast<HostedRefusal>(refusal);
            return IsValidRefusal(refusal);
        }
        case MessageKind::Resize:
            inoutMessage.Width = inoutReader.Read<uint32>();
            inoutMessage.Height = inoutReader.Read<uint32>();
            inoutMessage.Scale = GetScale(inoutReader.Read<uint32>());
            return IsValidExtent(inoutMessage.Width, inoutMessage.Height) == true &&
                   IsValidScale(inoutMessage.Scale) == true;
        case MessageKind::Visibility: {
            const uint8 visibility = inoutReader.Read<uint8>();
            inoutMessage.Visibility = static_cast<WindowVisibility>(visibility);
            return IsValidVisibility(visibility);
        }
        case MessageKind::Reset: {
            inoutMessage.Reason = static_cast<input::ResetReason>(inoutReader.Read<uint8>());
            const uint8* maskOrNull = inoutReader.ReadBytesOrNull(KEY_MASK_BYTES);
            return maskOrNull != nullptr && IsWireResetReason(inoutMessage.Reason) == true &&
                   TryReadKeyMask(maskOrNull,
                                  inoutMessage.Reason == input::ResetReason::FocusEntered,
                                  inoutMessage.Baseline);
        }
        case MessageKind::Key: {
            inoutMessage.PhysicalKey = static_cast<input::Key>(inoutReader.Read<uint16>());
            const uint8 transition = inoutReader.Read<uint8>();
            const uint8 flags = inoutReader.Read<uint8>();
            inoutMessage.Transition = static_cast<input::KeyTransition>(transition);
            inoutMessage.IsRepeat = (flags & KEY_FLAG_REPEAT) != 0;
            return input::IsValidKey(inoutMessage.PhysicalKey) == true &&
                   transition <= static_cast<uint8>(input::KeyTransition::Up) && (flags & ~KEY_FLAG_REPEAT) == 0;
        }
        case MessageKind::Close:
            return true;
    }
    return false;
}

bool TryReadHexDigit(char digit, uint8& outValue) noexcept
{
    if (digit >= '0' && digit <= '9')
    {
        outValue = static_cast<uint8>(digit - '0');
        return true;
    }
    if (digit >= 'a' && digit <= 'f')
    {
        outValue = static_cast<uint8>(digit - 'a' + HEX_LETTERS_OFFSET);
        return true;
    }
    return false;
}

bool IsValidSocketPath(std::string_view path) noexcept
{
    return path.empty() == false && path.size() <= MAX_SOCKET_PATH_BYTES && path.front() == '/' &&
           path.find('\0') == std::string_view::npos;
}
} // namespace

bool IsValidScale(float32 scale) noexcept
{
    // NaN fails both comparisons; infinity fails the upper bound.
    return scale > 0.0F && scale <= MAX_SCALE;
}

bool AreTokensEqual(const Token& left, const Token& right) noexcept
{
    uint8 difference = 0;
    for (usize i = 0; i < TOKEN_BYTES; ++i)
    {
        difference = static_cast<uint8>(difference | (left[i] ^ right[i]));
    }
    return difference == 0;
}

bool TryEncodeMessage(const Message& message, foundation::BorrowedView<uint8> outBytes, usize& outSize) noexcept
{
    foundation::StaticArray<uint8, MAX_MESSAGE_BYTES> payload{};
    MessageWriter payloadWriter({payload.GetData(), payload.GetSize()});
    if (TryEncodePayload(message, payloadWriter) == false || payloadWriter.HasFailed() == true ||
        outBytes.IsValid() == false || outBytes.GetSize() < HEADER_BYTES + payloadWriter.GetSize())
    {
        return false;
    }
    MessageWriter writer(outBytes);
    writer.Write<uint16>(static_cast<uint16>(message.Kind));
    writer.Write<uint16>(static_cast<uint16>(payloadWriter.GetSize()));
    writer.WriteBytes(payload.GetData(), payloadWriter.GetSize());
    if (writer.HasFailed() == true)
    {
        return false;
    }
    outSize = writer.GetSize();
    return true;
}

DecodeStatus
TryDecodeMessage(foundation::BorrowedView<const uint8> bytes, Message& outMessage, usize& outConsumed) noexcept
{
    if (bytes.IsValid() == false || bytes.GetSize() < HEADER_BYTES)
    {
        return DecodeStatus::NeedMore;
    }
    MessageReader header(bytes.GetDataOrNull(), HEADER_BYTES);
    const uint16 kind = header.Read<uint16>();
    const uint16 payloadSize = header.Read<uint16>();
    if (header.HasFailed() == true || IsKnownKind(kind) == false || payloadSize > MAX_FRAME_PAYLOAD_BYTES)
    {
        return DecodeStatus::Malformed;
    }
    if (bytes.GetSize() - HEADER_BYTES < payloadSize)
    {
        return DecodeStatus::NeedMore;
    }
    Message message;
    message.Kind = static_cast<MessageKind>(kind);
    MessageReader reader(bytes.GetDataOrNull() + HEADER_BYTES, payloadSize);
    if (TryDecodePayload(reader, payloadSize, message) == false || reader.HasFailed() == true)
    {
        return DecodeStatus::Malformed;
    }
    outMessage = message;
    outConsumed = HEADER_BYTES + payloadSize;
    return DecodeStatus::Ok;
}

bool TryFormatChannelAddress(const Token& token, std::string_view socketPath, ChannelAddressText& outAddress) noexcept
{
    if (IsValidSocketPath(socketPath) == false)
    {
        return false;
    }
    foundation::StaticString<TOKEN_HEX_BYTES> hex;
    for (usize i = 0; i < TOKEN_BYTES; ++i)
    {
        if (hex.TryAppend(HEX_DIGITS.substr(token[i] / HEX_RADIX, 1)) != foundation::StringStatus::Ok ||
            hex.TryAppend(HEX_DIGITS.substr(token[i] % HEX_RADIX, 1)) != foundation::StringStatus::Ok)
        {
            return false;
        }
    }
    ChannelAddressText address;
    if (address.TryAppend(ADDRESS_PREFIX) != foundation::StringStatus::Ok ||
        address.TryAppend(hex.GetView()) != foundation::StringStatus::Ok ||
        address.TryAppend(ADDRESS_SEPARATOR) != foundation::StringStatus::Ok ||
        address.TryAppend(socketPath) != foundation::StringStatus::Ok)
    {
        return false;
    }
    outAddress = address;
    return true;
}

bool TryParseChannelAddress(std::string_view address, Token& outToken, SocketPath& outSocketPath) noexcept
{
    const usize tokenEnd = ADDRESS_PREFIX.size() + TOKEN_HEX_BYTES;
    if (address.starts_with(ADDRESS_PREFIX) == false || address.size() <= tokenEnd ||
        address[tokenEnd] != ADDRESS_SEPARATOR.front())
    {
        return false;
    }
    Token token{};
    for (usize i = 0; i < TOKEN_BYTES; ++i)
    {
        uint8 high = 0;
        uint8 low = 0;
        if (TryReadHexDigit(address[ADDRESS_PREFIX.size() + (i * 2)], high) == false ||
            TryReadHexDigit(address[ADDRESS_PREFIX.size() + (i * 2) + 1], low) == false)
        {
            return false;
        }
        token[i] = static_cast<uint8>((high * HEX_RADIX) + low);
    }
    const std::string_view path = address.substr(tokenEnd + 1);
    SocketPath socketPath;
    if (IsValidSocketPath(path) == false || socketPath.TryAssign(path) != foundation::StringStatus::Ok)
    {
        return false;
    }
    outToken = token;
    outSocketPath = socketPath;
    return true;
}
} // namespace ludus::platform::hosted
