#include "internal/render_graph_comments.h"
#include "internal/render_graph_canvas.h"

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
constexpr float64 NOTES_HEIGHT = 50;
constexpr float64 CONTENT_PADDING = 24;
constexpr float64 LABEL_PADDING = 8;
constexpr float64 RESIZE_HANDLE = 18;
constexpr int32 BACKGROUND_ALPHA = 28;
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
    mFrames = {};
}

void RenderGraphComments::Rebuild(const QJsonObject& configuration)
{
    Clear();
    mFile = mCanvas.GetFile();
    mConfiguration = mCanvas.GetConfiguration();
    mRevision = mCanvas.GetDocumentRevision();
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
    renderFrames();
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
        if (QRectF(bounds.topLeft(), QSizeF(bounds.width(), HEADER_HEIGHT + NOTES_HEIGHT)).contains(position) == true ||
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

void RenderGraphComments::renderFrames()
{
    qDeleteAll(mItems);
    mItems.clear();
    for (const QJsonValueConstRef value : mFrames)
    {
        const QJsonObject frame = value.toObject();
        const QRectF bounds = GetRenderGraphCommentBounds(frame);
        QColor colour(frame.value(QStringLiteral("colour")).toString());
        const QColor border = colour;
        colour.setAlpha(BACKGROUND_ALPHA);
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
        title->setText(QFontMetricsF(font).elidedText(frame.value(QStringLiteral("title")).toString(),
                                                      Qt::ElideRight,
                                                      bounds.width() - 2 * LABEL_PADDING));
        title->setPos(bounds.topLeft() + QPointF(LABEL_PADDING, LABEL_PADDING));
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
    QAction* edit = menu.addAction(QStringLiteral("Edit Comment Frame…"));
    edit->setObjectName(QStringLiteral("renderGraphEditComment"));
    QAction* remove = menu.addAction(QStringLiteral("Remove Comment Frame"));
    remove->setObjectName(QStringLiteral("renderGraphRemoveComment"));
    QAction* chosenOrNull = menu.exec(globalPosition);
    if (isCurrent() == true && file == mFile && configuration == mConfiguration && revision == mRevision)
    {
        if (chosenOrNull == edit)
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
    EditFrame(mFrames.at(index).toObject().value(QStringLiteral("id")).toString());
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
    mIsResizing = GetResizeHandle(mBeforeBounds).contains(position);
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
    return true;
}
} // namespace ludus::editor
