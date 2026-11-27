// G1 capture owner, driven through the real device frame and session path with a
// controlled adapter for deterministic native outcomes. Contract:
// docs/architecture/graphics-debugging.md, "Capture request and lifetime contract".

#include "internal/graphics_capture.h"
#include "internal/lifecycle.h"
#include "reference_raster.h"

#include <ludus/foundation/strings/static_string.hpp>
#include <ludus/foundation/strings/string.hpp>
#include <ludus/foundation/time/time.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/graphics_capture.h>

#include "test_files.hpp"

#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace ludus::graphics::rhi::backend
{
extern foundation::uint32 PendingToken;
extern foundation::uint32 ShutdownCount;
extern StartupError ImmediateError;
extern FrameStatus NextFrame;
extern FrameStatus NextFrameEnd;
extern Backend ActiveKind;
} // namespace ludus::graphics::rhi::backend

using namespace ludus::foundation;
using namespace ludus::graphics::rhi;

namespace
{
/// Long enough that no test reaches it unintentionally.
constexpr uint64 LONG_DEADLINE_MILLISECONDS = 60'000;
constexpr uint64 SHORT_DEADLINE_MILLISECONDS = 1;
/// Leaves time to record one frame before the deadline, even on a loaded machine.
constexpr uint64 FRAME_DEADLINE_MILLISECONDS = 500;
constexpr std::string_view ARTIFACT_EXTENSION = ".testcapture";
constexpr std::string_view ARTIFACT_BYTES = "controlled capture";
/// Room for a scratch destination path plus an artifact name.
constexpr usize ADAPTER_PATH_BYTES = 4096;

/// How the controlled adapter answers EndFrameCapture.
enum class Finalization : uint8
{
    /// Report a verified artifact before EndFrameCapture returns.
    Synchronous,
    /// Report a verified artifact from the next PollFinalization.
    Polled,
    /// Never report, so the request stays finalizing.
    Never,
};

/// Native capture tool stand-in that writes real artifacts into the destination.
struct ControlledAdapter final : internal::GraphicsCaptureAdapter
{
    internal::GraphicsCaptureAdapterState State
    {
        .IsToolAttached = true,
        .IsRestartRequired = false,
        .IsExternalCaptureActive = false,
        .CanCaptureOffscreen = true,
    };
    GraphicsCaptureError BeginResult = GraphicsCaptureError::None;
    GraphicsCaptureError EndResult = GraphicsCaptureError::None;
    Finalization Mode = Finalization::Synchronous;
    /// Create a directory where the manifest belongs, so its publication fails.
    bool ShouldBlockManifest = false;
    /// Report an artifact name outside the reserved stem.
    bool ShouldReportForeignArtifact = false;
    usize Begins = 0;
    usize Ends = 0;
    usize Polls = 0;
    usize Abandons = 0;
    /// Frame state and backend shutdowns observed by the native calls.
    bool WasFrameOpenAtBegin = true;
    bool WasFrameOpenAtEnd = true;
    uint32 ShutdownsAtAbandon = 0;
    /// Set when a file operation inside a noexcept native call failed; checked by the tests.
    bool HasFileFailure = false;
    GraphicsCaptureIdentity Started;
    StaticString<ADAPTER_PATH_BYTES> Destination;
    StaticString<GRAPHICS_CAPTURE_NAME_BYTES> Stem;

    [[nodiscard]] GraphicsCaptureTool GetTool() const noexcept override
    {
        return GraphicsCaptureTool::Metal;
    }
    [[nodiscard]] internal::GraphicsCaptureAdapterState QueryState() noexcept override
    {
        return State;
    }
    [[nodiscard]] GraphicsCaptureError BeginFrameCapture(const internal::GraphicsCaptureStart& start) noexcept override
    {
        ++Begins;
        WasFrameOpenAtBegin = internal::FrameOpen();
        if (BeginResult == GraphicsCaptureError::None)
        {
            Started = start.Identity;
            HasFileFailure = HasFileFailure || Destination.TryAssign(start.Destination) != StringStatus::Ok ||
                             Stem.TryAssign(start.ArtifactStem) != StringStatus::Ok;
        }
        return BeginResult;
    }
    [[nodiscard]] GraphicsCaptureError EndFrameCapture(GraphicsCaptureIdentity identity) noexcept override
    {
        ++Ends;
        WasFrameOpenAtEnd = internal::FrameOpen();
        if (EndResult != GraphicsCaptureError::None)
        {
            return EndResult;
        }
        writeArtifact();
        if (Mode == Finalization::Synchronous)
        {
            Report(identity);
        }
        return GraphicsCaptureError::None;
    }
    void PollFinalization(GraphicsCaptureIdentity identity) noexcept override
    {
        ++Polls;
        if (Mode == Finalization::Polled)
        {
            Report(identity);
        }
    }
    void Abandon(GraphicsCaptureIdentity, internal::GraphicsCaptureCleanup& outCleanup) noexcept override
    {
        ++Abandons;
        ShutdownsAtAbandon = backend::ShutdownCount;
        StaticString<GRAPHICS_CAPTURE_NAME_BYTES> artifact;
        StaticString<ADAPTER_PATH_BYTES> path;
        if (makeArtifactName(artifact) == true && makePath(artifact.GetView(), path) == true &&
            ludus::test_support::IsExistingPath(path.GetView()) == true)
        {
            outCleanup.IsArtifactPreserved = true;
            std::memcpy(outCleanup.Artifact.GetData(), artifact.GetData(), artifact.GetSize());
        }
    }
    /// Report a verified artifact for identity, as the tool's completion would.
    void Report(GraphicsCaptureIdentity identity) noexcept
    {
        StaticString<GRAPHICS_CAPTURE_NAME_BYTES> artifact;
        if (ShouldReportForeignArtifact == true)
        {
            HasFileFailure = HasFileFailure || artifact.TryAssign("other.testcapture") != StringStatus::Ok;
        }
        else
        {
            HasFileFailure = HasFileFailure || makeArtifactName(artifact) == false;
        }
        const internal::GraphicsCaptureFinalization finalization
        {
            .Error = GraphicsCaptureError::None,
            .ArtifactOrNull = artifact.GetData(),
            .IsArtifactVerified = true,
            .IsArtifactPreserved = false,
            .ToolVersionOrNull = "controlled 1.0",
            .DiagnosticOrNull = nullptr,
            .ShaderSourceOrNull = "controlled source",
        };
        internal::ReportGraphicsCaptureFinalization(identity, finalization);
    }
    [[nodiscard]] std::string GetStem() const
    {
        return std::string(Stem.GetView());
    }
    [[nodiscard]] std::string GetArtifactName() const
    {
        return GetStem() + std::string(ARTIFACT_EXTENSION);
    }

private:
    [[nodiscard]] bool makeArtifactName(StaticString<GRAPHICS_CAPTURE_NAME_BYTES>& outName) const noexcept
    {
        return outName.TryAssign(Stem.GetView()) == StringStatus::Ok &&
               outName.TryAppend(ARTIFACT_EXTENSION) == StringStatus::Ok;
    }
    [[nodiscard]] bool makePath(std::string_view name, StaticString<ADAPTER_PATH_BYTES>& outPath) const noexcept
    {
        return outPath.TryAssign(Destination.GetView()) == StringStatus::Ok &&
               outPath.TryAppend("/") == StringStatus::Ok && outPath.TryAppend(name) == StringStatus::Ok;
    }
    void writeArtifact() noexcept
    {
        StaticString<GRAPHICS_CAPTURE_NAME_BYTES> artifact;
        StaticString<ADAPTER_PATH_BYTES> path;
        const bool isWritten = makeArtifactName(artifact) == true && makePath(artifact.GetView(), path) == true &&
                               ludus::test_support::WriteFile(path.GetView(),
                                                              {reinterpret_cast<const uint8*>(ARTIFACT_BYTES.data()),
                                                               ARTIFACT_BYTES.size()}) == true;
        HasFileFailure = HasFileFailure || isWritten == false;
        if (ShouldBlockManifest == true)
        {
            StaticString<GRAPHICS_CAPTURE_NAME_BYTES> manifest;
            const bool isBlocked = manifest.TryAssign(Stem.GetView()) == StringStatus::Ok &&
                                   manifest.TryAppend(".manifest.json") == StringStatus::Ok &&
                                   makePath(manifest.GetView(), path) == true &&
                                   ludus::test_support::CreateDirectories(path.GetView()) == true;
            HasFileFailure = HasFileFailure || isBlocked == false;
        }
    }
};

/// A ready reference-backend device with a scratch destination and a registered adapter.
struct CaptureSession final
{
    DeviceHandle Device;
    SurfaceHandle Surface;
    ControlledAdapter Adapter;
    String Destination;
    bool IsAdapterRegistered = true;

    explicit CaptureSession(bool shouldRegisterAdapter = true)
    {
        Shutdown();
        reference::Reset();
        backend::ImmediateError = StartupError::None;
        backend::NextFrame = FrameStatus::Ready;
        backend::NextFrameEnd = FrameStatus::Ready;
        backend::ActiveKind = Backend::Metal;
        REQUIRE(ludus::test_support::CreateScratchDirectory("ludus-rhi-capture", Destination) == true);
        REQUIRE(CreateDevice({}, {}, {}, Device, Surface) == DeviceStatus::Pending);
        internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
        IsAdapterRegistered = shouldRegisterAdapter;
        if (shouldRegisterAdapter == true)
        {
            internal::SetGraphicsCaptureAdapter(&Adapter);
        }
    }
    ~CaptureSession() noexcept
    {
        Shutdown();
        backend::NextFrameEnd = FrameStatus::Ready;
        static_cast<void>(ludus::test_support::RemoveTree(Destination.GetView()));
    }
    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    [[nodiscard]] GraphicsCaptureRequest MakeRequest(uint64 deadline = LONG_DEADLINE_MILLISECONDS) const noexcept
    {
        return
        {
            .Destination = {Destination.GetView().data(), Destination.GetView().size()},
            .DeadlineMilliseconds = deadline,
        };
    }
    [[nodiscard]] GraphicsCaptureIdentity Arm(uint64 deadline = LONG_DEADLINE_MILLISECONDS)
    {
        GraphicsCaptureIdentity identity;
        REQUIRE(RequestGraphicsCapture(Device, MakeRequest(deadline), identity) == GraphicsCaptureError::None);
        CHECK(Adapter.HasFileFailure == false);
        return identity;
    }
    void RenderFrame() const
    {
        REQUIRE(BeginFrame(Device, Surface) == DeviceStatus::Ready);
        REQUIRE(EndFrame(Device) == DeviceStatus::Ready);
        CHECK(Adapter.HasFileFailure == false);
    }
    [[nodiscard]] std::string GetPath(std::string_view name) const
    {
        return std::string(Destination.GetView()) + "/" + std::string(name);
    }
};

[[nodiscard]] GraphicsCaptureReport GetReport(GraphicsCaptureIdentity identity)
{
    GraphicsCaptureReport report;
    REQUIRE(GetGraphicsCaptureReport(identity, report) == GraphicsCaptureError::None);
    return report;
}
[[nodiscard]] std::string_view GetText(const char* text) noexcept
{
    return {text};
}
void WaitPastDeadline(uint64 deadlineMilliseconds = SHORT_DEADLINE_MILLISECONDS)
{
    const time::Timestamp start = time::Now();
    const uint64 waitNanoseconds = 2 * deadlineMilliseconds * time::NANOSECONDS_PER_MILLISECOND;
    while (time::Now().Nanoseconds - start.Nanoseconds < waitNanoseconds)
    {
    }
}
} // namespace

TEST_CASE("G1 capture admission reports tool, target and destination reasons", "[rhi][graphics-capture]")
{
    {
        CaptureSession unsupported(false);
        GraphicsCaptureCapabilities capabilities;
        REQUIRE(GetGraphicsCaptureCapabilities(unsupported.Device, capabilities) == GraphicsCaptureError::None);
        CHECK(capabilities.HasAdapter == false);
        CHECK(capabilities.CanCaptureFrame == false);
        CHECK(capabilities.Unavailable == GraphicsCaptureError::UnsupportedBackend);
        GraphicsCaptureIdentity identity;
        CHECK(RequestGraphicsCapture(unsupported.Device, unsupported.MakeRequest(), identity) ==
              GraphicsCaptureError::UnsupportedBackend);
        CHECK(identity.Request == 0);
    }
    CaptureSession session;
    GraphicsCaptureIdentity identity;
    CHECK(RequestGraphicsCapture(DeviceHandle{}, session.MakeRequest(), identity) ==
          GraphicsCaptureError::InvalidTarget);
    CHECK(RequestGraphicsCapture(session.Device, session.MakeRequest(0), identity) ==
          GraphicsCaptureError::InvalidRequest);
    CHECK(RequestGraphicsCapture(session.Device,
                                 session.MakeRequest(MAX_GRAPHICS_CAPTURE_DEADLINE_MILLISECONDS + 1),
                                 identity) == GraphicsCaptureError::InvalidRequest);
    CHECK(RequestGraphicsCapture(session.Device, {}, identity) == GraphicsCaptureError::InvalidRequest);
    const std::string missing = session.GetPath("missing");
    CHECK(RequestGraphicsCapture(session.Device,
                                 { .Destination = {missing.data(), missing.size()}, .DeadlineMilliseconds = 1 },
                                 identity) == GraphicsCaptureError::InvalidDestination);
    const std::string file = session.GetPath("file");
    REQUIRE(ludus::test_support::WriteFile(file, {}) == true);
    CHECK(RequestGraphicsCapture(session.Device,
                                 { .Destination = {file.data(), file.size()}, .DeadlineMilliseconds = 1 },
                                 identity) == GraphicsCaptureError::InvalidDestination);

    session.Adapter.State.IsRestartRequired = true;
    CHECK(RequestGraphicsCapture(session.Device, session.MakeRequest(), identity) ==
          GraphicsCaptureError::RestartRequired);
    session.Adapter.State.IsRestartRequired = false;
    session.Adapter.State.IsToolAttached = false;
    CHECK(RequestGraphicsCapture(session.Device, session.MakeRequest(), identity) ==
          GraphicsCaptureError::ToolUnavailable);
    session.Adapter.State.IsToolAttached = true;
    // An external tool session owns the capture facility; Ludus must not stop it.
    session.Adapter.State.IsExternalCaptureActive = true;
    GraphicsCaptureCapabilities capabilities;
    REQUIRE(GetGraphicsCaptureCapabilities(session.Device, capabilities) == GraphicsCaptureError::None);
    CHECK(capabilities.IsExternalCaptureActive == true);
    CHECK(capabilities.Unavailable == GraphicsCaptureError::Busy);
    CHECK(RequestGraphicsCapture(session.Device, session.MakeRequest(), identity) == GraphicsCaptureError::Busy);
    session.Adapter.State.IsExternalCaptureActive = false;
    CHECK(identity.Request == 0);

    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    CHECK(RequestGraphicsCapture(session.Device, session.MakeRequest(), identity) ==
          GraphicsCaptureError::InvalidState);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);

    REQUIRE(GetGraphicsCaptureCapabilities(session.Device, capabilities) == GraphicsCaptureError::None);
    CHECK(capabilities.Tool == GraphicsCaptureTool::Metal);
    CHECK(capabilities.CanCaptureFrame == true);
    const GraphicsCaptureIdentity armed = session.Arm();
    CHECK(armed.Session != 0);
    CHECK(armed.Request != 0);
    const GraphicsCaptureReport report = GetReport(armed);
    CHECK(report.State == GraphicsCaptureState::Armed);
    CHECK(report.RequestedFrame == 1);
    CHECK(session.Adapter.Begins == 0);
    // One request owns the adapter at a time.
    GraphicsCaptureIdentity competing;
    CHECK(RequestGraphicsCapture(session.Device, session.MakeRequest(), competing) == GraphicsCaptureError::Busy);
    REQUIRE(GetGraphicsCaptureCapabilities(session.Device, capabilities) == GraphicsCaptureError::None);
    CHECK(capabilities.Unavailable == GraphicsCaptureError::Busy);

    // A destination that already holds the next request's manifest is never overwritten.
    REQUIRE(CancelGraphicsCapture(armed) == GraphicsCaptureError::None);
    const std::string stem = "ludus-capture-" + std::to_string(armed.Session) + "-" + std::to_string(armed.Request + 1);
    REQUIRE(ludus::test_support::WriteFile(session.GetPath(stem + ".manifest.json"), {}) == true);
    CHECK(RequestGraphicsCapture(session.Device, session.MakeRequest(), competing) ==
          GraphicsCaptureError::InvalidDestination);
}

TEST_CASE("G1 skipped frames keep a capture armed until the next eligible frame", "[rhi][graphics-capture]")
{
    CaptureSession session;
    session.Adapter.Mode = Finalization::Polled;
    session.RenderFrame();
    const GraphicsCaptureIdentity identity = session.Arm();

    // A zero-size target is skipped without starting a native capture.
    REQUIRE(SetFrameTarget(session.Device, session.Surface, {}) == DeviceStatus::Ready);
    backend::NextFrame = FrameStatus::Skipped;
    CHECK(BeginFrame(session.Device, session.Surface) == DeviceStatus::Skipped);
    CHECK(session.Adapter.Begins == 0);
    // An unacquired drawable is only known after the start; the empty capture is discarded.
    REQUIRE(SetFrameTarget(session.Device, session.Surface, { .Width = 64, .Height = 32 }) == DeviceStatus::Ready);
    CHECK(BeginFrame(session.Device, session.Surface) == DeviceStatus::Skipped);
    CHECK(session.Adapter.Begins == 1);
    CHECK(session.Adapter.Abandons == 1);
    GraphicsCaptureReport report = GetReport(identity);
    CHECK(report.State == GraphicsCaptureState::Armed);
    CHECK(report.SkippedFrames == 2);
    CHECK(report.IsArtifactPreserved == false);

    backend::NextFrame = FrameStatus::Ready;
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    // Native capture starts before the frame's commands are recorded.
    CHECK(session.Adapter.Begins == 2);
    CHECK(session.Adapter.WasFrameOpenAtBegin == false);
    CHECK(session.Adapter.Started == identity);
    REQUIRE(GetGraphicsCaptureReport(identity, report) == GraphicsCaptureError::None);
    CHECK(report.State == GraphicsCaptureState::Capturing);
    CHECK(report.CapturedFrame == 2);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    CHECK(session.Adapter.WasFrameOpenAtEnd == false);
    CHECK(session.Adapter.Ends == 1);

    // Publication waits for the adapter's completion and happens on the poll, not the frame.
    CHECK(ludus::test_support::IsExistingPath(session.GetPath(session.Adapter.GetStem() + ".manifest.json")) == false);
    report = GetReport(identity);
    CHECK(session.Adapter.Polls == 1);
    CHECK(report.State == GraphicsCaptureState::Complete);
    CHECK(report.Error == GraphicsCaptureError::None);
    CHECK(report.Tool == GraphicsCaptureTool::Metal);
    CHECK(report.RequestedFrame == 1);
    CHECK(report.CapturedFrame == 2);
    CHECK(report.SkippedFrames == 2);
    CHECK(report.IsArtifactPreserved == true);
    CHECK(report.IsManifestPublished == true);
    CHECK(GetText(report.Artifact.GetData()) == session.Adapter.GetArtifactName());
    CHECK(GetText(report.ToolVersion.GetData()) == "controlled 1.0");
    CHECK(GetText(report.ShaderSource.GetData()) == "controlled source");
    String manifest;
    REQUIRE(ludus::test_support::ReadFile(session.GetPath(report.Manifest.GetData()), manifest) == true);
    const std::string_view text = manifest.GetView();
    CHECK(text.find("\"format\": \"ludus-graphics-capture\"") != std::string_view::npos);
    CHECK(text.find("\"version\": 2") != std::string_view::npos);
    CHECK(text.find("\"shaderSource\": \"controlled source\"") != std::string_view::npos);
    CHECK(text.find("\"backend\": \"Metal\"") != std::string_view::npos);
    CHECK(text.find("\"capturedFrame\": 2") != std::string_view::npos);
    CHECK(text.find("\"requestedFrame\": 1") != std::string_view::npos);
    CHECK(text.find("\"artifact\": \"" + session.Adapter.GetArtifactName() + "\"") != std::string_view::npos);
    // The terminal request releases the adapter; later frames are not captured.
    session.RenderFrame();
    CHECK(session.Adapter.Begins == 2);
    CHECK(GetReport(identity).State == GraphicsCaptureState::Complete);
}

TEST_CASE("G1 capture deadlines bound armed and finalizing requests", "[rhi][graphics-capture]")
{
    CaptureSession session;
    // A permanently hidden target cannot keep a request armed.
    REQUIRE(SetFrameTarget(session.Device, session.Surface, {}) == DeviceStatus::Ready);
    backend::NextFrame = FrameStatus::Skipped;
    const GraphicsCaptureIdentity hidden = session.Arm(SHORT_DEADLINE_MILLISECONDS);
    WaitPastDeadline();
    CHECK(BeginFrame(session.Device, session.Surface) == DeviceStatus::Skipped);
    GraphicsCaptureReport report = GetReport(hidden);
    CHECK(report.State == GraphicsCaptureState::Failed);
    CHECK(report.Error == GraphicsCaptureError::DeadlineExceeded);
    CHECK(session.Adapter.Begins == 0);

    // Without any frame, the poll expires the request.
    const GraphicsCaptureIdentity idle = session.Arm(SHORT_DEADLINE_MILLISECONDS);
    WaitPastDeadline();
    CHECK(GetReport(idle).Error == GraphicsCaptureError::DeadlineExceeded);

    // A tool that never completes is abandoned; its partial artifact stays identified.
    backend::NextFrame = FrameStatus::Ready;
    REQUIRE(SetFrameTarget(session.Device, session.Surface, { .Width = 64, .Height = 32 }) == DeviceStatus::Ready);
    session.Adapter.Mode = Finalization::Never;
    const GraphicsCaptureIdentity stuck = session.Arm(FRAME_DEADLINE_MILLISECONDS);
    session.RenderFrame();
    REQUIRE(GetReport(stuck).State == GraphicsCaptureState::Finalizing);
    WaitPastDeadline(FRAME_DEADLINE_MILLISECONDS);
    report = GetReport(stuck);
    CHECK(report.State == GraphicsCaptureState::Failed);
    CHECK(report.Error == GraphicsCaptureError::DeadlineExceeded);
    CHECK(session.Adapter.Abandons == 1);
    CHECK(report.IsArtifactPreserved == true);
    CHECK(report.IsManifestPublished == false);
    CHECK(GetText(report.Artifact.GetData()) == session.Adapter.GetArtifactName());
}

TEST_CASE("G1 cancellation ends armed, capturing and finalizing requests without a manifest", "[rhi][graphics-capture]")
{
    CaptureSession session;
    const GraphicsCaptureIdentity armed = session.Arm();
    REQUIRE(CancelGraphicsCapture(armed) == GraphicsCaptureError::None);
    CHECK(GetReport(armed).State == GraphicsCaptureState::Cancelled);
    CHECK(CancelGraphicsCapture(armed) == GraphicsCaptureError::InvalidState);
    session.RenderFrame();
    CHECK(session.Adapter.Begins == 0);

    // A capture in progress stops after its frame is submitted.
    const GraphicsCaptureIdentity capturing = session.Arm();
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    REQUIRE(CancelGraphicsCapture(capturing) == GraphicsCaptureError::None);
    CHECK(GetReport(capturing).State == GraphicsCaptureState::Capturing);
    REQUIRE(EndFrame(session.Device) == DeviceStatus::Ready);
    GraphicsCaptureReport report = GetReport(capturing);
    CHECK(report.State == GraphicsCaptureState::Cancelled);
    CHECK(session.Adapter.Ends == 0);
    CHECK(session.Adapter.Abandons == 1);
    CHECK(report.IsManifestPublished == false);

    // Cancellation during finalization uses the adapter's synchronous cleanup.
    session.Adapter.Mode = Finalization::Never;
    const GraphicsCaptureIdentity finalizing = session.Arm();
    session.RenderFrame();
    REQUIRE(GetReport(finalizing).State == GraphicsCaptureState::Finalizing);
    REQUIRE(CancelGraphicsCapture(finalizing) == GraphicsCaptureError::None);
    report = GetReport(finalizing);
    CHECK(report.State == GraphicsCaptureState::Cancelled);
    CHECK(session.Adapter.Abandons == 2);
    CHECK(report.IsArtifactPreserved == true);
    CHECK(report.IsManifestPublished == false);
    CHECK(ludus::test_support::IsExistingPath(session.GetPath(session.Adapter.GetStem() + ".manifest.json")) == false);
    // A late tool reply after cleanup is ignored.
    session.Adapter.Report(finalizing);
    CHECK(GetReport(finalizing).State == GraphicsCaptureState::Cancelled);
}

TEST_CASE("G1 stale capture identities and replies never affect the current request", "[rhi][graphics-capture]")
{
    CaptureSession session;
    session.Adapter.Mode = Finalization::Never;
    const GraphicsCaptureIdentity first = session.Arm();
    session.RenderFrame();
    REQUIRE(CancelGraphicsCapture(first) == GraphicsCaptureError::None);
    const std::string firstStem = session.Adapter.GetStem();

    const GraphicsCaptureIdentity second = session.Arm();
    CHECK((second == first) == false);
    session.RenderFrame();
    REQUIRE(GetReport(second).State == GraphicsCaptureState::Finalizing);
    // A reply carrying the earlier identity is rejected.
    const std::string staleArtifact = firstStem + std::string(ARTIFACT_EXTENSION);
    internal::ReportGraphicsCaptureFinalization(first,
                                                {
                                                    .Error = GraphicsCaptureError::None,
                                                    .ArtifactOrNull = staleArtifact.c_str(),
                                                    .IsArtifactVerified = true,
                                                });
    CHECK(GetReport(second).State == GraphicsCaptureState::Finalizing);
    GraphicsCaptureReport preserved;
    preserved.SkippedFrames = 7;
    CHECK(GetGraphicsCaptureReport(first, preserved) == GraphicsCaptureError::StaleIdentity);
    CHECK(preserved.SkippedFrames == 7);
    CHECK(CancelGraphicsCapture(first) == GraphicsCaptureError::StaleIdentity);
    CHECK(GetGraphicsCaptureReport({}, preserved) == GraphicsCaptureError::StaleIdentity);

    // The current reply is accepted once; a duplicate cannot replace it.
    session.Adapter.Report(second);
    internal::ReportGraphicsCaptureFinalization(second,
                                                {
                                                    .Error = GraphicsCaptureError::NativeCaptureFailed,
                                                    .DiagnosticOrNull = "duplicate",
                                                });
    const GraphicsCaptureReport report = GetReport(second);
    CHECK(report.State == GraphicsCaptureState::Complete);
    CHECK(GetText(report.Diagnostic.GetData()).empty() == true);
}

TEST_CASE("G1 device loss and shutdown abandon captures before native teardown", "[rhi][graphics-capture]")
{
    GraphicsCaptureIdentity lost;
    {
        CaptureSession session;
        lost = session.Arm();
        REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
        internal::Fail(backend::PendingToken, StartupError::DeviceLost);
        CHECK(EndFrame(session.Device) == DeviceStatus::DeviceLost);
        const GraphicsCaptureReport report = GetReport(lost);
        CHECK(report.State == GraphicsCaptureState::Failed);
        CHECK(report.Error == GraphicsCaptureError::DeviceLost);
        CHECK(session.Adapter.Abandons == 1);
        CHECK(session.Adapter.Ends == 0);
        GraphicsCaptureCapabilities capabilities;
        CHECK(GetGraphicsCaptureCapabilities(session.Device, capabilities) == GraphicsCaptureError::DeviceLost);
    }
    GraphicsCaptureIdentity finalizing;
    {
        CaptureSession session;
        session.Adapter.Mode = Finalization::Never;
        finalizing = session.Arm();
        session.RenderFrame();
        const uint32 shutdowns = backend::ShutdownCount;
        REQUIRE(DestroyDevice(session.Device) == DeviceStatus::Ready);
        // Cleanup ran before the backend released its native objects.
        CHECK(session.Adapter.Abandons == 1);
        CHECK(session.Adapter.ShutdownsAtAbandon == shutdowns);
        CHECK(backend::ShutdownCount == shutdowns + 1);
        // The terminal outcome stays readable after the device is gone.
        const GraphicsCaptureReport report = GetReport(finalizing);
        CHECK(report.State == GraphicsCaptureState::Failed);
        CHECK(report.Error == GraphicsCaptureError::SessionEnded);
        CHECK(report.IsArtifactPreserved == true);
        CHECK(CancelGraphicsCapture(lost) == GraphicsCaptureError::StaleIdentity);
    }
    {
        // A verified artifact no longer needs the device: its manifest is still published.
        CaptureSession session;
        const GraphicsCaptureIdentity verified = session.Arm();
        session.RenderFrame();
        Shutdown();
        const GraphicsCaptureReport report = GetReport(verified);
        CHECK(report.State == GraphicsCaptureState::Complete);
        CHECK(report.IsManifestPublished == true);
        CHECK(session.Adapter.Abandons == 0);
    }
    {
        // Session end clears the adapter registration.
        CaptureSession session(false);
        GraphicsCaptureCapabilities capabilities;
        REQUIRE(GetGraphicsCaptureCapabilities(session.Device, capabilities) == GraphicsCaptureError::None);
        CHECK(capabilities.Unavailable == GraphicsCaptureError::UnsupportedBackend);
    }
}

TEST_CASE("G1 failed native capture, frame or manifest publication preserves output honestly",
          "[rhi][graphics-capture]")
{
    CaptureSession session;
    session.Adapter.ShouldBlockManifest = true;
    const GraphicsCaptureIdentity blocked = session.Arm();
    session.RenderFrame();
    GraphicsCaptureReport report = GetReport(blocked);
    CHECK(report.State == GraphicsCaptureState::Failed);
    CHECK(report.Error == GraphicsCaptureError::ManifestFailed);
    CHECK(report.IsArtifactPreserved == true);
    CHECK(report.IsManifestPublished == false);
    CHECK(GetText(report.Manifest.GetData()).empty() == true);
    CHECK(GetText(report.Diagnostic.GetData()).empty() == false);
    uint64 artifactBytes = 0;
    REQUIRE(ludus::test_support::GetFileSize(session.GetPath(report.Artifact.GetData()), artifactBytes) == true);
    CHECK(artifactBytes == ARTIFACT_BYTES.size());
    // The blocking entry was not replaced.
    CHECK(ludus::test_support::IsDirectory(session.GetPath(session.Adapter.GetStem() + ".manifest.json")) == true);
    session.Adapter.ShouldBlockManifest = false;

    session.Adapter.ShouldReportForeignArtifact = true;
    const GraphicsCaptureIdentity foreign = session.Arm();
    session.RenderFrame();
    report = GetReport(foreign);
    CHECK(report.Error == GraphicsCaptureError::NativeCaptureFailed);
    CHECK(report.IsArtifactPreserved == false);
    session.Adapter.ShouldReportForeignArtifact = false;

    session.Adapter.BeginResult = GraphicsCaptureError::Busy;
    const GraphicsCaptureIdentity busy = session.Arm();
    session.RenderFrame();
    CHECK(GetReport(busy).Error == GraphicsCaptureError::Busy);
    session.Adapter.BeginResult = GraphicsCaptureError::NativeCaptureFailed;
    const GraphicsCaptureIdentity refused = session.Arm();
    session.RenderFrame();
    CHECK(GetReport(refused).Error == GraphicsCaptureError::NativeCaptureFailed);
    session.Adapter.BeginResult = GraphicsCaptureError::None;
    CHECK(session.Adapter.Abandons == 0);

    session.Adapter.EndResult = GraphicsCaptureError::NativeCaptureFailed;
    const GraphicsCaptureIdentity stopped = session.Arm();
    session.RenderFrame();
    CHECK(GetReport(stopped).Error == GraphicsCaptureError::NativeCaptureFailed);
    CHECK(session.Adapter.Abandons == 1);
    session.Adapter.EndResult = GraphicsCaptureError::None;

    const GraphicsCaptureIdentity failedFrame = session.Arm();
    REQUIRE(BeginFrame(session.Device, session.Surface) == DeviceStatus::Ready);
    backend::NextFrameEnd = FrameStatus::Failed;
    CHECK(EndFrame(session.Device) == DeviceStatus::Failed);
    backend::NextFrameEnd = FrameStatus::Ready;
    CHECK(GetReport(failedFrame).Error == GraphicsCaptureError::FrameFailed);
    CHECK(session.Adapter.Abandons == 2);

    // An offscreen frame the tool cannot capture fails instead of waiting silently.
    session.Adapter.State.CanCaptureOffscreen = false;
    const GraphicsCaptureIdentity offscreen = session.Arm();
    REQUIRE(SetFrameTarget(session.Device,
                           session.Surface,
                           { .Width = 64, .Height = 32, .ShouldPresentToSurface = false }) == DeviceStatus::Ready);
    session.RenderFrame();
    CHECK(GetReport(offscreen).Error == GraphicsCaptureError::InvalidTarget);
    // Rendering is unaffected by every failure above.
    session.RenderFrame();
}
