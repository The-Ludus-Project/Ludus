#pragma once

#include <ludus/foundation/base/types.h>

#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
QT_END_NAMESPACE

namespace ludus::editor
{
class RendererWorkspace;
class RenderPreview;

enum class ViewportRenderAction : foundation::uint8
{
    ShowDiagnostics,
    OpenShader,
    ShowGraphNode,
    RebuildGraph,
    UseProjectRenderer,
};

/// Copied notice for the current viewport failure. The diagnostic row is scoped to its build request.
struct ViewportRenderRecovery final
{
    QString Message;
    QString Diagnostics;
    ViewportRenderAction Action = ViewportRenderAction::ShowDiagnostics;
    foundation::uint64 Request = 0;
    foundation::int32 DiagnosticRow = -1;
    bool CanUseRaster = false;
};

/// Keyboard-accessible controls beside the viewport image. Qt owns this widget via its viewport parent.
class ViewportRenderRecoveryNotice final : public QWidget
{
    Q_OBJECT
public:
    ViewportRenderRecoveryNotice(RendererWorkspace& inoutWorkspace, RenderPreview& inoutPreview, QWidget* parent);
    [[nodiscard]] bool HasProblem() const;

Q_SIGNALS:
    void ProblemVisibilityChanged(bool hasProblem);

private:
    void refresh();
    void activatePrimary();
    void showDiagnostics();
    RendererWorkspace& mWorkspace;
    RenderPreview& mPreview;
    QLabel* mMessage;
    QLabel* mActionProblem;
    QPushButton* mPrimary;
    QPushButton* mDetails;
    QPushButton* mRaster;
    ViewportRenderRecovery mRecovery;
};
} // namespace ludus::editor
