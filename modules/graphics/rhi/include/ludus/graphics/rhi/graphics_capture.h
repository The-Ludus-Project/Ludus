#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/device.h>

// Graphics-debugger capture requests (G1). The request/state contract is owned by
// docs/architecture/graphics-debugging.md, "Capture request and lifetime contract".

namespace ludus::graphics::rhi
{
/// Bytes reserved for an artifact or manifest file name, including its terminating zero.
inline constexpr foundation::usize GRAPHICS_CAPTURE_NAME_BYTES = 128;
/// Bytes reserved for a native diagnostic or tool version, including its terminating zero.
/// Longer adapter text is shortened at a UTF-8 boundary.
inline constexpr foundation::usize GRAPHICS_CAPTURE_DIAGNOSTIC_BYTES = 256;
/// Longest admitted request deadline, in milliseconds (ten minutes).
inline constexpr foundation::uint64 MAX_GRAPHICS_CAPTURE_DEADLINE_MILLISECONDS = 600'000;
/// Default request deadline, in milliseconds: room for a hidden target to reappear and a
/// native trace to finish writing.
inline constexpr foundation::uint64 DEFAULT_GRAPHICS_CAPTURE_DEADLINE_MILLISECONDS = 10'000;

/// External graphics debugger behind a capture adapter.
enum class GraphicsCaptureTool : foundation::uint8
{
    /// No capture adapter is registered for this session.
    None,
    /// Apple's Metal capture manager, writing a `.gputrace` document (G2). The process needs
    /// `MTL_CAPTURE_ENABLED=1` (or the app's `MetalCaptureEnabled` property) before Metal starts.
    Metal,
    /// RenderDoc's in-application API on Vulkan (planned G3 adapter).
    RenderDoc,
};

/// Lifecycle of one capture request. Complete, Failed and Cancelled are terminal.
enum class GraphicsCaptureState : foundation::uint8
{
    /// No request owns the capture adapter.
    Idle,
    /// Admitted and waiting for an eligible frame; skipped frames keep it armed.
    Armed,
    /// Native capture started for the frame being recorded.
    Capturing,
    /// Native stop issued after submission; waiting for the adapter to verify its artifact.
    Finalizing,
    /// The artifact was verified and its manifest published. Opening a viewer is separate.
    Complete,
    /// Terminal failure; Error names the reason and any preserved artifact is identified.
    Failed,
    /// Cancelled by the caller; any preserved partial artifact is identified.
    Cancelled,
};

/// Reason a capture call was rejected or a request failed. None means accepted.
enum class GraphicsCaptureError : foundation::uint8
{
    /// Accepted, or no failure.
    None,
    /// Null, destroyed or foreign device handle, or a device that is not Ready.
    InvalidTarget,
    /// Missing destination or a deadline outside [1, MAX_GRAPHICS_CAPTURE_DEADLINE_MILLISECONDS].
    InvalidRequest,
    /// Requested inside an open frame, or cancelling a request that is already terminal.
    InvalidState,
    /// No capture adapter exists for the selected backend in this build.
    UnsupportedBackend,
    /// The adapter exists but its tool runtime is not attached to this process.
    ToolUnavailable,
    /// The tool needs startup configuration before device creation; restart the target.
    RestartRequired,
    /// Another request, or an external tool session, owns the capture facility.
    Busy,
    /// The destination is not an existing writable directory, or a capture there would
    /// overwrite an existing file.
    InvalidDestination,
    /// No eligible frame was captured, or finalization did not finish, before the deadline.
    DeadlineExceeded,
    /// The device was lost before the capture finished.
    DeviceLost,
    /// The device session failed or was destroyed before the capture finished.
    SessionEnded,
    /// The captured frame failed before or during submission.
    FrameFailed,
    /// The native capture tool reported a failure; see the diagnostic.
    NativeCaptureFailed,
    /// The artifact was verified but its manifest could not be published. The artifact
    /// is preserved and identified; no manifest is claimed.
    ManifestFailed,
    /// The identity does not name the current or most recent request.
    StaleIdentity,
};

/// Capture facilities of a live device session, distinct from installed tooling.
struct GraphicsCaptureCapabilities final
{
    /// Tool behind the session's adapter; None without an adapter.
    GraphicsCaptureTool Tool = GraphicsCaptureTool::None;
    /// True when a native adapter is registered for the selected backend.
    bool HasAdapter = false;
    /// True when the adapter's tool runtime is attached to this process.
    bool IsToolAttached = false;
    /// True when the tool must be enabled before device creation. For Metal, relaunch the
    /// process with `MTL_CAPTURE_ENABLED=1` in its environment.
    bool IsRestartRequired = false;
    /// True when an external user or tool session is capturing; Ludus never stops it.
    bool IsExternalCaptureActive = false;
    /// True when the adapter can capture offscreen-only frames.
    bool CanCaptureOffscreen = false;
    /// True when RequestGraphicsCapture would currently admit a frame capture.
    bool CanCaptureFrame = false;
    /// First reason CanCaptureFrame is false; None when it is true.
    GraphicsCaptureError Unavailable = GraphicsCaptureError::UnsupportedBackend;
};

/// Copied request for the next eligible frame of a device session.
struct GraphicsCaptureRequest final
{
    /// Native path of an existing, trusted directory, borrowed only during the call.
    /// Use a fresh request-owned directory under the project's ignored out/ tree; nothing
    /// that already exists there is overwritten.
    foundation::BorrowedView<const char> Destination;
    /// Real-time limit, from admission, for capturing an eligible frame and finalizing it.
    foundation::uint64 DeadlineMilliseconds = DEFAULT_GRAPHICS_CAPTURE_DEADLINE_MILLISECONDS;
};

/// Identity of one request; never reused within a process.
struct GraphicsCaptureIdentity final
{
    /// Device-session generation that admitted the request; zero is never issued.
    foundation::uint64 Session = 0;
    /// Request sequence within the process; zero is never issued.
    foundation::uint64 Request = 0;
    /// Compare both identity components.
    [[nodiscard]] LUDUS_INLINE bool operator==(const GraphicsCaptureIdentity&) const noexcept = default;
};

/// Copied snapshot of one request. Text fields are terminated UTF-8 and empty when absent.
struct GraphicsCaptureReport final
{
    /// Request this snapshot describes.
    GraphicsCaptureIdentity Identity;
    /// Current lifecycle state.
    GraphicsCaptureState State = GraphicsCaptureState::Idle;
    /// Failure reason; None unless State is Failed.
    GraphicsCaptureError Error = GraphicsCaptureError::None;
    /// Tool that owns the native capture.
    GraphicsCaptureTool Tool = GraphicsCaptureTool::None;
    /// Session frame sequence of the last frame opened before admission; zero if none was.
    foundation::uint64 RequestedFrame = 0;
    /// Session frame sequence of the captured frame; zero until capture starts.
    /// Each opened frame takes the next sequence, starting at one.
    foundation::uint64 CapturedFrame = 0;
    /// Frame attempts that did not open a frame while the request was armed.
    foundation::uint32 SkippedFrames = 0;
    /// True when Artifact names a capture left in the destination, whether or not complete.
    bool IsArtifactPreserved = false;
    /// True only after the manifest was published; then State is Complete.
    bool IsManifestPublished = false;
    /// Artifact file or bundle name relative to the destination.
    foundation::StaticArray<char, GRAPHICS_CAPTURE_NAME_BYTES> Artifact{};
    /// Manifest file name relative to the destination, set when it is published.
    foundation::StaticArray<char, GRAPHICS_CAPTURE_NAME_BYTES> Manifest{};
    /// Tool runtime identity reported by the adapter.
    foundation::StaticArray<char, GRAPHICS_CAPTURE_DIAGNOSTIC_BYTES> ToolVersion{};
    /// Bounded native detail of the failure, if any.
    foundation::StaticArray<char, GRAPHICS_CAPTURE_DIAGNOSTIC_BYTES> Diagnostic{};
    /// Shader source the artifact lets a viewer show, as reported by the adapter. For Metal,
    /// the generated MSL compiled at runtime; its `#line` directives, where the shader build
    /// kept them, name authored LUSL or Slang files and lines whose text is not embedded.
    foundation::StaticArray<char, GRAPHICS_CAPTURE_DIAGNOSTIC_BYTES> ShaderSource{};
};

/// Query capture facilities of a ready session without starting a capture or discovering
/// installed tools. Owner thread only, nonblocking.
/// @return None with outCapabilities written; InvalidTarget or DeviceLost preserve it.
[[nodiscard]] GraphicsCaptureError
GetGraphicsCaptureCapabilities(DeviceHandle device, GraphicsCaptureCapabilities& outCapabilities) noexcept;

/// Arm a capture of the device's next eligible frame: an acquired surface or offscreen
/// target that reaches submission. Owner thread only, between frames. One request owns
/// the adapter at a time; frames that are skipped (zero-size, hidden or unacquired) do
/// not consume it. Checks that the destination is a directory and that the request's
/// manifest name is free, and copies the destination; nothing is written yet.
/// @note On Metal the native stop is synchronous: EndFrame of the captured frame waits for
/// that frame's GPU work and the trace write, which takes seconds for a process's first trace.
/// @param device Ready device session whose frames are captured.
/// @param request Copied request; its destination storage is not retained.
/// @param outIdentity Receives the new identity on None; preserved otherwise.
/// @return None when Armed. InvalidTarget, InvalidRequest, InvalidState, DeviceLost,
/// UnsupportedBackend, ToolUnavailable, RestartRequired, Busy or InvalidDestination reject
/// the request without changing an existing request or rendering.
[[nodiscard]] GraphicsCaptureError RequestGraphicsCapture(DeviceHandle device,
                                                          const GraphicsCaptureRequest& request,
                                                          GraphicsCaptureIdentity& outIdentity) noexcept;

/// Cancel an active request. Armed requests end at once. A capture in progress is
/// abandoned after its frame is submitted, and a finalizing capture through the adapter's
/// synchronous cleanup; either may leave an identified partial artifact. No manifest is
/// published for a cancelled request. Owner thread only.
/// @return None when the request is or will become Cancelled; StaleIdentity for an unknown
/// identity; InvalidState when the request is already terminal.
[[nodiscard]] GraphicsCaptureError CancelGraphicsCapture(GraphicsCaptureIdentity identity) noexcept;

/// Copy the current or most recent request, advancing it first: an expired deadline fails
/// it, a finalizing capture is polled, and a verified artifact gets its manifest published.
/// Publication does file I/O, so call between frames, outside frame recording; inside an
/// open frame this only copies. Terminal reports stay readable after the device is destroyed,
/// until the next request. Owner thread only, never waits for the GPU or the tool.
/// @return None with outReport written; StaleIdentity preserves outReport.
[[nodiscard]] GraphicsCaptureError GetGraphicsCaptureReport(GraphicsCaptureIdentity identity,
                                                            GraphicsCaptureReport& outReport) noexcept;
} // namespace ludus::graphics::rhi
