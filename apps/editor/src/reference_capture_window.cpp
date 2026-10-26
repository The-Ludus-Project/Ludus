#include "internal/reference_capture_window.h"

#include "internal/reference_comparison.h"
#include "internal/render_preview.h"
#include "internal/texture_inspection.h"
#include "internal/viewport_reference_capture.h"
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/render_graph_preview.h"
#    include "internal/viewport_raster_capture.h"
#endif

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>

#include <ludus/graphics/rhi/ray.h>

#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
using namespace foundation;
namespace renderer = graphics::renderer;
/// Combo indices; they equal ReferenceTraversal values.
constexpr int32 CPU_ORACLE_INDEX = static_cast<int32>(renderer::ReferenceTraversal::CpuOracle);
constexpr int32 NATIVE_QUERIES_INDEX = static_cast<int32>(renderer::ReferenceTraversal::NativeQueries);
constexpr int32 MINIMUM_IMAGE_EXTENT = 160;
constexpr float64 MILLISECONDS_PER_SECOND = 1000;

QString DescribeTraversal(renderer::ReferenceTraversal traversal)
{
    return traversal == renderer::ReferenceTraversal::CpuOracle ? QStringLiteral("CPU oracle")
                                                                : QStringLiteral("native ray queries");
}

void ConfigureImageLabel(QLabel& inoutLabel, const QString& objectName, const QString& accessibleName)
{
    inoutLabel.setObjectName(objectName);
    inoutLabel.setAlignment(Qt::AlignCenter);
    inoutLabel.setMinimumSize(MINIMUM_IMAGE_EXTENT, MINIMUM_IMAGE_EXTENT);
    inoutLabel.setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    inoutLabel.setAccessibleName(accessibleName);
}

void ShowScaled(QLabel& inoutLabel, const QImage& image)
{
    if (image.isNull() == true)
    {
        inoutLabel.clear();
        return;
    }
    inoutLabel.setPixmap(
        QPixmap::fromImage(image).scaled(inoutLabel.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

bool WriteAtomically(const QString& path, const QByteArray& bytes, QString& outError)
{
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) == false || file.write(bytes) != bytes.size() || file.commit() == false)
    {
        outError = QStringLiteral("Could not write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool WriteImageAtomically(const QString& path, const QImage& image, QString& outError)
{
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) == false || image.save(&file, "PNG") == false || file.commit() == false)
    {
        outError = QStringLiteral("Could not write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}
} // namespace

ReferenceCaptureWindow::ReferenceCaptureWindow(RenderPreview* preview, QWidget* parentOrNull)
    : QWidget(parentOrNull, Qt::Window), mPreview(preview), mLongSide(new QSpinBox(this)), mSamples(new QSpinBox(this)),
      mMaxDepth(new QSpinBox(this)), mTraversal(new QComboBox(this)),
      mCompare(new QCheckBox(QStringLiteral("Compare with the viewport's render graph output"), this)),
      mCaptureButton(new QPushButton(QStringLiteral("Capture viewport"), this)),
      mStopButton(new QPushButton(QStringLiteral("Stop"), this)),
      mSaveButton(new QPushButton(QStringLiteral("Save…"), this)), mProgress(new QProgressBar(this)),
      mStatus(new QLabel(this)), mComparisonText(new QLabel(this)), mImages(new QTabWidget(this)),
      mImageLabel(new QLabel(this)), mRasterLabel(new QLabel(this)), mDifferenceLabel(new QLabel(this))
{
    setObjectName(QStringLiteral("referenceCaptureWindow"));
    setWindowTitle(QStringLiteral("Reference capture"));
    mLongSide->setObjectName(QStringLiteral("referenceLongSide"));
    mLongSide->setRange(static_cast<int32>(MINIMUM_REFERENCE_LONG_SIDE),
                        static_cast<int32>(MAXIMUM_REFERENCE_LONG_SIDE));
    mLongSide->setValue(static_cast<int32>(ViewportReferenceSettings{}.LongSide));
    mLongSide->setSuffix(QStringLiteral(" px"));
    mLongSide->setToolTip(QStringLiteral("Long side of the image; the short side follows the viewport's aspect. "
                                         "A comparison uses the render graph's output extent for this size."));
    mSamples->setObjectName(QStringLiteral("referenceSamples"));
    mSamples->setRange(1, static_cast<int32>(MAXIMUM_REFERENCE_SAMPLES));
    mSamples->setValue(static_cast<int32>(ViewportReferenceSettings{}.Samples));
    mSamples->setToolTip(QStringLiteral("Samples per pixel at completion. Noise falls with the square root "
                                        "of the sample count."));
    mMaxDepth->setObjectName(QStringLiteral("referenceMaxDepth"));
    mMaxDepth->setRange(1, static_cast<int32>(MAXIMUM_REFERENCE_DEPTH));
    mMaxDepth->setValue(static_cast<int32>(ViewportReferenceSettings{}.MaxDepth));
    mMaxDepth->setToolTip(QStringLiteral("Maximum scattering events per path. Light needing more bounces is "
                                         "omitted, which darkens the image slightly."));
    mTraversal->setObjectName(QStringLiteral("referenceTraversal"));
    // Item order follows ReferenceTraversal so indices convert directly.
    mTraversal->insertItem(NATIVE_QUERIES_INDEX, QStringLiteral("Native ray queries"));
    mTraversal->insertItem(CPU_ORACLE_INDEX, QStringLiteral("CPU oracle"));
    mTraversal->setCurrentIndex(CPU_ORACLE_INDEX);
    mTraversal->setToolTip(QStringLiteral("CPU oracle traces a CPU BVH and runs everywhere. Native ray queries "
                                          "use the GPU's ray tracing hardware; they never fall back to the CPU."));
    mCompare->setObjectName(QStringLiteral("referenceCompare"));
    mCompare->setChecked(true);
    QFormLayout* form = new QFormLayout;
    form->addRow(QStringLiteral("Long side"), mLongSide);
    form->addRow(QStringLiteral("Samples"), mSamples);
    form->addRow(QStringLiteral("Max depth"), mMaxDepth);
    form->addRow(QStringLiteral("Traversal"), mTraversal);
    form->addRow(mCompare);

    mCaptureButton->setObjectName(QStringLiteral("referenceCapture"));
    mCaptureButton->setToolTip(QStringLiteral("Render the viewport's current camera and scene. Navigating or "
                                              "editing afterwards does not change a running capture."));
    mStopButton->setObjectName(QStringLiteral("referenceStop"));
    mSaveButton->setObjectName(QStringLiteral("referenceSave"));
    mSaveButton->setToolTip(QStringLiteral("Write the linear LUHDR01 capture and a display PNG beside it, plus the "
                                           "raster image, difference image and metrics of a comparison. An "
                                           "unfinished capture is saved as partial."));
    QHBoxLayout* buttons = new QHBoxLayout;
    buttons->addWidget(mCaptureButton);
    buttons->addWidget(mStopButton);
    buttons->addStretch(1);
    buttons->addWidget(mSaveButton);

    mProgress->setObjectName(QStringLiteral("referenceProgress"));
    mProgress->setRange(0, 1);
    mProgress->setValue(0);
    mStatus->setObjectName(QStringLiteral("referenceStatus"));
    mStatus->setWordWrap(true);
    mStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mComparisonText->setObjectName(QStringLiteral("referenceComparison"));
    mComparisonText->setWordWrap(true);
    mComparisonText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ConfigureImageLabel(*mImageLabel, QStringLiteral("referenceImage"), QStringLiteral("Reference image"));
    ConfigureImageLabel(*mRasterLabel, QStringLiteral("referenceRasterImage"), QStringLiteral("Raster image"));
    ConfigureImageLabel(*mDifferenceLabel,
                        QStringLiteral("referenceDifferenceImage"),
                        QStringLiteral("Raster minus reference difference image"));
    mDifferenceLabel->setToolTip(QStringLiteral("Red: the raster image is brighter than the reference. Blue: it is "
                                                "darker. Black: they agree. Grey: no reference sample yet."));
    mImages->setObjectName(QStringLiteral("referenceImages"));
    mImages->addTab(mImageLabel, QStringLiteral("Reference"));
    mImages->addTab(mRasterLabel, QStringLiteral("Raster"));
    mImages->addTab(mDifferenceLabel, QStringLiteral("Difference"));

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addLayout(buttons);
    layout->addWidget(mProgress);
    layout->addWidget(mStatus);
    layout->addWidget(mComparisonText);
    layout->addWidget(mImages, 1);

    mServiceTimer.setInterval(0);
    connect(&mServiceTimer, &QTimer::timeout, this, &ReferenceCaptureWindow::service);
    connect(mCaptureButton, &QPushButton::clicked, this, [this]() { static_cast<void>(StartCapture()); });
    connect(mStopButton, &QPushButton::clicked, this, &ReferenceCaptureWindow::StopCapture);
    connect(mSaveButton, &QPushButton::clicked, this, &ReferenceCaptureWindow::saveWithDialog);
    connect(mImages, &QTabWidget::currentChanged, this, [this]() { showScaledImages(); });
    if (mPreview.isNull() == false)
    {
        // Both captures' renderers live on the preview's device and must go before it.
        connect(mPreview, &RenderPreview::DeviceStopping, this, [this]() {
            if (mCapture.GetPhase() != ViewportReferencePhase::Idle || isRasterRendering() == true)
            {
                mServiceTimer.stop();
                releaseCaptures();
                mMessage = QStringLiteral("The viewport device stopped; the capture was cancelled.");
                updateControls();
                updateStatus();
                Q_EMIT CaptureFinished();
            }
        });
    }
    mMessage = QStringLiteral("Captures the viewport's current camera with the RT1 progressive path tracer.");
    updateTraversalAvailability();
    updateComparisonAvailability();
    updateControls();
    updateStatus();
}

ReferenceCaptureWindow::~ReferenceCaptureWindow()
{
    mServiceTimer.stop();
    releaseCaptures();
}

void ReferenceCaptureWindow::SetProjectGraph(RenderGraphPreview* graphOrNull)
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    mProjectGraph = graphOrNull;
#else
    static_cast<void>(graphOrNull);
#endif
    updateComparisonAvailability();
}

void ReferenceCaptureWindow::releaseCaptures() noexcept
{
    mCapture.Release();
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    mRasterCapture.Release();
#endif
}

bool ReferenceCaptureWindow::isRasterRendering() const noexcept
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    return mRasterCapture.GetPhase() == ViewportRasterPhase::Rendering;
#else
    return false;
#endif
}

void ReferenceCaptureWindow::updateTraversalAvailability()
{
    bool isNativeAvailable = false;
#if defined(LUDUS_EDITOR_REFERENCE_NATIVE_QUERIES)
    if (mPreview.isNull() == false && mPreview->HasDevice() == true)
    {
        graphics::rhi::RayCapabilities capabilities;
        isNativeAvailable = graphics::rhi::GetRayCapabilities(mPreview->GetDevice(), capabilities) ==
                                graphics::rhi::RasterStatus::Ready &&
                            capabilities.MaxRays >= 2;
    }
#endif
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(mTraversal->model());
    if (model != nullptr)
    {
        model->item(NATIVE_QUERIES_INDEX)->setEnabled(isNativeAvailable);
    }
    if (isNativeAvailable == false && mTraversal->currentIndex() == NATIVE_QUERIES_INDEX)
    {
        mTraversal->setCurrentIndex(CPU_ORACLE_INDEX);
    }
}

void ReferenceCaptureWindow::updateComparisonAvailability()
{
    bool isAvailable = false;
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    isAvailable = mProjectGraph.isNull() == false && mProjectGraph->GetRequestedPackagePath().isEmpty() == false;
#endif
    mIsComparisonAvailable = isAvailable;
    mCompare->setToolTip(
        isAvailable == true
            ? QStringLiteral("Also render the viewport's project render graph, with its output and Force raster "
                             "choice, at the same camera and extent, and report raster-minus-reference error.")
            : QStringLiteral("Comparison needs the native editor's project render graph viewport."));
    updateControls();
}

bool ReferenceCaptureWindow::startRasterCapture(const QSize& requestedSize, QSize& outExtent, QString& outError)
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    mRasterSource = GetViewportRasterSource(*mProjectGraph);
    QSize extent;
    if (mRasterCapture
            .Load(mPreview->GetDevice(), mPreview->GetSurface(), mRasterSource, requestedSize, extent, outError) ==
        false)
    {
        return false;
    }
    renderer::ViewDescription view;
    QString error;
    if (mPreview->DescribeConventionalView(static_cast<uint32>(extent.width()),
                                           static_cast<uint32>(extent.height()),
                                           view,
                                           error) == false)
    {
        mRasterCapture.Release();
        outError = QStringLiteral("The viewport camera cannot be described for the raster capture: %1").arg(error);
        return false;
    }
    if (mRasterCapture.Begin(mPreview->Scene(), view, outError) == false)
    {
        return false;
    }
    outExtent = extent;
    return true;
#else
    static_cast<void>(requestedSize);
    static_cast<void>(outExtent);
    outError = QStringLiteral("Comparison needs the native editor's project render graph viewport.");
    return false;
#endif
}

bool ReferenceCaptureWindow::StartCapture()
{
    mServiceTimer.stop();
    releaseCaptures();
    mImage = {};
    mRasterImage = {};
    mDifferenceImage = {};
    mHasComparison = false;
    mComparisonError.clear();
    showScaledImages();
    updateTraversalAvailability();
    QString error;
    if (mPreview.isNull() == true || mPreview->HasDevice() == false)
    {
        mMessage = QStringLiteral("The viewport has no GPU device yet; open a World and wait for its first frame.");
        updateControls();
        updateStatus();
        return false;
    }
    ViewportReferenceSettings settings;
    settings.LongSide = static_cast<uint32>(mLongSide->value());
    settings.Samples = static_cast<uint32>(mSamples->value());
    settings.MaxDepth = static_cast<uint32>(mMaxDepth->value());
    settings.Traversal = static_cast<renderer::ReferenceTraversal>(mTraversal->currentIndex());
    const QSize requestedExtent = ComputeReferenceExtent(mPreview->GetDrawableSize(), settings.LongSide);
    QSize extent = requestedExtent;
    mIsComparing = mIsComparisonAvailable == true && mCompare->isChecked() == true;
    // The raster capture fixes the extent first: a graph may render at a scaled resolution, and the
    // reference must cover exactly the pixels it is compared with.
    if (mIsComparing == true && startRasterCapture(requestedExtent, extent, error) == false)
    {
        mMessage = error;
        updateControls();
        updateStatus();
        return false;
    }
    renderer::ViewDescription view;
    if (extent.isEmpty() == true || mPreview->DescribeView(static_cast<uint32>(extent.width()),
                                                           static_cast<uint32>(extent.height()),
                                                           view,
                                                           error) == false)
    {
        releaseCaptures();
        mMessage = QStringLiteral("The viewport camera cannot be described: %1").arg(error);
        updateControls();
        updateStatus();
        return false;
    }
    if (mCapture
            .Start(mPreview->GetDevice(), mPreview->GetSurface(), mPreview->Scene(), view, extent, settings, error) ==
        false)
    {
        releaseCaptures();
        mMessage = error;
        updateControls();
        updateStatus();
        return false;
    }
    mMessage = QStringLiteral("%1 × %2, %3 requested samples/pixel, depth %4, %5.")
                   .arg(extent.width())
                   .arg(extent.height())
                   .arg(settings.Samples)
                   .arg(settings.MaxDepth)
                   .arg(DescribeTraversal(settings.Traversal));
    mProgress->setRange(0, mSamples->value());
    mProgress->setValue(0);
    mCaptureTimer.start();
    mRefreshTimer.start();
    mServiceTimer.start();
    updateControls();
    updateStatus();
    return true;
}

void ReferenceCaptureWindow::StopCapture()
{
    if (mCapture.GetPhase() == ViewportReferencePhase::Idle && isRasterRendering() == false)
    {
        return;
    }
    mServiceTimer.stop();
    releaseCaptures();
    mHasComparison = false;
    mMessage = QStringLiteral("Capture stopped.");
    updateControls();
    updateStatus();
    Q_EMIT CaptureFinished();
}

QString ReferenceCaptureWindow::GetComparisonPath(const QString& hdrPath, const QString& suffix)
{
    const QFileInfo info(hdrPath);
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".") + suffix);
}

bool ReferenceCaptureWindow::SaveCapture(const QString& hdrPath, QString& outError) const
{
    if (mCapture.Save(hdrPath, outError) == false)
    {
        return false;
    }
    return mHasComparison == false || saveComparison(hdrPath, outError);
}

bool ReferenceCaptureWindow::saveComparison(const QString& hdrPath, QString& outError) const
{
    QJsonObject raster;
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    raster.insert(QStringLiteral("package"), mRasterSource.PackagePath);
    raster.insert(QStringLiteral("rasterFallback"), mRasterSource.FallbackPath);
    raster.insert(QStringLiteral("outputTarget"), mRasterSource.OutputTarget);
    raster.insert(QStringLiteral("forceRaster"), mRasterSource.ShouldForceRaster);
    raster.insert(QStringLiteral("encoding"), GetEncodingName(mRasterCapture.GetTexture().Encoding));
    raster.insert(QStringLiteral("frames"), static_cast<qint64>(mRasterCapture.GetFrameCount()));
    raster.insert(QStringLiteral("stillAccumulating"), mRasterCapture.WasStillAccumulating());
#endif
    graphics::renderer::ReferenceCaptureInfo info;
    QJsonObject reference;
    if (mCapture.GetInfo(info) == true)
    {
        reference.insert(QStringLiteral("file"), QFileInfo(hdrPath).fileName());
        reference.insert(QStringLiteral("minimumSamples"), static_cast<qint64>(info.MinimumSamples));
        reference.insert(QStringLiteral("targetSamples"), static_cast<qint64>(info.TargetSamples));
        reference.insert(QStringLiteral("maxDepth"), static_cast<qint64>(info.MaxDepth));
        reference.insert(QStringLiteral("complete"), info.Complete);
        reference.insert(QStringLiteral("failed"), mCapture.GetPhase() == ViewportReferencePhase::Failed);
        reference.insert(QStringLiteral("error"), mCapture.GetError());
        reference.insert(QStringLiteral("traversal"), DescribeTraversal(info.Traversal));
        reference.insert(QStringLiteral("seed"), QString::number(info.Seed));
    }
    const QJsonObject report{
        {QStringLiteral("reference"), reference},
        {QStringLiteral("raster"), raster},
        {QStringLiteral("metrics"), MakeReferenceComparisonJson(mComparison)},
    };
    return WriteImageAtomically(GetComparisonPath(hdrPath, QStringLiteral("raster.png")), mRasterImage, outError) ==
               true &&
           WriteImageAtomically(GetComparisonPath(hdrPath, QStringLiteral("difference.png")),
                                mDifferenceImage,
                                outError) == true &&
           WriteAtomically(GetComparisonPath(hdrPath, QStringLiteral("comparison.json")),
                           QJsonDocument(report).toJson(),
                           outError) == true;
}

void ReferenceCaptureWindow::service()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    const bool wasRasterRendering = isRasterRendering();
    const ViewportRasterPhase rasterPhase = mRasterCapture.Service();
    const bool hasRasterFinished = wasRasterRendering == true && rasterPhase != ViewportRasterPhase::Rendering;
    if (hasRasterFinished == true && rasterPhase == ViewportRasterPhase::Complete)
    {
        mRasterImage = mRasterCapture.GetImage();
    }
    else if (hasRasterFinished == true)
    {
        mComparisonError = mRasterCapture.GetError();
    }
#else
    const bool hasRasterFinished = false;
#endif
    const ViewportReferencePhase phase = mCapture.Service(REFERENCE_SERVICE_BUDGET_NANOSECONDS);
    const bool isReferenceFinished =
        phase != ViewportReferencePhase::Preparing && phase != ViewportReferencePhase::Tracing;
    const bool isFinished = isReferenceFinished == true && isRasterRendering() == false;
    if (isFinished == true)
    {
        mServiceTimer.stop();
    }
    if (isFinished == true || hasRasterFinished == true ||
        mRefreshTimer.elapsed() >= REFERENCE_DISPLAY_REFRESH_MILLISECONDS)
    {
        refreshImage();
        updateComparison();
        showScaledImages();
        mRefreshTimer.restart();
        updateControls();
        updateStatus();
    }
    if (isFinished == true)
    {
        Q_EMIT CaptureFinished();
    }
}

void ReferenceCaptureWindow::refreshImage()
{
    graphics::renderer::ReferenceCaptureInfo info;
    if (mCapture.GetInfo(info) == false)
    {
        return;
    }
    QString error;
    QImage image;
    if (mCapture.CopyDisplayImage(image, error) == true)
    {
        mImage = image;
    }
    mProgress->setValue(static_cast<int32>(info.MinimumSamples));
}

void ReferenceCaptureWindow::updateComparison()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mIsComparing == false || mRasterCapture.GetPhase() != ViewportRasterPhase::Complete)
    {
        return;
    }
    Array<graphics::renderer::ReferencePixel> pixels;
    QString error;
    if (mCapture.CopyPixels(pixels, error) == false)
    {
        return;
    }
    renderer::ReferenceCaptureInfo information;
    if (mCapture.GetInfo(information) == false)
    {
        return;
    }
    ReferenceComparison comparison;
    QImage difference;
    if (CompareRasterWithReference({pixels.GetData(), pixels.GetSize()},
                                   information,
                                   mCapture.GetPhase() == ViewportReferencePhase::Failed,
                                   mRasterCapture.GetTexture(),
                                   comparison,
                                   difference,
                                   error) == false)
    {
        mComparisonError = error;
        return;
    }
    mComparison = comparison;
    mDifferenceImage = difference;
    mHasComparison = true;
    mComparisonError.clear();
#endif
}

void ReferenceCaptureWindow::showScaledImages()
{
    ShowScaled(*mImageLabel, mImage);
    ShowScaled(*mRasterLabel, mRasterImage);
    ShowScaled(*mDifferenceLabel, mDifferenceImage);
}

void ReferenceCaptureWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    showScaledImages();
}

void ReferenceCaptureWindow::updateControls()
{
    const ViewportReferencePhase phase = mCapture.GetPhase();
    const bool isRunning = phase == ViewportReferencePhase::Preparing || phase == ViewportReferencePhase::Tracing ||
                           isRasterRendering() == true;
    graphics::renderer::ReferenceCaptureInfo info;
    const bool hasSamples = mCapture.GetInfo(info) == true;
    mCaptureButton->setEnabled(isRunning == false);
    mStopButton->setEnabled(isRunning == true);
    mSaveButton->setEnabled(hasSamples == true);
    for (QWidget* setting : {static_cast<QWidget*>(mLongSide),
                             static_cast<QWidget*>(mSamples),
                             static_cast<QWidget*>(mMaxDepth),
                             static_cast<QWidget*>(mTraversal)})
    {
        setting->setEnabled(isRunning == false);
    }
    mCompare->setEnabled(isRunning == false && mIsComparisonAvailable == true);
}

QString ReferenceCaptureWindow::describeRaster() const
{
    if (mIsComparing == false)
    {
        return {};
    }
    if (mCapture.GetPhase() == ViewportReferencePhase::Failed && mHasComparison == false)
    {
        return QStringLiteral("Reference FAILED / INCOMPLETE. Comparison unavailable: %1")
            .arg(mComparisonError.isEmpty() == true ? QStringLiteral("no sampled pixels") : mComparisonError);
    }
    if (mComparisonError.isEmpty() == false)
    {
        return QStringLiteral("Comparison unavailable: %1").arg(mComparisonError);
    }
    if (mHasComparison == true)
    {
        QString text = DescribeReferenceComparison(mComparison);
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
        if (mRasterCapture.WasStillAccumulating() == true)
        {
            text += QStringLiteral(" The render graph was still accumulating after %1 frames.")
                        .arg(mRasterCapture.GetFrameCount());
        }
#endif
        return text;
    }
    return isRasterRendering() == true ? QStringLiteral("Rendering the viewport's render graph output…")
                                       : QStringLiteral("Waiting for reference samples to compare.");
}

QString ReferenceCaptureWindow::GetStatusText() const
{
    QString phase;
    switch (mCapture.GetPhase())
    {
        case ViewportReferencePhase::Idle:
            return mMessage;
        case ViewportReferencePhase::Preparing:
            phase = QStringLiteral("Preparing the reference scene");
            break;
        case ViewportReferencePhase::Tracing:
            phase = QStringLiteral("Tracing");
            break;
        case ViewportReferencePhase::Complete:
            phase = QStringLiteral("Complete");
            break;
        case ViewportReferencePhase::Failed:
            return QStringLiteral("%1\n%2").arg(mMessage, mCapture.GetError());
    }
    QString progress;
    graphics::renderer::ReferenceCaptureInfo info;
    if (mCapture.GetInfo(info) == true)
    {
        progress = QStringLiteral(" · %1/%2 samples per pixel · %3 analytic lights · %4 s")
                       .arg(info.MinimumSamples)
                       .arg(info.TargetSamples)
                       .arg(mCapture.GetLightCount())
                       .arg(static_cast<float64>(mCaptureTimer.elapsed()) / MILLISECONDS_PER_SECOND, 0, 'f', 1);
    }
    return QStringLiteral("%1\n%2%3").arg(mMessage, phase, progress);
}

void ReferenceCaptureWindow::updateStatus()
{
    mStatus->setText(GetStatusText());
    const QString comparison = describeRaster();
    mComparisonText->setText(comparison);
    mComparisonText->setVisible(comparison.isEmpty() == false);
}

void ReferenceCaptureWindow::saveWithDialog()
{
    const QString path = QFileDialog::getSaveFileName(this,
                                                      QStringLiteral("Save reference capture"),
                                                      QStringLiteral("reference.luhdr"),
                                                      QStringLiteral("Ludus HDR capture (*.luhdr)"));
    if (path.isEmpty() == true)
    {
        return;
    }
    QString error;
    if (SaveCapture(path, error) == false)
    {
        mMessage = error;
    }
    else if (mHasComparison == true)
    {
        mMessage = QStringLiteral("Saved %1, its PNG and the comparison beside it (%2).")
                       .arg(path, GetComparisonPath(path, QStringLiteral("comparison.json")));
    }
    else
    {
        mMessage = QStringLiteral("Saved %1 and %2.").arg(path, ViewportReferenceCapture::GetDisplayImagePath(path));
    }
    updateStatus();
}
} // namespace ludus::editor
