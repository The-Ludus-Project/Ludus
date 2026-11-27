#pragma once

// Private capture adapter hooks of the G1 capture owner. Native adapters (Metal in G2,
// RenderDoc in G3) implement GraphicsCaptureAdapter and register it for their session;
// the facade calls the frame hooks at its real acquisition and submission boundaries.
// Contract: docs/architecture/graphics-debugging.md, "Capture request and lifetime contract".

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/graphics_capture.h>

namespace ludus::graphics::rhi::internal
{
/// Session-specific facilities an adapter reports; queried at admission and per frame.
struct GraphicsCaptureAdapterState final
{
    /// The tool runtime is attached to this process and device.
    bool IsToolAttached = false;
    /// The tool needs startup configuration that this session did not receive.
    bool IsRestartRequired = false;
    /// An external user or tool session is capturing; it must never be stopped.
    bool IsExternalCaptureActive = false;
    /// Offscreen-only frames can be captured.
    bool CanCaptureOffscreen = false;
};

/// Native start parameters; the pointed-to text lives until the request is terminal.
struct GraphicsCaptureStart final
{
    /// Request that owns this native capture.
    GraphicsCaptureIdentity Identity;
    /// Terminated native destination directory, checked at admission.
    const char* Destination = "";
    /// Owner-reserved artifact stem. The adapter appends its own extension and must not
    /// overwrite an existing entry with that name.
    const char* ArtifactStem = "";
};

/// Outcome an adapter reports once per request after EndFrameCapture succeeded.
struct GraphicsCaptureFinalization final
{
    /// None on success; NativeCaptureFailed or Busy otherwise.
    GraphicsCaptureError Error = GraphicsCaptureError::None;
    /// Artifact name relative to the destination, starting with "<ArtifactStem>."; null if none.
    const char* ArtifactOrNull = nullptr;
    /// The adapter's completion condition and file checks passed. Required on success.
    bool IsArtifactVerified = false;
    /// On failure, an identified partial artifact remains in the destination.
    bool IsArtifactPreserved = false;
    /// Tool runtime identity text; null if unknown.
    const char* ToolVersionOrNull = nullptr;
    /// Native failure detail; null if none.
    const char* DiagnosticOrNull = nullptr;
    /// What shader source the artifact lets a viewer show, for the manifest; null if unknown.
    const char* ShaderSourceOrNull = nullptr;
};

/// What Abandon left behind.
struct GraphicsCaptureCleanup final
{
    /// A partial artifact remains in the destination under Artifact.
    bool IsArtifactPreserved = false;
    /// Terminated artifact name relative to the destination; empty when none remains.
    foundation::StaticArray<char, GRAPHICS_CAPTURE_NAME_BYTES> Artifact{};
};

/// Native capture control for one device session. Every call happens on the RHI owner
/// thread. No call may wait for the GPU in the frame hooks, discover installed tools or
/// stop a capture this adapter did not start.
class GraphicsCaptureAdapter
{
public:
    /// Adapters are owned by their backend session; the capture owner only borrows them.
    virtual ~GraphicsCaptureAdapter() noexcept = default;
    /// Tool behind this adapter.
    [[nodiscard]] virtual GraphicsCaptureTool GetTool() const noexcept = 0;
    /// Current facilities of the live session; nonblocking.
    [[nodiscard]] virtual GraphicsCaptureAdapterState QueryState() noexcept = 0;
    /// Start a native capture before the frame's command buffers are created.
    /// @return None when started; Busy when another capture owns the tool; any other value
    /// is a native failure. A failed start leaves nothing to clean up.
    [[nodiscard]] virtual GraphicsCaptureError BeginFrameCapture(const GraphicsCaptureStart& start) noexcept = 0;
    /// Stop the native capture after the frame's submission. On None the adapter later reports
    /// through ReportGraphicsCaptureFinalization, possibly before returning; any other value
    /// is followed by Abandon.
    [[nodiscard]] virtual GraphicsCaptureError EndFrameCapture(GraphicsCaptureIdentity identity) noexcept = 0;
    /// Owner-thread opportunity to report a finalizing capture's outcome; nonblocking.
    virtual void PollFinalization(GraphicsCaptureIdentity identity) noexcept = 0;
    /// Stop or discard this request's native capture with the tool's documented cleanup and
    /// release everything it borrows, synchronously. Used when the frame was not opened (the
    /// request then stays armed), on failure, cancellation, deadline expiry and session end.
    /// After return no report for this identity is accepted.
    virtual void Abandon(GraphicsCaptureIdentity identity, GraphicsCaptureCleanup& outCleanup) noexcept = 0;
};

/// Register the adapter of the current session, or null. The adapter must outlive the
/// session or a later registration; session end clears the registration. Owner thread only.
void SetGraphicsCaptureAdapter(GraphicsCaptureAdapter* adapterOrNull) noexcept;
/// Accept an adapter's finalization for a finalizing request. A stale identity, or a request
/// that is no longer finalizing or has already reported, is ignored. Owner thread only.
void ReportGraphicsCaptureFinalization(GraphicsCaptureIdentity identity,
                                       const GraphicsCaptureFinalization& finalization) noexcept;
/// Facade hook before backend acquisition. Starts an armed capture unless the deadline has
/// passed or the target is zero-size; an offscreen target the tool cannot capture fails it.
void BeginGraphicsCaptureFrame(bool isTargetEmpty, bool isTargetOffscreen) noexcept;
/// Facade hook after backend acquisition. An unopened frame abandons a started capture and
/// keeps the request armed.
void FinishGraphicsCaptureBegin(bool isFrameOpen) noexcept;
/// Facade hook after backend submission; isSubmitted is false when the frame failed.
void EndGraphicsCaptureFrame(bool isSubmitted) noexcept;
/// Facade hook before native teardown on loss, failure or shutdown. Abandons an active
/// capture with reason (DeviceLost or SessionEnded) and clears the adapter registration.
void EndGraphicsCaptureSession(GraphicsCaptureError reason) noexcept;
} // namespace ludus::graphics::rhi::internal
