#include "internal/celestial_time_lapse_window.h"

#include "internal/captured_image_view.h"
#include "internal/render_graph_preview.h"
#include "internal/render_preview.h"
#include "internal/world_celestial_light.h"
#include "internal/world_earth_time_field.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/gameplay/world/celestial_transform.hpp>
#include <ludus/graphics/renderer/atmosphere.hpp>
#include <ludus/graphics/renderer/tone_mapping.hpp>

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <new>

namespace ludus::editor
{
namespace
{
using foundation::float64;
using foundation::int32;
using foundation::uint32;
namespace rr = graphics::renderer;
namespace math = foundation::math;

constexpr int32 DEFAULT_DURATION_SECONDS = 5;
constexpr int32 MAX_DURATION_SECONDS = 30;
constexpr int32 DEFAULT_FRAME_RATE = 24;
constexpr int32 MAX_FRAME_RATE = 60;
constexpr int32 HOURS_PER_DAY = 24;
constexpr int32 DEFAULT_WINDOW_WIDTH = 940;
constexpr int32 DEFAULT_WINDOW_HEIGHT = 520;
constexpr int32 SCRUB_DELAY_MILLISECONDS = 150;
constexpr uint32 SCRUB_LONG_SIDE = 480;
constexpr uint32 SCRUB_SAMPLES = 4;
constexpr foundation::uint64 SERVICE_BUDGET_NANOSECONDS = 8'000'000;
constexpr int32 FRAME_NAME_DIGITS = 4;
constexpr int32 DECIMAL_RADIX = 10;
constexpr qsizetype MAX_ENCODER_DIAGNOSTIC = 600;

QString GetFrameName(int32 ordinal)
{
    return QStringLiteral("frame-%1.png").arg(ordinal, FRAME_NAME_DIGITS, DECIMAL_RADIX, QLatin1Char('0'));
}

QString WriteFrame(const QString& path, const QImage& image)
{
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) == false || image.save(&file, "PNG") == false || file.commit() == false)
    {
        return QStringLiteral("Could not write %1. Check free space and choose another destination. %2")
            .arg(path, file.errorString());
    }
    return {};
}

void MeterExposure(const RenderScene& scene, rr::ViewDescription& inoutView)
{
    const rr::AtmosphereDescription atmosphere = scene.GetAtmosphereForView();
    float64 skyIlluminance = 0;
    math::Vector3 upWorld;
    if (rr::MeterAtmosphereIlluminance(atmosphere, skyIlluminance) == rr::AtmosphereEnvironmentStatus::Ready &&
        math::TryNormalize(atmosphere.SurfaceUpWorld, upWorld) == math::MathStatus::Success)
    {
        inoutView.Exposure = rr::ExposureMode::Physical;
        inoutView.ExposureValue100 =
            rr::ComputeIncidentExposureValue100(skyIlluminance + scene.MeterDirectionalIlluminance(upWorld));
    }
}
} // namespace

bool TryGetCelestialFrameTime(float64 startUtcDays,
                              float64 endUtcDays,
                              int32 frame,
                              int32 frameCount,
                              float64& outUtcDays) noexcept
{
    constexpr float64 maxDays = gameplay::world::MAX_EARTH_CELESTIAL_DAYS;
    if (math::IsFinite(startUtcDays) == false || math::IsFinite(endUtcDays) == false || startUtcDays < -maxDays ||
        endUtcDays > maxDays || startUtcDays >= endUtcDays || frameCount < 2 || frame < 0 || frame >= frameCount)
    {
        return false;
    }
    outUtcDays = frame == frameCount - 1
                     ? endUtcDays
                     : startUtcDays + (endUtcDays - startUtcDays) * (static_cast<float64>(frame) / (frameCount - 1));
    return true;
}

CelestialTimeLapseWindow::CelestialTimeLapseWindow(RenderPreview& inoutPreview,
                                                   RenderGraphPreview* liveGraphOrNull,
                                                   const CelestialTimeLapseSettings& settings,
                                                   QWidget& inoutParent)
    : QWidget(&inoutParent, Qt::Window), mPreview(&inoutPreview), mLiveGraph(liveGraphOrNull),
      mScene(inoutPreview.Scene()), mSettings(settings), mDuration(new QSpinBox(this)), mFrameRate(new QSpinBox(this)),
      mTimeline(new QSlider(Qt::Horizontal, this)), mInstant(new QLabel(this)), mStatus(new QLabel(this)),
      mExport(new QPushButton(QStringLiteral("Export MP4…"), this)),
      mStop(new QPushButton(QStringLiteral("Stop"), this)),
      mImage(new CapturedImageView(QStringLiteral("time-lapse-frame"), this))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Celestial Time-lapse"));
    resize(DEFAULT_WINDOW_WIDTH, DEFAULT_WINDOW_HEIGHT);
    mDuration->setRange(1, MAX_DURATION_SECONDS);
    mDuration->setValue(DEFAULT_DURATION_SECONDS);
    mDuration->setSuffix(QStringLiteral(" s"));
    mFrameRate->setRange(2, MAX_FRAME_RATE);
    mFrameRate->setValue(DEFAULT_FRAME_RATE);
    mFrameRate->setSuffix(QStringLiteral(" fps"));
    mTimeline->setAccessibleName(QStringLiteral("Time-lapse frame"));
    mTimeline->setObjectName(QStringLiteral("celestialTimeLapseFrame"));
    mExport->setObjectName(QStringLiteral("celestialTimeLapseExport"));
    mStatus->setWordWrap(true);
    mStatus->setObjectName(QStringLiteral("celestialTimeLapseStatus"));
    QJsonObject observer;
    if (mScene.EarthLights.isEmpty() == false)
    {
        observer = mScene.EarthLights.first().Properties;
    }
    const float64 start = observer.value(WORLD_CELESTIAL_TIME).toDouble();
    const float64 end = math::Min(start + 1.0 / HOURS_PER_DAY, gameplay::world::MAX_EARTH_CELESTIAL_DAYS);
    const QString timeZone = observer.value(WORLD_CELESTIAL_TIME_ZONE).toString(QStringLiteral("UTC"));
    mStartOrNull = new WorldEarthTimeField(start, timeZone, *this);
    mEndOrNull = new WorldEarthTimeField(end, timeZone, *this);

    QFormLayout* const fields = new QFormLayout;
    fields->addRow(QStringLiteral("Start"), mStartOrNull);
    fields->addRow(QStringLiteral("End"), mEndOrNull);
    fields->addRow(QStringLiteral("Video length"), mDuration);
    fields->addRow(QStringLiteral("Frame rate"), mFrameRate);
    QHBoxLayout* const buttons = new QHBoxLayout;
    buttons->addWidget(mExport);
    buttons->addWidget(mStop);
    QVBoxLayout* const controls = new QVBoxLayout;
    QLabel* const source = new QLabel(QStringLiteral("Saved World camera\n%1").arg(mSettings.SourceLabel), this);
    source->setWordWrap(true);
    controls->addWidget(source);
    controls->addLayout(fields);
    controls->addWidget(mTimeline);
    controls->addWidget(mInstant);
    controls->addLayout(buttons);
    controls->addWidget(mStatus);
    controls->addStretch(1);
    QHBoxLayout* const layout = new QHBoxLayout(this);
    layout->addLayout(controls);
    layout->addWidget(mImage, 1);

    mScrubDelay.setSingleShot(true);
    mScrubDelay.setInterval(SCRUB_DELAY_MILLISECONDS);
    connect(&mScrubDelay, &QTimer::timeout, this, &CelestialTimeLapseWindow::startScrub);
    connect(&mService, &QTimer::timeout, this, &CelestialTimeLapseWindow::service);
    connect(&mWriter, &QFutureWatcher<QString>::finished, this, &CelestialTimeLapseWindow::writeFinished);
    connect(&mEncoder, &QProcess::finished, this, &CelestialTimeLapseWindow::encoderFinished);
    connect(&mEncoder, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
        {
            mEncoderPath.clear();
            finish(QStringLiteral("FFmpeg could not start. Choose a working FFmpeg executable and retry. %1")
                       .arg(mEncoder.errorString()));
        }
    });
    connect(&inoutPreview, &RenderPreview::DeviceStopping, this, [this]() {
        cancel(QStringLiteral("The viewport device stopped. Wait for a new frame and reopen Time-lapse."));
        mSetupError = mStatus->text();
        updateControls();
    });
    connect(mStartOrNull, &WorldEarthTimeField::Edited, this, &CelestialTimeLapseWindow::requestScrub);
    connect(mEndOrNull, &WorldEarthTimeField::Edited, this, &CelestialTimeLapseWindow::requestScrub);
    connect(mDuration, &QSpinBox::valueChanged, this, &CelestialTimeLapseWindow::requestScrub);
    connect(mFrameRate, &QSpinBox::valueChanged, this, &CelestialTimeLapseWindow::requestScrub);
    connect(mTimeline, &QSlider::valueChanged, this, &CelestialTimeLapseWindow::requestScrub);
    connect(mExport, &QPushButton::clicked, this, &CelestialTimeLapseWindow::startExport);
    connect(mStop, &QPushButton::clicked, this, [this]() {
        cancel(QStringLiteral("Stopped. Choose Export MP4 to start a new export."));
    });
    RenderScene check = mScene;
    if (mScene.HasStartView == false)
    {
        mSetupError =
            QStringLiteral("Save a camera with View → Set start view from this view, then reopen Time-lapse.");
    }
    else if (check.TrySampleEarthTime(start, mSetupError) == true && mSettings.ShouldCaptureGraph == true)
    {
        static_cast<void>(pinGraph(mSetupError));
    }
    mStatus->setText(mSetupError);
    mTimeline->setRange(0, frameCount() - 1);
    updateControls();
    requestScrub();
}

CelestialTimeLapseWindow::~CelestialTimeLapseWindow()
{
    releaseFrame();
    disconnect(&mEncoder, nullptr, this, nullptr);
    disconnect(&mWriter, nullptr, this, nullptr);
    if (mEncoder.state() != QProcess::NotRunning)
    {
        mEncoder.kill();
        static_cast<void>(mEncoder.waitForFinished());
    }
    mWriter.waitForFinished();
}

bool CelestialTimeLapseWindow::pinGraph(QString& outError)
{
    mPinnedGraph.Reset(new (std::nothrow) QTemporaryDir);
    if (mPinnedGraph.Get() == nullptr || mPinnedGraph->isValid() == false)
    {
        outError = QStringLiteral("Could not stage the graph. Free temporary disk space and reopen Time-lapse.");
        return false;
    }
    const auto pin = [this](QString& inoutPath, const QString& name) {
        if (inoutPath.isEmpty() == true)
        {
            return true;
        }
        const QString staged = mPinnedGraph->filePath(name);
        if (QFile::copy(inoutPath, staged) == false)
        {
            return false;
        }
        inoutPath = staged;
        return true;
    };
    if (pin(mSettings.Graph.PackagePath, QStringLiteral("source.lurgrt")) == false ||
        pin(mSettings.GraphProfile, QStringLiteral("capture.lurgcp")) == false)
    {
        outError = QStringLiteral("The selected graph could not be staged. Wait for it to finish building and "
                                  "reopen Time-lapse.");
        return false;
    }
    return true;
}

int32 CelestialTimeLapseWindow::frameCount() const
{
    return mDuration->value() * mFrameRate->value();
}

bool CelestialTimeLapseWindow::isExporting() const noexcept
{
    return mPhase == Phase::ProbingEncoder || mPhase == Phase::Rendering || mPhase == Phase::Writing ||
           mPhase == Phase::Encoding || mPhase == Phase::Cancelling;
}

void CelestialTimeLapseWindow::updateControls()
{
    const bool isExportingNow = isExporting();
    const bool isReady = mSetupError.isEmpty();
    mStartOrNull->setEnabled(isReady == true && isExportingNow == false);
    mEndOrNull->setEnabled(isReady == true && isExportingNow == false);
    mDuration->setEnabled(isReady == true && isExportingNow == false);
    mFrameRate->setEnabled(isReady == true && isExportingNow == false);
    mTimeline->setEnabled(isReady == true && isExportingNow == false);
    mExport->setEnabled(isReady == true && mPhase == Phase::Idle);
    mStop->setEnabled(mPhase != Phase::Idle && mPhase != Phase::Cancelling);
}

void CelestialTimeLapseWindow::requestScrub()
{
    if (mSetupError.isEmpty() == false || isExporting() == true)
    {
        return;
    }
    mTimeline->setMaximum(frameCount() - 1);
    ++mRequestedScrub;
    mScrubDelay.start();
}

void CelestialTimeLapseWindow::startScrub()
{
    if (mPhase != Phase::Idle || mSetupError.isEmpty() == false)
    {
        return;
    }
    mRunningScrub = mRequestedScrub;
    float64 utcDays = 0;
    QString error;
    if (TryGetCelestialFrameTime(mStartOrNull->GetDaysSinceEpoch(),
                                 mEndOrNull->GetDaysSinceEpoch(),
                                 mTimeline->value(),
                                 frameCount(),
                                 utcDays) == false)
    {
        mStatus->setText(QStringLiteral("Choose an End time later than Start."));
        return;
    }
    mRequestedInstant = QStringLiteral("Frame %1 of %2 · %3")
                            .arg(mTimeline->value() + 1)
                            .arg(frameCount())
                            .arg(WorldEarthTimeField::FormatDaysSinceEpoch(utcDays));
    if (startFrame(utcDays, true, error) == false)
    {
        finish(error);
        return;
    }
    mPhase = Phase::Scrubbing;
    mStatus->setText(QStringLiteral("Rendering preview for %1…").arg(mRequestedInstant));
    startService();
    updateControls();
}

bool CelestialTimeLapseWindow::startFrame(float64 utcDays, bool isScrub, QString& outError)
{
    if (mPreview.isNull() == true || mPreview->HasDevice() == false)
    {
        outError = QStringLiteral("Wait for the World viewport to draw, then reopen Time-lapse.");
        return false;
    }
    RenderScene scene = mScene;
    if (scene.TrySampleEarthTime(utcDays, outError) == false)
    {
        return false;
    }
    ViewportReferenceSettings reference = mSettings.Reference;
    QSize extent = mSettings.Extent;
    if (isScrub == true)
    {
        reference.LongSide = math::Min(reference.LongSide, SCRUB_LONG_SIDE);
        reference.Samples = math::Min(reference.Samples, SCRUB_SAMPLES);
        extent = ComputeReferenceExtent(extent, reference.LongSide);
    }
    if (mSettings.ShouldCaptureGraph == true && mGraph.Load(mPreview->GetDevice(),
                                                            mPreview->GetSurface(),
                                                            mSettings.Graph,
                                                            mSettings.GraphProfile,
                                                            extent,
                                                            reference.Samples,
                                                            extent,
                                                            outError) == false)
    {
        return false;
    }
    rr::ViewDescription view;
    if (extent.isEmpty() == true || scene.DescribeSavedView(static_cast<uint32>(extent.width()),
                                                            static_cast<uint32>(extent.height()),
                                                            mSettings.ShouldCaptureGraph,
                                                            view,
                                                            outError) == false)
    {
        return false;
    }
    view.Exposure = mSettings.Exposure.Exposure;
    view.ExposureValue100 = mSettings.Exposure.ExposureValue100;
    view.RasterExposureStops = mSettings.Exposure.RasterExposureStops;
    if (mSettings.ShouldMeterAtmosphere == true)
    {
        MeterExposure(scene, view);
    }
    mFrameDisplay = ComputeReferenceDisplayTransform(view, mSettings.ToneMappingExposure, mSettings.Display.Curve);
    if (mSettings.ShouldCaptureGraph == true)
    {
        return mGraph.Begin(scene, view, outError);
    }
    reference.Display = mFrameDisplay;
    return mReference.Start(mPreview->GetDevice(), mPreview->GetSurface(), scene, view, extent, reference, outError);
}

void CelestialTimeLapseWindow::startExport()
{
    float64 utcDays = 0;
    mStartUtcDays = mStartOrNull->GetDaysSinceEpoch();
    mEndUtcDays = mEndOrNull->GetDaysSinceEpoch();
    mExportFrameCount = frameCount();
    if (TryGetCelestialFrameTime(mStartUtcDays, mEndUtcDays, 0, mExportFrameCount, utcDays) == false)
    {
        mStatus->setText(QStringLiteral("Choose an End time later than Start."));
        return;
    }
    mScrubDelay.stop();
    if (mEncoderPath.isEmpty() == true)
    {
        mEncoderPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (mEncoderPath.isEmpty() == true)
        {
            mEncoderPath = QFileDialog::getOpenFileName(this, QStringLiteral("Choose the FFmpeg executable"));
        }
    }
    if (mEncoderPath.isEmpty() == true || QFileInfo(mEncoderPath).isExecutable() == false)
    {
        mEncoderPath.clear();
        mStatus->setText(QStringLiteral("Install FFmpeg with libx264, then choose its executable to export MP4."));
        return;
    }
    mDestination = QFileDialog::getSaveFileName(this,
                                                QStringLiteral("Export time-lapse"),
                                                QStringLiteral("time-lapse.mp4"),
                                                QStringLiteral("MP4 video (*.mp4)"),
                                                nullptr,
                                                QFileDialog::DontConfirmOverwrite);
    if (mDestination.isEmpty() == true)
    {
        return;
    }
    if (mDestination.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive) == false)
    {
        mDestination += QStringLiteral(".mp4");
    }
    if (QFileInfo::exists(mDestination) == true)
    {
        mStatus->setText(QStringLiteral("Choose a new video filename. %1 already exists.").arg(mDestination));
        return;
    }
    mPhase = Phase::ProbingEncoder;
    updateControls();
    mStatus->setText(QStringLiteral("Checking the MP4 encoder…"));
    mEncoder.setWorkingDirectory({});
    mEncoder.start(mEncoderPath,
                   {QStringLiteral("-nostdin"),
                    QStringLiteral("-hide_banner"),
                    QStringLiteral("-h"),
                    QStringLiteral("encoder=libx264")});
}

void CelestialTimeLapseWindow::startRendering()
{
    const QDir directory = QFileInfo(mDestination).dir();
    mStaging.Reset(new (std::nothrow) QTemporaryDir(directory.filePath(QStringLiteral(".ludus-time-lapse-XXXXXX"))));
    if (mStaging.Get() == nullptr || mStaging->isValid() == false)
    {
        finish(QStringLiteral("Choose a destination where the editor can stage the rendered frames."));
        return;
    }
    mScrubDelay.stop();
    mExportFrame = 0;
    mExportExtent = {};
    QString error;
    if (startFrame(mStartUtcDays, false, error) == false)
    {
        finish(error);
        return;
    }
    mPhase = Phase::Rendering;
    mStatus->setText(QStringLiteral("Rendering frame 1 of %1…").arg(mExportFrameCount));
    startService();
    updateControls();
}

void CelestialTimeLapseWindow::startService()
{
    mPreview->BeginGpuCapture();
    mService.start();
}

void CelestialTimeLapseWindow::service()
{
    if (mPreview.isNull() == true || mPreview->HasOpenViewportSubmission() == true ||
        (mLiveGraph.isNull() == false && mLiveGraph->HasOpenSubmission() == true))
    {
        return;
    }
    QString error;
    QImage image;
    if (mSettings.ShouldCaptureGraph == true)
    {
        const rr::RenderGraphCapturePhase phase = mGraph.Service();
        if (mGraph.IsRunning() == true)
        {
            return;
        }
        if (phase == rr::RenderGraphCapturePhase::Complete)
        {
            image = mGraph.MakeDisplayImage(mFrameDisplay);
        }
        error = mGraph.GetError();
    }
    else
    {
        const ViewportReferencePhase phase = mReference.Service(SERVICE_BUDGET_NANOSECONDS);
        if (phase == ViewportReferencePhase::Preparing || phase == ViewportReferencePhase::Tracing)
        {
            return;
        }
        if (phase == ViewportReferencePhase::Complete)
        {
            static_cast<void>(mReference.CopyDisplayImage(image, error));
        }
        if (phase == ViewportReferencePhase::Failed)
        {
            error = mReference.GetError();
        }
    }
    if (mPhase == Phase::Scrubbing && mRunningScrub != mRequestedScrub)
    {
        releaseFrame();
        mPhase = Phase::Idle;
        mScrubDelay.start();
        updateControls();
        return;
    }
    if (image.isNull() == true)
    {
        finish(error.isEmpty() == false ? error : QStringLiteral("The selected capture did not finish an image."));
        return;
    }
    finishFrame(image);
}

void CelestialTimeLapseWindow::finishFrame(const QImage& image)
{
    releaseFrame();
    if (mPhase == Phase::Scrubbing)
    {
        mPhase = Phase::Idle;
        mImage->SetImage(image);
        mInstant->setText(mRequestedInstant);
        mStatus->setText(QStringLiteral("Preview · Export uses the quality selected in Reference."));
        updateControls();
        return;
    }
    if (mExportFrame == 0)
    {
        mExportExtent = image.size();
    }
    if (image.size() != mExportExtent)
    {
        finish(QStringLiteral("The graph changed output size during the export. Use a fixed-size output."));
        return;
    }
    mImage->SetImage(image);
    mInstant->clear();
    const QString path = mStaging->filePath(GetFrameName(mExportFrame));
    mPhase = Phase::Writing;
    mWriter.setFuture(QtConcurrent::run([path, image]() { return WriteFrame(path, image); }));
}

void CelestialTimeLapseWindow::writeFinished()
{
    if (mPhase == Phase::Cancelling)
    {
        finish(mCancelMessage);
        return;
    }
    const QString error = mWriter.result();
    if (error.isEmpty() == false)
    {
        finish(error);
        return;
    }
    ++mExportFrame;
    if (mExportFrame == mExportFrameCount)
    {
        startEncoder();
        return;
    }
    float64 utcDays = 0;
    QString frameError;
    if (TryGetCelestialFrameTime(mStartUtcDays, mEndUtcDays, mExportFrame, mExportFrameCount, utcDays) == false ||
        startFrame(utcDays, false, frameError) == false)
    {
        finish(QStringLiteral("Frame %1 failed. %2").arg(mExportFrame + 1).arg(frameError));
        return;
    }
    mPhase = Phase::Rendering;
    mStatus->setText(QStringLiteral("Rendering frame %1 of %2…").arg(mExportFrame + 1).arg(mExportFrameCount));
    startService();
}

void CelestialTimeLapseWindow::startEncoder()
{
    mPhase = Phase::Encoding;
    mStatus->setText(QStringLiteral("Encoding MP4…"));
    mEncoder.setWorkingDirectory(mStaging->path());
    mEncoder.start(mEncoderPath,
                   {QStringLiteral("-nostdin"),
                    QStringLiteral("-hide_banner"),
                    QStringLiteral("-loglevel"),
                    QStringLiteral("error"),
                    QStringLiteral("-framerate"),
                    QString::number(mFrameRate->value()),
                    QStringLiteral("-start_number"),
                    QStringLiteral("0"),
                    QStringLiteral("-i"),
                    QStringLiteral("frame-%04d.png"),
                    QStringLiteral("-frames:v"),
                    QString::number(mExportFrameCount),
                    QStringLiteral("-vf"),
                    QStringLiteral("scale=out_color_matrix=bt709,pad=ceil(iw/2)*2:ceil(ih/2)*2"),
                    QStringLiteral("-c:v"),
                    QStringLiteral("libx264"),
                    QStringLiteral("-pix_fmt"),
                    QStringLiteral("yuv420p"),
                    QStringLiteral("-colorspace"),
                    QStringLiteral("bt709"),
                    QStringLiteral("-color_primaries"),
                    QStringLiteral("bt709"),
                    QStringLiteral("-color_trc"),
                    QStringLiteral("iec61966-2-1"),
                    QStringLiteral("-movflags"),
                    QStringLiteral("+faststart"),
                    QStringLiteral("video.mp4")});
}

void CelestialTimeLapseWindow::encoderFinished(int32 exitCode, QProcess::ExitStatus exitStatus)
{
    if (mPhase == Phase::Cancelling)
    {
        finish(mCancelMessage);
        return;
    }
    if (mPhase == Phase::ProbingEncoder)
    {
        const QByteArray output = mEncoder.readAllStandardOutput();
        if (exitCode != 0 || exitStatus != QProcess::NormalExit || output.contains("Encoder libx264") == false)
        {
            mEncoderPath.clear();
            finish(QStringLiteral("Choose an FFmpeg build that includes the libx264 encoder."));
            return;
        }
        startRendering();
        return;
    }
    if (exitCode != 0 || exitStatus != QProcess::NormalExit)
    {
        const QString diagnostic =
            QString::fromLocal8Bit(mEncoder.readAllStandardError().right(MAX_ENCODER_DIAGNOSTIC));
        mEncoderPath.clear();
        finish(
            QStringLiteral("FFmpeg could not encode MP4. Choose a build with libx264 and retry. %1").arg(diagnostic));
        return;
    }
    if (QFile::rename(mStaging->filePath(QStringLiteral("video.mp4")), mDestination) == false)
    {
        finish(QStringLiteral("Could not publish %1. Choose a new writable destination and retry.").arg(mDestination));
        return;
    }
    finish(QStringLiteral("Saved %1 frames to %2.").arg(mExportFrameCount).arg(mDestination));
}

void CelestialTimeLapseWindow::releaseFrame() noexcept
{
    const bool wasServicing = mService.isActive();
    mService.stop();
    mGraph.Release();
    mReference.Release();
    if (wasServicing == true && mPreview.isNull() == false)
    {
        mPreview->EndGpuCapture();
    }
}

void CelestialTimeLapseWindow::cancel(const QString& message)
{
    mScrubDelay.stop();
    releaseFrame();
    if (mPhase == Phase::Writing || mPhase == Phase::Encoding || mPhase == Phase::ProbingEncoder)
    {
        mCancelMessage = message;
        mPhase = Phase::Cancelling;
        if (mEncoder.state() != QProcess::NotRunning)
        {
            mEncoder.kill();
        }
        mStatus->setText(message);
        updateControls();
        return;
    }
    if (mPhase != Phase::Cancelling)
    {
        finish(message);
    }
}

void CelestialTimeLapseWindow::finish(const QString& message)
{
    releaseFrame();
    mStaging.Reset();
    mPhase = Phase::Idle;
    mStatus->setText(message);
    updateControls();
}
} // namespace ludus::editor
