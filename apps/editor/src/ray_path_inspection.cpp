#include "internal/ray_path_inspection.h"

#include "internal/render_graph_preview.h"

namespace ludus::editor
{
using namespace foundation;
namespace renderer = graphics::renderer;

RayPathInspection::RayPathInspection(QObject* parentOrNull) : QObject(parentOrNull) {}

void RayPathInspection::SetPreview(RenderGraphPreview* previewOrNull)
{
    if (mPreview.data() == previewOrNull)
    {
        return;
    }
    Leave();
    disconnect(mCaptureConnection);
    mPreview = previewOrNull;
    if (previewOrNull != nullptr)
    {
        mCaptureConnection = connect(previewOrNull, &RenderGraphPreview::RayPathCaptureChanged, this, [this]() {
            Q_EMIT Changed();
            Q_EMIT OverlayChanged();
        });
    }
    Q_EMIT Changed();
}

bool RayPathInspection::Begin(const RayPathTarget& target)
{
    if (mPreview.isNull() == true)
    {
        return false;
    }
    Leave();
    const QString problem = mPreview->DescribeRayPathCaptureProblem();
    if (problem.isEmpty() == false)
    {
        // Nothing freezes: the live renderer keeps drawing and navigating normally.
        mPreview->FailRayPathCapture(problem, true);
        return false;
    }
    mTarget = target;
    setStage(RayPathInspectionStage::PickingPixel);
    return true;
}

bool RayPathInspection::PickPixel(math::Vector2 pixel)
{
    if (mStage != RayPathInspectionStage::PickingPixel || mPreview.isNull() == true)
    {
        return false;
    }
    QString error;
    if (mPreview->FreezeRayPaths(error) == false)
    {
        setStage(RayPathInspectionStage::Inactive);
        mPreview->FailRayPathCapture(error, true);
        return false;
    }
    mPixel = pixel;
    mSelectedEvent = -1;
    mSampleFilter = -1;
    mPreview->BeginRayPathBuild();
    setStage(RayPathInspectionStage::Inspecting);
    Q_EMIT BuildRequested();
    return true;
}

void RayPathInspection::AcceptBuild(const QString& packagePath)
{
    if (mStage != RayPathInspectionStage::Inspecting || mPreview.isNull() == true)
    {
        return;
    }
    static_cast<void>(mPreview->StartRayPathCapture(packagePath, mPixel, mTarget, SAMPLE_COUNT));
}

void RayPathInspection::RejectBuild(const QString& reason)
{
    if (mStage != RayPathInspectionStage::Inspecting || mPreview.isNull() == true)
    {
        return;
    }
    mPreview->FailRayPathCapture(reason);
}

void RayPathInspection::Leave()
{
    if (mPreview.isNull() == false)
    {
        mPreview->ThawRayPaths();
    }
    mSelectedEvent = -1;
    setStage(RayPathInspectionStage::Inactive);
}

RayPathInspectionStage RayPathInspection::GetStage() const noexcept
{
    return mStage;
}

const RayPathTarget& RayPathInspection::GetTarget() const noexcept
{
    return mTarget;
}

RenderGraphPreview* RayPathInspection::GetPreviewOrNull() const noexcept
{
    return mPreview.data();
}

QString RayPathInspection::GetStatus() const
{
    if (mPreview.isNull() == true)
    {
        return QStringLiteral("Ray path capture needs the native World viewport.");
    }
    const RayPathCapture& capture = mPreview->GetRayPathCapture();
    if (mStage == RayPathInspectionStage::PickingPixel)
    {
        return QStringLiteral("Click a pixel of the graph output to trace %1 for %2 samples. Esc cancels.")
            .arg(mTarget.NodeId.isEmpty() == true ? QStringLiteral("every ray pass") : mTarget.NodeId)
            .arg(SAMPLE_COUNT);
    }
    if (mStage == RayPathInspectionStage::Inactive)
    {
        return capture.GetState() == RayPathCaptureState::Unavailable ? capture.GetMessage() : QString{};
    }
    QString status;
    switch (capture.GetState())
    {
        case RayPathCaptureState::Ready: {
            const renderer::RayPathCaptureReport& report = capture.GetReport();
            status = QStringLiteral("%1 events in %2 invocations at pixel (%3, %4).")
                         .arg(capture.GetEvents().size())
                         .arg(capture.GetSummaries().size())
                         .arg(capture.GetPixelX())
                         .arg(capture.GetPixelY());
            if (report.TruncatedSummaries != 0)
            {
                status += QStringLiteral(" Truncated: %1 paths lost %2 records to capacity; they are drawn faded.")
                              .arg(report.TruncatedSummaries)
                              .arg(report.DroppedRecords);
            }
            break;
        }
        case RayPathCaptureState::Idle:
            status = QStringLiteral("Frozen.");
            break;
        case RayPathCaptureState::Building:
        case RayPathCaptureState::Preparing:
        case RayPathCaptureState::Tracing:
        case RayPathCaptureState::Reading:
            status = capture.GetMessage();
            break;
        case RayPathCaptureState::Failed:
            status = QStringLiteral("Capture failed: %1").arg(capture.GetMessage());
            break;
        case RayPathCaptureState::Unavailable:
            status = capture.GetMessage();
            break;
    }
    const QString& stale = mPreview->GetRayPathStaleReason();
    return stale.isEmpty() == true ? status : QStringLiteral("Stale: %1 %2").arg(stale, status);
}

void RayPathInspection::SetXRay(bool isXRay)
{
    if (mIsXRay != isXRay)
    {
        mIsXRay = isXRay;
        Q_EMIT OverlayChanged();
    }
}

bool RayPathInspection::IsXRay() const noexcept
{
    return mIsXRay;
}

void RayPathInspection::SetColorMode(RayPathColorMode mode)
{
    if (mColorMode != mode)
    {
        mColorMode = mode;
        Q_EMIT OverlayChanged();
    }
}

RayPathColorMode RayPathInspection::GetColorMode() const noexcept
{
    return mColorMode;
}

void RayPathInspection::SetSampleFilter(int32 sample)
{
    if (mSampleFilter != sample)
    {
        mSampleFilter = sample;
        Q_EMIT OverlayChanged();
    }
}

int32 RayPathInspection::GetSampleFilter() const noexcept
{
    return mSampleFilter;
}

void RayPathInspection::SelectEvent(int32 event)
{
    if (mSelectedEvent == event || mPreview.isNull() == true)
    {
        return;
    }
    const QVector<renderer::RayPathEvent>& events = mPreview->GetRayPathCapture().GetEvents();
    mSelectedEvent = event >= 0 && event < events.size() ? event : -1;
    Q_EMIT OverlayChanged();
    if (mSelectedEvent >= 0 && events.at(mSelectedEvent).Outcome == renderer::RayPathOutcome::Hit)
    {
        const QString object = mPreview->GetRayPathCapture().GetObjectId(events.at(mSelectedEvent).SourceId);
        if (object.isEmpty() == false)
        {
            Q_EMIT ObjectSelectionRequested(object);
        }
    }
}

int32 RayPathInspection::GetSelectedEvent() const noexcept
{
    return mSelectedEvent;
}

RayPathOverlay RayPathInspection::BuildOverlay(const math::Aabb3* sceneBoundsOrNull) const
{
    if (mStage != RayPathInspectionStage::Inspecting || mPreview.isNull() == true)
    {
        return {};
    }
    const RayPathCapture& capture = mPreview->GetRayPathCapture();
    RayPathCamera camera;
    camera.WorldToClip = mPreview->GetRayPathFreeze().View.WorldToClip;
    camera.LaunchExtent = capture.GetLaunchExtent();
    camera.PixelX = capture.GetPixelX();
    camera.PixelY = capture.GetPixelY();
    RayPathOverlayOptions options;
    options.ColorMode = mColorMode;
    options.IsXRay = mIsXRay;
    options.PassFilter = capture.GetTargetPass();
    options.SampleFilter = mSampleFilter;
    options.SelectedEvent = mSelectedEvent;
    if (sceneBoundsOrNull != nullptr)
    {
        options.SceneBoundsWorld = *sceneBoundsOrNull;
        options.HasSceneBounds = true;
    }
    const QVector<renderer::RayPathEvent>& events = capture.GetEvents();
    const QVector<renderer::RayPathSummary>& summaries = capture.GetSummaries();
    return BuildRayPathOverlay(camera,
                               {events.constData(), static_cast<usize>(events.size())},
                               {summaries.constData(), static_cast<usize>(summaries.size())},
                               options);
}

void RayPathInspection::setStage(RayPathInspectionStage stage)
{
    if (mStage != stage)
    {
        mStage = stage;
        Q_EMIT OverlayChanged();
    }
    Q_EMIT Changed();
}
} // namespace ludus::editor
