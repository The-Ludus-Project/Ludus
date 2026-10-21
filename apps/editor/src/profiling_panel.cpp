#include "internal/profiling_panel.h"

#include <QAction>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolBar>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

namespace ludus::editor
{
ProfilingPanel::ProfilingPanel(RenderPreview* preview, QAction* recordAction, QWidget* parentOrNull)
    : QWidget(parentOrNull), mController(preview, this), mRecordAction(recordAction), mMode(new QComboBox(this)),
      mDestination(new QLineEdit(this)), mStatus(new QLabel(this)),
      mRetry(new QPushButton(QStringLiteral("Retry Save..."), this)),
      mDiscard(new QPushButton(QStringLiteral("Discard Unsaved Recording"), this)),
      mReveal(new QPushButton(QStringLiteral("Reveal Last Trace"), this))
{
    LUDUS_REQUIRE(recordAction != nullptr);
    setObjectName(QStringLiteral("profilingPanel"));
    mMode->setObjectName(QStringLiteral("profilingMode"));
    mMode->addItems({QStringLiteral("CPU"), QStringLiteral("GPU"), QStringLiteral("Both")});
    mDestination->setObjectName(QStringLiteral("profilingDestination"));
    mDestination->setPlaceholderText(QStringLiteral("Automatic: project out/profiling/<unique-id>.json"));
    mStatus->setObjectName(QStringLiteral("profilingStatus"));
    mReveal->setObjectName(QStringLiteral("profilingReveal"));
    mReveal->setToolTip(QStringLiteral("Available after a recording has been saved successfully."));
    mStatus->setWordWrap(true);
    QVBoxLayout* layout = new QVBoxLayout(this);
    QToolBar* toolbar = new QToolBar(this);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolbar->addAction(recordAction);
    layout->addWidget(toolbar);
    QFormLayout* fields = new QFormLayout;
    fields->addRow(QStringLiteral("Record"), mMode);
    fields->addRow(QStringLiteral("Output file"), mDestination);
    layout->addLayout(fields);
    QLabel* explanation = new QLabel(QStringLiteral("CPU: editor process scopes. GPU: scene viewport device duration. "
                                                    "GPU counters are attributed to submission, not aligned execution "
                                                    "slices. F9 starts/stops recording; 30 seconds is the maximum."),
                                     this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    layout->addWidget(mStatus);
    QPushButton* viewer = new QPushButton(QStringLiteral("Open Perfetto"), this);
    viewer->setToolTip(
        QStringLiteral("Open Perfetto, then load the saved JSON file. No file is uploaded automatically."));
    QHBoxLayout* buttons = new QHBoxLayout;
    buttons->addWidget(mRetry);
    buttons->addWidget(mDiscard);
    buttons->addWidget(mReveal);
    buttons->addWidget(viewer);
    layout->addLayout(buttons);
    layout->addStretch();
    connect(recordAction, &QAction::triggered, this, &ProfilingPanel::toggleRecording);
    connect(&mController, &ProfilingController::Changed, this, &ProfilingPanel::refresh);
    connect(mRetry, &QPushButton::clicked, this, [this]() {
        const QString destination = QFileDialog::getSaveFileName(this,
                                                                 QStringLiteral("Save Perfetto Recording"),
                                                                 mProjectRoot,
                                                                 QStringLiteral("Perfetto JSON (*.json)"));
        static_cast<void>(mController.RetrySave(destination));
    });
    connect(mDiscard, &QPushButton::clicked, &mController, &ProfilingController::Discard);
    connect(mReveal, &QPushButton::clicked, this, [this]() {
        static_cast<void>(
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(mController.GetLastPath()).absolutePath())));
    });
    connect(viewer, &QPushButton::clicked, this, []() {
        static_cast<void>(QDesktopServices::openUrl(QUrl(QStringLiteral("https://ui.perfetto.dev"))));
    });
    refresh();
}

void ProfilingPanel::SetProjectRoot(const QString& root)
{
    mProjectRoot = root;
}
ProfilingController& ProfilingPanel::GetController() noexcept
{
    return mController;
}

void ProfilingPanel::toggleRecording()
{
    if (mController.GetState() == ProfilingState::Recording)
    {
        mController.Stop();
        return;
    }
    QString destination = mDestination->text().trimmed();
    if (destination.isEmpty() == true)
    {
        const QString root = mProjectRoot.isEmpty() == true ? QDir::currentPath() : mProjectRoot;
        destination = QDir(root).filePath(
            QStringLiteral("out/profiling/%1.json").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    }
    static_cast<void>(mController.Start(static_cast<ProfilingMode>(mMode->currentIndex()), destination));
    refresh();
}

void ProfilingPanel::refresh()
{
    const ProfilingState state = mController.GetState();
    const bool isRecording = state == ProfilingState::Recording;
    const bool isBusy = isRecording == true || state == ProfilingState::Finalizing;
    const QSignalBlocker blocker(mRecordAction);
    mRecordAction->setText(isRecording == true ? QStringLiteral("Stop Profiling") : QStringLiteral("Record Profiling"));
    mRecordAction->setIconText(isRecording == true ? QStringLiteral("Stop Profiling")
                                                   : QStringLiteral("Record Profiling"));
    mRecordAction->setChecked(isRecording);
    mRecordAction->setEnabled(state != ProfilingState::Finalizing && mController.HasUnsavedSnapshot() == false);
    mMode->setEnabled(isBusy == false);
    mDestination->setEnabled(isBusy == false);
    mStatus->setText(mController.GetStatus().isEmpty() == true
                         ? QStringLiteral("Ready. Choose CPU, GPU or Both, then Record (F9).")
                         : mController.GetStatus());
    mRetry->setEnabled(mController.HasUnsavedSnapshot());
    mDiscard->setEnabled(state == ProfilingState::Failed);
    mReveal->setEnabled(mController.GetLastPath().isEmpty() == false);
}
} // namespace ludus::editor
