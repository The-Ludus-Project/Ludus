#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/filesystem.hpp>
#include <ludus/foundation/filesystem/persistence.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/foundation/time/timers.hpp>
#include <ludus/graphics/rhi/graphics_capture.h>

#include "internal/backend.h"
#include "internal/device.h"
#include "internal/graphics_capture.h"
#include "internal/lifecycle.h"
#include "internal/measurement.h"
#include "internal/owner.h"
#include "internal/raster.h"

#include <span>
#include <string_view>

// G1 capture owner: one bounded request per process, driven by the facade's real frame
// boundaries. See docs/architecture/graphics-debugging.md, "Capture request and lifetime
// contract", for the state machine, eligibility, cleanup and manifest rules implemented here.

namespace ludus::graphics::rhi
{
namespace
{
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;
namespace filesystem = foundation::filesystem;
namespace time = foundation::time;

/// Version of the manifest record written beside each completed capture.
constexpr uint64 MANIFEST_VERSION = 1;
/// Bound on a serialized manifest, in bytes; all fields are bounded well below it.
constexpr usize MANIFEST_BYTES = 4096;
/// Artifact stem capacity, in bytes: "ludus-capture-" and two 20-digit decimal identities.
constexpr usize ARTIFACT_STEM_BYTES = 64;
/// Digits of the largest uint64 value in decimal.
constexpr usize MAX_UINT64_DECIMAL_DIGITS = 20;
constexpr uint64 DECIMAL_BASE = 10;
/// JSON requires escaping bytes below the first printable ASCII character.
constexpr uint8 FIRST_PRINTABLE_CHARACTER = 0x20;
constexpr uint8 HEXADECIMAL_DIGIT_BITS = 4;
constexpr uint8 HEXADECIMAL_DIGIT_MASK = 0xF;
/// Bytes of a JSON "\\u00XX" control-character escape.
constexpr usize CONTROL_ESCAPE_BYTES = 6;
/// UTF-8 continuation bytes are 10xxxxxx.
constexpr uint8 UTF8_CONTINUATION_MASK = 0xC0;
constexpr uint8 UTF8_CONTINUATION_TAG = 0x80;
constexpr std::string_view ARTIFACT_STEM_PREFIX = "ludus-capture-";
constexpr std::string_view MANIFEST_SUFFIX = ".manifest.json";

using ArtifactStem = foundation::StaticString<ARTIFACT_STEM_BYTES>;
using ManifestText = foundation::StaticString<MANIFEST_BYTES>;
using DestinationPath = foundation::StaticString<filesystem::MAX_ROOT_BYTES>;
using CaptureName = foundation::StaticArray<char, GRAPHICS_CAPTURE_NAME_BYTES>;

struct CaptureRecord final
{
    GraphicsCaptureReport Report;
    DestinationPath Destination;
    ArtifactStem Stem;
    time::Deadline Deadline;
    /// Backend and GPU identity copied at admission, while the session is live.
    Backend SelectedBackend = Backend::Vulkan;
    MeasurementAdapter Gpu;
    bool IsCancelRequested = false;
    /// The adapter reported a verified artifact; the manifest is published at the next poll.
    bool HasVerifiedArtifact = false;
};

/// Capture record, native adapter and frame sequence of one owner slot (R14c).
struct CaptureState final
{
    CaptureRecord Record;
    internal::GraphicsCaptureAdapter* AdapterOrNull = nullptr;
    uint64 FrameSequence = 0;
};
foundation::StaticArray<CaptureState, MAX_DEVICE_OWNERS> gCaptures{};
/// Request numbers are process-wide, so an identity names one owner's capture.
uint64 gNextRequest = 1;
/// The capture state of the bound owner slot.
CaptureState& GetCapture() noexcept
{
    return gCaptures[internal::GetBoundOwnerSlot()];
}

[[nodiscard]] bool IsContinuationByte(char character) noexcept
{
    return (static_cast<uint8>(character) & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION_TAG;
}
/// Copy adapter text, shortening it at a UTF-8 boundary when it does not fit.
template <usize N>
void CopyText(std::string_view source, foundation::StaticArray<char, N>& outText) noexcept
{
    usize length = source.size() < N ? source.size() : N - 1;
    if (length < source.size())
    {
        while (length > 0 && IsContinuationByte(source[length]) == true)
        {
            --length;
        }
    }
    for (usize i = 0; i < N; ++i)
    {
        outText[i] = i < length ? source[i] : '\0';
    }
}
template <usize N>
void CopyText(const char* sourceOrNull, foundation::StaticArray<char, N>& outText) noexcept
{
    CopyText(sourceOrNull == nullptr ? std::string_view{} : std::string_view{sourceOrNull}, outText);
}
template <usize N>
[[nodiscard]] std::string_view GetText(const foundation::StaticArray<char, N>& text) noexcept
{
    return {text.GetData()};
}
[[nodiscard]] bool IsActive(GraphicsCaptureState state) noexcept
{
    return state == GraphicsCaptureState::Armed || state == GraphicsCaptureState::Capturing ||
           state == GraphicsCaptureState::Finalizing;
}
/// Bind the owner slot whose capture `identity` names; false for a stale or zero identity.
[[nodiscard]] bool IsCurrent(GraphicsCaptureIdentity identity) noexcept
{
    for (usize slot = 0; identity.Request != 0 && slot < MAX_DEVICE_OWNERS; ++slot)
    {
        if (identity == gCaptures[slot].Record.Report.Identity)
        {
            internal::BindOwnerSlot(slot);
            return true;
        }
    }
    return false;
}
/// One native capture runs per process: capture tools such as MTLCaptureManager are process-wide.
[[nodiscard]] bool IsAnyCaptureActive() noexcept
{
    for (const CaptureState& capture : gCaptures)
    {
        if (IsActive(capture.Record.Report.State) == true)
        {
            return true;
        }
    }
    return false;
}
template <usize N>
[[nodiscard]] bool AppendDecimal(uint64 value, foundation::StaticString<N>& inoutText) noexcept
{
    foundation::StaticArray<char, MAX_UINT64_DECIMAL_DIGITS> digits{};
    usize count = 0;
    do
    {
        digits[MAX_UINT64_DECIMAL_DIGITS - 1 - count] = static_cast<char>('0' + value % DECIMAL_BASE);
        value /= DECIMAL_BASE;
        ++count;
    } while (value != 0);
    return inoutText.TryAppend({digits.GetData() + MAX_UINT64_DECIMAL_DIGITS - count, count}) ==
           foundation::StringStatus::Ok;
}
[[nodiscard]] bool MakeArtifactStem(GraphicsCaptureIdentity identity, ArtifactStem& outStem) noexcept
{
    ArtifactStem stem;
    const bool isWritten = stem.TryAssign(ARTIFACT_STEM_PREFIX) == foundation::StringStatus::Ok &&
                           AppendDecimal(identity.Session, stem) == true &&
                           stem.TryAppend("-") == foundation::StringStatus::Ok &&
                           AppendDecimal(identity.Request, stem) == true;
    if (isWritten == false)
    {
        return false;
    }
    outStem = stem;
    return true;
}
/// "<stem>.manifest.json"; always fits GRAPHICS_CAPTURE_NAME_BYTES for a valid stem.
[[nodiscard]] bool MakeManifestName(std::string_view stem, CaptureName& outName) noexcept
{
    if (stem.size() + MANIFEST_SUFFIX.size() >= GRAPHICS_CAPTURE_NAME_BYTES)
    {
        return false;
    }
    CaptureName name{};
    for (usize i = 0; i < stem.size(); ++i)
    {
        name[i] = stem[i];
    }
    for (usize i = 0; i < MANIFEST_SUFFIX.size(); ++i)
    {
        name[stem.size() + i] = MANIFEST_SUFFIX[i];
    }
    outName = name;
    return true;
}
/// A single relative entry named "<stem>.<extension>", other than the manifest.
[[nodiscard]] bool IsArtifactName(std::string_view name, std::string_view stem) noexcept
{
    if (name.size() >= GRAPHICS_CAPTURE_NAME_BYTES || name.size() <= stem.size() + 1 ||
        filesystem::ValidPath(name) == false || name.find('/') != std::string_view::npos ||
        name.substr(0, stem.size()) != stem || name[stem.size()] != '.' || name.substr(stem.size()) == MANIFEST_SUFFIX)
    {
        return false;
    }
    return true;
}
[[nodiscard]] std::string_view GetBackendName(Backend backend) noexcept
{
    switch (backend)
    {
        case Backend::Vulkan:
            return "Vulkan";
        case Backend::WebGPU:
            return "WebGPU";
        case Backend::WebGL2:
            return "WebGL2";
        case Backend::Metal:
            return "Metal";
        case Backend::Direct3D12:
            return "Direct3D12";
    }
    return "Unknown";
}
[[nodiscard]] std::string_view GetToolName(GraphicsCaptureTool tool) noexcept
{
    switch (tool)
    {
        case GraphicsCaptureTool::None:
            return "None";
        case GraphicsCaptureTool::Metal:
            return "Metal";
        case GraphicsCaptureTool::RenderDoc:
            return "RenderDoc";
    }
    return "Unknown";
}
[[nodiscard]] std::string_view GetBuildConfigurationName() noexcept
{
#if defined(LUDUS_BUILD_DEBUG)
    return "Debug";
#elif defined(LUDUS_BUILD_DEVELOPMENT)
    return "Development";
#elif defined(LUDUS_BUILD_PROFILE)
    return "Profile";
#else
    return "Release";
#endif
}

/// Manifest field name, a distinct type so a key and its text value cannot be swapped.
struct ManifestKey final
{
    std::string_view Name;
};

/// Bounded JSON object writer for the manifest. A field that does not fit fails the whole
/// manifest rather than publishing a truncated record.
class ManifestWriter final
{
public:
    void WriteText(ManifestKey key, std::string_view value) noexcept
    {
        beginField(key);
        appendString(value);
    }
    void WriteNumber(ManifestKey key, uint64 value) noexcept
    {
        beginField(key);
        ArtifactStem digits;
        mIsOk = mIsOk && AppendDecimal(value, digits) == true;
        append(digits.GetView());
    }
    [[nodiscard]] bool Finish(std::span<const uint8>& outBytes) noexcept
    {
        append(mHasField == true ? "\n}\n" : "{}\n");
        if (mIsOk == false)
        {
            return false;
        }
        outBytes = {reinterpret_cast<const uint8*>(mText.GetData()), mText.GetSize()};
        return true;
    }

private:
    void append(std::string_view text) noexcept
    {
        mIsOk = mIsOk && mText.TryAppend(text) == foundation::StringStatus::Ok;
    }
    void beginField(ManifestKey key) noexcept
    {
        append(mHasField == true ? ",\n  " : "{\n  ");
        mHasField = true;
        appendString(key.Name);
        append(": ");
    }
    void appendString(std::string_view value) noexcept
    {
        constexpr std::string_view HEXADECIMAL_DIGITS = "0123456789abcdef";
        append("\"");
        for (const char character : value)
        {
            const uint8 byte = static_cast<uint8>(character);
            if (character == '"' || character == '\\')
            {
                const foundation::StaticArray<char, 2> escaped = {'\\', character};
                append({escaped.GetData(), escaped.GetSize()});
            }
            else if (byte < FIRST_PRINTABLE_CHARACTER)
            {
                const foundation::StaticArray<char, CONTROL_ESCAPE_BYTES> escaped = {
                    '\\',
                    'u',
                    '0',
                    '0',
                    HEXADECIMAL_DIGITS[byte >> HEXADECIMAL_DIGIT_BITS],
                    HEXADECIMAL_DIGITS[byte & HEXADECIMAL_DIGIT_MASK],
                };
                append({escaped.GetData(), escaped.GetSize()});
            }
            else
            {
                append({&character, 1});
            }
        }
        append("\"");
    }

    ManifestText mText;
    bool mHasField = false;
    bool mIsOk = true;
};

void Terminate(GraphicsCaptureState state, GraphicsCaptureError error) noexcept
{
    GetCapture().Record.Report.State = state;
    GetCapture().Record.Report.Error = error;
    GetCapture().Record.IsCancelRequested = false;
    GetCapture().Record.HasVerifiedArtifact = false;
    GetCapture().Record.Deadline.Cancel();
}
/// Run the adapter's synchronous cleanup and record any partial artifact it identifies.
void AbandonNativeCapture() noexcept
{
    if (GetCapture().AdapterOrNull == nullptr)
    {
        return;
    }
    internal::GraphicsCaptureCleanup cleanup;
    GetCapture().AdapterOrNull->Abandon(GetCapture().Record.Report.Identity, cleanup);
    const std::string_view artifact = GetText(cleanup.Artifact);
    if (cleanup.IsArtifactPreserved == true && IsArtifactName(artifact, GetCapture().Record.Stem.GetView()) == true)
    {
        GetCapture().Record.Report.IsArtifactPreserved = true;
        CopyText(artifact, GetCapture().Record.Report.Artifact);
    }
}
void PublishManifest() noexcept
{
    CaptureName manifestName{};
    std::span<const uint8> bytes;
    const MeasurementAdapter& gpu = GetCapture().Record.Gpu;
    ManifestWriter writer;
    writer.WriteText({"format"}, "ludus-graphics-capture");
    writer.WriteNumber({"version"}, MANIFEST_VERSION);
    writer.WriteNumber({"session"}, GetCapture().Record.Report.Identity.Session);
    writer.WriteNumber({"request"}, GetCapture().Record.Report.Identity.Request);
    writer.WriteText({"build"}, GetBuildConfigurationName());
    writer.WriteText({"backend"}, GetBackendName(GetCapture().Record.SelectedBackend));
    writer.WriteText({"tool"}, GetToolName(GetCapture().Record.Report.Tool));
    writer.WriteText({"toolVersion"}, GetText(GetCapture().Record.Report.ToolVersion));
    writer.WriteText({"adapter"}, std::string_view{gpu.Name});
    writer.WriteNumber({"vendorId"}, gpu.VendorId);
    writer.WriteNumber({"deviceId"}, gpu.DeviceId);
    writer.WriteNumber({"driverVersion"}, gpu.DriverVersion);
    writer.WriteText({"captureScope"}, "device frame");
    writer.WriteNumber({"requestedFrame"}, GetCapture().Record.Report.RequestedFrame);
    writer.WriteNumber({"capturedFrame"}, GetCapture().Record.Report.CapturedFrame);
    writer.WriteNumber({"skippedFrames"}, GetCapture().Record.Report.SkippedFrames);
    writer.WriteText({"artifact"}, GetText(GetCapture().Record.Report.Artifact));
    filesystem::WriteDirectory directory;
    filesystem::PublicationResult published;
    if (writer.Finish(bytes) == true && MakeManifestName(GetCapture().Record.Stem.GetView(), manifestName) == true &&
        directory.Open(GetCapture().Record.Destination.GetView()).Succeeded() == true)
    {
        const filesystem::WriteOptions options =
        {
            .MaxBytes = MANIFEST_BYTES,
            .Sync = filesystem::SyncPolicy::FileAndDirectory,
            .Condition = filesystem::WriteCondition::Missing,
        };
        published = directory.Publish(GetText(manifestName), bytes, options);
    }
    // The verified artifact stays in place either way; only the manifest claim differs.
    GetCapture().Record.Report.IsArtifactPreserved = true;
    if (published.Published == true)
    {
        GetCapture().Record.Report.IsManifestPublished = true;
        GetCapture().Record.Report.Manifest = manifestName;
        Terminate(GraphicsCaptureState::Complete, GraphicsCaptureError::None);
        return;
    }
    CopyText("the capture manifest could not be published; the artifact is preserved",
             GetCapture().Record.Report.Diagnostic);
    Terminate(GraphicsCaptureState::Failed, GraphicsCaptureError::ManifestFailed);
}
/// Poll finalization, publish a verified artifact and expire deadlines, outside frames.
void AdvanceRecord() noexcept
{
    if (IsActive(GetCapture().Record.Report.State) == false || internal::FrameOpen() == true)
    {
        return;
    }
    if (GetCapture().Record.Report.State == GraphicsCaptureState::Finalizing &&
        GetCapture().Record.HasVerifiedArtifact == false && GetCapture().AdapterOrNull != nullptr)
    {
        GetCapture().AdapterOrNull->PollFinalization(GetCapture().Record.Report.Identity);
    }
    if (GetCapture().Record.Report.State == GraphicsCaptureState::Finalizing &&
        GetCapture().Record.HasVerifiedArtifact == true)
    {
        PublishManifest();
        return;
    }
    if (IsActive(GetCapture().Record.Report.State) == true &&
        GetCapture().Record.Deadline.IsExpired(time::Now()) == true)
    {
        if (GetCapture().Record.Report.State == GraphicsCaptureState::Finalizing)
        {
            AbandonNativeCapture();
        }
        Terminate(GraphicsCaptureState::Failed, GraphicsCaptureError::DeadlineExceeded);
    }
}
[[nodiscard]] GraphicsCaptureError ValidateDevice(DeviceHandle device) noexcept
{
    switch (internal::ValidateDeviceReadiness(device))
    {
        case RasterStatus::Ready:
            return GraphicsCaptureError::None;
        case RasterStatus::DeviceLost:
            return GraphicsCaptureError::DeviceLost;
        default:
            return GraphicsCaptureError::InvalidTarget;
    }
}
/// First reason the registered adapter cannot start a capture now; None when it can.
[[nodiscard]] GraphicsCaptureError GetAdapterAvailability(const internal::GraphicsCaptureAdapterState& state) noexcept
{
    if (state.IsRestartRequired == true)
    {
        return GraphicsCaptureError::RestartRequired;
    }
    if (state.IsToolAttached == false)
    {
        return GraphicsCaptureError::ToolUnavailable;
    }
    if (state.IsExternalCaptureActive == true)
    {
        return GraphicsCaptureError::Busy;
    }
    return GraphicsCaptureError::None;
}
[[nodiscard]] bool IsValidDestination(foundation::BorrowedView<const char> destination) noexcept
{
    if (destination.IsValid() == false || destination.GetSize() == 0 ||
        destination.GetSize() > filesystem::MAX_ROOT_BYTES)
    {
        return false;
    }
    for (usize i = 0; i < destination.GetSize(); ++i)
    {
        if (destination[i] == '\0')
        {
            return false;
        }
    }
    return true;
}
/// The destination must be an existing writable directory without this request's manifest.
[[nodiscard]] bool CanWriteDestination(std::string_view destination, std::string_view stem) noexcept
{
    CaptureName manifestName{};
    filesystem::Directory directory;
    filesystem::WriteDirectory writable;
    filesystem::FileStamp stamp;
    return MakeManifestName(stem, manifestName) == true && directory.Open(destination).Succeeded() == true &&
           writable.Open(destination).Succeeded() == true &&
           directory.Observe(GetText(manifestName), stamp).Code == filesystem::Status::NotFound;
}
} // namespace

namespace internal
{
void SetGraphicsCaptureAdapter(GraphicsCaptureAdapter* adapterOrNull) noexcept
{
    GetCapture().AdapterOrNull = adapterOrNull;
}
void ReportGraphicsCaptureFinalization(GraphicsCaptureIdentity identity,
                                       const GraphicsCaptureFinalization& finalization) noexcept
{
    if (IsCurrent(identity) == false || GetCapture().Record.Report.State != GraphicsCaptureState::Finalizing ||
        GetCapture().Record.HasVerifiedArtifact == true)
    {
        return;
    }
    CopyText(finalization.ToolVersionOrNull, GetCapture().Record.Report.ToolVersion);
    CopyText(finalization.DiagnosticOrNull, GetCapture().Record.Report.Diagnostic);
    const std::string_view artifact =
        finalization.ArtifactOrNull == nullptr ? std::string_view{} : std::string_view{finalization.ArtifactOrNull};
    const bool isArtifactNamed = IsArtifactName(artifact, GetCapture().Record.Stem.GetView());
    if (finalization.Error == GraphicsCaptureError::None && finalization.IsArtifactVerified == true &&
        isArtifactNamed == true)
    {
        CopyText(artifact, GetCapture().Record.Report.Artifact);
        GetCapture().Record.HasVerifiedArtifact = true;
        return;
    }
    if ((finalization.IsArtifactPreserved == true || finalization.Error == GraphicsCaptureError::None) &&
        isArtifactNamed == true)
    {
        GetCapture().Record.Report.IsArtifactPreserved = true;
        CopyText(artifact, GetCapture().Record.Report.Artifact);
    }
    if (finalization.Error == GraphicsCaptureError::None && finalization.DiagnosticOrNull == nullptr)
    {
        CopyText("the adapter reported no verified artifact", GetCapture().Record.Report.Diagnostic);
    }
    Terminate(GraphicsCaptureState::Failed,
              finalization.Error == GraphicsCaptureError::Busy ? GraphicsCaptureError::Busy
                                                               : GraphicsCaptureError::NativeCaptureFailed);
}
void BeginGraphicsCaptureFrame(bool isTargetEmpty, bool isTargetOffscreen) noexcept
{
    if (GetCapture().Record.Report.State != GraphicsCaptureState::Armed)
    {
        return;
    }
    if (GetCapture().Record.Deadline.IsExpired(time::Now()) == true)
    {
        Terminate(GraphicsCaptureState::Failed, GraphicsCaptureError::DeadlineExceeded);
        return;
    }
    if (isTargetEmpty == true)
    {
        return;
    }
    if (GetCapture().AdapterOrNull == nullptr)
    {
        Terminate(GraphicsCaptureState::Failed, GraphicsCaptureError::SessionEnded);
        return;
    }
    const GraphicsCaptureAdapterState state = GetCapture().AdapterOrNull->QueryState();
    const GraphicsCaptureError availability = GetAdapterAvailability(state);
    if (availability != GraphicsCaptureError::None)
    {
        Terminate(GraphicsCaptureState::Failed, availability);
        return;
    }
    if (isTargetOffscreen == true && state.CanCaptureOffscreen == false)
    {
        CopyText("this tool cannot capture offscreen-only frames", GetCapture().Record.Report.Diagnostic);
        Terminate(GraphicsCaptureState::Failed, GraphicsCaptureError::InvalidTarget);
        return;
    }
    const GraphicsCaptureStart start =
    {
        .Identity = GetCapture().Record.Report.Identity,
        .Destination = GetCapture().Record.Destination.GetData(),
        .ArtifactStem = GetCapture().Record.Stem.GetData(),
    };
    const GraphicsCaptureError started = GetCapture().AdapterOrNull->BeginFrameCapture(start);
    if (started == GraphicsCaptureError::None)
    {
        GetCapture().Record.Report.State = GraphicsCaptureState::Capturing;
        return;
    }
    Terminate(GraphicsCaptureState::Failed,
              started == GraphicsCaptureError::Busy ? GraphicsCaptureError::Busy
                                                    : GraphicsCaptureError::NativeCaptureFailed);
}
void FinishGraphicsCaptureBegin(bool isFrameOpen) noexcept
{
    if (isFrameOpen == true)
    {
        ++GetCapture().FrameSequence;
    }
    if (GetCapture().Record.Report.State == GraphicsCaptureState::Capturing)
    {
        if (isFrameOpen == true)
        {
            GetCapture().Record.Report.CapturedFrame = GetCapture().FrameSequence;
            return;
        }
        // A skipped acquisition must not consume the request: discard the empty capture.
        AbandonNativeCapture();
        GetCapture().Record.Report.State = GraphicsCaptureState::Armed;
        GetCapture().Record.Report.IsArtifactPreserved = false;
        GetCapture().Record.Report.Artifact = {};
    }
    if (GetCapture().Record.Report.State == GraphicsCaptureState::Armed && isFrameOpen == false)
    {
        ++GetCapture().Record.Report.SkippedFrames;
    }
}
void EndGraphicsCaptureFrame(bool isSubmitted) noexcept
{
    if (GetCapture().Record.Report.State != GraphicsCaptureState::Capturing)
    {
        return;
    }
    if (GetCapture().AdapterOrNull == nullptr)
    {
        Terminate(GraphicsCaptureState::Failed, GraphicsCaptureError::SessionEnded);
        return;
    }
    if (isSubmitted == false)
    {
        AbandonNativeCapture();
        Terminate(GraphicsCaptureState::Failed, GraphicsCaptureError::FrameFailed);
        return;
    }
    if (GetCapture().Record.IsCancelRequested == true)
    {
        AbandonNativeCapture();
        Terminate(GraphicsCaptureState::Cancelled, GraphicsCaptureError::None);
        return;
    }
    // Finalizing first: the adapter may report its outcome before EndFrameCapture returns.
    GetCapture().Record.Report.State = GraphicsCaptureState::Finalizing;
    const GraphicsCaptureIdentity identity = GetCapture().Record.Report.Identity;
    const GraphicsCaptureError stopped = GetCapture().AdapterOrNull->EndFrameCapture(identity);
    if (stopped != GraphicsCaptureError::None && IsCurrent(identity) == true &&
        GetCapture().Record.Report.State == GraphicsCaptureState::Finalizing)
    {
        GetCapture().Record.HasVerifiedArtifact = false;
        AbandonNativeCapture();
        Terminate(GraphicsCaptureState::Failed,
                  stopped == GraphicsCaptureError::Busy ? GraphicsCaptureError::Busy
                                                        : GraphicsCaptureError::NativeCaptureFailed);
    }
}
void EndGraphicsCaptureSession(GraphicsCaptureError reason) noexcept
{
    // A verified artifact no longer depends on native objects; its manifest is still
    // published at the next poll. Anything else is abandoned before the device goes away.
    if (IsActive(GetCapture().Record.Report.State) == true && GetCapture().Record.HasVerifiedArtifact == false)
    {
        if (GetCapture().Record.Report.State != GraphicsCaptureState::Armed)
        {
            AbandonNativeCapture();
        }
        Terminate(GraphicsCaptureState::Failed, reason);
    }
    GetCapture().AdapterOrNull = nullptr;
    GetCapture().FrameSequence = 0;
}
} // namespace internal

GraphicsCaptureError GetGraphicsCaptureCapabilities(DeviceHandle device,
                                                    GraphicsCaptureCapabilities& outCapabilities) noexcept
{
    const GraphicsCaptureError admission = ValidateDevice(device);
    if (admission != GraphicsCaptureError::None)
    {
        return admission;
    }
    AdvanceRecord();
    GraphicsCaptureCapabilities capabilities;
    if (GetCapture().AdapterOrNull != nullptr)
    {
        const internal::GraphicsCaptureAdapterState state = GetCapture().AdapterOrNull->QueryState();
        capabilities.Tool = GetCapture().AdapterOrNull->GetTool();
        capabilities.HasAdapter = true;
        capabilities.IsToolAttached = state.IsToolAttached;
        capabilities.IsRestartRequired = state.IsRestartRequired;
        capabilities.IsExternalCaptureActive = state.IsExternalCaptureActive;
        capabilities.CanCaptureOffscreen = state.CanCaptureOffscreen;
        capabilities.Unavailable = GetAdapterAvailability(state);
        if (capabilities.Unavailable == GraphicsCaptureError::None && IsAnyCaptureActive() == true)
        {
            capabilities.Unavailable = GraphicsCaptureError::Busy;
        }
        capabilities.CanCaptureFrame = capabilities.Unavailable == GraphicsCaptureError::None;
    }
    outCapabilities = capabilities;
    return GraphicsCaptureError::None;
}

GraphicsCaptureError RequestGraphicsCapture(DeviceHandle device,
                                            const GraphicsCaptureRequest& request,
                                            GraphicsCaptureIdentity& outIdentity) noexcept
{
    const GraphicsCaptureError admission = ValidateDevice(device);
    if (admission != GraphicsCaptureError::None)
    {
        return admission;
    }
    if (internal::FrameOpen() == true)
    {
        return GraphicsCaptureError::InvalidState;
    }
    if (IsValidDestination(request.Destination) == false || request.DeadlineMilliseconds == 0 ||
        request.DeadlineMilliseconds > MAX_GRAPHICS_CAPTURE_DEADLINE_MILLISECONDS)
    {
        return GraphicsCaptureError::InvalidRequest;
    }
    AdvanceRecord();
    if (IsAnyCaptureActive() == true)
    {
        return GraphicsCaptureError::Busy;
    }
    if (GetCapture().AdapterOrNull == nullptr)
    {
        return GraphicsCaptureError::UnsupportedBackend;
    }
    const GraphicsCaptureError availability = GetAdapterAvailability(GetCapture().AdapterOrNull->QueryState());
    if (availability != GraphicsCaptureError::None)
    {
        return availability;
    }
    if (gNextRequest == 0)
    {
        return GraphicsCaptureError::InvalidState;
    }
    const GraphicsCaptureIdentity identity = { .Session = internal::DeviceOwner(), .Request = gNextRequest };
    const std::string_view destination{request.Destination.GetDataOrNull(), request.Destination.GetSize()};
    ArtifactStem stem;
    time::Deadline deadline;
    if (MakeArtifactStem(identity, stem) == false ||
        deadline.Arm(time::Now(), time::Milliseconds(request.DeadlineMilliseconds)) != time::TimeStatus::Ok)
    {
        return GraphicsCaptureError::InvalidRequest;
    }
    if (CanWriteDestination(destination, stem.GetView()) == false)
    {
        return GraphicsCaptureError::InvalidDestination;
    }
    ++gNextRequest;
    GetCapture().Record = {};
    if (GetCapture().Record.Destination.TryAssign(destination) != foundation::StringStatus::Ok)
    {
        return GraphicsCaptureError::InvalidRequest;
    }
    GetCapture().Record.Stem = stem;
    GetCapture().Record.Deadline = deadline;
    GetCapture().Record.Report.Identity = identity;
    GetCapture().Record.Report.State = GraphicsCaptureState::Armed;
    GetCapture().Record.Report.Tool = GetCapture().AdapterOrNull->GetTool();
    GetCapture().Record.Report.RequestedFrame = GetCapture().FrameSequence;
    GetCapture().Record.SelectedBackend = backend::Kind();
    GetCapture().Record.Gpu = backend::TimingAdapter();
    GetCapture().Record.Gpu.Name[sizeof(GetCapture().Record.Gpu.Name) - 1] = '\0';
    outIdentity = identity;
    return GraphicsCaptureError::None;
}

GraphicsCaptureError CancelGraphicsCapture(GraphicsCaptureIdentity identity) noexcept
{
    if (IsCurrent(identity) == false)
    {
        return GraphicsCaptureError::StaleIdentity;
    }
    switch (GetCapture().Record.Report.State)
    {
        case GraphicsCaptureState::Armed:
            Terminate(GraphicsCaptureState::Cancelled, GraphicsCaptureError::None);
            return GraphicsCaptureError::None;
        case GraphicsCaptureState::Capturing:
            // The frame's commands are already inside the native interval; stop after submission.
            GetCapture().Record.IsCancelRequested = true;
            return GraphicsCaptureError::None;
        case GraphicsCaptureState::Finalizing:
            if (GetCapture().Record.HasVerifiedArtifact == true)
            {
                GetCapture().Record.Report.IsArtifactPreserved = true;
            }
            else
            {
                AbandonNativeCapture();
            }
            Terminate(GraphicsCaptureState::Cancelled, GraphicsCaptureError::None);
            return GraphicsCaptureError::None;
        case GraphicsCaptureState::Idle:
        case GraphicsCaptureState::Complete:
        case GraphicsCaptureState::Failed:
        case GraphicsCaptureState::Cancelled:
            return GraphicsCaptureError::InvalidState;
    }
    return GraphicsCaptureError::InvalidState;
}

GraphicsCaptureError GetGraphicsCaptureReport(GraphicsCaptureIdentity identity,
                                              GraphicsCaptureReport& outReport) noexcept
{
    if (IsCurrent(identity) == false)
    {
        return GraphicsCaptureError::StaleIdentity;
    }
    AdvanceRecord();
    outReport = GetCapture().Record.Report;
    return GraphicsCaptureError::None;
}
} // namespace ludus::graphics::rhi
