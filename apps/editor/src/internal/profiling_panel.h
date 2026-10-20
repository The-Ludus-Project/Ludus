#pragma once

#include "internal/profiling_controller.h"

#include <QWidget>

QT_BEGIN_NAMESPACE
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Recording controls and summary; the shared QAction supplies menu/toolbar/F9 behavior.
class ProfilingPanel final : public QWidget
{
    Q_OBJECT
public:
    ProfilingPanel(RenderPreview* preview, QAction* recordAction, QWidget* parentOrNull = nullptr);
    void SetProjectRoot(const QString& root);
    [[nodiscard]] ProfilingController& GetController() noexcept;

private:
    void toggleRecording();
    void refresh();
    ProfilingController mController;
    QAction* mRecordAction;
    QComboBox* mMode;
    QLineEdit* mDestination;
    QLabel* mStatus;
    QPushButton* mRetry;
    QPushButton* mDiscard;
    QPushButton* mReveal;
    QString mProjectRoot;
};
} // namespace ludus::editor
