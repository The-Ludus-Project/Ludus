#include "internal/graph_comparison_run.h"

#include "internal/captured_image_view.h"
#include "internal/reference_capture_window.h"
#include "internal/reference_comparison.h"
#include "internal/render_graph_capture_profile.h"
#include "internal/render_preview.h"
#include "internal/renderer_workspace.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace renderer = graphics::renderer;

QString DescribeExtent(const QSize& size)
{
    return QStringLiteral("%1 × %2").arg(size.width()).arg(size.height());
}

QString MakeCandidateStem(qsizetype index)
{
    return QStringLiteral("candidate-%1").arg(index + 1);
}

QByteArray ReadPackageIdentity(const QString& path)
{
    if (path.isEmpty() == true)
    {
        return {};
    }
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (file.open(QIODevice::ReadOnly) == false || hash.addData(&file) == false)
    {
        return {};
    }
    return hash.result();
}

bool HasReferenceSamples(BorrowedView<const renderer::ReferencePixel> pixels)
{
    for (usize i = 0; i < pixels.GetSize(); ++i)
    {
        if (pixels[i].Samples != 0)
        {
            return true;
        }
    }
    return false;
}
} // namespace

bool GraphComparisonRun::Start(RenderPreview& inoutPreview,
                               RendererWorkspace& inoutWorkspace,
                               const ViewportReferenceSettings& settings,
                               const QVector<GraphComparisonCandidate>& candidates,
                               const QImage& displayBaseline,
                               const QString& baselineSource,
                               const QString& directory,
                               QString& outError)
{
    RenderGraphPreview* graphOrNull = inoutWorkspace.GetPreviewOrNull();
    if (IsRunning() == true || inoutPreview.HasDevice() == false || graphOrNull == nullptr ||
        candidates.isEmpty() == true)
    {
        outError = QStringLiteral("Open a World with a ready project graph and add a candidate.");
        return false;
    }
    const ViewportRasterSource source = GetViewportRasterSource(*graphOrNull);
    QByteArray identity;
    if (ViewportGraphCapture::ReadGenerationIdentity(source, identity, outError) == false)
    {
        return false;
    }
    const QSize extent = displayBaseline.isNull() == true
                             ? ComputeReferenceExtent(inoutPreview.GetDrawableSize(), settings.LongSide)
                             : displayBaseline.size();
    if (extent.isEmpty() == true)
    {
        outError =
            QStringLiteral("Wait for the World viewport to draw, or supply a display baseline with a fixed extent.");
        return false;
    }
    renderer::ViewDescription view;
    if (inoutPreview.DescribeConventionalView(static_cast<uint32>(extent.width()),
                                              static_cast<uint32>(extent.height()),
                                              view,
                                              outError) == false)
    {
        return false;
    }
    view.WorldGrid.IsVisible = false;
    if (displayBaseline.isNull() == true)
    {
        if (mReference.Start(inoutPreview.GetDevice(),
                             inoutPreview.GetSurface(),
                             inoutPreview.Scene(),
                             view,
                             extent,
                             settings,
                             outError) == false)
        {
            return false;
        }
    }
    else if (WritePngAtomically(QDir(directory).filePath(QStringLiteral("baseline.png")), displayBaseline, outError) ==
             false)
    {
        return false;
    }
    mWorkspace = &inoutWorkspace;
    mDevice = inoutPreview.GetDevice();
    mSurface = inoutPreview.GetSurface();
    mScene = inoutPreview.Scene();
    mView = view;
    mExtent = extent;
    mViewportSource = source;
    mViewportPackageIdentity = ReadPackageIdentity(source.PackagePath);
    mViewportFallbackIdentity = ReadPackageIdentity(source.FallbackPath);
    mViewportConfiguration = inoutWorkspace.GetConfiguration();
    mGenerationIdentity = identity;
    mSettings = settings;
    mCandidates = candidates;
    mDisplayBaseline = displayBaseline;
    mBaselineSource = baselineSource;
    mBaselineState = displayBaseline.isNull() == true ? QStringLiteral("rendering") : QStringLiteral("supplied");
    mBaselineError.clear();
    mDirectory = directory;
    mReferenceInfo = {};
    mReferencePixels = {};
    mResults = {};
    mCandidateIndex = 0;
    mHasRequestedCandidate = false;
    mPhase = displayBaseline.isNull() == true ? Phase::Reference : Phase::NextCandidate;
    mStatus = QStringLiteral("Frozen World and camera at %1.").arg(DescribeExtent(extent));
    writeManifest(QStringLiteral("running"));
    return true;
}

void GraphComparisonRun::Service()
{
    switch (mPhase)
    {
        case Phase::Idle:
            return;
        case Phase::Reference: {
            const ViewportReferencePhase phase = mReference.Service(REFERENCE_SERVICE_BUDGET_NANOSECONDS);
            if (phase == ViewportReferencePhase::Complete || phase == ViewportReferencePhase::Failed)
            {
                finishReference();
                return;
            }
            renderer::ReferenceCaptureInfo progress;
            const uint32 samples = mReference.GetInfo(progress) == true ? progress.MinimumSamples : 0;
            mStatus = QStringLiteral("RT1 baseline · %1 / %2 samples/pixel.").arg(samples).arg(mSettings.Samples);
            return;
        }
        case Phase::NextCandidate:
            if (mCandidateIndex == mCandidates.size())
            {
                finishRun(QStringLiteral("finished"), QStringLiteral("All candidates processed."));
                return;
            }
            mPhase = Phase::PreparingCandidate;
            break;
        case Phase::PreparingCandidate:
            break;
        case Phase::Graph: {
            const renderer::RenderGraphCapturePhase phase = mGraph.Service();
            if (phase == renderer::RenderGraphCapturePhase::Complete ||
                phase == renderer::RenderGraphCapturePhase::Failed)
            {
                finishCandidate(phase == renderer::RenderGraphCapturePhase::Complete ? QStringLiteral("complete")
                                                                                     : QStringLiteral("failed"),
                                mGraph.GetError());
                return;
            }
            const renderer::RenderGraphCaptureInfo info = mGraph.GetInfo();
            mStatus = QStringLiteral("%1 · %2 / %3 invocations including warm-up.")
                          .arg(mCandidates.at(mCandidateIndex).Label)
                          .arg(info.SubmittedSamples)
                          .arg(info.TargetSamples);
            return;
        }
        case Phase::DisplayFrame: {
            const ViewportRasterPhase phase = mRaster.Service();
            if (phase == ViewportRasterPhase::Complete || phase == ViewportRasterPhase::Failed)
            {
                finishCandidate(phase == ViewportRasterPhase::Complete ? QStringLiteral("complete")
                                                                       : QStringLiteral("failed"),
                                mRaster.GetError());
            }
            return;
        }
    }
    prepareCandidate();
}

void GraphComparisonRun::finishReference()
{
    QString error;
    const bool hasFailed = mReference.GetPhase() == ViewportReferencePhase::Failed;
    mBaselineState = hasFailed == true ? QStringLiteral("failed") : QStringLiteral("complete");
    mBaselineError = mReference.GetError();
    if (mReference.GetInfo(mReferenceInfo) == false || mReference.CopyPixels(mReferencePixels, error) == false ||
        HasReferenceSamples({mReferencePixels.GetData(), mReferencePixels.GetSize()}) == false)
    {
        mBaselineState = QStringLiteral("failed");
        mBaselineError = mBaselineError.isEmpty() == false ? mBaselineError : error;
        finishRun(QStringLiteral("failed"),
                  QStringLiteral("The baseline has no readable samples. %1").arg(mBaselineError));
        return;
    }
    if (mReference.Save(QDir(mDirectory).filePath(QStringLiteral("baseline.luhdr")), error) == false)
    {
        mBaselineError = error;
        finishRun(QStringLiteral("failed"), error);
        return;
    }
    mReference.Release();
    mPhase = Phase::NextCandidate;
}

void GraphComparisonRun::prepareCandidate()
{
    const GraphComparisonCandidate& candidate = mCandidates.at(mCandidateIndex);
    mStatus = QStringLiteral("Preparing %1.").arg(candidate.Label);
    ViewportRasterSource source = mViewportSource;
    QString profile;
    QString error;
    if (mWorkspace.isNull() == true)
    {
        finishRun(QStringLiteral("stopped"), QStringLiteral("The graph workspace closed."));
        return;
    }
    if (candidate.HasSavedTarget == true)
    {
        RenderGraphLibraryCapture* libraryOrNull = mWorkspace->GetLibraryCaptureOrNull();
        if (libraryOrNull == nullptr)
        {
            finishCandidate(QStringLiteral("failed"),
                            QStringLiteral("Build the project graph before capturing libraries."));
            return;
        }
        const bool isRequested = libraryOrNull->GetGenerationIdentity() == mGenerationIdentity &&
                                 libraryOrNull->GetTarget() == candidate.Target;
        if (mHasRequestedCandidate == false &&
            libraryOrNull->IsReadyFor(mGenerationIdentity, candidate.Target) == false)
        {
            if (mWorkspace->IsJobActive() == false)
            {
                mHasRequestedCandidate = true;
                libraryOrNull->Request(mGenerationIdentity, candidate.Target);
            }
            return;
        }
        if (isRequested == false || libraryOrNull->GetState() == LibraryCaptureState::Failed)
        {
            finishCandidate(
                QStringLiteral("failed"),
                isRequested == true
                    ? libraryOrNull->GetError()
                    : QStringLiteral(
                          "The capture build request was replaced. Start a new run after other graph jobs finish."));
            return;
        }
        if (libraryOrNull->IsReadyFor(mGenerationIdentity, candidate.Target) == false)
        {
            return;
        }
        source = {};
        source.PackagePath = libraryOrNull->GetPackagePath();
        source.OutputTarget = candidate.Target.IsRootConfiguration == true
                                  ? candidate.Target.Output
                                  : QString::fromLatin1(LIBRARY_CAPTURE_EXPORT);
        if (source.OutputTarget.isEmpty() == false && ViewportGraphCapture::FindExportResource(source.PackagePath,
                                                                                               source.OutputTarget,
                                                                                               source.OutputResource,
                                                                                               error) == false)
        {
            finishCandidate(QStringLiteral("failed"), error);
            return;
        }
        profile = libraryOrNull->GetProfilePath();
    }
    else
    {
        RenderGraphCaptureProfile* profileOrNull = mWorkspace->GetCaptureProfileOrNull();
        if (candidate.IsDisplayFrame == false && profileOrNull != nullptr)
        {
            if (mWorkspace->GetConfiguration() != mViewportConfiguration)
            {
                finishCandidate(
                    QStringLiteral("failed"),
                    QStringLiteral(
                        "The viewport configuration changed. "
                        "Select a saved root configuration row to capture it independently of the viewport."));
                return;
            }
            if (mHasRequestedCandidate == false && profileOrNull->IsReadyFor(mGenerationIdentity) == false)
            {
                if (mWorkspace->IsJobActive() == false)
                {
                    mHasRequestedCandidate = true;
                    profileOrNull->Request(mGenerationIdentity);
                }
                return;
            }
            if (profileOrNull->GetGenerationIdentity() != mGenerationIdentity ||
                profileOrNull->GetState() == CaptureProfileState::Failed)
            {
                finishCandidate(QStringLiteral("failed"),
                                profileOrNull->GetError().isEmpty() == false
                                    ? profileOrNull->GetError()
                                    : QStringLiteral("The capture profile request was replaced. Start a new run."));
                return;
            }
            if (profileOrNull->IsReadyFor(mGenerationIdentity) == false)
            {
                return;
            }
            profile = profileOrNull->GetPackagePath();
        }
        if (mViewportPackageIdentity.isEmpty() == true ||
            ReadPackageIdentity(source.PackagePath) != mViewportPackageIdentity ||
            (candidate.IsDisplayFrame == true && ReadPackageIdentity(source.FallbackPath) != mViewportFallbackIdentity))
        {
            finishCandidate(QStringLiteral("failed"),
                            QStringLiteral("The frozen viewport package is unavailable. "
                                           "Build and start a new run, or add a saved root configuration row."));
            return;
        }
    }
    startCandidate(source, profile);
}

void GraphComparisonRun::startCandidate(const ViewportRasterSource& source, const QString& profile)
{
    const GraphComparisonCandidate& candidate = mCandidates.at(mCandidateIndex);
    QString error;
    QSize extent;
    const bool isLoaded =
        candidate.IsDisplayFrame == true
            ? mRaster.Load(mDevice, mSurface, source, mExtent, extent, error)
            : mGraph.Load(mDevice, mSurface, source, profile, mExtent, candidate.Samples, extent, error);
    if (isLoaded == false)
    {
        finishCandidate(QStringLiteral("failed"), error);
        return;
    }
    if (extent != mExtent)
    {
        finishCandidate(QStringLiteral("failed"),
                        QStringLiteral("The graph produces %1 at the frozen %2 extent. Choose a full-size graph "
                                       "output or author it at scale 1.")
                            .arg(DescribeExtent(extent), DescribeExtent(mExtent)));
        return;
    }
    const bool hasStarted =
        candidate.IsDisplayFrame == true ? mRaster.Begin(mScene, mView, error) : mGraph.Begin(mScene, mView, error);
    if (hasStarted == false)
    {
        finishCandidate(QStringLiteral("failed"), error);
        return;
    }
    mPhase = candidate.IsDisplayFrame == true ? Phase::DisplayFrame : Phase::Graph;
}

void GraphComparisonRun::finishCandidate(const QString& state, const QString& message)
{
    const GraphComparisonCandidate& candidate = mCandidates.at(mCandidateIndex);
    const QDir directory(mDirectory);
    const QString stem = MakeCandidateStem(mCandidateIndex);
    const QString imageName = stem + QStringLiteral(".png");
    QJsonObject result{
        {QStringLiteral("label"), candidate.Label},
        {QStringLiteral("state"), state},
        {QStringLiteral("message"), message},
        {QStringLiteral("requestedSamples"), static_cast<qint64>(candidate.Samples)},
        {QStringLiteral("expectedBounces"), static_cast<qint64>(candidate.ExpectedBounces)},
        {QStringLiteral("actualBounces"), QJsonValue::Null},
        {QStringLiteral("mode"),
         candidate.IsDisplayFrame == true ? QStringLiteral("display_frame") : QStringLiteral("accumulation")},
    };
    const renderer::RenderGraphCaptureInfo graph = mGraph.GetInfo();
    QImage image;
    QString error;
    if (mPhase == Phase::Graph && mGraph.GetPixels().IsEmpty() == false)
    {
        const QString captureName = stem + QStringLiteral(".lurgcap");
        result.insert(QStringLiteral("warmupSamples"), static_cast<qint64>(graph.WarmupSamples));
        result.insert(QStringLiteral("targetInvocations"), static_cast<qint64>(graph.TargetSamples));
        result.insert(QStringLiteral("minimumCommittedInvocations"), static_cast<qint64>(graph.MinimumSamples));
        result.insert(QStringLiteral("submittedInvocations"), static_cast<qint64>(graph.SubmittedSamples));
        result.insert(QStringLiteral("failedPixels"), static_cast<qint64>(graph.FailedPixels));
        result.insert(QStringLiteral("authoredAccumulationLimit"), static_cast<qint64>(graph.AuthoredLimit));
        result.insert(QStringLiteral("outputResource"), static_cast<qint64>(graph.Output));
        image = mGraph.MakeDisplayImage();
        if (mGraph.Save(directory.filePath(captureName), error) == true)
        {
            result.insert(QStringLiteral("image"), imageName);
            result.insert(QStringLiteral("capture"), captureName);
        }
    }
    else if (mPhase == Phase::DisplayFrame && mRaster.GetImage().isNull() == false)
    {
        image = mRaster.GetImage();
        result.insert(QStringLiteral("renderedFrames"), static_cast<qint64>(mRaster.GetFrameCount()));
        result.insert(QStringLiteral("wasStillAccumulating"), mRaster.WasStillAccumulating());
        if (WritePngAtomically(directory.filePath(imageName), image, error) == true)
        {
            result.insert(QStringLiteral("image"), imageName);
        }
    }
    if (error.isEmpty() == false)
    {
        result.insert(QStringLiteral("exportError"), error);
    }
    if (image.isNull() == false)
    {
        ReferenceComparison comparison;
        QImage difference;
        const BorrowedView<const renderer::ReferencePixel> reference{mReferencePixels.GetData(),
                                                                     mReferencePixels.GetSize()};
        const bool hasReferenceFailed = mBaselineState == QStringLiteral("failed");
        bool hasMetrics = false;
        if (mDisplayBaseline.isNull() == false)
        {
            hasMetrics =
                CompareDisplayImages(mDisplayBaseline, image, mGraph.GetPixels(), comparison, difference, error);
        }
        else if (candidate.IsDisplayFrame == true)
        {
            hasMetrics = CompareRasterWithReference(reference,
                                                    mReferenceInfo,
                                                    hasReferenceFailed,
                                                    mRaster.GetTexture(),
                                                    comparison,
                                                    difference,
                                                    error);
        }
        else
        {
            hasMetrics = CompareGraphCaptureWithReference(reference,
                                                          mReferenceInfo,
                                                          hasReferenceFailed,
                                                          mGraph.GetPixels(),
                                                          graph.Width,
                                                          graph.Height,
                                                          comparison,
                                                          difference,
                                                          error);
        }
        if (hasMetrics == true)
        {
            result.insert(QStringLiteral("metrics"), MakeReferenceComparisonJson(comparison));
            const QString space = comparison.Space == ReferenceComparisonSpace::Linear
                                      ? QStringLiteral("Scene-linear RGB")
                                      : QStringLiteral("sRGB display");
            const bool isDiagnostic = hasReferenceFailed == true || state != QStringLiteral("complete") ||
                                      (candidate.IsDisplayFrame == true && mRaster.WasStillAccumulating() == true);
            result.insert(QStringLiteral("summary"),
                          QStringLiteral("%1%2 · %3 pixels · MAE %4 · RMSE %5 · max %6 · luminance bias %7")
                              .arg(space, isDiagnostic == true ? QStringLiteral(" · partial diagnostic") : QString{})
                              .arg(comparison.ComparedPixels)
                              .arg(comparison.MeanAbsoluteError)
                              .arg(comparison.RootMeanSquareError)
                              .arg(comparison.MaximumAbsoluteError)
                              .arg(comparison.MeanSignedLuminanceError));
            const QString differenceName = stem + QStringLiteral(".difference.png");
            if (WritePngAtomically(directory.filePath(differenceName), difference, error) == true)
            {
                result.insert(QStringLiteral("difference"), differenceName);
            }
            else
            {
                result.insert(QStringLiteral("differenceError"), error);
            }
        }
        else
        {
            result.insert(QStringLiteral("comparisonError"), error);
        }
    }
    mResults.append(result);
    mGraph.Release();
    mRaster.Release();
    ++mCandidateIndex;
    mHasRequestedCandidate = false;
    mPhase = Phase::NextCandidate;
    writeManifest(QStringLiteral("running"));
}

void GraphComparisonRun::finishRun(const QString& state, const QString& message)
{
    mReference.Release();
    mGraph.Release();
    mRaster.Release();
    mReferencePixels = {};
    mDisplayBaseline = {};
    mPhase = Phase::Idle;
    mStatus = QStringLiteral("%1 Results: %2").arg(message, QDir::toNativeSeparators(mDirectory));
    writeManifest(state);
    mScene = {};
}

void GraphComparisonRun::writeManifest(const QString& state)
{
    QJsonArray candidates;
    for (const GraphComparisonCandidate& candidate : mCandidates)
    {
        candidates.append(QJsonObject{
            {QStringLiteral("label"), candidate.Label},
            {QStringLiteral("requestedSamples"), static_cast<qint64>(candidate.Samples)},
            {QStringLiteral("expectedBounces"), static_cast<qint64>(candidate.ExpectedBounces)},
            {QStringLiteral("mode"),
             candidate.IsDisplayFrame == true ? QStringLiteral("display_frame") : QStringLiteral("accumulation")},
        });
    }
    QJsonObject baseline{
        {QStringLiteral("source"), mBaselineSource},
        {QStringLiteral("state"), mBaselineState},
        {QStringLiteral("error"), mBaselineError},
    };
    if (mBaselineSource.isEmpty() == true)
    {
        baseline.insert(QStringLiteral("requestedSamples"), static_cast<qint64>(mSettings.Samples));
        baseline.insert(QStringLiteral("minimumSamples"), static_cast<qint64>(mReferenceInfo.MinimumSamples));
        baseline.insert(QStringLiteral("maxDepth"), static_cast<qint64>(mSettings.MaxDepth));
        baseline.insert(QStringLiteral("centerRays"), mSettings.ShouldCenterRays);
    }
    QJsonArray worldToClip;
    for (const math::Vector4& column : mView.WorldToClip.Columns)
    {
        worldToClip.append(static_cast<float64>(column.X));
        worldToClip.append(static_cast<float64>(column.Y));
        worldToClip.append(static_cast<float64>(column.Z));
        worldToClip.append(static_cast<float64>(column.W));
    }
    const QJsonObject manifest{
        {QStringLiteral("state"), state},
        {QStringLiteral("width"), mExtent.width()},
        {QStringLiteral("height"), mExtent.height()},
        {QStringLiteral("sceneIdentity"), QString::fromLatin1(mScene.Identity.toHex())},
        {QStringLiteral("graphGeneration"), QString::fromLatin1(mGenerationIdentity.toHex())},
        {QStringLiteral("baseline"), baseline},
        {QStringLiteral("worldToClip"), worldToClip},
        {QStringLiteral("cameraPosition"),
         QJsonArray{static_cast<float64>(mView.CameraPosition.X),
                    static_cast<float64>(mView.CameraPosition.Y),
                    static_cast<float64>(mView.CameraPosition.Z)}},
        {QStringLiteral("orthographic"), mView.OrthographicCamera},
        {QStringLiteral("captureDisplayTransform"), QStringLiteral("Exposure one, Khronos PBR Neutral, sRGB")},
        {QStringLiteral("candidates"), candidates},
        {QStringLiteral("results"), mResults},
    };
    QSaveFile file(QDir(mDirectory).filePath(QStringLiteral("comparison.json")));
    const QByteArray bytes = QJsonDocument(manifest).toJson();
    if (file.open(QIODevice::WriteOnly) == false || file.write(bytes) != bytes.size() || file.commit() == false)
    {
        mStatus += QStringLiteral(" Could not write %1: %2. Choose a writable results folder.")
                       .arg(file.fileName(), file.errorString());
    }
}

void GraphComparisonRun::Stop()
{
    if (IsRunning() == false)
    {
        return;
    }
    if (mPhase == Phase::Graph)
    {
        finishCandidate(QStringLiteral("partial"), QStringLiteral("Stopped; showing the last committed readback."));
    }
    else if (mPhase == Phase::Reference)
    {
        mBaselineState = QStringLiteral("partial");
        if (mReference.GetInfo(mReferenceInfo) == true)
        {
            if (mReference.Save(QDir(mDirectory).filePath(QStringLiteral("baseline.luhdr")), mBaselineError) == false)
            {
                mStatus = mBaselineError;
            }
        }
    }
    finishRun(QStringLiteral("stopped"), QStringLiteral("Comparison stopped."));
}

bool GraphComparisonRun::IsRunning() const noexcept
{
    return mPhase != Phase::Idle;
}

const QString& GraphComparisonRun::GetStatus() const noexcept
{
    return mStatus;
}

const QString& GraphComparisonRun::GetDirectory() const noexcept
{
    return mDirectory;
}

const QJsonArray& GraphComparisonRun::GetResults() const noexcept
{
    return mResults;
}
} // namespace ludus::editor
