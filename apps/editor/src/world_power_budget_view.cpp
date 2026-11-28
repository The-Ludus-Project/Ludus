#include "internal/world_power_budget.h"

#include "internal/reference_capture_window.h"
#include "internal/render_preview.h"
#include "internal/render_scene.h"
#include "internal/viewport_reference_capture.h"

#include <QByteArray>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace ludus::editor
{
using namespace foundation;
namespace
{
namespace renderer = graphics::renderer;

constexpr math::Vector3d REC709_LUMINANCE{0.2126, 0.7152, 0.0722};
constexpr int32 PERCENT_SCALE = 100;
constexpr int32 FLUX_DIGITS = 8;
constexpr int32 PERCENT_DIGITS = 2;
constexpr uint32 SAMPLES_PER_PROBE = 8;
constexpr int32 SERVICE_INTERVAL_MILLISECONDS = 10;

QString FormatPower(math::Vector3d flux, float64 emitted)
{
    const float64 power = math::Dot(flux, REC709_LUMINANCE);
    const QString value = QString::number(power, 'g', FLUX_DIGITS);
    return emitted > 0 ? QStringLiteral("%1 (%2%)")
                             .arg(value, QString::number(PERCENT_SCALE * power / emitted, 'f', PERCENT_DIGITS))
                       : value;
}

struct PowerReadout final
{
    QLabel* Status;
    QLabel* Emitted;
    QLabel* Absorbed;
    QLabel* Escaped;
    QLabel* Residual;
};

PowerReadout CreatePowerReadout(QGridLayout& inoutTable, QWidget* parent, int32 column)
{
    PowerReadout result{new QLabel(parent),
                        new QLabel(parent),
                        new QLabel(parent),
                        new QLabel(parent),
                        new QLabel(parent)};
    result.Status->setWordWrap(true);
    result.Status->setTextFormat(Qt::PlainText);
    inoutTable.addWidget(result.Emitted, 1, column);
    inoutTable.addWidget(result.Absorbed, 2, column);
    constexpr int32 ESCAPED_ROW = 3;
    constexpr int32 RESIDUAL_ROW = 4;
    constexpr int32 STATUS_ROW = 5;
    inoutTable.addWidget(result.Escaped, ESCAPED_ROW, column);
    inoutTable.addWidget(result.Residual, RESIDUAL_ROW, column);
    inoutTable.addWidget(result.Status, STATUS_ROW, column);
    return result;
}

class WorldPowerBudgetView final : public QWidget
{
public:
    WorldPowerBudgetView(RenderPreview* previewOrNull, QWidget* parentOrNull)
        : QWidget(parentOrNull), mPreview(previewOrNull),
          mMeasure(new QPushButton(QStringLiteral("Measure both tracers"), this)),
          mCancel(new QPushButton(QStringLiteral("Cancel"), this)), mDepth(new QSpinBox(this)),
          mDensity(new QSpinBox(this)), mStatus(new QLabel(this)), mTable(new QGridLayout),
          mCpu(CreatePowerReadout(*mTable, this, 1)), mGpu(CreatePowerReadout(*mTable, this, 2))
    {
        setObjectName(QStringLiteral("worldPowerBudgetTab"));
        QVBoxLayout* layout = new QVBoxLayout(this);
        QLabel* description = new QLabel(
            QStringLiteral(
                "Measure flux with the CPU oracle and GPU-compute reference tracers. Use untextured Diffuse materials, "
                "emissive meshes and a black environment."),
            this);
        description->setWordWrap(true);
        layout->addWidget(description);
        QHBoxLayout* controls = new QHBoxLayout;
        mDepth->setRange(1, static_cast<int32>(MAXIMUM_REFERENCE_DEPTH));
        mDepth->setValue(static_cast<int32>(DEFAULT_REFERENCE_DEPTH));
        controls->addWidget(new QLabel(QStringLiteral("Bounce depth"), this));
        controls->addWidget(mDepth);
        mDensity->setRange(1, static_cast<int32>(MAX_WORLD_POWER_PROBE_DENSITY));
        mDensity->setSuffix(QStringLiteral("×"));
        mDensity->setObjectName(QStringLiteral("worldPowerProbeDensity"));
        mDensity->setToolTip(
            QStringLiteral("More probes reduce the uncertainty of the surface and boundary integrals."));
        controls->addWidget(new QLabel(QStringLiteral("Probe density"), this));
        controls->addWidget(mDensity);
        mMeasure->setObjectName(QStringLiteral("measureWorldPower"));
        controls->addWidget(mMeasure);
        controls->addWidget(mCancel);
        controls->addStretch();
        layout->addLayout(controls);
        mStatus->setObjectName(QStringLiteral("worldPowerStatus"));
        mStatus->setTextFormat(Qt::PlainText);
        mStatus->setWordWrap(true);
        layout->addWidget(mStatus);
        mTable->addWidget(new QLabel(QStringLiteral("CPU oracle"), this), 0, 1);
        mTable->addWidget(new QLabel(QStringLiteral("GPU compute"), this), 0, 2);
        mTable->addWidget(new QLabel(QStringLiteral("Emitted"), this), 1, 0);
        mTable->addWidget(new QLabel(QStringLiteral("Absorbed"), this), 2, 0);
        constexpr int32 ESCAPED_ROW = 3;
        constexpr int32 RESIDUAL_ROW = 4;
        mTable->addWidget(new QLabel(QStringLiteral("Escaped"), this), ESCAPED_ROW, 0);
        mTable->addWidget(new QLabel(QStringLiteral("Signed residual"), this), RESIDUAL_ROW, 0);
        mCpu.Status->setObjectName(QStringLiteral("cpuWorldPowerStatus"));
        mGpu.Status->setObjectName(QStringLiteral("gpuWorldPowerStatus"));
        mCpu.Absorbed->setObjectName(QStringLiteral("cpuWorldPowerAbsorbed"));
        mGpu.Absorbed->setObjectName(QStringLiteral("gpuWorldPowerAbsorbed"));
        mCpu.Escaped->setObjectName(QStringLiteral("cpuWorldPowerEscaped"));
        mGpu.Escaped->setObjectName(QStringLiteral("gpuWorldPowerEscaped"));
        layout->addLayout(mTable);
        QLabel* units =
            new QLabel(QStringLiteral("Scene-linear flux (Rec. 709 weighted). Residual = emitted − absorbed − escaped. "
                                      "Sampling noise and the bounce limit contribute to the residual."),
                       this);
        units->setWordWrap(true);
        layout->addWidget(units);
        layout->addStretch();
        if (previewOrNull != nullptr)
        {
            connect(previewOrNull, &RenderPreview::Changed, this, &WorldPowerBudgetView::updateWorld);
            connect(previewOrNull, &RenderPreview::DeviceStopping, this, &WorldPowerBudgetView::stopForDevice);
            connect(previewOrNull, &RenderPreview::Stopped, this, &WorldPowerBudgetView::updateWorld);
            connect(previewOrNull, &QObject::destroyed, this, &WorldPowerBudgetView::updateWorld);
        }
        connect(mMeasure, &QPushButton::clicked, this, &WorldPowerBudgetView::startMeasurement);
        connect(mCancel, &QPushButton::clicked, this, [this]() {
            stopMeasurement();
            mStatus->setText(QStringLiteral("Measurement cancelled."));
        });
        connect(&mService, &QTimer::timeout, this, &WorldPowerBudgetView::serviceMeasurement);
        mService.setInterval(SERVICE_INTERVAL_MILLISECONDS);
        updateWorld();
    }

    ~WorldPowerBudgetView() override
    {
        stopMeasurement();
    }

private:
    void updateControls()
    {
        mMeasure->setEnabled(mPreview.isNull() == false && mPreview->HasDevice() == true &&
                             mPreview->Scene().Identity.isEmpty() == false && mIsMeasuring == false);
        mDepth->setEnabled(mIsMeasuring == false);
        mDensity->setEnabled(mIsMeasuring == false);
        mCancel->setEnabled(mIsMeasuring == true);
    }

    void clearReadout(const PowerReadout& readout)
    {
        readout.Status->clear();
        readout.Emitted->clear();
        readout.Absorbed->clear();
        readout.Escaped->clear();
        readout.Residual->clear();
    }

    void updateWorld()
    {
        const QByteArray identity = mPreview.isNull() == false ? mPreview->Scene().Identity : QByteArray{};
        if (mWorldIdentity != identity)
        {
            stopMeasurement();
            mWorldIdentity = identity;
            clearReadout(mCpu);
            clearReadout(mGpu);
            mStatus->setText(QStringLiteral("Measure the currently loaded World."));
        }
        updateControls();
        if (mPreview.isNull() == true || mPreview->HasDevice() == false)
        {
            mStatus->setText(QStringLiteral("Open a World in the viewport and wait for its first frame."));
        }
    }

    void startMeasurement()
    {
        if (mMeasure->isEnabled() == false || mPreview.isNull() == true)
        {
            return;
        }
        clearReadout(mCpu);
        clearReadout(mGpu);
        const RenderScene& scene = mPreview->Scene();
        const QString error = BuildWorldPowerProbes(scene, static_cast<uint32>(mDensity->value()), mProbes);
        if (error.isEmpty() == false)
        {
            mStatus->setText(error);
            return;
        }
        mIsMeasuring = true;
        mPreview->BeginGpuCapture();
        mTraversal = renderer::ReferenceTraversal::CpuOracle;
        mStatus->setText(QStringLiteral("Tracing matched whole-World radiance probes."));
        startBackend();
        mService.start();
        updateControls();
    }

    const PowerReadout& getReadout() const noexcept
    {
        return mTraversal == renderer::ReferenceTraversal::CpuOracle ? mCpu : mGpu;
    }

    void startBackend()
    {
        const uint32 height = static_cast<uint32>(mProbes.Rays.GetSize() / mProbes.RowWidth);
        ViewportReferenceSettings settings;
        settings.LongSide = mProbes.RowWidth;
        settings.Samples = SAMPLES_PER_PROBE;
        settings.MaxDepth = static_cast<uint32>(mDepth->value());
        settings.Traversal = mTraversal;
        settings.ProbeRays = {mProbes.Rays.GetData(), mProbes.Rays.GetSize()};
        renderer::ViewDescription view;
        view.OrthographicCamera = true;
        const QSize extent(static_cast<int32>(mProbes.RowWidth), static_cast<int32>(height));
        QString error;
        if (mCapture.Start(mPreview->GetDevice(),
                           mPreview->GetSurface(),
                           mPreview->Scene(),
                           view,
                           extent,
                           settings,
                           error) == false)
        {
            getReadout().Status->setText(error);
            return;
        }
        getReadout().Status->setText(QStringLiteral("Preparing…"));
    }

    void publishBackend()
    {
        const PowerReadout& readout = getReadout();
        Array<renderer::ReferencePixel> pixels;
        QString error;
        if (mCapture.CopyPixels(pixels, error) == false)
        {
            readout.Status->setText(error);
            return;
        }
        WorldPowerFlux flux;
        if (IntegrateWorldPower(mProbes, {pixels.GetData(), pixels.GetSize()}, flux) == false)
        {
            readout.Status->setText(
                QStringLiteral("Probe capture returned incomplete or invalid radiance. Measure again."));
            return;
        }
        const float64 total = math::Dot(flux.Emitted, REC709_LUMINANCE);
        readout.Emitted->setText(FormatPower(flux.Emitted, total));
        readout.Absorbed->setText(FormatPower(flux.Absorbed, total));
        readout.Escaped->setText(FormatPower(flux.Escaped, total));
        readout.Residual->setText(FormatPower(flux.Emitted - flux.Absorbed - flux.Escaped, total));
        readout.Status->setText(
            QStringLiteral("Complete at depth %1, density %2×").arg(mDepth->value()).arg(mDensity->value()));
    }

    void serviceMeasurement()
    {
        if (mPreview.isNull() == true || mPreview->HasOpenViewportSubmission() == true)
        {
            return;
        }
        const ViewportReferencePhase phase = mCapture.Service(REFERENCE_SERVICE_BUDGET_NANOSECONDS);
        if (phase == ViewportReferencePhase::Preparing)
        {
            return;
        }
        if (phase == ViewportReferencePhase::Tracing)
        {
            renderer::ReferenceCaptureInfo info;
            if (mCapture.GetInfo(info) == true)
            {
                getReadout().Status->setText(QStringLiteral("Tracing (%1/%2 samples per probe)…")
                                                 .arg(info.MinimumSamples)
                                                 .arg(info.TargetSamples));
            }
            return;
        }
        if (phase == ViewportReferencePhase::Complete)
        {
            publishBackend();
        }
        else if (phase == ViewportReferencePhase::Failed)
        {
            getReadout().Status->setText(mCapture.GetError());
        }
        mCapture.Release();
        if (mTraversal == renderer::ReferenceTraversal::CpuOracle)
        {
            mTraversal = renderer::ReferenceTraversal::GpuCompute;
            startBackend();
            return;
        }
        stopMeasurement();
        mStatus->setText(QStringLiteral("Measurement finished. Check each backend’s status."));
    }

    void stopMeasurement()
    {
        if (mIsMeasuring == false)
        {
            return;
        }
        mService.stop();
        mCapture.Release();
        mProbes = {};
        mIsMeasuring = false;
        if (mPreview.isNull() == false)
        {
            mPreview->EndGpuCapture();
        }
        updateControls();
    }

    void stopForDevice()
    {
        if (mIsMeasuring == true)
        {
            stopMeasurement();
            mStatus->setText(QStringLiteral("The viewport device stopped. Reopen the World and measure again."));
        }
    }

    QPointer<RenderPreview> mPreview;
    QPushButton* mMeasure;
    QPushButton* mCancel;
    QSpinBox* mDepth;
    QSpinBox* mDensity;
    QLabel* mStatus;
    QGridLayout* mTable;
    PowerReadout mCpu;
    PowerReadout mGpu;
    QTimer mService;
    ViewportReferenceCapture mCapture;
    WorldPowerProbes mProbes;
    QByteArray mWorldIdentity;
    renderer::ReferenceTraversal mTraversal = renderer::ReferenceTraversal::CpuOracle;
    bool mIsMeasuring = false;
};
} // namespace

QWidget* CreateWorldPowerBudgetView(RenderPreview* previewOrNull, QWidget* parentOrNull)
{
    return new WorldPowerBudgetView(previewOrNull, parentOrNull);
}
} // namespace ludus::editor
