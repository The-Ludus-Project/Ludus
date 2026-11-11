#include "internal/edit_history.h"
#include "design_tokens.h"

#include "internal/theme_preferences.h"

#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QPalette>
#include <QPropertyAnimation>
#include <QTimer>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 TOAST_HORIZONTAL_PADDING = design::SPACING_MEDIUM;
constexpr foundation::int32 TOAST_VERTICAL_PADDING = design::SPACING_SMALL;
constexpr foundation::int32 TOAST_BOTTOM_MARGIN = design::SPACING_PAGE;
constexpr foundation::int32 TOAST_BORDER_WIDTH = 1;
constexpr foundation::int32 PANEL_ROW_SPACING = design::SPACING_EXTRA_SMALL;
const QString VERB_SEPARATOR = QStringLiteral(": ");

/// Elides only text that does not fit; integer advances round down, so eliding at an exact fit would cut a short name.
QString ElideToFit(const QFontMetrics& metrics, const QString& text, foundation::int32 width)
{
    return metrics.horizontalAdvance(text) <= width ? text : metrics.elidedText(text, Qt::ElideRight, width);
}

/// Width of a notice drawn as its verb and its name, measured as paintEvent draws them so the two never disagree.
foundation::int32 MeasureNotice(const QFontMetrics& metrics, const QString& text)
{
    const qsizetype split = text.indexOf(VERB_SEPARATOR);
    if (split < 0)
    {
        return metrics.horizontalAdvance(text);
    }
    const qsizetype nameStart = split + VERB_SEPARATOR.size();
    return metrics.horizontalAdvance(text.left(nameStart)) + metrics.horizontalAdvance(text.mid(nameStart));
}

QString MakeMenuText(const QString& verb, const QString& name)
{
    return name.isEmpty() == true ? verb : verb + QLatin1Char(' ') + name;
}
} // namespace

QString MakeUndoMenuText(const QString& name)
{
    return MakeMenuText(QStringLiteral("&Undo"), name);
}

QString MakeRedoMenuText(const QString& name)
{
    return MakeMenuText(QStringLiteral("&Redo"), name);
}

QString MakeUndoFeedbackText(const QString& name)
{
    return QStringLiteral("Undid") + VERB_SEPARATOR + name;
}

QString MakeRedoFeedbackText(const QString& name)
{
    return QStringLiteral("Redid") + VERB_SEPARATOR + name;
}

QString MakeHistoryJumpFeedbackText(const EditHistoryState& before, foundation::int32 cursor)
{
    const foundation::int32 count = static_cast<foundation::int32>(before.Names.size());
    if (cursor < 0 || cursor > count || before.Cursor < 0 || before.Cursor > count || cursor == before.Cursor)
    {
        return {};
    }
    if (cursor < before.Cursor)
    {
        const foundation::int32 steps = before.Cursor - cursor;
        if (steps == 1)
        {
            return MakeUndoFeedbackText(before.Names.at(cursor));
        }
        const QString reached = cursor == 0 ? QString::fromUtf8(EditHistoryModel::INITIAL_STATE_NAME).toLower()
                                            : before.Names.at(cursor - 1);
        return QStringLiteral("Undid %1 edits, back to").arg(steps) + VERB_SEPARATOR + reached;
    }
    const foundation::int32 steps = cursor - before.Cursor;
    if (steps == 1)
    {
        return MakeRedoFeedbackText(before.Names.at(before.Cursor));
    }
    return QStringLiteral("Redid %1 edits, up to").arg(steps) + VERB_SEPARATOR + before.Names.at(cursor - 1);
}

EditHistoryModel::EditHistoryModel(QObject* parentOrNull) : QAbstractListModel(parentOrNull) {}

void EditHistoryModel::SetState(const EditHistoryState& state)
{
    if (state == mState)
    {
        return;
    }
    beginResetModel();
    mState = state;
    endResetModel();
}

int EditHistoryModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid() == true || mState.DocumentName.isEmpty() == true)
    {
        return 0;
    }
    return static_cast<int>(mState.Names.size()) + 1;
}

QVariant EditHistoryModel::data(const QModelIndex& index, int role) const
{
    if (index.isValid() == false || index.row() >= rowCount())
    {
        return {};
    }
    const foundation::int32 row = index.row();
    const bool isCurrent = row == mState.Cursor;
    const bool isUndone = row > mState.Cursor;
    const QString name = row == 0 ? QString::fromUtf8(INITIAL_STATE_NAME) : mState.Names.at(row - 1);
    switch (role)
    {
        case Qt::DisplayRole:
            return name;
        case Qt::AccessibleTextRole:
            return isCurrent == true  ? name + QStringLiteral(", current state")
                   : isUndone == true ? name + QStringLiteral(", undone")
                                      : name;
        case Qt::ToolTipRole:
            if (isCurrent == true)
            {
                return QStringLiteral("The document is in this state now.");
            }
            return mState.CanJump == true
                       ? (isUndone == true ? QStringLiteral("Redo up to here.") : QStringLiteral("Undo back to here."))
                       : QVariant{};
        case Qt::FontRole:
            if (isCurrent == true)
            {
                QFont font;
                font.setBold(true);
                return font;
            }
            return {};
        case Qt::ForegroundRole:
            // Undone edits read as quieter text (text.secondary); they return with Redo or a click.
            return isUndone == true ? QVariant::fromValue(QPalette().brush(QPalette::PlaceholderText)) : QVariant{};
        case IsCurrentRole:
            return isCurrent;
        case IsUndoneRole:
            return isUndone;
        default:
            return {};
    }
}

Qt::ItemFlags EditHistoryModel::flags(const QModelIndex& index) const
{
    if (index.isValid() == false)
    {
        return Qt::NoItemFlags;
    }
    return mState.CanJump == true ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::ItemIsSelectable;
}

EditHistoryPanel::EditHistoryPanel(QWidget* parentOrNull)
    : QWidget(parentOrNull), mModel(new EditHistoryModel(this)), mList(new QListView(this)), mCaption(new QLabel(this)),
      mEmpty(new QLabel(this))
{
    setObjectName(QStringLiteral("editHistoryPanel"));
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(design::SPACING_SMALL, design::SPACING_SMALL, design::SPACING_SMALL, 0);
    layout->setSpacing(design::SPACING_SMALL);
    mCaption->setObjectName(QStringLiteral("editHistoryCaption"));
    mCaption->setForegroundRole(QPalette::PlaceholderText);
    mCaption->setWordWrap(true);
    mList->setObjectName(QStringLiteral("editHistoryList"));
    mList->setAccessibleName(QStringLiteral("Undo history"));
    mList->setModel(mModel);
    mList->setSelectionMode(QAbstractItemView::SingleSelection);
    mList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mList->setUniformItemSizes(true);
    mList->setSpacing(PANEL_ROW_SPACING);
    mList->setFrameShape(QFrame::NoFrame);
    mEmpty->setObjectName(QStringLiteral("editHistoryEmpty"));
    mEmpty->setForegroundRole(QPalette::PlaceholderText);
    mEmpty->setWordWrap(true);
    mEmpty->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->addWidget(mCaption);
    layout->addWidget(mList, 1);
    layout->addWidget(mEmpty, 1);
    connect(mList, &QListView::clicked, this, &EditHistoryPanel::requestJump);
    connect(mList, &QListView::activated, this, &EditHistoryPanel::requestJump);
    renderCaption();
}

void EditHistoryPanel::SetState(const EditHistoryState& state)
{
    if (state == mModel->GetState() && mList->currentIndex().row() == state.Cursor)
    {
        return;
    }
    mModel->SetState(state);
    const QModelIndex current = mModel->index(state.Cursor, 0);
    if (current.isValid() == true)
    {
        mList->setCurrentIndex(current);
        mList->scrollTo(current);
    }
    renderCaption();
}

void EditHistoryPanel::requestJump(const QModelIndex& index)
{
    const EditHistoryState& state = mModel->GetState();
    if (index.isValid() == false || state.CanJump == false || index.row() == state.Cursor)
    {
        return;
    }
    Q_EMIT JumpRequested(index.row());
}

void EditHistoryPanel::renderCaption()
{
    const EditHistoryState& state = mModel->GetState();
    const bool hasRows = mModel->rowCount() > 0;
    const bool hasEdits = state.Names.isEmpty() == false;
    if (state.DocumentName.isEmpty() == true)
    {
        mCaption->setText(QString());
        mEmpty->setText(state.CanJump == true
                            ? QStringLiteral("Open a document to see its edits here.")
                            : QStringLiteral("This field keeps its own typing history. Use Undo and Redo, or "
                                             "click the document to see its edits here."));
    }
    else
    {
        const qsizetype undone = state.Names.size() - state.Cursor;
        QString caption = state.Names.size() == 1
                              ? QStringLiteral("%1 · 1 edit").arg(state.DocumentName)
                              : QStringLiteral("%1 · %2 edits").arg(state.DocumentName).arg(state.Names.size());
        if (undone > 0)
        {
            caption += QStringLiteral(" (%1 undone)").arg(undone);
        }
        mCaption->setText(caption);
        mEmpty->setText(QStringLiteral("No edits yet. Changes to the %1 appear here; click one to return to it.")
                            .arg(state.DocumentName));
    }
    mCaption->setVisible(state.DocumentName.isEmpty() == false);
    mList->setVisible(hasRows == true && hasEdits == true);
    mEmpty->setVisible(hasRows == false || hasEdits == false);
}

UndoFeedbackToast::UndoFeedbackToast(QWidget* parentOrNull)
    : QWidget(parentOrNull,
              Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint),
      mTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("undoFeedbackToast"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    mTimer->setSingleShot(true);
    mTimer->setInterval(VISIBLE_MILLISECONDS);
    connect(mTimer, &QTimer::timeout, this, &UndoFeedbackToast::beginFadeOut);
}

void UndoFeedbackToast::ShowNotice(const QString& text, const QWidget& anchor)
{
    mText = text;
    setAccessibleName(text);
    // Measure with the polished font that paintEvent uses; a tool-tip window takes its font on polish.
    ensurePolished();
    const QFontMetrics metrics(font());
    const QSize size(MeasureNotice(metrics, text) + 2 * TOAST_HORIZONTAL_PADDING,
                     metrics.height() + 2 * TOAST_VERTICAL_PADDING);
    const QRect area(anchor.mapToGlobal(QPoint(0, 0)), anchor.size());
    const foundation::int32 width = qMin(size.width(), qMax(area.width() - 2 * TOAST_BOTTOM_MARGIN, size.height()));
    resize(width, size.height());
    move(area.center().x() - width / 2, area.bottom() - TOAST_BOTTOM_MARGIN - size.height());
    const foundation::int32 fade = GetMotionDurationMilliseconds(GetThemePreferences(), FADE_MILLISECONDS);
    if (mFadeOrNull != nullptr)
    {
        mFadeOrNull->stop();
    }
    if (fade > 0 && isVisible() == false)
    {
        if (mFadeOrNull == nullptr)
        {
            mFadeOrNull = new QPropertyAnimation(this, QByteArrayLiteral("windowOpacity"), this);
            mFadeOrNull->setEasingCurve(QEasingCurve::OutCubic);
        }
        disconnect(mFadeOrNull, &QPropertyAnimation::finished, this, nullptr);
        mFadeOrNull->setDuration(fade);
        mFadeOrNull->setStartValue(0.0);
        mFadeOrNull->setEndValue(1.0);
        setWindowOpacity(0.0);
        show();
        mFadeOrNull->start();
    }
    else
    {
        // Reduced motion, or a notice already showing: appear at full strength at once.
        setWindowOpacity(1.0);
        show();
    }
    raise();
    update();
    mTimer->start();
}

void UndoFeedbackToast::Dismiss()
{
    mTimer->stop();
    if (mFadeOrNull != nullptr)
    {
        mFadeOrNull->stop();
    }
    hide();
    setWindowOpacity(1.0);
}

void UndoFeedbackToast::beginFadeOut()
{
    const foundation::int32 fade = GetMotionDurationMilliseconds(GetThemePreferences(), FADE_MILLISECONDS);
    if (fade <= 0 || mFadeOrNull == nullptr)
    {
        Dismiss();
        return;
    }
    mFadeOrNull->stop();
    disconnect(mFadeOrNull, &QPropertyAnimation::finished, this, nullptr);
    mFadeOrNull->setDuration(fade);
    mFadeOrNull->setStartValue(windowOpacity());
    mFadeOrNull->setEndValue(0.0);
    connect(mFadeOrNull, &QPropertyAnimation::finished, this, &UndoFeedbackToast::Dismiss);
    mFadeOrNull->start();
}

void UndoFeedbackToast::paintEvent(QPaintEvent* event)
{
    static_cast<void>(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette& colours = palette();
    const qreal inset = static_cast<qreal>(TOAST_BORDER_WIDTH) / 2;
    painter.setPen(QPen(colours.color(QPalette::Mid), TOAST_BORDER_WIDTH));
    painter.setBrush(colours.color(QPalette::ToolTipBase));
    painter.drawRoundedRect(QRectF(rect()).adjusted(inset, inset, -inset, -inset),
                            design::RADIUS_GROUP,
                            design::RADIUS_GROUP);
    const QRect text = rect().adjusted(TOAST_HORIZONTAL_PADDING, 0, -TOAST_HORIZONTAL_PADDING, 0);
    const qsizetype split = mText.indexOf(VERB_SEPARATOR);
    const QFontMetrics metrics(font());
    if (split < 0)
    {
        painter.setPen(colours.color(QPalette::ToolTipText));
        painter.drawText(text, Qt::AlignCenter, ElideToFit(metrics, mText, text.width()));
        return;
    }
    // The verb ("Undid:") is quieter than the command it names, so the eye lands on what changed.
    const QString verb = mText.left(split + VERB_SEPARATOR.size());
    const QString name = mText.mid(split + VERB_SEPARATOR.size());
    const foundation::int32 verbWidth = metrics.horizontalAdvance(verb);
    const foundation::int32 nameWidth = qMin(metrics.horizontalAdvance(name), qMax(0, text.width() - verbWidth));
    const foundation::int32 left = text.left() + qMax(0, (text.width() - verbWidth - nameWidth) / 2);
    painter.setPen(colours.color(QPalette::PlaceholderText));
    painter.drawText(QRect(left, text.top(), verbWidth, text.height()), Qt::AlignLeft | Qt::AlignVCenter, verb);
    painter.setPen(colours.color(QPalette::ToolTipText));
    painter.drawText(QRect(left + verbWidth, text.top(), nameWidth, text.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     ElideToFit(metrics, name, nameWidth));
}
} // namespace ludus::editor
