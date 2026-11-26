#include "internal/activity_chip.h"
#include "design_tokens.h"
#include "internal/editor_icons.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QSize>
#include <QSizePolicy>
#include <QToolButton>

#include <algorithm>

namespace ludus::editor
{
namespace
{
constexpr int MARKER_HEIGHT = 12;
constexpr int DOT_DIAMETER = 8;
constexpr int BAR_WIDTH = 56;
constexpr int BAR_HEIGHT = 4;
constexpr int CHIP_SPACING = design::SPACING_SMALL;
constexpr int GROUP_SPACING = design::SPACING_MEDIUM;
constexpr int STATUS_ICON_SIZE = 14;
/// Space on each side of a count button's icon and number.
constexpr int COUNT_BUTTON_PADDING = design::SPACING_EXTRA_SMALL;
constexpr qreal HALF = 0.5;

/// A static dot, or a bar filled to a fraction. It never animates.
class ActivityMarker final : public QWidget
{
public:
    explicit ActivityMarker(QWidget* parentOrNull) : QWidget(parentOrNull)
    {
        setObjectName(QStringLiteral("activityMarker"));
        setFixedHeight(MARKER_HEIGHT);
        SetProgress(-1.0);
    }

    void SetProgress(double progressOrNegative)
    {
        mProgress = progressOrNegative;
        setFixedWidth(mProgress < 0.0 ? DOT_DIAMETER : BAR_WIDTH);
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        const QColor accent = palette().color(QPalette::Link);
        if (mProgress < 0.0)
        {
            const qreal top = (static_cast<qreal>(height()) - static_cast<qreal>(DOT_DIAMETER)) * HALF;
            painter.setBrush(accent);
            painter.drawEllipse(QRectF(0.0, top, DOT_DIAMETER, DOT_DIAMETER));
            return;
        }
        const qreal top = (static_cast<qreal>(height()) - static_cast<qreal>(BAR_HEIGHT)) * HALF;
        const QRectF track(0.0, top, BAR_WIDTH, BAR_HEIGHT);
        painter.setBrush(palette().color(QPalette::Mid));
        painter.drawRoundedRect(track, BAR_HEIGHT * HALF, BAR_HEIGHT * HALF);
        painter.setBrush(accent);
        painter.drawRoundedRect(QRectF(0.0, top, BAR_WIDTH * std::clamp(mProgress, 0.0, 1.0), BAR_HEIGHT),
                                BAR_HEIGHT * HALF,
                                BAR_HEIGHT * HALF);
    }

private:
    double mProgress = -1.0;
};

QToolButton* MakeCountButton(QWidget* parent, const QString& name, EditorIcon icon)
{
    QToolButton* button = new QToolButton(parent);
    button->setObjectName(name);
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIcon(GetEditorIcon(icon));
    button->setIconSize(QSize(STATUS_ICON_SIZE, STATUS_ICON_SIZE));
    button->setToolTip(QStringLiteral("Show the Error List"));
    return button;
}

/// Show count compactly, as the bare number beside the button's icon, so the status bar's right edge never clips
/// it; the accessible name and tooltip say it in words ("2 Problems"). When there is no report and the count is
/// zero, a dash and "Not checked" stand in, so a missing check is not read as a clean project.
void ShowCount(QToolButton& inoutButton,
               foundation::int32 count,
               const QString& singular,
               const QString& plural,
               bool hasProblemReport)
{
    const bool isUnchecked = hasProblemReport == false && count == 0;
    const QString words =
        isUnchecked == true ? QStringLiteral("Not checked") : FormatProblemCount(count, singular, plural);
    inoutButton.setText(isUnchecked == true ? QStringLiteral("–") : QString::number(count));
    inoutButton.setAccessibleName(words);
    inoutButton.setToolTip(QStringLiteral("%1. Show the Error List").arg(words));
    // The button is never narrower than its icon, gap and number in its own font, whatever the platform style
    // measures, so a narrow window shortens the status line instead.
    const QFontMetrics metrics(inoutButton.font());
    const int contentWidth =
        STATUS_ICON_SIZE + CHIP_SPACING + metrics.horizontalAdvance(inoutButton.text()) + 2 * COUNT_BUTTON_PADDING;
    inoutButton.setMinimumWidth(std::max(inoutButton.sizeHint().width(), contentWidth));
}

} // namespace

QString GetActivityLabel(Phase phase, bool isImporting, bool isGraphJobActive)
{
    switch (phase)
    {
        case Phase::Starting:
            return QStringLiteral("Starting");
        case Phase::Configuring:
            return QStringLiteral("Configuring");
        case Phase::Building:
            return QStringLiteral("Building");
        case Phase::Publishing:
            return QStringLiteral("Publishing");
        case Phase::Launching:
            return QStringLiteral("Launching");
        case Phase::Stopping:
            return QStringLiteral("Stopping");
        case Phase::Idle:
        case Phase::Debugging:
        case Phase::Running:
        case Phase::CleanupUnknown:
            break;
    }
    if (isGraphJobActive == true)
    {
        return QStringLiteral("Building render graph");
    }
    return isImporting == true ? QStringLiteral("Importing content") : QString();
}

QString FormatProblemCount(foundation::int32 count, const QString& singular, const QString& plural)
{
    return QStringLiteral("%1 %2").arg(count).arg(count == 1 ? singular : plural);
}

StatusActivityBar::StatusActivityBar(QWidget* parentOrNull)
    : QWidget(parentOrNull), mMarker(new ActivityMarker(this)), mActivityGroup(new QWidget(this)),
      mProblems(MakeCountButton(this, QStringLiteral("statusProblems"), EditorIcon::Error)),
      mWarnings(MakeCountButton(this, QStringLiteral("statusWarnings"), EditorIcon::Warning))
{
    setObjectName(QStringLiteral("statusActivityBar"));
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(GROUP_SPACING);
    QHBoxLayout* group = new QHBoxLayout(mActivityGroup);
    group->setContentsMargins(0, 0, 0, 0);
    group->setSpacing(CHIP_SPACING);
    // The marker moves into the group; the label sits beside it.
    mMarker->setParent(mActivityGroup);
    QLabel* label = new QLabel(mActivityGroup);
    label->setObjectName(QStringLiteral("activityLabel"));
    label->setAccessibleName(QStringLiteral("Current activity"));
    group->addWidget(mMarker);
    group->addWidget(label);
    mActivityGroup->setObjectName(QStringLiteral("activityChip"));
    mActivityGroup->hide();
    layout->addWidget(mActivityGroup);
    layout->addWidget(mProblems);
    layout->addWidget(mWarnings);
    connect(mProblems, &QToolButton::clicked, this, &StatusActivityBar::ProblemsRequested);
    connect(mWarnings, &QToolButton::clicked, this, &StatusActivityBar::ProblemsRequested);
    mTimer.setSingleShot(true);
    mTimer.setTimerType(Qt::PreciseTimer);
    connect(&mTimer, &QTimer::timeout, this, &StatusActivityBar::applyPending);
    mPendingLabel.clear();
    // The bar keeps its own width: the status line beside it gives way first.
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    ShowCount(*mProblems, 0, QStringLiteral("Problem"), QStringLiteral("Problems"), false);
    ShowCount(*mWarnings, 0, QStringLiteral("Warning"), QStringLiteral("Warnings"), false);
}

void StatusActivityBar::SetActivity(const QString& label, double progressOrNegative)
{
    mPendingLabel = label;
    mPendingProgress = label.isEmpty() == true ? -1.0 : progressOrNegative;
    schedule();
}

void StatusActivityBar::SetCounts(foundation::int32 errors, foundation::int32 warnings, bool hasProblemReport)
{
    mPendingErrors = errors;
    mPendingWarnings = warnings;
    mHasProblemReport = hasProblemReport;
    schedule();
}

void StatusActivityBar::schedule()
{
    mHasPending = true;
    if (mSinceApplied.isValid() == false || mSinceApplied.elapsed() >= ACTIVITY_UPDATE_INTERVAL_MILLISECONDS)
    {
        applyPending();
    }
    else if (mTimer.isActive() == false)
    {
        mTimer.start(ACTIVITY_UPDATE_INTERVAL_MILLISECONDS - static_cast<int>(mSinceApplied.elapsed()));
    }
}

void StatusActivityBar::Flush()
{
    mTimer.stop();
    applyPending();
}

QString StatusActivityBar::GetActivityText() const
{
    const QLabel* label = mActivityGroup->findChild<QLabel*>(QStringLiteral("activityLabel"));
    return label != nullptr ? label->text() : QString();
}

bool StatusActivityBar::IsActivityShown() const
{
    return mActivityGroup->isHidden() == false;
}

void StatusActivityBar::applyPending()
{
    if (mHasPending == false)
    {
        return;
    }
    mHasPending = false;
    mTimer.stop();
    mSinceApplied.start();
    ++mAppliedChangeCount;
    QLabel* label = mActivityGroup->findChild<QLabel*>(QStringLiteral("activityLabel"));
    label->setText(mPendingLabel);
    mShownProgress = mPendingProgress;
    static_cast<ActivityMarker*>(mMarker)->SetProgress(mPendingProgress);
    mActivityGroup->setVisible(mPendingLabel.isEmpty() == false);
    ShowCount(*mProblems, mPendingErrors, QStringLiteral("Problem"), QStringLiteral("Problems"), mHasProblemReport);
    ShowCount(*mWarnings, mPendingWarnings, QStringLiteral("Warning"), QStringLiteral("Warnings"), mHasProblemReport);
}
} // namespace ludus::editor
