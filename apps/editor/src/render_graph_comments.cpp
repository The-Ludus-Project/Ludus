#include "internal/render_graph_comments.h"
#include "internal/render_graph_canvas.h"
#include "internal/render_graph_diagnostics.h"
#include "internal/render_graph_execution_plan.h"
#include "internal/theme_colour.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>

#include <QAction>
#include <QBrush>
#include <QColor>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QGraphicsEllipseItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsTextItem>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPen>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizeF>
#include <QStringList>
#include <QVBoxLayout>
#include <QtAlgorithms>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr float64 HEADER_HEIGHT = 34;
constexpr float64 STAGE_WIDTH = 320;
constexpr float64 STAGE_ROW_HEIGHT = 24;
constexpr int32 STAGE_METADATA_ROWS = 3;
constexpr float64 STAGE_PORT_RADIUS = 5;
constexpr float64 STAGE_PORT_HIT_RADIUS = 12;
constexpr float64 STAGE_DIMMED_OPACITY = 0.25;

QString GetNode(const QString& endpoint)
{
    return endpoint.section(QLatin1Char('.'), 0, 0);
}
constexpr float64 NOTES_HEIGHT = 50;
constexpr float64 CONTENT_PADDING = 24;
constexpr float64 LABEL_PADDING = 8;
constexpr float64 RESIZE_HANDLE = 18;
constexpr int32 BACKGROUND_ALPHA = 28;
constexpr int32 STAGE_BACKGROUND_ALPHA = 235;
constexpr int32 HEADER_ALPHA = 155;
constexpr float64 FRAME_DEPTH = -2;
constexpr float64 DEFAULT_WIDTH = 360;
constexpr float64 DEFAULT_HEIGHT = 220;
constexpr int32 DIALOG_WIDTH = 480;
constexpr int32 DIALOG_HEIGHT = 320;

QRectF GetResizeHandle(const QRectF& bounds)
{
    return QRectF(bounds.bottomRight() - QPointF(RESIZE_HANDLE, RESIZE_HANDLE), QSizeF(RESIZE_HANDLE, RESIZE_HANDLE));
}

void TranslatePositions(const QVector<RenderGraphNodePosition>& positions,
                        QPointF offset,
                        QVector<RenderGraphNodePosition>& outPositions)
{
    outPositions.clear();
    for (const RenderGraphNodePosition& position : positions)
    {
        outPositions.append({position.Node, position.X + qRound(offset.x()), position.Y + qRound(offset.y())});
    }
}
} // namespace

RenderGraphComments::RenderGraphComments(RenderGraphCanvas& inoutCanvas, QGraphicsScene& inoutScene)
    : QObject(&inoutCanvas), mCanvas(inoutCanvas), mScene(inoutScene)
{
}
RenderGraphComments::~RenderGraphComments()
{
    Clear();
}

void RenderGraphComments::Clear()
{
    mGestureIndex = -1;
    mBeforePositions.clear();
    mPreviewPositions.clear();
    qDeleteAll(mItems);
    mItems.clear();
    mBoundaryPorts.clear();
    mCounts.clear();
    mPlanSummaries.clear();
    mCollapsedOwners.clear();
    mMembers.clear();
    mFrames = {};
}

void RenderGraphComments::Rebuild(const QJsonObject& configuration)
{
    Clear();
    if (mFile != mCanvas.GetFile() || mConfiguration != mCanvas.GetConfiguration())
    {
        mRevealedFrames.clear();
    }
    mFile = mCanvas.GetFile();
    mConfiguration = mCanvas.GetConfiguration();
    mRevision = mCanvas.GetDocumentRevision();
    mConfigurationRecord = configuration;
    mHasValidMetadata = true;
    const QJsonValue frames = configuration.value(QStringLiteral("layout")).toObject().value(QStringLiteral("frames"));
    if (frames.isUndefined() == true)
    {
        return;
    }
    if (frames.isArray() == false || AreRenderGraphCommentFramesValid(frames.toArray()) == false)
    {
        mHasValidMetadata = false;
        Q_EMIT Rejected(QStringLiteral("Comment frame metadata is invalid; repair layout.frames in Source."));
        return;
    }
    mFrames = frames.toArray();
    refreshPresentation();
}

bool RenderGraphComments::isCurrent() const
{
    return mFile.isEmpty() == false && mHasValidMetadata == true && mFile == mCanvas.GetFile() &&
           mConfiguration == mCanvas.GetConfiguration() && mRevision == mCanvas.GetDocumentRevision();
}

int32 RenderGraphComments::findFrame(const QString& identity) const
{
    for (int32 index = 0; index < mFrames.size(); ++index)
    {
        if (mFrames.at(index).toObject().value(QStringLiteral("id")).toString() == identity)
        {
            return index;
        }
    }
    return -1;
}

int32 RenderGraphComments::hitFrame(QPointF position) const
{
    for (int32 index = static_cast<int32>(mItems.size()); index > 0; --index)
    {
        const QRectF bounds = mItems.at(index - 1)->rect().translated(mItems.at(index - 1)->pos());
        const QString identity = mFrames.at(index - 1).toObject().value(QStringLiteral("id")).toString();
        if ((mCollapsedOwners.values().contains(identity) == true && bounds.contains(position) == true) ||
            QRectF(bounds.topLeft(), QSizeF(bounds.width(), HEADER_HEIGHT + NOTES_HEIGHT)).contains(position) == true ||
            GetResizeHandle(bounds).contains(position) == true)
        {
            return index - 1;
        }
    }
    return -1;
}

QRectF RenderGraphComments::GetBounds() const
{
    QRectF bounds;
    for (const QGraphicsRectItem* item : mItems)
    {
        bounds = bounds.united(item->rect().translated(item->pos()));
    }
    return bounds;
}
QRectF RenderGraphComments::GetFrameBounds(const QString& identity) const
{
    const int32 index = findFrame(identity);
    return index < 0 ? QRectF{} : mItems.at(index)->rect().translated(mItems.at(index)->pos());
}

bool RenderGraphComments::CanCollapse(const QString& identity) const
{
    const int32 index = findFrame(identity);
    if (index < 0 || mMembers.value(identity).isEmpty() == true)
    {
        return false;
    }
    const QRectF bounds = GetRenderGraphCommentBounds(mFrames.at(index).toObject());
    for (int32 other = 0; other < mFrames.size(); ++other)
    {
        if (other != index && bounds.intersects(GetRenderGraphCommentBounds(mFrames.at(other).toObject())) == true)
        {
            return false;
        }
    }
    return true;
}

QString RenderGraphComments::GetCollapsedFrame(const QString& node) const
{
    return mCollapsedOwners.value(node);
}

void RenderGraphComments::refreshPresentation()
{
    mMembers.clear();
    mCollapsedOwners.clear();
    const QVector<RenderGraphLayoutNode> nodes = mCanvas.GetAuthoredNodeBounds();
    for (const QJsonValueConstRef value : mFrames)
    {
        const QJsonObject frame = value.toObject();
        QStringList members;
        const QRectF bounds = GetRenderGraphCommentBounds(frame);
        for (const RenderGraphLayoutNode& node : nodes)
        {
            if (bounds.contains(node.Bounds) == true)
            {
                members.append(node.Identity);
            }
        }
        mMembers.insert(frame.value(QStringLiteral("id")).toString(), members);
    }
    for (const QJsonValueConstRef value : mFrames)
    {
        const QJsonObject frame = value.toObject();
        const QString identity = frame.value(QStringLiteral("id")).toString();
        if (frame.value(QStringLiteral("collapsed")).toBool() == true && CanCollapse(identity) == true &&
            mRevealedFrames.contains(identity) == false)
        {
            for (const QString& node : mMembers.value(identity))
            {
                mCollapsedOwners.insert(node, identity);
            }
        }
    }
    renderFrames();
    mCanvas.RefreshCommentPresentation();
}

void RenderGraphComments::SetCollapsed(const QString& identity, bool isCollapsed)
{
    const int32 index = findFrame(identity);
    if (index < 0 || isCurrent() == false)
    {
        return;
    }
    if (isCollapsed == true && CanCollapse(identity) == false)
    {
        Q_EMIT Rejected(
            QStringLiteral("Collapse requires a nonempty frame that does not overlap or nest another frame."));
        return;
    }
    mRevealedFrames.removeAll(identity);
    QJsonArray frames = mFrames;
    QJsonObject frame = frames.at(index).toObject();
    frame.insert(QStringLiteral("collapsed"), isCollapsed);
    frames.replace(index, frame);
    if (frames == mFrames)
    {
        refreshPresentation();
    }
    else
    {
        publish(frames);
    }
}

void RenderGraphComments::RevealNode(const QString& node)
{
    const QString identity = GetCollapsedFrame(node);
    if (identity.isEmpty() == false)
    {
        mRevealedFrames.append(identity);
        refreshPresentation();
    }
}
QStringList RenderGraphComments::GetRevealedFrames() const
{
    return mRevealedFrames;
}
void RenderGraphComments::SetRevealedFrames(const QStringList& frames)
{
    mRevealedFrames.clear();
    for (const QString& identity : frames)
    {
        if (findFrame(identity) >= 0 && mRevealedFrames.contains(identity) == false)
        {
            mRevealedFrames.append(identity);
        }
    }
    refreshPresentation();
}
QGraphicsEllipseItem* RenderGraphComments::GetBoundaryPortOrNull(const QString& endpoint, bool isOutput) const
{
    for (const BoundaryPort& port : mBoundaryPorts)
    {
        if (port.Endpoint == endpoint && port.IsOutput == isOutput)
        {
            return port.Item;
        }
    }
    return nullptr;
}
QString RenderGraphComments::GetPortAt(QPointF position, bool& outIsOutput) const
{
    for (const BoundaryPort& port : mBoundaryPorts)
    {
        const QPointF offset = port.Item->mapToScene(port.Item->rect().center()) - position;
        if (QPointF::dotProduct(offset, offset) <= STAGE_PORT_HIT_RADIUS * STAGE_PORT_HIT_RADIUS)
        {
            outIsOutput = port.IsOutput;
            return port.Endpoint;
        }
    }
    return {};
}
void RenderGraphComments::RefreshCounts()
{
    for (const QString& identity : mCounts.keys())
    {
        const int32 errors = mCanvas.GetDiagnostics().CountNodeErrors(mFile, mConfiguration, mMembers.value(identity));
        const RenderGraphDiagnosticFreshness freshness = mCanvas.GetDiagnostics().GetFreshness();
        const QString suffix = freshness == RenderGraphDiagnosticFreshness::None    ? QStringLiteral(" · no report")
                               : freshness == RenderGraphDiagnosticFreshness::Stale ? QStringLiteral(" · stale")
                               : freshness == RenderGraphDiagnosticFreshness::Unverified
                                   ? QStringLiteral(" · unverified")
                                   : QString{};
        QGraphicsSimpleTextItem* label = mCounts.value(identity);
        const QString text =
            QStringLiteral("%1 nodes · %2 errors%3").arg(mMembers.value(identity).size()).arg(errors).arg(suffix);
        label->setText(QFontMetricsF(label->font())
                           .elidedText(text, Qt::ElideRight, GetFrameBounds(identity).width() - 2 * LABEL_PADDING));
        label->setToolTip(text +
                          QStringLiteral(". Node-targeted report errors; graph-level errors remain in the toolbar."));
    }
    for (const QString& identity : mPlanSummaries.keys())
    {
        if (mCanvas.GetExecutionPlan().IsEnabled() == false)
        {
            mPlanSummaries.value(identity)->hide();
            continue;
        }
        const RenderGraphPlanAnnotation annotation =
            mCanvas.GetExecutionPlan().GetStageAnnotation(mMembers.value(identity));
        QGraphicsSimpleTextItem* label = mPlanSummaries.value(identity);
        label->setText(
            QFontMetricsF(label->font())
                .elidedText(annotation.Label, Qt::ElideRight, GetFrameBounds(identity).width() - 2 * LABEL_PADDING));
        label->setToolTip(annotation.Explanation.toHtmlEscaped());
        label->setVisible(mCanvas.GetExecutionPlan().IsEnabled());
    }
}
void RenderGraphComments::RefreshHighlights(const RenderGraphCommentHighlights& highlights)
{
    for (int32 index = 0; index < mFrames.size(); ++index)
    {
        const QJsonObject frame = mFrames.at(index).toObject();
        const QString identity = frame.value(QStringLiteral("id")).toString();
        bool isSelected = false;
        bool isTraced = false;
        for (const QString& node : mMembers.value(identity))
        {
            isSelected = isSelected == true || highlights.Selection.contains(node) == true;
            isTraced = isTraced == true || highlights.TracedNodes.contains(node) == true;
        }
        QPen pen(isSelected == true || isTraced == true ? GetCanvasColour(design::ThemeColour::SelectionAmber)
                                                        : QColor(frame.value(QStringLiteral("colour")).toString()),
                 2);
        pen.setCosmetic(true);
        mItems.at(index)->setPen(pen);
        const bool isPruned = mCanvas.GetExecutionPlan().IsEnabled() == true &&
                              mCanvas.GetExecutionPlan().GetStageAnnotation(mMembers.value(identity)).IsPruned == true;
        mItems.at(index)->setOpacity(
            isPruned == true || (highlights.TracedNodes.isEmpty() == false && isTraced == false) ? STAGE_DIMMED_OPACITY
                                                                                                 : 1);
    }
    for (const BoundaryPort& port : mBoundaryPorts)
    {
        const bool isHighlighted =
            (port.IsOutput == true ? highlights.Outputs : highlights.Inputs).contains(port.Endpoint);
        QPen pen(isHighlighted == true ? GetCanvasColour(design::ThemeColour::SelectionAmber)
                                       : GetCanvasColour(design::ThemeColour::TextHud),
                 isHighlighted == true ? 2 : 1);
        pen.setCosmetic(true);
        port.Item->setPen(pen);
    }
}

void RenderGraphComments::renderFrames()
{
    qDeleteAll(mItems);
    mItems.clear();
    mBoundaryPorts.clear();
    mCounts.clear();
    mPlanSummaries.clear();
    for (const QJsonValueConstRef value : mFrames)
    {
        const QJsonObject frame = value.toObject();
        const QString identity = frame.value(QStringLiteral("id")).toString();
        const QStringList members = mMembers.value(identity);
        const bool isCollapsed = members.isEmpty() == false && GetCollapsedFrame(members.first()) == identity;
        QStringList inputs;
        QStringList outputs;
        QJsonArray connections = mConfigurationRecord.value(QStringLiteral("edges")).toArray();
        for (const QJsonValueConstRef output : mConfigurationRecord.value(QStringLiteral("outputs")).toArray())
        {
            const QJsonObject record = output.toObject();
            connections.append(QJsonObject{
                {QStringLiteral("from"), record.value(QStringLiteral("source"))},
                {QStringLiteral("to"), QStringLiteral("#outputs.") + record.value(QStringLiteral("id")).toString()}});
        }
        for (const QJsonValueConstRef connection : connections)
        {
            const QJsonObject edge = connection.toObject();
            const QString from = edge.value(QStringLiteral("from")).toString();
            const QString to = edge.value(QStringLiteral("to")).toString();
            if (members.contains(GetNode(from)) != members.contains(GetNode(to)))
            {
                if (members.contains(GetNode(from)) == true && outputs.contains(from) == false)
                {
                    outputs.append(from);
                }
                if (members.contains(GetNode(to)) == true && inputs.contains(to) == false)
                {
                    inputs.append(to);
                }
            }
        }
        inputs.sort();
        outputs.sort();
        QRectF bounds = GetRenderGraphCommentBounds(frame);
        if (isCollapsed == true)
        {
            bounds.setSize(
                QSizeF(qMin(bounds.width(), STAGE_WIDTH),
                       HEADER_HEIGHT + STAGE_ROW_HEIGHT * (STAGE_METADATA_ROWS +
                                                           static_cast<float64>(qMax(inputs.size(), outputs.size())))));
        }
        QColor colour(frame.value(QStringLiteral("colour")).toString());
        const QColor border = colour;
        colour.setAlpha(isCollapsed == true ? STAGE_BACKGROUND_ALPHA : BACKGROUND_ALPHA);
        QPen pen(border, 2);
        pen.setCosmetic(true);
        QGraphicsRectItem* item = mScene.addRect(bounds, pen, colour);
        item->setZValue(FRAME_DEPTH);
        item->setAcceptedMouseButtons(Qt::NoButton);
        item->setData(0, QStringLiteral("#comment:") + frame.value(QStringLiteral("id")).toString());
        item->setToolTip(QStringLiteral("<qt><b>%1</b><br/>%2</qt>")
                             .arg(frame.value(QStringLiteral("title")).toString().toHtmlEscaped(),
                                  frame.value(QStringLiteral("notes"))
                                      .toString()
                                      .toHtmlEscaped()
                                      .replace(QLatin1Char('\n'), QStringLiteral("<br/>"))));
        QColor headerColour = border;
        headerColour.setAlpha(HEADER_ALPHA);
        QGraphicsRectItem* header =
            new QGraphicsRectItem(QRectF(bounds.topLeft(), QSizeF(bounds.width(), HEADER_HEIGHT)), item);
        header->setBrush(headerColour);
        header->setPen(Qt::NoPen);
        QGraphicsSimpleTextItem* title = new QGraphicsSimpleTextItem(header);
        QFont font = title->font();
        font.setBold(true);
        title->setFont(font);
        title->setBrush(Qt::white);
        title->setText(QFontMetricsF(font).elidedText((isCollapsed == true ? QStringLiteral("▶ ") : QString{}) +
                                                          frame.value(QStringLiteral("title")).toString(),
                                                      Qt::ElideRight,
                                                      bounds.width() - 2 * LABEL_PADDING));
        title->setPos(bounds.topLeft() + QPointF(LABEL_PADDING, LABEL_PADDING));
        if (isCollapsed == true)
        {
            QGraphicsSimpleTextItem* counts = new QGraphicsSimpleTextItem(item);
            counts->setBrush(Qt::white);
            counts->setPos(bounds.topLeft() + QPointF(LABEL_PADDING, HEADER_HEIGHT));
            counts->setToolTip(QStringLiteral(
                "Node-targeted report errors in this stage; graph-level errors remain in the diagnostic toolbar."));
            mCounts.insert(identity, counts);
            QGraphicsSimpleTextItem* summary = new QGraphicsSimpleTextItem(item);
            summary->setBrush(Qt::white);
            summary->setPos(bounds.topLeft() + QPointF(LABEL_PADDING, HEADER_HEIGHT + STAGE_ROW_HEIGHT));
            mPlanSummaries.insert(identity, summary);
            for (const bool isOutput : {false, true})
            {
                const QStringList& endpoints = isOutput == true ? outputs : inputs;
                for (int32 row = 0; row < endpoints.size(); ++row)
                {
                    const QString endpoint = endpoints.at(row);
                    const QPointF position(isOutput == true ? bounds.right() : bounds.left(),
                                           bounds.top() + HEADER_HEIGHT +
                                               STAGE_ROW_HEIGHT * (STAGE_METADATA_ROWS + row));
                    QGraphicsEllipseItem* connector = new QGraphicsEllipseItem(-STAGE_PORT_RADIUS,
                                                                               -STAGE_PORT_RADIUS,
                                                                               2 * STAGE_PORT_RADIUS,
                                                                               2 * STAGE_PORT_RADIUS,
                                                                               item);
                    connector->setPos(position);
                    connector->setBrush(GetCanvasColour(design::ThemeColour::StatusSuccess));
                    connector->setPen(QPen(GetCanvasColour(design::ThemeColour::TextHud)));
                    const QString description =
                        endpoint + QStringLiteral(" · ") + mCanvas.GetPortType(endpoint, isOutput);
                    connector->setToolTip(description.toHtmlEscaped());
                    connector->setData(0, QStringLiteral("#stage-port:") + endpoint);
                    QGraphicsSimpleTextItem* label = new QGraphicsSimpleTextItem(item);
                    label->setBrush(Qt::white);
                    const float64 labelWidth = bounds.width() / 2 - 2 * LABEL_PADDING;
                    label->setText(QFontMetricsF(label->font()).elidedText(endpoint, Qt::ElideMiddle, labelWidth));
                    label->setToolTip(description.toHtmlEscaped());
                    label->setPos(isOutput == true ? bounds.right() - LABEL_PADDING - label->boundingRect().width()
                                                   : bounds.left() + LABEL_PADDING,
                                  position.y() - label->boundingRect().height() / 2);
                    mBoundaryPorts.append({endpoint, isOutput, connector});
                }
            }
            mItems.append(item);
            continue;
        }
        // Plain text only: notes cannot become HTML, external links or executable document content.
        QGraphicsTextItem* notes = new QGraphicsTextItem(item);
        notes->setDefaultTextColor(Qt::white);
        notes->setTextWidth(bounds.width() - 2 * LABEL_PADDING);
        QString text = frame.value(QStringLiteral("notes")).toString();
        constexpr qsizetype MAX_PREVIEW_CODE_UNITS = 120;
        if (text.size() > MAX_PREVIEW_CODE_UNITS)
        {
            text = text.left(MAX_PREVIEW_CODE_UNITS) + QStringLiteral("…");
        }
        text.replace(QLatin1Char('\n'), QLatin1Char(' '));
        notes->setPlainText(
            QFontMetricsF(notes->font()).elidedText(text, Qt::ElideRight, bounds.width() - 2 * LABEL_PADDING));
        notes->setPos(bounds.topLeft() + QPointF(LABEL_PADDING, HEADER_HEIGHT));
        QGraphicsRectItem* handle = new QGraphicsRectItem(GetResizeHandle(bounds), item);
        handle->setPen(pen);
        handle->setBrush(border);
        handle->setToolTip(QStringLiteral("Drag to resize this comment frame"));
        mItems.append(item);
    }
    RefreshCounts();
}

void RenderGraphComments::publish(const QJsonArray& frames, const QVector<RenderGraphNodePosition>& positions)
{
    if (isCurrent() == false || AreRenderGraphCommentFramesValid(frames) == false)
    {
        Q_EMIT Rejected(QStringLiteral("The graph changed or the frame exceeds its limits. Reopen the frame action."));
        return;
    }
    if (frames != mFrames || positions.isEmpty() == false)
    {
        Q_EMIT EditRequested(frames, positions);
    }
}

void RenderGraphComments::CreateFrame(QPointF position)
{
    if (isCurrent() == false || mFrames.size() >= layout_policy::MAX_COMMENT_FRAMES)
    {
        Q_EMIT Rejected(QStringLiteral("Open a valid configuration with room for another comment frame."));
        return;
    }
    QRectF bounds;
    const QStringList selected = mCanvas.GetSelectedNodes();
    for (const RenderGraphLayoutNode& node : mCanvas.GetAuthoredNodeBounds())
    {
        if (selected.contains(node.Identity) == true)
        {
            bounds = bounds.united(node.Bounds);
        }
    }
    bounds = bounds.isEmpty() == true ? QRectF(position, QSizeF(DEFAULT_WIDTH, DEFAULT_HEIGHT))
                                      : bounds.adjusted(-CONTENT_PADDING,
                                                        -HEADER_HEIGHT - NOTES_HEIGHT - CONTENT_PADDING,
                                                        CONTENT_PADDING,
                                                        CONTENT_PADDING);
    bounds.setWidth(qMax(bounds.width(), static_cast<float64>(layout_policy::MIN_COMMENT_WIDTH)));
    bounds.setHeight(qMax(bounds.height(), static_cast<float64>(layout_policy::MIN_COMMENT_HEIGHT)));
    int32 suffix = 1;
    QString identity;
    do
    {
        identity = QStringLiteral("comment%1").arg(suffix++);
    } while (findFrame(identity) >= 0);
    QJsonObject frame{{QStringLiteral("id"), identity},
                      {QStringLiteral("title"), QStringLiteral("Pipeline stage")},
                      {QStringLiteral("notes"), QString()},
                      {QStringLiteral("colour"), QStringLiteral("#547FA3")}};
    frame = SetRenderGraphCommentBounds(frame, bounds);
    editFrame(frame, true);
}

void RenderGraphComments::editFrame(QJsonObject frame, bool isNew)
{
    const QString file = mFile;
    const QString configuration = mConfiguration;
    const uint64 revision = mRevision;
    QDialog dialog(&mCanvas);
    dialog.setObjectName(QStringLiteral("renderGraphCommentDialog"));
    dialog.setWindowTitle(isNew == true ? QStringLiteral("Add Comment Frame") : QStringLiteral("Edit Comment Frame"));
    dialog.resize(DIALOG_WIDTH, DIALOG_HEIGHT);
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QFormLayout* form = new QFormLayout;
    QLineEdit* title = new QLineEdit(frame.value(QStringLiteral("title")).toString(), &dialog);
    title->setObjectName(QStringLiteral("renderGraphCommentTitle"));
    title->setMaxLength(2 * layout_policy::MAX_COMMENT_TITLE_CODEPOINTS);
    QPlainTextEdit* notes = new QPlainTextEdit(frame.value(QStringLiteral("notes")).toString(), &dialog);
    notes->setObjectName(QStringLiteral("renderGraphCommentNotes"));
    QLineEdit* colour = new QLineEdit(frame.value(QStringLiteral("colour")).toString(), &dialog);
    colour->setObjectName(QStringLiteral("renderGraphCommentColour"));
    QPushButton* chooseColour = new QPushButton(QStringLiteral("Choose Colour…"), &dialog);
    chooseColour->setObjectName(QStringLiteral("renderGraphChooseCommentColour"));
    connect(chooseColour, &QPushButton::clicked, &dialog, [&dialog, colour]() {
        const QColor chosen = QColorDialog::getColor(QColor(colour->text()), &dialog, QStringLiteral("Frame Colour"));
        if (chosen.isValid() == true)
        {
            colour->setText(chosen.name());
        }
    });
    form->addRow(QStringLiteral("Title"), title);
    form->addRow(QStringLiteral("Notes"), notes);
    form->addRow(QStringLiteral("Colour (#RRGGBB)"), colour);
    form->addRow(QString(), chooseColour);
    layout->addLayout(form);
    QLabel* feedback = new QLabel(&dialog);
    feedback->setWordWrap(true);
    feedback->setObjectName(QStringLiteral("renderGraphCommentFeedback"));
    layout->addWidget(feedback);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons,
            &QDialogButtonBox::accepted,
            &dialog,
            [this, &dialog, &frame, file, configuration, revision, title, notes, colour, feedback]() {
                if (file != mCanvas.GetFile() || configuration != mCanvas.GetConfiguration() ||
                    revision != mCanvas.GetDocumentRevision())
                {
                    feedback->setText(
                        QStringLiteral("The graph changed while editing. Cancel and reopen this action."));
                    return;
                }
                QJsonObject candidate = frame;
                candidate.insert(QStringLiteral("title"), title->text());
                candidate.insert(QStringLiteral("notes"), notes->toPlainText());
                candidate.insert(QStringLiteral("colour"), colour->text());
                if (AreRenderGraphCommentFramesValid(QJsonArray{candidate}) == false)
                {
                    feedback->setText(
                        QStringLiteral("Use a nonempty title (up to %1 characters), notes up to %2 characters, "
                                       "a #RRGGBB colour and a frame within canvas bounds.")
                            .arg(layout_policy::MAX_COMMENT_TITLE_CODEPOINTS)
                            .arg(layout_policy::MAX_COMMENT_NOTES_CODEPOINTS));
                    return;
                }
                frame = Move(candidate);
                dialog.accept();
            });
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    QJsonArray frames = mFrames;
    if (isNew == true)
    {
        frames.append(frame);
    }
    else
    {
        const int32 index = findFrame(frame.value(QStringLiteral("id")).toString());
        if (index < 0)
        {
            return;
        }
        frames.replace(index, frame);
    }
    publish(frames);
}

void RenderGraphComments::EditFrame(const QString& identity)
{
    const int32 index = findFrame(identity);
    if (index >= 0 && isCurrent() == true)
    {
        editFrame(mFrames.at(index).toObject(), false);
    }
}
void RenderGraphComments::RemoveFrame(const QString& identity)
{
    const int32 index = findFrame(identity);
    if (index >= 0)
    {
        QJsonArray frames = mFrames;
        frames.removeAt(index);
        publish(frames);
    }
}

bool RenderGraphComments::ShowContextMenu(QPointF position, const QPoint& globalPosition)
{
    const int32 index = hitFrame(position);
    if (index < 0)
    {
        return false;
    }
    const QString identity = mFrames.at(index).toObject().value(QStringLiteral("id")).toString();
    const QString file = mFile;
    const QString configuration = mConfiguration;
    const uint64 revision = mRevision;
    QMenu menu(&mCanvas);
    menu.setObjectName(QStringLiteral("renderGraphCommentMenu"));
    const bool isCollapsed = mCollapsedOwners.values().contains(identity) == true ||
                             (mFrames.at(index).toObject().value(QStringLiteral("collapsed")).toBool() == true &&
                              CanCollapse(identity) == false);
    QAction* collapse =
        menu.addAction(isCollapsed == true ? QStringLiteral("Expand Stage") : QStringLiteral("Collapse Stage"));
    collapse->setObjectName(QStringLiteral("renderGraphToggleStage"));
    collapse->setEnabled(isCollapsed == true || CanCollapse(identity) == true);
    collapse->setToolTip(QStringLiteral("Collapse nonempty, non-overlapping frames. Double-click a collapsed card to "
                                        "expand. Saved node and wire positions are retained."));
    QAction* edit = menu.addAction(QStringLiteral("Edit Comment Frame…"));
    edit->setObjectName(QStringLiteral("renderGraphEditComment"));
    QAction* remove = menu.addAction(QStringLiteral("Remove Comment Frame"));
    remove->setObjectName(QStringLiteral("renderGraphRemoveComment"));
    QAction* chosenOrNull = menu.exec(globalPosition);
    if (isCurrent() == true && file == mFile && configuration == mConfiguration && revision == mRevision)
    {
        if (chosenOrNull == collapse)
        {
            SetCollapsed(identity, isCollapsed == false);
        }
        else if (chosenOrNull == edit)
        {
            EditFrame(identity);
        }
        else if (chosenOrNull == remove)
        {
            RemoveFrame(identity);
        }
    }
    return true;
}

bool RenderGraphComments::ActivateAt(QPointF position)
{
    const int32 index = hitFrame(position);
    if (index < 0)
    {
        return false;
    }
    const QString identity = mFrames.at(index).toObject().value(QStringLiteral("id")).toString();
    if (mCollapsedOwners.values().contains(identity) == true)
    {
        SetCollapsed(identity, false);
    }
    else
    {
        EditFrame(identity);
    }
    return true;
}

bool RenderGraphComments::BeginGesture(QPointF position)
{
    if (isCurrent() == false || mGestureIndex >= 0)
    {
        return false;
    }
    const int32 index = hitFrame(position);
    if (index < 0)
    {
        return false;
    }
    mGestureIndex = index;
    mPressPosition = position;
    mBeforeBounds = GetRenderGraphCommentBounds(mFrames.at(index).toObject());
    mPreviewBounds = mBeforeBounds;
    const QString identity = mFrames.at(index).toObject().value(QStringLiteral("id")).toString();
    mIsResizing =
        mCollapsedOwners.values().contains(identity) == false && GetResizeHandle(mBeforeBounds).contains(position);
    mBeforePositions.clear();
    if (mIsResizing == false)
    {
        for (const RenderGraphLayoutNode& node : mCanvas.GetAuthoredNodeBounds())
        {
            if (mBeforeBounds.contains(node.Bounds) == true)
            {
                mBeforePositions.append({node.Identity, qRound(node.Bounds.x()), qRound(node.Bounds.y())});
            }
        }
    }
    mPreviewPositions = mBeforePositions;
    return true;
}

bool RenderGraphComments::UpdateGesture(QPointF position)
{
    if (mGestureIndex < 0)
    {
        return false;
    }
    if (isCurrent() == false)
    {
        static_cast<void>(CancelGesture());
        return true;
    }
    if (math::IsFinite(position.x()) == false || math::IsFinite(position.y()) == false)
    {
        return true;
    }
    constexpr float64 LIMIT = layout_policy::MAX_COORDINATE;
    const QPointF offset(qRound(qBound(-2 * LIMIT, position.x() - mPressPosition.x(), 2 * LIMIT)),
                         qRound(qBound(-2 * LIMIT, position.y() - mPressPosition.y(), 2 * LIMIT)));
    if (mIsResizing == true)
    {
        mPreviewBounds = QRectF(mBeforeBounds.topLeft(),
                                QSizeF(qBound(static_cast<float64>(layout_policy::MIN_COMMENT_WIDTH),
                                              mBeforeBounds.width() + offset.x(),
                                              LIMIT - mBeforeBounds.x()),
                                       qBound(static_cast<float64>(layout_policy::MIN_COMMENT_HEIGHT),
                                              mBeforeBounds.height() + offset.y(),
                                              LIMIT - mBeforeBounds.y())));
    }
    else
    {
        const QPointF bounded(qBound(-LIMIT - mBeforeBounds.left(), offset.x(), LIMIT - mBeforeBounds.right()),
                              qBound(-LIMIT - mBeforeBounds.top(), offset.y(), LIMIT - mBeforeBounds.bottom()));
        mPreviewBounds = mBeforeBounds.translated(bounded);
        TranslatePositions(mBeforePositions, bounded, mPreviewPositions);
        mCanvas.PreviewNodePositions(mPreviewPositions);
    }
    mItems.at(mGestureIndex)->setPos(mPreviewBounds.topLeft() - mBeforeBounds.topLeft());
    // Resize redraws all child geometry from copied metadata, retaining the saved array for cancellation.
    if (mIsResizing == true)
    {
        const QJsonArray saved = mFrames;
        mFrames.replace(mGestureIndex,
                        SetRenderGraphCommentBounds(mFrames.at(mGestureIndex).toObject(), mPreviewBounds));
        renderFrames();
        mFrames = saved;
    }
    mCanvas.RefreshCommentPresentation();
    return true;
}

bool RenderGraphComments::EndGesture()
{
    if (mGestureIndex < 0)
    {
        return false;
    }
    if (isCurrent() == false)
    {
        static_cast<void>(CancelGesture());
        Q_EMIT Rejected(QStringLiteral("The graph changed during this gesture. Its preview was cancelled."));
        return true;
    }
    const int32 index = mGestureIndex;
    mGestureIndex = -1;
    if (mPreviewBounds != mBeforeBounds)
    {
        QJsonArray frames = mFrames;
        frames.replace(index, SetRenderGraphCommentBounds(frames.at(index).toObject(), mPreviewBounds));
        publish(frames, mPreviewPositions);
    }
    return true;
}

bool RenderGraphComments::CancelGesture()
{
    if (mGestureIndex < 0)
    {
        return false;
    }
    mGestureIndex = -1;
    mCanvas.PreviewNodePositions(mBeforePositions);
    renderFrames();
    mCanvas.RefreshCommentPresentation();
    return true;
}
} // namespace ludus::editor
