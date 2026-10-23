#pragma once

#include "internal/render_graph_resolution_settings.h"

#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QSpinBox;
QT_END_NAMESPACE

namespace ludus::editor
{
class RendererWorkspace;
/// Project Settings view of the active renderer document's authored size providers.
/// Borrows the workspace for its lifetime; all edits use the existing graph history/save.
class ProjectRenderingSettings final : public QWidget
{
    Q_OBJECT
public:
    explicit ProjectRenderingSettings(RendererWorkspace& inoutRenderer, QWidget* parentOrNull = nullptr);
    /// Accept pending spin-box text before project Save. GUI thread only.
    void Commit();
    /// Disable editing while project commands are unavailable; refreshing never creates nodes.
    void SetProjectEditable(bool canEdit);
    /// True when keyboard focus belongs to these graph-backed controls.
    [[nodiscard]] bool ContainsFocus() const;

private:
    void refresh();
    void apply();
    RendererWorkspace& mRenderer;
    QVector<RenderGraphResolutionSelection> mSelections;
    QFormLayout* mFields;
    QComboBox* mProvider;
    QComboBox* mOutputMode;
    QSpinBox* mWidth;
    QSpinBox* mHeight;
    QComboBox* mScale;
    QSpinBox* mNumerator;
    QSpinBox* mDenominator;
    QLabel* mStatus;
    QPushButton* mEnable;
    bool mCanEdit = false;
    bool mIsRefreshing = false;
};
} // namespace ludus::editor
