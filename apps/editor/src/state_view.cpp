#include "internal/state_view.h"
#include "design_tokens.h"

#include "internal/theme_fonts.h"

#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLabel>
#include <QLayout>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QStringList>
#include <QTextDocument>
#include <QTextOption>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace ludus::editor
{
namespace
{
constexpr int STATE_ICON_SIZE = 32;
constexpr int STATE_COLUMN_WIDTH = 440;
constexpr int STATE_SPACING = design::SPACING_SMALL;
constexpr int STATE_ACTION_SPACING = design::SPACING_MEDIUM;
/// How many characters a label of long words asks for at least, so a narrow dock never pushes it wider than itself.
constexpr int LONG_WORD_LABEL_MINIMUM_CHARACTERS = 8;
/// Zero-width character that forbids a line break on either side of it.
constexpr QChar WORD_JOINER = QChar(0x2060);
/// Lines of the details text shown before it scrolls.
constexpr int STATE_DETAILS_VISIBLE_LINES = 5;

/// Lays its widgets out left to right, centred, and starts a new row when the next one does not fit the width, so a
/// narrow dock stacks the actions instead of clipping one. Hidden widgets take no space. GUI thread only.
class ActionFlowLayout final : public QLayout
{
public:
    ActionFlowLayout(QWidget* parentOrNull, int spacingPixels) : QLayout(parentOrNull), mSpacingPixels(spacingPixels)
    {
        setContentsMargins(0, 0, 0, 0);
    }
    ~ActionFlowLayout() override
    {
        qDeleteAll(mItems);
    }
    ActionFlowLayout(const ActionFlowLayout&) = delete;
    ActionFlowLayout& operator=(const ActionFlowLayout&) = delete;

    void addItem(QLayoutItem* item) override
    {
        mItems.append(item);
    }
    [[nodiscard]] int count() const override
    {
        return static_cast<int>(mItems.size());
    }
    [[nodiscard]] QLayoutItem* itemAt(int index) const override
    {
        return mItems.value(index, nullptr);
    }
    [[nodiscard]] QLayoutItem* takeAt(int index) override
    {
        return index >= 0 && index < mItems.size() ? mItems.takeAt(index) : nullptr;
    }
    [[nodiscard]] Qt::Orientations expandingDirections() const override
    {
        return {};
    }
    [[nodiscard]] bool hasHeightForWidth() const override
    {
        return true;
    }
    [[nodiscard]] int heightForWidth(int width) const override
    {
        return arrange(QRect(0, 0, width, 0), false);
    }
    void setGeometry(const QRect& rectangle) override
    {
        QLayout::setGeometry(rectangle);
        static_cast<void>(arrange(rectangle, true));
    }
    [[nodiscard]] QSize sizeHint() const override
    {
        // Every visible action in one row.
        int width = 0;
        int height = 0;
        for (const QLayoutItem* item : mItems)
        {
            if (item->isEmpty() == false)
            {
                width += item->sizeHint().width() + (width > 0 ? mSpacingPixels : 0);
                height = std::max(height, item->sizeHint().height());
            }
        }
        return QSize(width, height);
    }
    [[nodiscard]] QSize minimumSize() const override
    {
        // The widest single action, one row high.
        int width = 0;
        int height = 0;
        for (const QLayoutItem* item : mItems)
        {
            if (item->isEmpty() == false)
            {
                width = std::max(width, item->sizeHint().width());
                height = std::max(height, item->sizeHint().height());
            }
        }
        return QSize(width, height);
    }

private:
    /// Places the actions in area (when shouldPlace) and returns the height they need.
    [[nodiscard]] int arrange(const QRect& area, bool shouldPlace) const
    {
        QVector<QVector<QLayoutItem*>> rows(1);
        QVector<int> rowWidths(1, 0);
        QVector<int> rowHeights(1, 0);
        for (QLayoutItem* item : mItems)
        {
            if (item->isEmpty() == true)
            {
                continue;
            }
            const QSize hint = item->sizeHint();
            const int widthWithItem =
                rowWidths.last() + (rows.last().isEmpty() == true ? 0 : mSpacingPixels) + hint.width();
            if (rows.last().isEmpty() == false && widthWithItem > area.width())
            {
                rows.emplace_back();
                rowWidths.append(0);
                rowHeights.append(0);
            }
            rowWidths.last() += (rows.last().isEmpty() == true ? 0 : mSpacingPixels) + hint.width();
            rowHeights.last() = std::max(rowHeights.last(), hint.height());
            rows.last().push_back(item);
        }
        int y = area.top();
        for (qsizetype row = 0; row < rows.size(); ++row)
        {
            int x = area.left() + std::max((area.width() - rowWidths[row]) / 2, 0);
            for (QLayoutItem* item : rows[row])
            {
                if (shouldPlace == true)
                {
                    const QSize hint = item->sizeHint();
                    const int width = std::min(hint.width(), area.width());
                    item->setGeometry(QRect(x, y + (rowHeights[row] - hint.height()) / 2, width, hint.height()));
                }
                x += item->sizeHint().width() + mSpacingPixels;
            }
            y += rowHeights[row] + (row + 1 < rows.size() ? mSpacingPixels : 0);
        }
        return y - area.top();
    }

    QVector<QLayoutItem*> mItems;
    int mSpacingPixels = 0;
};
} // namespace

LongWordLabel::LongWordLabel(QWidget* parentOrNull) : QLabel(parentOrNull)
{
    setWordWrap(true);
    setTextFormat(Qt::PlainText);
}

void LongWordLabel::SetFullText(const QString& text)
{
    mFullText = text;
    showTextForWidth(width());
}

const QString& LongWordLabel::GetFullText() const noexcept
{
    return mFullText;
}

bool LongWordLabel::IsShortened() const
{
    return text() != mFullText;
}

QSize LongWordLabel::minimumSizeHint() const
{
    // Width for a few characters, one line high: the real height comes from heightForWidth at the width the layout
    // gives, so a long word never sets a minimum wider than the dock.
    return QSize(fontMetrics().averageCharWidth() * LONG_WORD_LABEL_MINIMUM_CHARACTERS + 2 * margin(),
                 fontMetrics().height() + 2 * margin());
}

bool LongWordLabel::hasHeightForWidth() const
{
    return true;
}

int LongWordLabel::heightForWidth(int width) const
{
    const int textWidth = getTextWidth(width);
    const QRect bounds = fontMetrics().boundingRect(QRect(0, 0, textWidth, std::numeric_limits<int>::max() / 2),
                                                    Qt::TextWordWrap,
                                                    getTextForWidth(width));
    const QMargins margins = contentsMargins();
    return bounds.height() + 2 * margin() + margins.top() + margins.bottom();
}

void LongWordLabel::resizeEvent(QResizeEvent* event)
{
    QLabel::resizeEvent(event);
    showTextForWidth(event->size().width());
}

int LongWordLabel::getTextWidth(int width) const
{
    const QMargins margins = contentsMargins();
    return std::max(width - 2 * margin() - margins.left() - margins.right(), 1);
}

QString LongWordLabel::getTextForWidth(int width) const
{
    const QFontMetrics metrics = fontMetrics();
    const int textWidth = getTextWidth(width);
    static const QRegularExpression WORD(QStringLiteral("\\S+"));
    // Qt wraps a path after each of its slashes, so a path breaks into pieces that fit. A piece that is wider than
    // the label by itself, such as a hash or the ID after the slash of a resource ID, has to be shortened.
    static const QRegularExpression PIECE(QStringLiteral("[^/\\\\]*[/\\\\]?"));
    QString shown;
    qsizetype copied = 0;
    for (QRegularExpressionMatchIterator words = WORD.globalMatch(mFullText); words.hasNext();)
    {
        const QRegularExpressionMatch word = words.next();
        shown += mFullText.mid(copied, word.capturedStart() - copied);
        for (QRegularExpressionMatchIterator pieces = PIECE.globalMatch(word.captured()); pieces.hasNext();)
        {
            const QString piece = pieces.next().captured();
            if (metrics.horizontalAdvance(piece) <= textWidth)
            {
                shown += piece;
                continue;
            }
            // Word joiners keep the shortened piece from breaking again at its ellipsis.
            QString elided = metrics.elidedText(piece, Qt::ElideMiddle, textWidth);
            elided.replace(QStringLiteral("…"), QString(WORD_JOINER) + QStringLiteral("…") + QString(WORD_JOINER));
            shown += elided;
        }
        copied = word.capturedEnd();
    }
    return shown + mFullText.mid(copied);
}

void LongWordLabel::showTextForWidth(int width)
{
    const QString shown = getTextForWidth(width);
    if (shown != text())
    {
        setText(shown);
    }
    // The tooltip carries the whole text only when some of it is hidden.
    setToolTip(shown == mFullText ? QString() : mFullText);
    updateGeometry();
}

/// Draws the state's icon in the widget's own text colour, so it also reads on a fixed-colour surface.
class StateIcon final : public QWidget
{
public:
    explicit StateIcon(QWidget* parentOrNull) : QWidget(parentOrNull)
    {
        setFixedSize(STATE_ICON_SIZE, STATE_ICON_SIZE);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    void SetIcon(EditorIcon icon)
    {
        mIcon = icon;
        update();
    }
    void SetColour(const QColor& colour)
    {
        mColour = colour;
        update();
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal scale = static_cast<qreal>(STATE_ICON_SIZE) / EDITOR_ICON_GRID;
        painter.scale(scale, scale);
        QPen pen(mColour.isValid() == true ? mColour : palette().color(QPalette::WindowText));
        pen.setWidthF(EDITOR_ICON_STROKE_WIDTH);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(GetEditorIconOutline(mIcon));
    }

private:
    EditorIcon mIcon = EditorIcon::Information;
    QColor mColour;
};

StateViewContent MakeViewportProblem(const QString& status)
{
    StateViewContent content;
    content.Icon = EditorIcon::Warning;
    content.Message = QStringLiteral("The viewport cannot show the scene.");
    content.Cause = status;
    content.Fix = QStringLiteral("Editing stays available. Update the graphics driver and restart the Editor to "
                                 "bring the viewport back.");
    content.Details = status;
    return content;
}

QString GetStateDetailsText(const StateViewContent& content)
{
    QStringList lines;
    for (const QString& line : {content.Message, content.Cause, content.Fix, content.Details})
    {
        if (line.isEmpty() == false)
        {
            lines.append(line);
        }
    }
    return lines.join(QLatin1Char('\n'));
}

StateView::StateView(QWidget* parentOrNull) : QWidget(parentOrNull)
{
    setObjectName(QStringLiteral("stateView"));
    setMaximumWidth(STATE_COLUMN_WIDTH);
    QVBoxLayout* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(STATE_SPACING);
    mIconOrNull = new StateIcon(this);
    mIconOrNull->setObjectName(QStringLiteral("stateIcon"));
    column->addWidget(mIconOrNull, 0, Qt::AlignHCenter);
    const auto makeLabel = [this, column](const char* name) {
        LongWordLabel* label = new LongWordLabel(this);
        label->setObjectName(QString::fromLatin1(name));
        label->setAlignment(Qt::AlignHCenter);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        column->addWidget(label);
        return label;
    };
    mMessageOrNull = makeLabel("stateMessage");
    mMessageOrNull->setFont(MakeTypeFont(mMessageOrNull->font(), TypeRole::Title));
    mCauseOrNull = makeLabel("stateCause");
    mFixOrNull = makeLabel("stateFix");
    for (LongWordLabel* label : {mCauseOrNull, mFixOrNull})
    {
        label->setProperty("visualRole", QStringLiteral("secondary"));
    }
    // The actions sit in one row while they fit and stack when the dock is too narrow for them, so none is clipped.
    QWidget* actionRow = new QWidget(this);
    actionRow->setObjectName(QStringLiteral("stateActions"));
    ActionFlowLayout* actions = new ActionFlowLayout(actionRow, STATE_ACTION_SPACING);
    mActionOrNull = new QPushButton(actionRow);
    mActionOrNull->setObjectName(QStringLiteral("stateAction"));
    mActionOrNull->setProperty("visualRole", QStringLiteral("primary"));
    actions->addWidget(mActionOrNull);
    mShortcutOrNull = new QLabel(actionRow);
    mShortcutOrNull->setObjectName(QStringLiteral("stateShortcut"));
    mShortcutOrNull->setProperty("visualRole", QStringLiteral("secondary"));
    actions->addWidget(mShortcutOrNull);
    mCopyOrNull = new QPushButton(QStringLiteral("Copy Details"), actionRow);
    mCopyOrNull->setObjectName(QStringLiteral("stateCopyDetails"));
    actions->addWidget(mCopyOrNull);
    mDetailsToggleOrNull = new QPushButton(QStringLiteral("Show Details"), actionRow);
    mDetailsToggleOrNull->setObjectName(QStringLiteral("stateShowDetails"));
    mDetailsToggleOrNull->setCheckable(true);
    mDetailsToggleOrNull->setToolTip(QStringLiteral("Show the whole text, with every ID and path in full."));
    actions->addWidget(mDetailsToggleOrNull);
    column->addWidget(actionRow);
    // The whole text with nothing shortened, wrapped at any character so an ID or a path breaks instead of clipping.
    mDetailsTextOrNull = new QPlainTextEdit(this);
    mDetailsTextOrNull->setObjectName(QStringLiteral("stateDetails"));
    mDetailsTextOrNull->setReadOnly(true);
    mDetailsTextOrNull->setWordWrapMode(QTextOption::WrapAnywhere);
    mDetailsTextOrNull->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mDetailsTextOrNull->setAccessibleName(QStringLiteral("Details"));
    mDetailsTextOrNull->setMaximumHeight(mDetailsTextOrNull->fontMetrics().lineSpacing() * STATE_DETAILS_VISIBLE_LINES +
                                         2 * mDetailsTextOrNull->frameWidth() +
                                         static_cast<int>(mDetailsTextOrNull->document()->documentMargin()) * 2);
    mDetailsTextOrNull->hide();
    column->addWidget(mDetailsTextOrNull);
    connect(mActionOrNull, &QPushButton::clicked, this, &StateView::ActionTriggered);
    connect(mCopyOrNull, &QPushButton::clicked, this, [this]() {
        QGuiApplication::clipboard()->setText(GetStateDetailsText(mContent));
    });
    connect(mDetailsToggleOrNull, &QPushButton::toggled, this, [this](bool isChecked) {
        mDetailsToggleOrNull->setText(isChecked == true ? QStringLiteral("Hide Details")
                                                        : QStringLiteral("Show Details"));
        mDetailsTextOrNull->setVisible(isChecked == true);
    });
    SetContent({});
}

void StateView::SetContent(const StateViewContent& content)
{
    mContent = content;
    mIconOrNull->SetIcon(content.Icon);
    const auto setRow = [](LongWordLabel* label, const QString& text) {
        label->SetFullText(text);
        label->setVisible(text.isEmpty() == false);
    };
    setRow(mMessageOrNull, content.Message);
    setRow(mCauseOrNull, content.Cause);
    setRow(mFixOrNull, content.Fix);
    mShortcutOrNull->setText(content.ShortcutHint);
    mShortcutOrNull->setVisible(content.ShortcutHint.isEmpty() == false);
    mActionOrNull->setText(content.ActionLabel);
    mActionOrNull->setVisible(content.ActionLabel.isEmpty() == false);
    mCopyOrNull->setVisible(content.Details.isEmpty() == false);
    mDetailsToggleOrNull->setVisible(content.Details.isEmpty() == false);
    mDetailsTextOrNull->setPlainText(GetStateDetailsText(content));
    if (content.Details.isEmpty() == true)
    {
        mDetailsToggleOrNull->setChecked(false);
    }
    setAccessibleName(content.Message);
    setAccessibleDescription(QStringList{content.Cause, content.Fix}.join(QLatin1Char(' ')).trimmed());
    applyColours();
}

const StateViewContent& StateView::GetContent() const noexcept
{
    return mContent;
}

void StateView::SetFixedColours(const StateViewColours& colours)
{
    mColours = colours;
    mHasFixedColours = true;
    applyColours();
}

void StateView::applyColours()
{
    if (mHasFixedColours == false)
    {
        mIconOrNull->SetColour(QColor());
        return;
    }
    mIconOrNull->SetColour(mColours.Text);
    const auto colourLabel = [](QLabel* label, const QColor& colour) {
        // Secondary labels read PlaceholderText (EditorStyle), the rest WindowText; set both.
        QPalette palette = label->palette();
        palette.setColor(QPalette::WindowText, colour);
        palette.setColor(QPalette::PlaceholderText, colour);
        label->setPalette(palette);
    };
    colourLabel(mMessageOrNull, mColours.Text);
    for (QLabel* label : {static_cast<QLabel*>(mCauseOrNull), static_cast<QLabel*>(mFixOrNull), mShortcutOrNull})
    {
        colourLabel(label, mColours.Secondary);
    }
}
} // namespace ludus::editor
