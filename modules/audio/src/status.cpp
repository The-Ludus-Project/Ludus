#include <ludus/audio/audio_types.h>

namespace ludus::audio
{
const char* ToString(Status status) noexcept
{
    switch (status)
    {
        case Status::Ok:
            return "Ok";
        case Status::Pending:
            return "Pending";
        case Status::Disabled:
            return "Disabled";
        case Status::Suspended:
            return "Suspended";
        case Status::NotReady:
            return "NotReady";
        case Status::Unsupported:
            return "Unsupported";
        case Status::InvalidArgument:
            return "InvalidArgument";
        case Status::InvalidHandle:
            return "InvalidHandle";
        case Status::QueueFull:
            return "QueueFull";
        case Status::VoiceCapacity:
            return "VoiceCapacity";
        case Status::GroupCapacity:
            return "GroupCapacity";
        case Status::StreamCapacity:
            return "StreamCapacity";
        case Status::AssetCapacity:
            return "AssetCapacity";
        case Status::OutOfMemory:
            return "OutOfMemory";
        case Status::DecodeError:
            return "DecodeError";
        case Status::IoError:
            return "IoError";
        case Status::DeviceError:
            return "DeviceError";
        case Status::TooLate:
            return "TooLate";
        case Status::SequenceExhausted:
            return "SequenceExhausted";
    }
    return "Unknown";
}
} // namespace ludus::audio
