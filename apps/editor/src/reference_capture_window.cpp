#include "internal/reference_capture_window.h"

#include "internal/render_preview.h"
#include "internal/viewport_reference_capture.h"

#include <ludus/graphics/rhi/ray.h>

#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
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
} // namespace

ReferenceCaptureWindow::ReferenceCaptureWindow(RenderPreview* preview, QWidget* parentOrNull)
    : QWidget(parentOrNull, Qt::Window), mPreview(preview), mLongSide(new QSpinBox(this)), mSamples(new QSpinBox(this)),
      mMaxDepth(new QSpinBox(this)), mTraversal(new QComboBox(this)),
      mCaptureButton(new QPushButton(QStringLiteral("Capture viewport"), this)),
      mStopButton(new QPushButton(QStringLiteral("Stop"), this)),
      mSaveButton(new QPushButton(QStringLiteral("Save…"), this)), mProgress(new QProgressBar(this)),
      mStatus(new QLabel(this)), mImageLabel(new QLabel(this))
{
    setObjectName(QStringLiteral("referenceCaptureWindow"));
    setWindowTitle(QStringLiteral("Reference capture"));
    mLongSide->setObjectName(QStringLiteral("referenceLongSide"));
    mLongSide->setRange(static_cast<int32>(MINIMUM_REFERENCE_LONG_SIDE),
                        static_cast<int32>(MAXIMUM_REFERENCE_LONG_SIDE));
    mLongSide->setValue(static_cast<int32>(ViewportReferenceSettings{}.LongSide));
    mLongSide->setSuffix(QStringLiteral(" px"));
    mLongSide->setToolTip(QStringLiteral("Long side of the image; the short side follows the viewport's aspect."));
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
    QFormLayout* form = new QFormLayout;
    form->addRow(QStringLiteral("Long side"), mLongSide);
    form->addRow(QStringLiteral("Samples"), mSamples);
    form->addRow(QStringLiteral("Max depth"), mMaxDepth);
    form->addRow(QStringLiteral("Traversal"), mTraversal);

    mCaptureButton->setObjectName(QStringLiteral("referenceCapture"));
    mCaptureButton->setToolTip(QStringLiteral("Render the viewport's current camera and scene. Navigating or "
                                              "editing afterwards does not change a running capture."));
    mStopButton->setObjectName(QStringLiteral("referenceStop"));
    mSaveButton->setObjectName(QStringLiteral("referenceSave"));
    mSaveButton->setToolTip(QStringLiteral("Write the linear LUHDR01 capture and a display PNG beside it. "
                                           "An unfinished capture is saved as partial."));
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
    mImageLabel->setObjectName(QStringLiteral("referenceImage"));
    mImageLabel->setAlignment(Qt::AlignCenter);
    mImageLabel->setMinimumSize(MINIMUM_IMAGE_EXTENT, MINIMUM_IMAGE_EXTENT);
    mImageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mImageLabel->setAccessibleName(QStringLiteral("Reference image"));

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addLayout(buttons);
    layout->addWidget(mProgress);
    layout->addWidget(mStatus);
    layout->addWidget(mImageLabel, 1);

    mServiceTimer.setInterval(0);
    connect(&mServiceTimer, &QTimer::timeout, this, &ReferenceCaptureWindow::service);
    connect(mCaptureButton, &QPushButton::clicked, this, [this]() { static_cast<void>(StartCapture()); });
    connect(mStopButton, &QPushButton::clicked, this, &ReferenceCaptureWindow::StopCapture);
    connect(mSaveButton, &QPushButton::clicked, this, &ReferenceCaptureWindow::saveWithDialog);
    if (mPreview.isNull() == false)
    {
        // The capture's renderer lives on the preview's device and must go before it.
        connect(mPreview, &RenderPreview::DeviceStopping, this, [this]() {
            if (mCapture.GetPhase() != ViewportReferencePhase::Idle)
            {
                mServiceTimer.stop();
                mCapture.Release();
                mMessage = QStringLiteral("The viewport device stopped; the capture was cancelled.");
                updateControls();
                updateStatus();
                Q_EMIT CaptureFinished();
            }
        });
    }
    mMessage = QStringLiteral("Captures the viewport's current camera with the RT1 progressive path tracer.");
    updateTraversalAvailability();
    updateControls();
    updateStatus();
}

ReferenceCaptureWindow::~ReferenceCaptureWindow()
{
    mServiceTimer.stop();
    mCapture.Release();
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

bool ReferenceCaptureWindow::StartCapture()
{
    mServiceTimer.stop();
    mCapture.Release();
    mImage = {};
    mImageLabel->clear();
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
    const QSize extent = ComputeReferenceExtent(mPreview->GetDrawableSize(), settings.LongSide);
    renderer::ViewDescription view;
    if (extent.isEmpty() == true || mPreview->DescribeView(static_cast<uint32>(extent.width()),
                                                           static_cast<uint32>(extent.height()),
                                                           view,
                                                           error) == false)
    {
        mMessage = QStringLiteral("The viewport camera cannot be described: %1").arg(error);
        updateControls();
        updateStatus();
        return false;
    }
    if (mCapture
            .Start(mPreview->GetDevice(), mPreview->GetSurface(), mPreview->Scene(), view, extent, settings, error) ==
        false)
    {
        mMessage = error;
        updateControls();
        updateStatus();
        return false;
    }
    mMessage = QStringLiteral("%1 × %2, %3 samples, depth %4, %5.")
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
    if (mCapture.GetPhase() == ViewportReferencePhase::Idle)
    {
        return;
    }
    mServiceTimer.stop();
    mCapture.Release();
    mMessage = QStringLiteral("Capture stopped.");
    updateControls();
    updateStatus();
    Q_EMIT CaptureFinished();
}

bool ReferenceCaptureWindow::SaveCapture(const QString& hdrPath, QString& outError) const
{
    return mCapture.Save(hdrPath, outError);
}

void ReferenceCaptureWindow::service()
{
    const ViewportReferencePhase phase = mCapture.Service(REFERENCE_SERVICE_BUDGET_NANOSECONDS);
    const bool isFinished = phase != ViewportReferencePhase::Preparing && phase != ViewportReferencePhase::Tracing;
    if (isFinished == true)
    {
        mServiceTimer.stop();
    }
    if (isFinished == true || mRefreshTimer.elapsed() >= REFERENCE_DISPLAY_REFRESH_MILLISECONDS)
    {
        refreshImage();
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
        showScaledImage();
    }
    mProgress->setValue(static_cast<int32>(info.MinimumSamples));
}

void ReferenceCaptureWindow::showScaledImage()
{
    if (mImage.isNull() == true)
    {
        return;
    }
    mImageLabel->setPixmap(
        QPixmap::fromImage(mImage).scaled(mImageLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void ReferenceCaptureWindow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    showScaledImage();
}

void ReferenceCaptureWindow::updateControls()
{
    const ViewportReferencePhase phase = mCapture.GetPhase();
    const bool isRunning = phase == ViewportReferencePhase::Preparing || phase == ViewportReferencePhase::Tracing;
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
    else
    {
        mMessage = QStringLiteral("Saved %1 and %2.").arg(path, ViewportReferenceCapture::GetDisplayImagePath(path));
    }
    updateStatus();
}
} // namespace ludus::editor
