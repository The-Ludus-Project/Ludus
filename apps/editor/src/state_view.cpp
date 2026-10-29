#include "internal/state_view.h"

#include "internal/theme_fonts.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
constexpr int STATE_ICON_SIZE = 32;
constexpr int STATE_COLUMN_WIDTH = 440;
constexpr int STATE_SPACING = 8;
constexpr int STATE_ACTION_SPACING = 12;
} // namespace

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
    column->setAlignment(Qt::AlignHCenter);
    mIconOrNull = new StateIcon(this);
    mIconOrNull->setObjectName(QStringLiteral("stateIcon"));
    column->addWidget(mIconOrNull, 0, Qt::AlignHCenter);
    const auto makeLabel = [this, column](const char* name) {
        QLabel* label = new QLabel(this);
        label->setObjectName(QString::fromLatin1(name));
        label->setWordWrap(true);
        label->setAlignment(Qt::AlignHCenter);
        label->setTextFormat(Qt::PlainText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        column->addWidget(label);
        return label;
    };
    mMessageOrNull = makeLabel("stateMessage");
    mMessageOrNull->setFont(MakeTypeFont(mMessageOrNull->font(), TypeRole::Title));
    mCauseOrNull = makeLabel("stateCause");
    mFixOrNull = makeLabel("stateFix");
    for (QLabel* label : {mCauseOrNull, mFixOrNull})
    {
        label->setProperty("visualRole", QStringLiteral("secondary"));
    }
    QHBoxLayout* actions = new QHBoxLayout;
    actions->setSpacing(STATE_ACTION_SPACING);
    actions->addStretch();
    mActionOrNull = new QPushButton(this);
    mActionOrNull->setObjectName(QStringLiteral("stateAction"));
    mActionOrNull->setProperty("visualRole", QStringLiteral("primary"));
    actions->addWidget(mActionOrNull);
    mShortcutOrNull = new QLabel(this);
    mShortcutOrNull->setObjectName(QStringLiteral("stateShortcut"));
    mShortcutOrNull->setProperty("visualRole", QStringLiteral("secondary"));
    actions->addWidget(mShortcutOrNull);
    mCopyOrNull = new QPushButton(QStringLiteral("Copy Details"), this);
    mCopyOrNull->setObjectName(QStringLiteral("stateCopyDetails"));
    actions->addWidget(mCopyOrNull);
    actions->addStretch();
    column->addLayout(actions);
    connect(mActionOrNull, &QPushButton::clicked, this, &StateView::ActionTriggered);
    connect(mCopyOrNull, &QPushButton::clicked, this, [this]() {
        QGuiApplication::clipboard()->setText(GetStateDetailsText(mContent));
    });
    SetContent({});
}

void StateView::SetContent(const StateViewContent& content)
{
    mContent = content;
    mIconOrNull->SetIcon(content.Icon);
    const auto setRow = [](QLabel* label, const QString& text) {
        label->setText(text);
        label->setVisible(text.isEmpty() == false);
    };
    setRow(mMessageOrNull, content.Message);
    setRow(mCauseOrNull, content.Cause);
    setRow(mFixOrNull, content.Fix);
    setRow(mShortcutOrNull, content.ShortcutHint);
    mActionOrNull->setText(content.ActionLabel);
    mActionOrNull->setVisible(content.ActionLabel.isEmpty() == false);
    mCopyOrNull->setVisible(content.Details.isEmpty() == false);
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
    for (QLabel* label : {mCauseOrNull, mFixOrNull, mShortcutOrNull})
    {
        colourLabel(label, mColours.Secondary);
    }
}
} // namespace ludus::editor
