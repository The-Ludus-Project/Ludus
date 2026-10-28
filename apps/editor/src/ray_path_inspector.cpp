#include "internal/ray_path_inspector.h"

#include "internal/item_selection.h"
#include "internal/ray_path_overlay.h"
#include "internal/render_graph_preview.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QTextOption>
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
constexpr int32 SAMPLE_ROLE = EVENT_ROLE + 1;
constexpr int32 PASS_ROLE = SAMPLE_ROLE + 1;
/// Significant digits of world-space values in the inspector.
constexpr int32 VALUE_PRECISION = 4;
/// Keep the hierarchy compact enough for the shared inspector dock.
constexpr int32 TREE_INDENT_PIXELS = 12;
constexpr int32 MINIMUM_PANEL_WIDTH = 280;
constexpr int32 INITIAL_EVENTS_HEIGHT = 360;
constexpr int32 INITIAL_DETAILS_HEIGHT = 180;

QString FormatEventCount(uint32 eventCount)
{
    return QStringLiteral("%1 %2")
        .arg(eventCount)
        .arg(eventCount == 1 ? QStringLiteral("event") : QStringLiteral("events"));
}

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
      mDetails(new QPlainTextEdit(this))
{
    setObjectName(QStringLiteral("rayPathInspector"));
    setMinimumWidth(MINIMUM_PANEL_WIDTH);
    mStatus->setObjectName(QStringLiteral("rayPathStatus"));
    mStatus->setWordWrap(true);
    mStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    mStatus->setTextFormat(Qt::PlainText);
    mStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mXRay->setObjectName(QStringLiteral("rayPathXRay"));
    mXRay->setToolTip(QStringLiteral("Draw paths through geometry instead of depth-testing them against the scene."));
    mColorMode->setObjectName(QStringLiteral("rayPathColorMode"));
    mColorMode->addItem(QStringLiteral("Ray kind"), static_cast<int32>(RayPathColorMode::Kind));
    mColorMode->addItem(QStringLiteral("Bounce depth"), static_cast<int32>(RayPathColorMode::BounceDepth));
    mSample->setObjectName(QStringLiteral("rayPathSample"));
    mSample->addItem(QStringLiteral("All samples"), -1);
    mColorMode->setAccessibleName(QStringLiteral("Path coloring"));
    mColorMode->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    mColorMode->setMinimumContentsLength(1);
    mSample->setAccessibleName(QStringLiteral("Sample filter"));
    mSample->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    mSample->setMinimumContentsLength(1);
    mSample->setToolTip(QStringLiteral("Show all samples or one sample in both the event list and viewport."));
    mCaptureAgain->setObjectName(QStringLiteral("rayPathCaptureAgain"));
    mCaptureAgain->setToolTip(QStringLiteral("Pick another pixel and trace the same target again."));
    mLeave->setObjectName(QStringLiteral("rayPathLeave"));
    mLeave->setToolTip(QStringLiteral("Return the World viewport to the live graph from the current editor camera."));
    mEvents->setObjectName(QStringLiteral("rayPathEvents"));
    mEvents->setAccessibleName(QStringLiteral("Captured ray path events"));
    // Keep IDs and distances in the details pane rather than competing with event names.
    mEvents->setHeaderLabels({QStringLiteral("Captured events")});
    mEvents->header()->setSectionResizeMode(EVENT_COLUMN, QHeaderView::Stretch);
    mEvents->setIndentation(TREE_INDENT_PIXELS);
    mEvents->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mEvents->setUniformRowHeights(true);
    ConfigureItemSelection(*mEvents);
    mDetails->setObjectName(QStringLiteral("rayPathDetails"));
    mDetails->setAccessibleName(QStringLiteral("Ray event and invocation details"));
    mDetails->setReadOnly(true);
    mDetails->setTabChangesFocus(true);
    mDetails->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    mDetails->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    mDetails->setPlaceholderText(QStringLiteral("Select an event or invocation to inspect its fields."));
    mDetails->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    QHBoxLayout* actions = new QHBoxLayout;
    actions->addWidget(mCaptureAgain, 1);
    actions->addWidget(mLeave, 1);
    QFormLayout* controls = new QFormLayout;
    controls->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    controls->addRow(QStringLiteral("Color by"), mColorMode);
    controls->addRow(QStringLiteral("Sample"), mSample);
    controls->addRow(QString{}, mXRay);
    QPushButton* expand = new QPushButton(QStringLiteral("Expand all"), this);
    QPushButton* collapse = new QPushButton(QStringLiteral("Collapse all"), this);
    expand->setObjectName(QStringLiteral("rayPathExpandAll"));
    collapse->setObjectName(QStringLiteral("rayPathCollapseAll"));
    expand->setProperty("density", QStringLiteral("compact"));
    collapse->setProperty("density", QStringLiteral("compact"));
    QHBoxLayout* treeActions = new QHBoxLayout;
    treeActions->addWidget(expand);
    treeActions->addWidget(collapse);
    treeActions->addStretch(1);
    QWidget* eventPane = new QWidget(this);
    QVBoxLayout* eventLayout = new QVBoxLayout(eventPane);
    eventLayout->setContentsMargins(0, 0, 0, 0);
    eventLayout->addLayout(treeActions);
    eventLayout->addWidget(mEvents);
    QWidget* detailPane = new QWidget(this);
    QVBoxLayout* detailLayout = new QVBoxLayout(detailPane);
    QLabel* detailHeading = new QLabel(QStringLiteral("Selection details"), detailPane);
    detailLayout->addWidget(detailHeading);
    detailLayout->addWidget(mDetails, 1);
    QSplitter* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setObjectName(QStringLiteral("rayPathSplitter"));
    splitter->addWidget(eventPane);
    splitter->addWidget(detailPane);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({INITIAL_EVENTS_HEIGHT, INITIAL_DETAILS_HEIGHT});
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(mStatus);
    layout->addLayout(actions);
    layout->addLayout(controls);
    layout->addWidget(splitter, 1);
    setTabOrder(mCaptureAgain, mLeave);
    setTabOrder(mLeave, mColorMode);
    setTabOrder(mColorMode, mSample);
    setTabOrder(mSample, mXRay);
    setTabOrder(mXRay, expand);
    setTabOrder(expand, collapse);
    setTabOrder(collapse, mEvents);
    setTabOrder(mEvents, mDetails);
    connect(expand, &QPushButton::clicked, mEvents, &QTreeWidget::expandAll);
    connect(collapse, &QPushButton::clicked, mEvents, &QTreeWidget::collapseAll);
    connect(mXRay, &QCheckBox::toggled, this, [this](bool isChecked) { mInspection.SetXRay(isChecked); });
    connect(mColorMode, &QComboBox::currentIndexChanged, this, [this]() {
        mInspection.SetColorMode(static_cast<RayPathColorMode>(mColorMode->currentData().toInt()));
    });
    connect(mSample, &QComboBox::currentIndexChanged, this, [this]() {
        mInspection.SetSampleFilter(mSample->currentData().isValid() == true ? mSample->currentData().toInt() : -1);
    });
    connect(mCaptureAgain, &QPushButton::clicked, this, [this]() {
        const RayPathTarget target =
            mInspection.GetStage() == RayPathInspectionStage::Inactive ? RayPathTarget{} : mInspection.GetTarget();
        static_cast<void>(mInspection.Begin(target));
    });
    connect(mLeave, &QPushButton::clicked, this, [this]() { mInspection.Leave(); });
    connect(mEvents, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* currentOrNull) {
        const int32 event = currentOrNull == nullptr ? -1 : currentOrNull->data(EVENT_COLUMN, EVENT_ROLE).toInt();
        // Selection listeners may leave inspection and delete the current row. Finish row reads first.
        showDetails(event);
        showInvocationDetails(currentOrNull);
        mInspection.SelectEvent(event);
    });
    connect(&mInspection, &RayPathInspection::Changed, this, &RayPathInspector::refresh);
    connect(&mInspection, &RayPathInspection::OverlayChanged, this, &RayPathInspector::syncPresentation);
    refresh();
}

QString RayPathInspector::GetStatusText() const
{
    return mStatus->text();
}

QString RayPathInspector::GetDetailsText() const
{
    return mDetails->toPlainText();
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
        if (event >= 0 && (*it)->isHidden() == false && (*it)->data(EVENT_COLUMN, EVENT_ROLE).toInt() == event)
        {
            QTreeWidgetItem* ancestorOrNull = (*it)->parent();
            while (ancestorOrNull != nullptr)
            {
                ancestorOrNull->setExpanded(true);
                ancestorOrNull = ancestorOrNull->parent();
            }
            // Selecting may synchronously clear the tree. Do not read the row after this notification.
            mEvents->scrollToItem(*it);
            mEvents->setCurrentItem(*it);
            return true;
        }
    }
    return false;
}

void RayPathInspector::refresh()
{
    const QString status = mInspection.GetStatus();
    const RenderGraphPreview* previewOrNull = mInspection.GetPreviewOrNull();
    const RayPathInspectionStage stage = mInspection.GetStage();
    const RayPathCaptureState state =
        previewOrNull == nullptr ? RayPathCaptureState::Idle : previewOrNull->GetRayPathCapture().GetState();
    const QString captureProblem = stage == RayPathInspectionStage::Inactive && previewOrNull != nullptr
                                       ? previewOrNull->DescribeRayPathCaptureProblem()
                                       : QString{};
    QString shownStatus = status;
    if (shownStatus.isEmpty() == true)
    {
        shownStatus = captureProblem.isEmpty() == false
                          ? captureProblem
                          : QStringLiteral("Capture a pixel to inspect its rays. Capture ray paths traces all passes; "
                                           "a ray node's context menu captures one pass.");
    }
    mStatus->setText(shownStatus);
    const bool hasCapture = stage == RayPathInspectionStage::Inspecting && state == RayPathCaptureState::Ready;
    mSample->setEnabled(hasCapture);
    mColorMode->setEnabled(hasCapture);
    mXRay->setEnabled(hasCapture);
    mLeave->setText(stage == RayPathInspectionStage::PickingPixel ? QStringLiteral("Cancel picking")
                                                                  : QStringLiteral("Leave Inspection"));
    mLeave->setToolTip(stage == RayPathInspectionStage::PickingPixel
                           ? QStringLiteral("Cancel choosing a pixel and keep the graph live.")
                           : QStringLiteral("Return to the live graph from the current editor camera."));
    mLeave->setEnabled(stage != RayPathInspectionStage::Inactive);
    const bool canStartCapture =
        stage == RayPathInspectionStage::Inactive && previewOrNull != nullptr && captureProblem.isEmpty() == true;
    mCaptureAgain->setText(stage == RayPathInspectionStage::Inactive ? QStringLiteral("Capture ray paths")
                                                                     : QStringLiteral("Capture Again"));
    QString captureHelp = QStringLiteral("Pick another pixel and trace the same target again.");
    if (stage == RayPathInspectionStage::Inactive)
    {
        captureHelp = canStartCapture == true
                          ? QStringLiteral("Pick a pixel and capture every ray pass of the drawing graph.")
                          : shownStatus;
    }
    mCaptureAgain->setToolTip(captureHelp);
    mCaptureAgain->setEnabled(stage == RayPathInspectionStage::Inspecting || canStartCapture == true);
    if (stage != mShownStage || state != mShownState)
    {
        mShownStage = stage;
        mShownState = state;
        rebuildTree();
    }
    syncPresentation();
}

void RayPathInspector::rebuildTree()
{
    const QSignalBlocker blocker(mEvents);
    mEvents->clear();
    mShownSampleFilter = UNSHOWN_SAMPLE_FILTER;
    mDetails->clear();
    {
        const QSignalBlocker sampleBlocker(mSample);
        mSample->clear();
        mSample->addItem(QStringLiteral("All samples"), -1);
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
            sampleOrNull->setData(EVENT_COLUMN, SAMPLE_ROLE, static_cast<int32>(sample));
            passOrNull = nullptr;
            const QSignalBlocker sampleBlocker(mSample);
            mSample->addItem(QStringLiteral("Sample %1").arg(sample), static_cast<int32>(sample));
        }
        if (passOrNull == nullptr || event.Pass != pass)
        {
            pass = event.Pass;
            const renderer::RayPathSummary* summaryOrNull = FindSummaryOrNull(summaries, sample, pass);
            QString text = GetPassName(capture.GetPassNames(), pass);
            if (summaryOrNull != nullptr)
            {
                text += QStringLiteral(" · %1").arg(FormatEventCount(summaryOrNull->EventCount));
                text += summaryOrNull->IsTruncated == true ? QStringLiteral(" · truncated") : QString{};
                text += summaryOrNull->HasFailed == true ? QStringLiteral(" · failed") : QString{};
                // Keep incomplete/failed invocations visible even when other samples start collapsed.
                if (summaryOrNull->IsTruncated == true || summaryOrNull->HasFailed == true)
                {
                    sampleOrNull->setExpanded(true);
                }
            }
            passOrNull = new QTreeWidgetItem(sampleOrNull, {text});
            passOrNull->setData(EVENT_COLUMN, EVENT_ROLE, -1);
            passOrNull->setData(EVENT_COLUMN, SAMPLE_ROLE, static_cast<int32>(sample));
            passOrNull->setData(EVENT_COLUMN, PASS_ROLE, static_cast<int32>(pass));
            passOrNull->setToolTip(EVENT_COLUMN, text);
        }
        const bool isHit = event.Outcome == renderer::RayPathOutcome::Hit;
        QString text = QStringLiteral("#%1 %2").arg(event.Sequence).arg(DescribeRayPathEvent(event));
        if (isHit == true)
        {
            text +=
                QStringLiteral(" · %1 m").arg(static_cast<float64>(event.HitDistanceWorld), 0, 'g', VALUE_PRECISION);
        }
        QTreeWidgetItem* row = new QTreeWidgetItem(passOrNull, {text});
        row->setToolTip(EVENT_COLUMN, text);
        row->setData(EVENT_COLUMN, SAMPLE_ROLE, static_cast<int32>(sample));
        row->setData(EVENT_COLUMN, EVENT_ROLE, static_cast<int32>(i));
    }
    // Start with one expanded sample instead of flooding a narrow dock with every invocation.
    if (mEvents->topLevelItemCount() > 0)
    {
        mEvents->topLevelItem(0)->setExpanded(true);
        for (int32 i = 0; i < mEvents->topLevelItemCount(); ++i)
        {
            QTreeWidgetItem* sampleItem = mEvents->topLevelItem(i);
            for (int32 j = 0; j < sampleItem->childCount(); ++j)
            {
                sampleItem->child(j)->setExpanded(true);
            }
        }
    }
}

void RayPathInspector::syncPresentation()
{
    const QSignalBlocker xRayBlocker(mXRay);
    const QSignalBlocker colorBlocker(mColorMode);
    const QSignalBlocker sampleBlocker(mSample);
    mXRay->setChecked(mInspection.IsXRay());
    mColorMode->setCurrentIndex(mColorMode->findData(static_cast<int32>(mInspection.GetColorMode())));
    const int32 filter = mInspection.GetSampleFilter();
    const int32 filterIndex = mSample->findData(filter);
    mSample->setCurrentIndex(filterIndex >= 0 ? filterIndex : 0);
    if (mShownSampleFilter != filter)
    {
        mShownSampleFilter = filter;
        for (QTreeWidgetItemIterator iterator(mEvents); *iterator != nullptr; ++iterator)
        {
            QTreeWidgetItem* row = *iterator;
            row->setHidden(filter >= 0 && row->data(EVENT_COLUMN, SAMPLE_ROLE).toInt() != filter);
            if (row->parent() == nullptr && filter >= 0 && row->isHidden() == false)
            {
                row->setExpanded(true);
            }
        }
        QTreeWidgetItem* currentOrNull = mEvents->currentItem();
        if (currentOrNull != nullptr && currentOrNull->isHidden() == true)
        {
            mEvents->setCurrentItem(nullptr);
            mInspection.SelectEvent(-1);
            mDetails->clear();
        }
    }
    const int32 selectedEvent = mInspection.GetSelectedEvent();
    QTreeWidgetItem* currentOrNull = mEvents->currentItem();
    if (selectedEvent < 0 && currentOrNull != nullptr && currentOrNull->data(EVENT_COLUMN, EVENT_ROLE).toInt() >= 0)
    {
        mEvents->setCurrentItem(nullptr);
        mDetails->clear();
    }
    if (selectedEvent >= 0 &&
        (currentOrNull == nullptr || currentOrNull->data(EVENT_COLUMN, EVENT_ROLE).toInt() != selectedEvent))
    {
        if (SelectEventRow(selectedEvent) == false)
        {
            mInspection.SelectEvent(-1);
        }
    }
}

void RayPathInspector::showInvocationDetails(const QTreeWidgetItem* itemOrNull)
{
    if (itemOrNull == nullptr || itemOrNull->data(EVENT_COLUMN, EVENT_ROLE).toInt() >= 0 ||
        itemOrNull->data(EVENT_COLUMN, PASS_ROLE).isValid() == false)
    {
        return;
    }
    const RenderGraphPreview* previewOrNull = mInspection.GetPreviewOrNull();
    if (previewOrNull == nullptr)
    {
        return;
    }
    const RayPathCapture& capture = previewOrNull->GetRayPathCapture();
    const uint32 sample = itemOrNull->data(EVENT_COLUMN, SAMPLE_ROLE).toUInt();
    const uint32 pass = itemOrNull->data(EVENT_COLUMN, PASS_ROLE).toUInt();
    const renderer::RayPathSummary* summaryOrNull = FindSummaryOrNull(capture.GetSummaries(), sample, pass);
    if (summaryOrNull != nullptr)
    {
        mDetails->setPlainText(
            QStringLiteral("%1\nSample: %2\nEvents: %3\nRadiance (linear RGB): %4\n%5%6")
                .arg(GetPassName(capture.GetPassNames(), pass))
                .arg(sample)
                .arg(summaryOrNull->EventCount)
                .arg(FormatVector(summaryOrNull->Radiance))
                .arg(summaryOrNull->IsTruncated == true ? QStringLiteral("Truncated capture\n") : QString{})
                .arg(summaryOrNull->HasFailed == true ? QStringLiteral("Invocation failed") : QString{}));
    }
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
        QStringLiteral("Origin (world, m): %1\nDirection (world): %2")
            .arg(FormatVector(value.OriginWorld), FormatVector(value.DirectionWorld)),
        QStringLiteral("Interval [%1, %2] m")
            .arg(static_cast<float64>(value.MinDistanceWorld), 0, 'g', VALUE_PRECISION)
            .arg(static_cast<float64>(value.MaxDistanceWorld), 0, 'g', VALUE_PRECISION),
    };
    if (value.Outcome == renderer::RayPathOutcome::Hit)
    {
        const QString object = capture.GetObjectId(value.SourceId);
        lines.insert(1,
                     QStringLiteral("Object: %1\nHit distance: %2 m")
                         .arg(object.isEmpty() == true ? QStringLiteral("source %1").arg(value.SourceId) : object,
                              QString::number(static_cast<float64>(value.HitDistanceWorld), 'g', VALUE_PRECISION)));
        lines.append(QStringLiteral("Triangle: %1\nInstance: %2\nPrimitive: %3")
                         .arg(value.Triangle)
                         .arg(value.Instance)
                         .arg(value.Primitive));
    }
    lines.append(QStringLiteral("Alpha-rejected candidates: %1").arg(value.RejectedCandidates));
    const renderer::RayPathSummary* summaryOrNull = FindSummaryOrNull(capture.GetSummaries(), value.Sample, value.Pass);
    if (summaryOrNull != nullptr)
    {
        lines.append(QStringLiteral("Sample radiance: %1\nInvocation: %2%3%4")
                         .arg(FormatVector(summaryOrNull->Radiance))
                         .arg(FormatEventCount(summaryOrNull->EventCount))
                         .arg(summaryOrNull->IsTruncated == true ? QStringLiteral(", truncated") : QString{})
                         .arg(summaryOrNull->HasFailed == true ? QStringLiteral(", failed") : QString{}));
    }
    mDetails->setPlainText(lines.join(QLatin1Char('\n')));
}
} // namespace ludus::editor
