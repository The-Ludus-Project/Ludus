#include "internal/world_power_budget.h"

#include "internal/render_preview.h"
#include "internal/render_scene.h"

#include <QByteArray>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrentRun>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr math::Vector3d REC709_LUMINANCE{0.2126, 0.7152, 0.0722};
constexpr int32 PERCENT_SCALE = 100;
constexpr int32 FLUX_DIGITS = 8;
constexpr int32 PERCENT_DIGITS = 4;

QString FormatPower(math::Vector3d flux, float64 emitted)
{
    const float64 power = math::Dot(flux, REC709_LUMINANCE);
    const QString value = QString::number(power, 'g', FLUX_DIGITS);
    if (emitted <= 0)
    {
        return value;
    }
    return QStringLiteral("%1 (%2%)").arg(value, QString::number(PERCENT_SCALE * power / emitted, 'f', PERCENT_DIGITS));
}

void AddPowerValue(QFormLayout& inoutForm, QLabel& inoutValue, const QString& title, const QString& name)
{
    inoutValue.setObjectName(name);
    inoutValue.setTextInteractionFlags(Qt::TextSelectableByMouse);
    inoutForm.addRow(title, &inoutValue);
}

class WorldPowerBudgetView final : public QWidget
{
public:
    WorldPowerBudgetView(RenderPreview* previewOrNull, QWidget* parentOrNull)
        : QWidget(parentOrNull), mPreview(previewOrNull),
          mMeasure(new QPushButton(QStringLiteral("Measure World power"), this)), mStatus(new QLabel(this)),
          mReadout(new QWidget(this)), mEmitted(new QLabel(mReadout)), mAbsorbed(new QLabel(mReadout)),
          mEscaped(new QLabel(mReadout)), mUnresolved(new QLabel(mReadout)), mResidual(new QLabel(mReadout)),
          mSampling(new QLabel(mReadout))
    {
        setObjectName(QStringLiteral("worldPowerBudgetTab"));
        QVBoxLayout* layout = new QVBoxLayout(this);
        QLabel* description = new QLabel(
            QStringLiteral(
                "Estimate flux for the loaded World using diffuse surface transfer. Use untextured Diffuse materials, "
                "emissive meshes and a black environment. No capture or camera setup is required."),
            this);
        description->setWordWrap(true);
        layout->addWidget(description);
        mMeasure->setObjectName(QStringLiteral("measureWorldPower"));
        layout->addWidget(mMeasure, 0, Qt::AlignLeft);
        mStatus->setObjectName(QStringLiteral("worldPowerStatus"));
        mStatus->setTextFormat(Qt::PlainText);
        mStatus->setWordWrap(true);
        mStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(mStatus);

        QFormLayout* form = new QFormLayout(mReadout);
        QLabel* units =
            new QLabel(QStringLiteral("Scene-linear flux (Rec. 709 weighted) and percentage of emitted flux."),
                       mReadout);
        units->setWordWrap(true);
        form->addRow(units);
        AddPowerValue(*form, *mEmitted, QStringLiteral("Emitted"), QStringLiteral("worldPowerEmitted"));
        AddPowerValue(*form, *mAbsorbed, QStringLiteral("Absorbed by surfaces"), QStringLiteral("worldPowerAbsorbed"));
        AddPowerValue(*form, *mEscaped, QStringLiteral("Escaped the World"), QStringLiteral("worldPowerEscaped"));
        AddPowerValue(*form,
                      *mUnresolved,
                      QStringLiteral("Remaining in transport"),
                      QStringLiteral("worldPowerUnresolved"));
        AddPowerValue(*form, *mResidual, QStringLiteral("Accounting residual"), QStringLiteral("worldPowerResidual"));
        AddPowerValue(*form,
                      *mSampling,
                      QStringLiteral("Largest sampling shift"),
                      QStringLiteral("worldPowerSampling"));
        mSampling->setToolTip(QStringLiteral("Largest change in the absorbed, escaped or remaining fraction of any RGB "
                                             "channel between two visibility sample sets. This is a stability check."));
        QLabel* model = new QLabel(QStringLiteral("The accounting residual checks this diffuse model. Triangle size "
                                                  "and visibility sampling affect the estimate. "
                                                  "Use an independent fixture comparison to validate a renderer."),
                                   mReadout);
        model->setWordWrap(true);
        form->addRow(model);
        layout->addWidget(mReadout);
        layout->addStretch();
        mReadout->hide();

        if (previewOrNull != nullptr)
        {
            connect(previewOrNull, &RenderPreview::Changed, this, &WorldPowerBudgetView::updateWorld);
            connect(previewOrNull, &QObject::destroyed, this, &WorldPowerBudgetView::updateWorld);
        }
        connect(mMeasure, &QPushButton::clicked, this, &WorldPowerBudgetView::startMeasurement);
        connect(&mWatcher, &QFutureWatcher<WorldPowerBudget>::finished, this, &WorldPowerBudgetView::finishMeasurement);
        updateWorld();
    }

private:
    void updateWorld()
    {
        const bool hasWorld = mPreview.isNull() == false && mPreview->Scene().Identity.isEmpty() == false;
        mMeasure->setEnabled(hasWorld == true && mIsMeasuring == false);
        if (mIsMeasuring == true)
        {
            return;
        }
        if (hasWorld == false)
        {
            mReadout->hide();
            mStatus->setText(QStringLiteral("Load a World in the viewport to measure its power."));
        }
        else if (mMeasuredIdentity != mPreview->Scene().Identity)
        {
            mReadout->hide();
            mStatus->setText(QStringLiteral("Measure the currently loaded World."));
        }
    }

    void startMeasurement()
    {
        if (mPreview.isNull() == true || mIsMeasuring == true || mPreview->Scene().Identity.isEmpty() == true)
        {
            return;
        }
        const RenderScene snapshot = mPreview->Scene();
        mMeasuredIdentity = snapshot.Identity;
        mIsMeasuring = true;
        mReadout->hide();
        mMeasure->setEnabled(false);
        mStatus->setText(QStringLiteral("Measuring diffuse surface transfer…"));
        mWatcher.setFuture(QtConcurrent::run([snapshot]() { return MeasureWorldPower(snapshot); }));
    }

    void finishMeasurement()
    {
        mIsMeasuring = false;
        updateWorld();
        if (mPreview.isNull() == true || mMeasuredIdentity != mPreview->Scene().Identity)
        {
            return;
        }
        const WorldPowerBudget budget = mWatcher.result();
        if (budget.Error.isEmpty() == false)
        {
            mStatus->setText(budget.Error);
            return;
        }
        const WorldPowerFlux& flux = budget.Flux;
        const float64 total = math::Dot(flux.Emitted, REC709_LUMINANCE);
        mEmitted->setText(FormatPower(flux.Emitted, total));
        mAbsorbed->setText(FormatPower(flux.Absorbed, total));
        mEscaped->setText(FormatPower(flux.Escaped, total));
        mUnresolved->setText(FormatPower(flux.Unresolved, total));
        mResidual->setText(FormatPower(flux.Emitted - flux.Absorbed - flux.Escaped - flux.Unresolved, total));
        mSampling->setText(QStringLiteral("%1 percentage points")
                               .arg(QString::number(PERCENT_SCALE * budget.SamplingChange, 'f', PERCENT_DIGITS)));
        mStatus->setText(
            total > 0
                ? QStringLiteral("Measured the loaded World.")
                : QStringLiteral("No mesh emission. Add or enable an emissive mesh to send power into the scene."));
        mReadout->show();
    }

    QPointer<RenderPreview> mPreview;
    QPushButton* mMeasure;
    QLabel* mStatus;
    QWidget* mReadout;
    QLabel* mEmitted;
    QLabel* mAbsorbed;
    QLabel* mEscaped;
    QLabel* mUnresolved;
    QLabel* mResidual;
    QLabel* mSampling;
    QFutureWatcher<WorldPowerBudget> mWatcher;
    QByteArray mMeasuredIdentity;
    bool mIsMeasuring = false;
};
} // namespace

QWidget* CreateWorldPowerBudgetView(RenderPreview* previewOrNull, QWidget* parentOrNull)
{
    return new WorldPowerBudgetView(previewOrNull, parentOrNull);
}
} // namespace ludus::editor
