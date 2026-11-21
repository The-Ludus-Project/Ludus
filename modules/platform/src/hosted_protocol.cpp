#include "internal/hosted_protocol.hpp"

#include <ludus/foundation/base/borrowed_view.hpp>
#include <ludus/foundation/base/byte_order.hpp>
#include <ludus/foundation/strings/utf8.hpp>

#include <cstring>

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
        const foundation::BorrowedView<uint8> destination(mBytes.GetDataOrNull() + mSize, sizeof(ValueType));
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
        const foundation::BorrowedView<const uint8> source(mBytes + mOffset, sizeof(ValueType));
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

uint32 GetFloatBits(float32 value) noexcept
{
    static_assert(sizeof(float32) == sizeof(uint32));
    uint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float32 GetFloat(uint32 bits) noexcept
{
    float32 value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
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

/// Reserved bytes after the image index of Present and Release.
constexpr usize RESERVED_REFERENCE_BYTES = 3;

bool IsValidImageReference(uint32 generation, uint32 image) noexcept
{
    return generation != 0 && image < HOSTED_SURFACE_IMAGE_COUNT;
}

void WriteReserved(usize count, MessageWriter& inoutWriter) noexcept
{
    for (usize i = 0; i < count; ++i)
    {
        inoutWriter.Write<uint8>(0);
    }
}

[[nodiscard]] bool IsReservedZero(usize count, MessageReader& inoutReader) noexcept
{
    uint8 bits = 0;
    for (usize i = 0; i < count; ++i)
    {
        bits = static_cast<uint8>(bits | inoutReader.Read<uint8>());
    }
    return bits == 0;
}

bool TryReadUuid(MessageReader& inoutReader, AdapterUuid& outUuid) noexcept
{
    const uint8* bytesOrNull = inoutReader.ReadBytesOrNull(HOSTED_ADAPTER_UUID_BYTES);
    if (bytesOrNull == nullptr)
    {
        return false;
    }
    std::memcpy(outUuid.GetData(), bytesOrNull, HOSTED_ADAPTER_UUID_BYTES);
    return true;
}

bool TryDecodeImages(MessageReader& inoutReader, Message& inoutMessage) noexcept
{
    inoutMessage.Generation = inoutReader.Read<uint32>();
    inoutMessage.Width = inoutReader.Read<uint32>();
    inoutMessage.Height = inoutReader.Read<uint32>();
    inoutMessage.Format = static_cast<HostedImageFormat>(inoutReader.Read<uint8>());
    inoutMessage.HandleKind = static_cast<HostedHandleKind>(inoutReader.Read<uint8>());
    const bool hasZeroReserved = inoutReader.Read<uint16>() == 0;
    foundation::StaticArray<uint64, HOSTED_SURFACE_IMAGE_COUNT>& imageWords =
        inoutMessage.HandleKind == HostedHandleKind::Win32SharedResource ? inoutMessage.Win32HandleValues
                                                                         : inoutMessage.AllocationSizes;
    for (uint64& word : imageWords)
    {
        word = inoutReader.Read<uint64>();
    }
    inoutMessage.MemoryTypeIndex = inoutReader.Read<uint32>();
    return TryReadUuid(inoutReader, inoutMessage.DeviceUuid) == true &&
           TryReadUuid(inoutReader, inoutMessage.DriverUuid) == true && hasZeroReserved == true &&
           IsValidImagesMessage(inoutMessage) == true;
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

/// @brief Length of a terminated name stored in `capacity + 1` bytes.
/// @return false when no terminator lies within the first capacity + 1 bytes, a byte after
/// the terminator is nonzero, or the text is not valid UTF-8.
template <usize Capacity>
[[nodiscard]] bool TryGetNameLength(const foundation::StaticArray<char, Capacity + 1>& name, usize& outLength) noexcept
{
    usize length = 0;
    while (length <= Capacity && name[length] != '\0')
    {
        ++length;
    }
    if (length > Capacity)
    {
        return false;
    }
    for (usize i = length; i <= Capacity; ++i)
    {
        if (name[i] != '\0')
        {
            return false;
        }
    }
    foundation::Utf8View validated;
    usize errorOffset = 0;
    if (foundation::ValidateUtf8({name.GetData(), length}, validated, errorOffset) != foundation::StringStatus::Ok)
    {
        return false;
    }
    outLength = length;
    return true;
}

/// Write the name's bytes, zero-padded to Capacity.
template <usize Capacity>
void WriteName(const foundation::StaticArray<char, Capacity + 1>& name, MessageWriter& inoutWriter) noexcept
{
    // The bytes after the terminator are zero (TryGetNameLength), so the padding is written as stored.
    inoutWriter.WriteBytes(reinterpret_cast<const uint8*>(name.GetData()), Capacity);
}

/// @brief Read a zero-padded name of `length` bytes.
/// @return false for a length above Capacity, nonzero padding, an embedded zero or invalid UTF-8.
template <usize Capacity>
[[nodiscard]] bool
TryReadName(MessageReader& inoutReader, usize length, foundation::StaticArray<char, Capacity + 1>& outName) noexcept
{
    const uint8* bytesOrNull = inoutReader.ReadBytesOrNull(Capacity);
    if (bytesOrNull == nullptr || length > Capacity)
    {
        return false;
    }
    foundation::StaticArray<char, Capacity + 1> name{};
    std::memcpy(name.GetData(), bytesOrNull, Capacity);
    usize decodedLength = 0;
    if (TryGetNameLength<Capacity>(name, decodedLength) == false || decodedLength != length)
    {
        return false;
    }
    outName = name;
    return true;
}

[[nodiscard]] bool IsValidRequestKind(uint8 value) noexcept
{
    return value == static_cast<uint8>(HostedRecordingRequestKind::Start) ||
           value == static_cast<uint8>(HostedRecordingRequestKind::Stop);
}

[[nodiscard]] bool IsValidRecordingResult(uint8 value) noexcept
{
    return value <= static_cast<uint8>(HostedRecordingResult::UnknownRecording);
}

[[nodiscard]] bool IsValidPassKind(uint8 value) noexcept
{
    return value <= static_cast<uint8>(HostedPassKind::RayTracing);
}

void WriteCoverage(const HostedRecordingCoverage& coverage, MessageWriter& inoutWriter) noexcept
{
    inoutWriter.Write<uint64>(coverage.SubmittedFrames);
    inoutWriter.Write<uint64>(coverage.MeasuredFrames);
    inoutWriter.Write<uint64>(coverage.SkippedFrames);
    inoutWriter.Write<uint64>(coverage.DeliveredSamples);
    inoutWriter.Write<uint64>(coverage.DroppedSamples);
    inoutWriter.Write<uint64>(coverage.InvalidSamples);
    inoutWriter.Write<uint64>(coverage.QueryDroppedPasses);
    inoutWriter.Write<uint64>(coverage.UnresolvedBatches);
}

void ReadCoverage(MessageReader& inoutReader, HostedRecordingCoverage& outCoverage) noexcept
{
    outCoverage.SubmittedFrames = inoutReader.Read<uint64>();
    outCoverage.MeasuredFrames = inoutReader.Read<uint64>();
    outCoverage.SkippedFrames = inoutReader.Read<uint64>();
    outCoverage.DeliveredSamples = inoutReader.Read<uint64>();
    outCoverage.DroppedSamples = inoutReader.Read<uint64>();
    outCoverage.InvalidSamples = inoutReader.Read<uint64>();
    outCoverage.QueryDroppedPasses = inoutReader.Read<uint64>();
    outCoverage.UnresolvedBatches = inoutReader.Read<uint64>();
}

[[nodiscard]] bool TryEncodeRecordingMessage(const Message& message, MessageWriter& inoutWriter) noexcept
{
    if (message.Kind == MessageKind::RecordingRequest)
    {
        const HostedRecordingRequest& request = message.RecordingRequest;
        if (IsValidRecordingRequest(request) == false)
        {
            return false;
        }
        inoutWriter.Write<uint8>(static_cast<uint8>(request.Kind));
        inoutWriter.Write<uint64>(request.Recording);
        inoutWriter.Write<uint64>(request.Request);
        return true;
    }
    if (message.Kind == MessageKind::RecordingAcknowledgement)
    {
        const HostedRecordingAcknowledgement& acknowledgement = message.RecordingAcknowledgement;
        usize nameLength = 0;
        if (IsValidRecordingAcknowledgement(acknowledgement) == false ||
            TryGetNameLength<HOSTED_RECORDING_PRODUCER_NAME_BYTES>(acknowledgement.ProducerName, nameLength) == false)
        {
            return false;
        }
        inoutWriter.Write<uint8>(static_cast<uint8>(acknowledgement.Kind));
        inoutWriter.Write<uint8>(static_cast<uint8>(acknowledgement.Result));
        inoutWriter.Write<uint8>(static_cast<uint8>(nameLength));
        inoutWriter.Write<uint8>(acknowledgement.IsPassTimingSupported == true ? ACKNOWLEDGEMENT_FLAG_PASS_TIMING : 0);
        inoutWriter.Write<uint64>(acknowledgement.Recording);
        inoutWriter.Write<uint64>(acknowledgement.Request);
        inoutWriter.Write<uint64>(acknowledgement.Frame);
        inoutWriter.Write<uint64>(acknowledgement.Ticks);
        WriteCoverage(acknowledgement.Coverage, inoutWriter);
        WriteName<HOSTED_RECORDING_PRODUCER_NAME_BYTES>(acknowledgement.ProducerName, inoutWriter);
        return true;
    }
    const HostedRecordingSample& sample = message.RecordingSample;
    usize nameLength = 0;
    if (IsValidRecordingSample(sample) == false ||
        TryGetNameLength<HOSTED_RECORDING_PASS_NAME_BYTES>(sample.Name, nameLength) == false)
    {
        return false;
    }
    inoutWriter.Write<uint64>(sample.Recording);
    inoutWriter.Write<uint64>(sample.Frame);
    inoutWriter.Write<uint64>(sample.SubmissionTicks);
    inoutWriter.Write<uint64>(sample.CollectionTicks);
    inoutWriter.Write<uint64>(sample.DurationNanoseconds);
    inoutWriter.Write<uint64>(sample.ScopeIdentity);
    inoutWriter.Write<uint32>(sample.ScopePassIndex);
    inoutWriter.Write<uint32>(sample.LogicalPassCount);
    inoutWriter.Write<uint8>(static_cast<uint8>(sample.Kind));
    inoutWriter.Write<uint8>(static_cast<uint8>((sample.HasCpuInterval == true ? SAMPLE_FLAG_CPU_INTERVAL : 0) |
                                                (sample.IsFrame == true ? SAMPLE_FLAG_FRAME : 0)));
    inoutWriter.Write<uint8>(static_cast<uint8>(nameLength));
    inoutWriter.Write<uint8>(0);
    inoutWriter.Write<uint64>(sample.StartTicks);
    inoutWriter.Write<uint64>(sample.EndTicks);
    inoutWriter.Write<uint64>(sample.ClockUncertaintyNanoseconds);
    WriteName<HOSTED_RECORDING_PASS_NAME_BYTES>(sample.Name, inoutWriter);
    return true;
}

[[nodiscard]] bool TryDecodeRecordingMessage(MessageReader& inoutReader, Message& inoutMessage) noexcept
{
    if (inoutMessage.Kind == MessageKind::RecordingRequest)
    {
        HostedRecordingRequest& request = inoutMessage.RecordingRequest;
        const uint8 kind = inoutReader.Read<uint8>();
        request.Kind = static_cast<HostedRecordingRequestKind>(kind);
        request.Recording = inoutReader.Read<uint64>();
        request.Request = inoutReader.Read<uint64>();
        return IsValidRequestKind(kind) == true && IsValidRecordingRequest(request) == true;
    }
    if (inoutMessage.Kind == MessageKind::RecordingAcknowledgement)
    {
        HostedRecordingAcknowledgement& acknowledgement = inoutMessage.RecordingAcknowledgement;
        const uint8 kind = inoutReader.Read<uint8>();
        const uint8 result = inoutReader.Read<uint8>();
        const uint8 nameLength = inoutReader.Read<uint8>();
        const uint8 flags = inoutReader.Read<uint8>();
        acknowledgement.Kind = static_cast<HostedRecordingRequestKind>(kind);
        acknowledgement.Result = static_cast<HostedRecordingResult>(result);
        acknowledgement.IsPassTimingSupported = (flags & ACKNOWLEDGEMENT_FLAG_PASS_TIMING) != 0;
        acknowledgement.Recording = inoutReader.Read<uint64>();
        acknowledgement.Request = inoutReader.Read<uint64>();
        acknowledgement.Frame = inoutReader.Read<uint64>();
        acknowledgement.Ticks = inoutReader.Read<uint64>();
        ReadCoverage(inoutReader, acknowledgement.Coverage);
        return IsValidRequestKind(kind) == true && IsValidRecordingResult(result) == true &&
               (flags & ~ACKNOWLEDGEMENT_FLAG_PASS_TIMING) == 0 &&
               TryReadName<HOSTED_RECORDING_PRODUCER_NAME_BYTES>(inoutReader,
                                                                 nameLength,
                                                                 acknowledgement.ProducerName) == true &&
               IsValidRecordingAcknowledgement(acknowledgement) == true;
    }
    HostedRecordingSample& sample = inoutMessage.RecordingSample;
    sample.Recording = inoutReader.Read<uint64>();
    sample.Frame = inoutReader.Read<uint64>();
    sample.SubmissionTicks = inoutReader.Read<uint64>();
    sample.CollectionTicks = inoutReader.Read<uint64>();
    sample.DurationNanoseconds = inoutReader.Read<uint64>();
    sample.ScopeIdentity = inoutReader.Read<uint64>();
    sample.ScopePassIndex = inoutReader.Read<uint32>();
    sample.LogicalPassCount = inoutReader.Read<uint32>();
    const uint8 kind = inoutReader.Read<uint8>();
    const uint8 flags = inoutReader.Read<uint8>();
    const uint8 nameLength = inoutReader.Read<uint8>();
    const bool hasZeroReserved = inoutReader.Read<uint8>() == 0;
    sample.Kind = static_cast<HostedPassKind>(kind);
    sample.HasCpuInterval = (flags & SAMPLE_FLAG_CPU_INTERVAL) != 0;
    sample.IsFrame = (flags & SAMPLE_FLAG_FRAME) != 0;
    sample.StartTicks = inoutReader.Read<uint64>();
    sample.EndTicks = inoutReader.Read<uint64>();
    sample.ClockUncertaintyNanoseconds = inoutReader.Read<uint64>();
    return IsValidPassKind(kind) == true && (flags & ~(SAMPLE_FLAG_CPU_INTERVAL | SAMPLE_FLAG_FRAME)) == 0 &&
           hasZeroReserved == true &&
           TryReadName<HOSTED_RECORDING_PASS_NAME_BYTES>(inoutReader, nameLength, sample.Name) == true &&
           IsValidRecordingSample(sample) == true;
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
            if (message.ShouldReceiveRelativePointer == true)
            {
                inoutWriter.Write<uint8>(HELLO_FLAG_RELATIVE_POINTER);
            }
            return true;
        case MessageKind::Welcome:
            if (IsValidExtent(message.Width, message.Height) == false || IsValidScale(message.Scale) == false ||
                IsValidVisibility(static_cast<uint8>(message.Visibility)) == false)
            {
                return false;
            }
            inoutWriter.Write<uint16>(PROTOCOL_VERSION);
            inoutWriter.Write<uint8>(static_cast<uint8>(message.Visibility));
            inoutWriter.Write<uint8>(message.HasConsumerAdapter == true ? WELCOME_FLAG_CONSUMER_ADAPTER : 0);
            inoutWriter.Write<uint32>(message.Width);
            inoutWriter.Write<uint32>(message.Height);
            inoutWriter.Write<uint32>(GetFloatBits(message.Scale));
            inoutWriter.WriteBytes(message.Nonce.GetData(), message.Nonce.GetSize());
            if (message.HasConsumerAdapter == true)
            {
                inoutWriter.WriteBytes(message.DeviceUuid.GetData(), message.DeviceUuid.GetSize());
                inoutWriter.WriteBytes(message.DriverUuid.GetData(), message.DriverUuid.GetSize());
            }
            else
            {
                WriteReserved(2 * HOSTED_ADAPTER_UUID_BYTES, inoutWriter);
            }
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
            inoutWriter.Write<uint32>(GetFloatBits(message.Scale));
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
        case MessageKind::RelativePointerMotion:
            if (IsValidPointerMotion(message.PointerMotion) == false)
            {
                return false;
            }
            inoutWriter.Write<uint32>(GetFloatBits(message.PointerMotion.Horizontal));
            inoutWriter.Write<uint32>(GetFloatBits(message.PointerMotion.Vertical));
            return true;
        case MessageKind::Close:
            return true;
        case MessageKind::Images: {
            if (IsValidImagesMessage(message) == false)
            {
                return false;
            }
            inoutWriter.Write<uint32>(message.Generation);
            inoutWriter.Write<uint32>(message.Width);
            inoutWriter.Write<uint32>(message.Height);
            inoutWriter.Write<uint8>(static_cast<uint8>(message.Format));
            inoutWriter.Write<uint8>(static_cast<uint8>(message.HandleKind));
            inoutWriter.Write<uint16>(0);
            const foundation::StaticArray<uint64, HOSTED_SURFACE_IMAGE_COUNT>& imageWords =
                message.HandleKind == HostedHandleKind::Win32SharedResource ? message.Win32HandleValues
                                                                            : message.AllocationSizes;
            for (const uint64 word : imageWords)
            {
                inoutWriter.Write<uint64>(word);
            }
            inoutWriter.Write<uint32>(message.MemoryTypeIndex);
            inoutWriter.WriteBytes(message.DeviceUuid.GetData(), message.DeviceUuid.GetSize());
            inoutWriter.WriteBytes(message.DriverUuid.GetData(), message.DriverUuid.GetSize());
            return true;
        }
        case MessageKind::ImagesCopied:
            inoutWriter.Write<uint32>(message.Generation);
            return message.Generation != 0;
        case MessageKind::Present:
            if (IsValidImageReference(message.Generation, message.Image) == false)
            {
                return false;
            }
            inoutWriter.Write<uint32>(message.Generation);
            inoutWriter.Write<uint8>(static_cast<uint8>(message.Image));
            WriteReserved(RESERVED_REFERENCE_BYTES, inoutWriter);
            inoutWriter.Write<uint64>(message.Sequence);
            return true;
        case MessageKind::Release:
            if (IsValidImageReference(message.Generation, message.Image) == false)
            {
                return false;
            }
            inoutWriter.Write<uint32>(message.Generation);
            inoutWriter.Write<uint8>(static_cast<uint8>(message.Image));
            WriteReserved(RESERVED_REFERENCE_BYTES, inoutWriter);
            return true;
        case MessageKind::RecordingRequest:
        case MessageKind::RecordingAcknowledgement:
        case MessageKind::RecordingSample:
            return TryEncodeRecordingMessage(message, inoutWriter);
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
        case MessageKind::RelativePointerMotion:
            return POINTER_MOTION_PAYLOAD_BYTES;
        case MessageKind::Close:
            return CLOSE_PAYLOAD_BYTES;
        case MessageKind::Images:
            return IMAGES_PAYLOAD_BYTES;
        case MessageKind::Present:
            return PRESENT_PAYLOAD_BYTES;
        case MessageKind::Release:
            return RELEASE_PAYLOAD_BYTES;
        case MessageKind::ImagesCopied:
            return IMAGES_COPIED_PAYLOAD_BYTES;
        case MessageKind::RecordingRequest:
            return RECORDING_REQUEST_PAYLOAD_BYTES;
        case MessageKind::RecordingAcknowledgement:
            return RECORDING_ACKNOWLEDGEMENT_PAYLOAD_BYTES;
        case MessageKind::RecordingSample:
            return RECORDING_SAMPLE_PAYLOAD_BYTES;
    }
    return 0;
}

bool IsKnownKind(uint16 value) noexcept
{
    return value >= static_cast<uint16>(MessageKind::Hello) &&
           value <= static_cast<uint16>(MessageKind::RelativePointerMotion);
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
    if (payloadSize != HELLO_PAYLOAD_BYTES && payloadSize != POINTER_HELLO_PAYLOAD_BYTES)
    {
        return false;
    }
    const uint8* tokenOrNull = inoutReader.ReadBytesOrNull(TOKEN_BYTES);
    if (tokenOrNull == nullptr)
    {
        return false;
    }
    std::memcpy(inoutMessage.SessionToken.GetData(), tokenOrNull, TOKEN_BYTES);
    if (payloadSize == POINTER_HELLO_PAYLOAD_BYTES)
    {
        if (inoutReader.Read<uint8>() != HELLO_FLAG_RELATIVE_POINTER)
        {
            return false;
        }
        inoutMessage.ShouldReceiveRelativePointer = true;
    }
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
            const uint8 flags = inoutReader.Read<uint8>();
            inoutMessage.Width = inoutReader.Read<uint32>();
            inoutMessage.Height = inoutReader.Read<uint32>();
            inoutMessage.Scale = GetFloat(inoutReader.Read<uint32>());
            inoutMessage.Visibility = static_cast<WindowVisibility>(visibility);
            const uint8* nonceOrNull = inoutReader.ReadBytesOrNull(RENDEZVOUS_NONCE_BYTES);
            if (nonceOrNull == nullptr)
            {
                return false;
            }
            std::memcpy(inoutMessage.Nonce.GetData(), nonceOrNull, RENDEZVOUS_NONCE_BYTES);
            inoutMessage.HasConsumerAdapter = (flags & WELCOME_FLAG_CONSUMER_ADAPTER) != 0;
            // Without the flag the UUID bytes are reserved.
            const bool hasValidAdapter = inoutMessage.HasConsumerAdapter == true
                                             ? TryReadUuid(inoutReader, inoutMessage.DeviceUuid) == true &&
                                                   TryReadUuid(inoutReader, inoutMessage.DriverUuid) == true
                                             : IsReservedZero(2 * HOSTED_ADAPTER_UUID_BYTES, inoutReader) == true;
            return inoutMessage.Version == PROTOCOL_VERSION &&
                   (flags | WELCOME_FLAG_CONSUMER_ADAPTER) == WELCOME_FLAG_CONSUMER_ADAPTER &&
                   hasValidAdapter == true && IsValidVisibility(visibility) == true &&
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
            inoutMessage.Scale = GetFloat(inoutReader.Read<uint32>());
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
        case MessageKind::RelativePointerMotion:
            inoutMessage.PointerMotion.Horizontal = GetFloat(inoutReader.Read<uint32>());
            inoutMessage.PointerMotion.Vertical = GetFloat(inoutReader.Read<uint32>());
            return IsValidPointerMotion(inoutMessage.PointerMotion);
        case MessageKind::Close:
            return true;
        case MessageKind::Images:
            return TryDecodeImages(inoutReader, inoutMessage);
        case MessageKind::Present: {
            inoutMessage.Generation = inoutReader.Read<uint32>();
            inoutMessage.Image = inoutReader.Read<uint8>();
            const bool hasZeroReserved = IsReservedZero(RESERVED_REFERENCE_BYTES, inoutReader);
            inoutMessage.Sequence = inoutReader.Read<uint64>();
            return hasZeroReserved == true && IsValidImageReference(inoutMessage.Generation, inoutMessage.Image);
        }
        case MessageKind::ImagesCopied:
            inoutMessage.Generation = inoutReader.Read<uint32>();
            return inoutMessage.Generation != 0;
        case MessageKind::Release: {
            inoutMessage.Generation = inoutReader.Read<uint32>();
            inoutMessage.Image = inoutReader.Read<uint8>();
            return IsReservedZero(RESERVED_REFERENCE_BYTES, inoutReader) == true &&
                   IsValidImageReference(inoutMessage.Generation, inoutMessage.Image);
        }
        case MessageKind::RecordingRequest:
        case MessageKind::RecordingAcknowledgement:
        case MessageKind::RecordingSample:
            return TryDecodeRecordingMessage(inoutReader, inoutMessage);
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

bool IsValidPointerMotion(const input::RelativePointerMotion& motion) noexcept
{
    // A single hardware motion event cannot plausibly cross this many native pointer units.
    constexpr float32 MAX_POINTER_MOTION = 1048576;
    return motion.Horizontal >= -MAX_POINTER_MOTION && motion.Horizontal <= MAX_POINTER_MOTION &&
           motion.Vertical >= -MAX_POINTER_MOTION && motion.Vertical <= MAX_POINTER_MOTION;
}

bool IsValidScale(float32 scale) noexcept
{
    // NaN fails both comparisons; infinity fails the upper bound.
    return scale > 0.0F && scale <= MAX_SCALE;
}

bool IsValidImagesMessage(const Message& message) noexcept
{
    return message.Generation != 0 && message.Width != 0 && message.Height != 0 &&
           IsValidExtent(message.Width, message.Height) == true && message.Format == HostedImageFormat::Rgba8Unorm &&
           (message.HandleKind == HostedHandleKind::MachPort ||
            message.HandleKind == HostedHandleKind::OpaqueFileDescriptor ||
            message.HandleKind == HostedHandleKind::Win32SharedResource);
}

bool IsValidRecordingRequest(const HostedRecordingRequest& request) noexcept
{
    return IsValidRequestKind(static_cast<uint8>(request.Kind)) == true && request.Recording != 0 &&
           request.Request != 0;
}

bool IsValidRecordingAcknowledgement(const HostedRecordingAcknowledgement& acknowledgement) noexcept
{
    usize nameLength = 0;
    return IsValidRequestKind(static_cast<uint8>(acknowledgement.Kind)) == true &&
           IsValidRecordingResult(static_cast<uint8>(acknowledgement.Result)) == true &&
           acknowledgement.Recording != 0 && acknowledgement.Request != 0 &&
           (acknowledgement.IsPassTimingSupported == false ||
            (acknowledgement.Kind == HostedRecordingRequestKind::Start &&
             acknowledgement.Result == HostedRecordingResult::Ok)) &&
           TryGetNameLength<HOSTED_RECORDING_PRODUCER_NAME_BYTES>(acknowledgement.ProducerName, nameLength) == true;
}

bool IsValidRecordingSample(const HostedRecordingSample& sample) noexcept
{
    usize nameLength = 0;
    if (sample.Recording == 0 || sample.Frame == 0 || sample.LogicalPassCount == 0 ||
        IsValidPassKind(static_cast<uint8>(sample.Kind)) == false || sample.CollectionTicks < sample.SubmissionTicks ||
        (sample.HasCpuInterval == true && sample.EndTicks < sample.StartTicks) ||
        TryGetNameLength<HOSTED_RECORDING_PASS_NAME_BYTES>(sample.Name, nameLength) == false)
    {
        return false;
    }
    if (sample.IsFrame == true)
    {
        // A whole frame names no pass: every pass field keeps its neutral value.
        return sample.ScopeIdentity == 0 && sample.ScopePassIndex == 0 && sample.LogicalPassCount == 1 &&
               sample.Kind == HostedPassKind::Raster && sample.HasCpuInterval == false && sample.StartTicks == 0 &&
               sample.EndTicks == 0 && sample.ClockUncertaintyNanoseconds == 0 && nameLength == 0;
    }
    return sample.ScopeIdentity != 0 && nameLength != 0;
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
