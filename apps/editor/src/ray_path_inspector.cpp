#include "internal/ray_path_inspector.h"

#include "internal/item_selection.h"
#include "internal/ray_path_overlay.h"
#include "internal/render_graph_preview.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;
namespace renderer = graphics::renderer;
namespace
{
constexpr int32 EVENT_ROLE = Qt::UserRole;
constexpr int32 EVENT_COLUMN = 0;
/// Significant digits of world-space values in the inspector.
constexpr int32 VALUE_PRECISION = 4;

QString FormatVector(math::Vector3 value)
{
    return QStringLiteral("(%1, %2, %3)")
        .arg(static_cast<float64>(value.X), 0, 'g', VALUE_PRECISION)
        .arg(static_cast<float64>(value.Y), 0, 'g', VALUE_PRECISION)
        .arg(static_cast<float64>(value.Z), 0, 'g', VALUE_PRECISION);
}

const renderer::RayPathSummary*
FindSummaryOrNull(const QVector<renderer::RayPathSummary>& summaries, uint32 sample, uint32 pass)
{
    for (const renderer::RayPathSummary& summary : summaries)
    {
        if (summary.Sample == sample && summary.Pass == pass)
        {
            return &summary;
        }
    }
    return nullptr;
}

QString GetPassName(const QStringList& names, uint32 pass)
{
    const QString name =
        static_cast<qsizetype>(pass) < names.size() ? names.at(static_cast<qsizetype>(pass)) : QString{};
    return name.isEmpty() == true ? QStringLiteral("Pass %1").arg(pass) : name;
}
} // namespace

RayPathInspector::RayPathInspector(RayPathInspection& inoutInspection, QWidget* parentOrNull)
    : QWidget(parentOrNull), mInspection(inoutInspection), mStatus(new QLabel(this)),
      mXRay(new QCheckBox(QStringLiteral("X-ray"), this)), mColorMode(new QComboBox(this)),
      mSample(new QComboBox(this)), mCaptureAgain(new QPushButton(QStringLiteral("Capture Again"), this)),
      mLeave(new QPushButton(QStringLiteral("Leave Inspection"), this)), mEvents(new QTreeWidget(this)),
      mDetails(new QLabel(this))
{
    setObjectName(QStringLiteral("rayPathInspector"));
    mStatus->setObjectName(QStringLiteral("rayPathStatus"));
    mStatus->setWordWrap(true);
    mStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mXRay->setObjectName(QStringLiteral("rayPathXRay"));
    mXRay->setToolTip(QStringLiteral("Draw paths through geometry instead of depth-testing them against the scene."));
    mColorMode->setObjectName(QStringLiteral("rayPathColorMode"));
    mColorMode->addItem(QStringLiteral("Color by kind"), static_cast<int32>(RayPathColorMode::Kind));
    mColorMode->addItem(QStringLiteral("Color by bounce depth"), static_cast<int32>(RayPathColorMode::BounceDepth));
    mSample->setObjectName(QStringLiteral("rayPathSample"));
    mSample->setToolTip(QStringLiteral("Draw every sample's paths, or one sample's."));
    mCaptureAgain->setObjectName(QStringLiteral("rayPathCaptureAgain"));
    mCaptureAgain->setToolTip(QStringLiteral("Pick another pixel and trace the same target again."));
    mLeave->setObjectName(QStringLiteral("rayPathLeave"));
    mLeave->setToolTip(QStringLiteral("Return the World viewport to the live graph from the current editor camera."));
    mEvents->setObjectName(QStringLiteral("rayPathEvents"));
    mEvents->setAccessibleName(QStringLiteral("Captured ray path events"));
    mEvents->setHeaderLabels({QStringLiteral("Event"), QStringLiteral("Distance (m)"), QStringLiteral("Object")});
    mEvents->header()->setSectionResizeMode(EVENT_COLUMN, QHeaderView::Stretch);
    ConfigureItemSelection(*mEvents);
    mDetails->setObjectName(QStringLiteral("rayPathDetails"));
    mDetails->setWordWrap(true);
    mDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QHBoxLayout* controls = new QHBoxLayout;
    controls->addWidget(mXRay);
    controls->addWidget(mColorMode);
    controls->addWidget(mSample);
    controls->addStretch(1);
    controls->addWidget(mCaptureAgain);
    controls->addWidget(mLeave);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(mStatus);
    layout->addLayout(controls);
    layout->addWidget(mEvents, 1);
    layout->addWidget(mDetails);
    connect(mXRay, &QCheckBox::toggled, this, [this](bool isChecked) { mInspection.SetXRay(isChecked); });
    connect(mColorMode, &QComboBox::currentIndexChanged, this, [this]() {
        mInspection.SetColorMode(static_cast<RayPathColorMode>(mColorMode->currentData().toInt()));
    });
    connect(mSample, &QComboBox::currentIndexChanged, this, [this]() {
        mInspection.SetSampleFilter(mSample->currentData().isValid() == true ? mSample->currentData().toInt() : -1);
    });
    connect(mCaptureAgain, &QPushButton::clicked, this, [this]() {
        const RayPathTarget target = mInspection.GetTarget();
        static_cast<void>(mInspection.Begin(target));
    });
    connect(mLeave, &QPushButton::clicked, this, [this]() { mInspection.Leave(); });
    connect(mEvents, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* currentOrNull) {
        const int32 event = currentOrNull == nullptr ? -1 : currentOrNull->data(EVENT_COLUMN, EVENT_ROLE).toInt();
        mInspection.SelectEvent(event);
        showDetails(event);
    });
    connect(&mInspection, &RayPathInspection::Changed, this, &RayPathInspector::refresh);
    refresh();
}

QString RayPathInspector::GetStatusText() const
{
    return mStatus->text();
}

QString RayPathInspector::GetDetailsText() const
{
    return mDetails->text();
}

int32 RayPathInspector::GetEventRowCount() const
{
    int32 count = 0;
    for (QTreeWidgetItemIterator it(mEvents); *it != nullptr; ++it)
    {
        count += (*it)->data(EVENT_COLUMN, EVENT_ROLE).toInt() >= 0 ? 1 : 0;
    }
    return count;
}

bool RayPathInspector::SelectEventRow(int32 event)
{
    for (QTreeWidgetItemIterator it(mEvents); *it != nullptr; ++it)
    {
        if ((*it)->data(EVENT_COLUMN, EVENT_ROLE).toInt() == event)
        {
            mEvents->setCurrentItem(*it);
            return true;
        }
    }
    return false;
}

void RayPathInspector::refresh()
{
    const QString status = mInspection.GetStatus();
    mStatus->setText(status.isEmpty() == true ? QStringLiteral("Right-click a RayTracingPipeline or scene "
                                                               "ComputePipeline node, or the canvas background, and "
                                                               "choose Capture Ray Paths.")
                                              : status);
    const RenderGraphPreview* previewOrNull = mInspection.GetPreviewOrNull();
    const RayPathInspectionStage stage = mInspection.GetStage();
    const RayPathCaptureState state =
        previewOrNull == nullptr ? RayPathCaptureState::Idle : previewOrNull->GetRayPathCapture().GetState();
    mLeave->setEnabled(stage != RayPathInspectionStage::Inactive);
    mCaptureAgain->setEnabled(stage == RayPathInspectionStage::Inspecting);
    if (stage != mShownStage || state != mShownState)
    {
        mShownStage = stage;
        mShownState = state;
        rebuildTree();
    }
}

void RayPathInspector::rebuildTree()
{
    const QSignalBlocker blocker(mEvents);
    mEvents->clear();
    mDetails->clear();
    {
        const QSignalBlocker sampleBlocker(mSample);
        mSample->clear();
        mSample->addItem(QStringLiteral("All samples"));
    }
    const RenderGraphPreview* previewOrNull = mInspection.GetPreviewOrNull();
    if (previewOrNull == nullptr || mInspection.GetStage() != RayPathInspectionStage::Inspecting ||
        previewOrNull->GetRayPathCapture().GetState() != RayPathCaptureState::Ready)
    {
        return;
    }
    const RayPathCapture& capture = previewOrNull->GetRayPathCapture();
    const QVector<renderer::RayPathEvent>& events = capture.GetEvents();
    const QVector<renderer::RayPathSummary>& summaries = capture.GetSummaries();
    const int32 targetPass = capture.GetTargetPass();
    QTreeWidgetItem* sampleOrNull = nullptr;
    QTreeWidgetItem* passOrNull = nullptr;
    uint32 sample = 0;
    uint32 pass = 0;
    for (qsizetype i = 0; i < events.size(); ++i)
    {
        const renderer::RayPathEvent& event = events.at(i);
        if (targetPass >= 0 && event.Pass != static_cast<uint32>(targetPass))
        {
            continue;
        }
        if (sampleOrNull == nullptr || event.Sample != sample)
        {
            sample = event.Sample;
            sampleOrNull = new QTreeWidgetItem(mEvents, {QStringLiteral("Sample %1").arg(sample)});
            sampleOrNull->setData(EVENT_COLUMN, EVENT_ROLE, -1);
            passOrNull = nullptr;
            const QSignalBlocker sampleBlocker(mSample);
            mSample->addItem(QStringLiteral("Sample %1").arg(sample), static_cast<int32>(sample));
        }
        if (passOrNull == nullptr || event.Pass != pass)
        {
            pass = event.Pass;
            QString text = GetPassName(capture.GetPassNames(), pass);
            const renderer::RayPathSummary* summaryOrNull = FindSummaryOrNull(summaries, sample, pass);
            if (summaryOrNull != nullptr)
            {
                text += QStringLiteral(" · radiance %1").arg(FormatVector(summaryOrNull->Radiance));
                text += summaryOrNull->IsTruncated == true ? QStringLiteral(" · truncated") : QString{};
                text += summaryOrNull->HasFailed == true ? QStringLiteral(" · failed") : QString{};
            }
            passOrNull = new QTreeWidgetItem(sampleOrNull, {text});
            passOrNull->setData(EVENT_COLUMN, EVENT_ROLE, -1);
        }
        const bool isHit = event.Outcome == renderer::RayPathOutcome::Hit;
        const QString object = isHit == true ? capture.GetObjectId(event.SourceId) : QString{};
        QTreeWidgetItem* row = new QTreeWidgetItem(
            passOrNull,
            {QStringLiteral("#%1 %2").arg(event.Sequence).arg(DescribeRayPathEvent(event)),
             isHit == true ? QString::number(static_cast<float64>(event.HitDistanceWorld), 'g', VALUE_PRECISION)
                           : QStringLiteral("—"),
             object});
        row->setData(EVENT_COLUMN, EVENT_ROLE, static_cast<int32>(i));
    }
    mEvents->expandAll();
}

void RayPathInspector::showDetails(int32 event)
{
    const RenderGraphPreview* previewOrNull = mInspection.GetPreviewOrNull();
    if (previewOrNull == nullptr || event < 0 || event >= previewOrNull->GetRayPathCapture().GetEvents().size())
    {
        mDetails->clear();
        return;
    }
    const RayPathCapture& capture = previewOrNull->GetRayPathCapture();
    const renderer::RayPathEvent& value = capture.GetEvents().at(event);
    QStringList lines{
        QStringLiteral("%1, sample %2, %3, sequence %4")
            .arg(DescribeRayPathEvent(value))
            .arg(value.Sample)
            .arg(GetPassName(capture.GetPassNames(), value.Pass))
            .arg(value.Sequence),
        QStringLiteral("Origin %1, direction %2")
            .arg(FormatVector(value.OriginWorld), FormatVector(value.DirectionWorld)),
        QStringLiteral("Interval [%1, %2] m")
            .arg(static_cast<float64>(value.MinDistanceWorld), 0, 'g', VALUE_PRECISION)
            .arg(static_cast<float64>(value.MaxDistanceWorld), 0, 'g', VALUE_PRECISION),
    };
    if (value.Outcome == renderer::RayPathOutcome::Hit)
    {
        const QString object = capture.GetObjectId(value.SourceId);
        lines.append(QStringLiteral("Hit at %1 m: triangle %2, instance %3, primitive %4, object %5")
                         .arg(static_cast<float64>(value.HitDistanceWorld), 0, 'g', VALUE_PRECISION)
                         .arg(value.Triangle)
                         .arg(value.Instance)
                         .arg(value.Primitive)
                         .arg(object.isEmpty() == true ? QStringLiteral("source %1").arg(value.SourceId) : object));
    }
    lines.append(QStringLiteral("Alpha-rejected candidates: %1").arg(value.RejectedCandidates));
    const renderer::RayPathSummary* summaryOrNull = FindSummaryOrNull(capture.GetSummaries(), value.Sample, value.Pass);
    if (summaryOrNull != nullptr)
    {
        lines.append(QStringLiteral("Sample radiance %1, %2 events%3%4")
                         .arg(FormatVector(summaryOrNull->Radiance))
                         .arg(summaryOrNull->EventCount)
                         .arg(summaryOrNull->IsTruncated == true ? QStringLiteral(", truncated") : QString{})
                         .arg(summaryOrNull->HasFailed == true ? QStringLiteral(", failed") : QString{}));
    }
    mDetails->setText(lines.join(QLatin1Char('\n')));
}
} // namespace ludus::editor
