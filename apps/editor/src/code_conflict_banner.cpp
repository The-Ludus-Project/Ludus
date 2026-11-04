#include "internal/code_conflict_banner.h"
#include "design_tokens.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace ludus::editor
{
namespace
{
/// Banner layout spacing in pixels, kept beside its sole use.
constexpr int BANNER_MARGIN_HORIZONTAL = design::SPACING_SMALL;
constexpr int BANNER_MARGIN_VERTICAL = design::SPACING_EXTRA_SMALL;
constexpr int BANNER_SPACING = design::SPACING_SMALL;
} // namespace

CodeConflictBanner::CodeConflictBanner(QWidget* parentOrNull) : QFrame(parentOrNull)
{
    setObjectName(QStringLiteral("codeConflictBanner"));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);

    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(BANNER_MARGIN_HORIZONTAL,
                               BANNER_MARGIN_VERTICAL,
                               BANNER_MARGIN_HORIZONTAL,
                               BANNER_MARGIN_VERTICAL);
    layout->setSpacing(BANNER_SPACING);

    mMessage = new QLabel(this);
    mMessage->setObjectName(QStringLiteral("codeConflictMessage"));
    mMessage->setWordWrap(true);
    layout->addWidget(mMessage, 1);

    mCompare = new QPushButton(tr("Compare"), this);
    mCompare->setObjectName(QStringLiteral("codeConflictCompare"));
    mKeepMine = new QPushButton(tr("Keep Mine"), this);
    mKeepMine->setObjectName(QStringLiteral("codeConflictKeepMine"));
    mReload = new QPushButton(tr("Reload"), this);
    mReload->setObjectName(QStringLiteral("codeConflictReload"));
    layout->addWidget(mCompare);
    layout->addWidget(mKeepMine);
    layout->addWidget(mReload);

    connect(mCompare, &QPushButton::clicked, this, &CodeConflictBanner::CompareRequested);
    connect(mKeepMine, &QPushButton::clicked, this, &CodeConflictBanner::KeepMineRequested);
    connect(mReload, &QPushButton::clicked, this, &CodeConflictBanner::ReloadRequested);

    Dismiss();
}

void CodeConflictBanner::setActionsVisible(bool isVisible)
{
    mIsShowingConflict = isVisible;
    mCompare->setVisible(isVisible);
    mKeepMine->setVisible(isVisible);
    mReload->setVisible(isVisible);
}

void CodeConflictBanner::ShowConflict(const QString& fileName)
{
    mMessage->setText(tr("%1 changed on disk. Your unsaved edits are kept.").arg(fileName));
    setActionsVisible(true);
    setVisible(true);
}

void CodeConflictBanner::ShowRecoveredConflict(const QString& fileName)
{
    mMessage->setText(tr("Recovered unsaved changes to %1 were made before the file changed on disk. "
                         "Compare them, keep them, or reload the file.")
                          .arg(fileName));
    setActionsVisible(true);
    setVisible(true);
}

void CodeConflictBanner::ShowExplanation(const QString& explanation)
{
    if (explanation.isEmpty() == true)
    {
        Dismiss();
        return;
    }
    mMessage->setText(explanation);
    setActionsVisible(false);
    setVisible(true);
}

void CodeConflictBanner::Dismiss()
{
    mIsShowingConflict = false;
    setVisible(false);
}
} // namespace ludus::editor
