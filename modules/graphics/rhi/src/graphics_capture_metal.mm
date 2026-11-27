#include "internal/graphics_capture_metal.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graphics_capture.h>

#include "internal/graphics_capture.h"
#include "internal/owner.h"

#include <string_view>

// Apple's SDK 26 Metal umbrella references INFINITY without including its defining header;
// keep the compatibility macro inside the SDK import boundary, as rhi_metal.mm does.
#if !defined(INFINITY)
#    define INFINITY (__builtin_inff())
#    define LUDUS_METAL_LOCAL_INFINITY 1
#endif
#import <Foundation/Foundation.h>
#import <Metal/MTLCaptureManager.h>
#import <Metal/MTLDevice.h>
#if defined(LUDUS_METAL_LOCAL_INFINITY)
#    undef INFINITY
#    undef LUDUS_METAL_LOCAL_INFINITY
#endif

// G2 Metal capture adapter. Thanks to Apple, "Capturing a Metal workload programmatically"
// and "MTLCaptureManager", Metal documentation, for the descriptor, GPU trace destination and
// enablement rules:
// https://developer.apple.com/documentation/xcode/capturing-a-metal-workload-programmatically
// https://developer.apple.com/documentation/metal/mtlcapturemanager
// The capture covers the device, so it includes every queue this process uses on it during
// the frame. Apple documents no completion query for a GPU trace document; stopCapture is
// synchronous and returns after the trace is written, so the adapter verifies the stopped
// manager and a nonempty .gputrace bundle. Opening it in Xcode is the separate acceptance gate.
// Contract: docs/architecture/graphics-debugging.md, "Metal on macOS".

namespace ludus::graphics::rhi::backend
{
namespace
{
using foundation::usize;

constexpr foundation::logging::LogCategory LOG_CAPTURE{"RHI"};
constexpr std::string_view ARTIFACT_EXTENSION = ".gputrace";
/// What a Ludus Metal trace can show as shader source: every RHI library is compiled at
/// runtime from generated MSL, which the trace records. Slang keeps `#line` directives in that
/// MSL unless the build disabled them; they name the authored file and line, but the trace
/// does not embed that file's text.
constexpr const char* SHADER_SOURCE =
    "generated MSL compiled at runtime; any #line directives name the authored LUSL/Slang file and line, "
    "whose text is not embedded";

using ArtifactName = foundation::StaticString<GRAPHICS_CAPTURE_NAME_BYTES>;

[[nodiscard]] MTLCaptureManager* GetCaptureManager() noexcept
{
    return [MTLCaptureManager sharedCaptureManager];
}
[[nodiscard]] NSString* MakeText(const char* text) noexcept
{
    return [NSString stringWithUTF8String:text];
}
/// "Metal <framework version>; <OS version>", autoreleased.
[[nodiscard]] NSString* MakeToolVersion() noexcept
{
    NSBundle* metal = [NSBundle bundleForClass:[MTLCaptureManager class]];
    NSString* versionOrNull = [metal objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    return [NSString stringWithFormat:@"Metal %@; macOS %@",
                                      versionOrNull != nil ? versionOrNull : @"unknown",
                                      [NSProcessInfo processInfo].operatingSystemVersionString];
}
/// A stopped capture produced a .gputrace bundle directory with content.
[[nodiscard]] bool IsTraceWritten(NSString* path) noexcept
{
    BOOL isDirectory = NO;
    if ([[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDirectory] == NO || isDirectory == NO)
    {
        return false;
    }
    NSArray<NSString*>* entriesOrNull = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:path error:nil];
    return entriesOrNull != nil && entriesOrNull.count > 0;
}
void LogCaptureError(std::string_view operation, NSError* errorOrNull) noexcept
{
    const char* messageOrNull = errorOrNull != nil ? errorOrNull.localizedDescription.UTF8String : nullptr;
    LUDUS_LOG_WARN(LOG_CAPTURE,
                   "Metal capture: {} failed: {} [{}]",
                   operation,
                   messageOrNull != nullptr ? messageOrNull : "no error reported",
                   static_cast<foundation::int64>(errorOrNull != nil ? errorOrNull.code : 0));
}

/// Capture control of one owner slot's Metal device. Every call runs on the RHI owner
/// thread, which for Metal is the main thread.
class MetalCaptureAdapter final : public internal::GraphicsCaptureAdapter
{
public:
    void Register(id<MTLDevice> device) noexcept
    {
        mDeviceOrNull = device;
        internal::SetGraphicsCaptureAdapter(this);
    }
    void Release() noexcept
    {
        @autoreleasepool
        {
            // The facade abandons an active capture first; never leave one this adapter started.
            if (mIsCapturing == true)
            {
                internal::GraphicsCaptureCleanup cleanup;
                Abandon(mActive, cleanup);
            }
            mDeviceOrNull = nil;
            mActive = {};
            mArtifactPathOrNull = nil;
        }
    }
    [[nodiscard]] GraphicsCaptureTool GetTool() const noexcept override
    {
        return GraphicsCaptureTool::Metal;
    }
    [[nodiscard]] internal::GraphicsCaptureAdapterState QueryState() noexcept override
    {
        @autoreleasepool
        {
            MTLCaptureManager* manager = GetCaptureManager();
            // Without MTL_CAPTURE_ENABLED (or MetalCaptureEnabled) at launch the capture
            // layer is not inserted, and only a relaunch can insert it.
            const bool isEnabled = [manager supportsDestination:MTLCaptureDestinationGPUTraceDocument] == YES;
            const internal::GraphicsCaptureAdapterState state
            {
                .IsToolAttached = isEnabled,
                .IsRestartRequired = isEnabled == false,
                .IsExternalCaptureActive = manager.isCapturing == YES && mIsCapturing == false,
                .CanCaptureOffscreen = true,
            };
            return state;
        }
    }
    [[nodiscard]] GraphicsCaptureError BeginFrameCapture(const internal::GraphicsCaptureStart& start) noexcept override
    {
        @autoreleasepool
        {
            MTLCaptureManager* manager = GetCaptureManager();
            if (manager.isCapturing == YES)
            {
                return GraphicsCaptureError::Busy;
            }
            ArtifactName artifact;
            NSString* destinationOrNull = MakeText(start.Destination);
            if (mDeviceOrNull == nil || mIsCapturing == true || destinationOrNull == nil ||
                artifact.TryAssign(start.ArtifactStem) != foundation::StringStatus::Ok ||
                artifact.TryAppend(ARTIFACT_EXTENSION) != foundation::StringStatus::Ok)
            {
                return GraphicsCaptureError::NativeCaptureFailed;
            }
            NSString* path = [destinationOrNull stringByAppendingPathComponent:MakeText(artifact.GetData())];
            // Never write over an existing entry; Metal itself also refuses to.
            if ([[NSFileManager defaultManager] fileExistsAtPath:path] == YES)
            {
                LUDUS_LOG_WARN(LOG_CAPTURE, "Metal capture: {} already exists", artifact.GetView());
                return GraphicsCaptureError::NativeCaptureFailed;
            }
            MTLCaptureDescriptor* descriptor = [MTLCaptureDescriptor new];
            descriptor.captureObject = mDeviceOrNull;
            descriptor.destination = MTLCaptureDestinationGPUTraceDocument;
            descriptor.outputURL = [NSURL fileURLWithPath:path isDirectory:YES];
            NSError* errorOrNull = nil;
            if ([manager startCaptureWithDescriptor:descriptor error:&errorOrNull] == NO)
            {
                LogCaptureError("start", errorOrNull);
                return errorOrNull != nil && errorOrNull.code == MTLCaptureErrorAlreadyCapturing
                           ? GraphicsCaptureError::Busy
                           : GraphicsCaptureError::NativeCaptureFailed;
            }
            mActive = start.Identity;
            mArtifact = artifact;
            mArtifactPathOrNull = path;
            mIsCapturing = true;
            return GraphicsCaptureError::None;
        }
    }
    [[nodiscard]] GraphicsCaptureError EndFrameCapture(GraphicsCaptureIdentity identity) noexcept override
    {
        @autoreleasepool
        {
            if (identity != mActive || mIsCapturing == false)
            {
                return GraphicsCaptureError::NativeCaptureFailed;
            }
            // Synchronous: returns once the captured command buffers finished and the trace is
            // written. The first capture in a process takes seconds; later ones are faster.
            [GetCaptureManager() stopCapture];
            mIsCapturing = false;
            const bool isStopped = GetCaptureManager().isCapturing == NO;
            const bool isWritten = IsTraceWritten(mArtifactPathOrNull);
            const bool isVerified = isStopped == true && isWritten == true;
            const char* diagnosticOrNull = nullptr;
            if (isStopped == false)
            {
                diagnosticOrNull = "Metal still reports a capture after stopCapture";
            }
            else if (isWritten == false)
            {
                diagnosticOrNull = "Metal stopped the capture without writing a nonempty .gputrace bundle";
            }
            const internal::GraphicsCaptureFinalization finalization
            {
                .Error = isVerified == true ? GraphicsCaptureError::None : GraphicsCaptureError::NativeCaptureFailed,
                .ArtifactOrNull = mArtifact.GetData(),
                .IsArtifactVerified = isVerified,
                .IsArtifactPreserved = [[NSFileManager defaultManager] fileExistsAtPath:mArtifactPathOrNull] == YES,
                .ToolVersionOrNull = MakeToolVersion().UTF8String,
                .DiagnosticOrNull = diagnosticOrNull,
                .ShaderSourceOrNull = SHADER_SOURCE,
            };
            internal::ReportGraphicsCaptureFinalization(identity, finalization);
            return GraphicsCaptureError::None;
        }
    }
    void PollFinalization(GraphicsCaptureIdentity) noexcept override
    {
        // EndFrameCapture reports synchronously; stopCapture leaves nothing to poll.
    }
    void Abandon(GraphicsCaptureIdentity identity, internal::GraphicsCaptureCleanup& outCleanup) noexcept override
    {
        @autoreleasepool
        {
            if (identity != mActive || mArtifactPathOrNull == nil)
            {
                return;
            }
            NSFileManager* files = [NSFileManager defaultManager];
            if (mIsCapturing == true)
            {
                // An unfinished capture, such as one whose frame was skipped, is not a usable
                // trace. The adapter created this request-owned bundle at a name that was free,
                // so removing it overwrites nothing and lets a re-armed request reuse the name.
                [GetCaptureManager() stopCapture];
                mIsCapturing = false;
                NSError* errorOrNull = nil;
                if ([files fileExistsAtPath:mArtifactPathOrNull] == YES && [files removeItemAtPath:mArtifactPathOrNull
                                                                                             error:&errorOrNull] == NO)
                {
                    LogCaptureError("discarding an unfinished trace", errorOrNull);
                }
            }
            if ([files fileExistsAtPath:mArtifactPathOrNull] == YES)
            {
                outCleanup.IsArtifactPreserved = true;
                const std::string_view name = mArtifact.GetView();
                for (usize i = 0; i < outCleanup.Artifact.GetSize(); ++i)
                {
                    outCleanup.Artifact[i] = i < name.size() ? name[i] : '\0';
                }
            }
            mActive = {};
            mArtifactPathOrNull = nil;
        }
    }

private:
    id<MTLDevice> mDeviceOrNull = nil;
    NSString* mArtifactPathOrNull = nil;
    GraphicsCaptureIdentity mActive;
    ArtifactName mArtifact;
    /// This adapter started the native capture and has not stopped it.
    bool mIsCapturing = false;
};

foundation::StaticArray<MetalCaptureAdapter, MAX_DEVICE_OWNERS> gCaptureAdapters{};
} // namespace

void RegisterMetalCaptureAdapter(id<MTLDevice> device) noexcept
{
    gCaptureAdapters[internal::GetBoundOwnerSlot()].Register(device);
}
void ReleaseMetalCaptureAdapter() noexcept
{
    gCaptureAdapters[internal::GetBoundOwnerSlot()].Release();
}
} // namespace ludus::graphics::rhi::backend
