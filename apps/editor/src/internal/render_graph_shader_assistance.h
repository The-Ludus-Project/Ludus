#pragma once

#include "internal/code_assistance.h"
#include "internal/render_graph_build_settings.h"
#include "internal/render_graph_job.h"

#include <QJsonObject>
#include <QTemporaryDir>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
QT_END_NAMESPACE

namespace ludus::editor
{
class RenderGraphDocument;
/// Renderer-specific context provider. Captures draft packages, asks the shared
/// compiler owner to prepare sources, and supplies the reusable editing controller.
/// Temporary/generated files never enter the project package or its history.
class RenderGraphShaderAssistance final : public QWidget
{
    Q_OBJECT
public:
    RenderGraphShaderAssistance(RenderGraphDocument& inoutDocument,
                                CodeEditor& inoutEditor,
                                CodeDocumentController& inoutController,
                                const ToolingPaths& tooling,
                                QWidget* parentOrNull = nullptr);
    void Update(const QString& project,
                foundation::uint64 epoch,
                const QString& manifest,
                const QString& file,
                const RenderGraphBuildSettings& settings);

    /// The one-line status of shader assistance. It is not part of this widget's own layout: the host places it
    /// (under the editor) so the controls above the code stay on one row.
    [[nodiscard]] QLabel* GetStatusLabel() noexcept
    {
        return mStatus;
    }

private:
    void setStatus(const QString& message);
    void prepare();
    void accept(const RenderGraphJobResult& result);
    void publish();
    [[nodiscard]] QByteArray getIdentity() const;
    RenderGraphDocument& mDocument;
    QTemporaryDir mTemporary;
    CodeAssistance mAssistance;
    ToolingPaths mTooling;
    RenderGraphJob mJob;
    QTimer mDeadline;
    QComboBox* mContexts;
    QLabel* mStatus;
    QString mProject;
    QString mManifest;
    QString mFile;
    QString mSelectedContext;
    RenderGraphBuildSettings mSettings;
    QJsonObject mResult;
    foundation::uint64 mEpoch = 0;
    foundation::uint64 mNextRequest = 0;
    bool mHasPending = false;
};
} // namespace ludus::editor
