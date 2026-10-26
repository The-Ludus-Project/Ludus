#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_pipeline_node.h"
#include "internal/render_graph_scene_query_budgets.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QFormLayout;
class QLabel;
class QTableWidget;
class QTabWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Edits read inputs and paired writable resources, program references, state and invocation.
/// Each writable row creates both ports and serializes their target association automatically.
/// Ray pipelines and scene compute passes show Launch size and automatic state; adding or removing a
/// ComputePipeline's Scene input applies that profile's state and invocation defaults.
/// Completed control edits emit a candidate for the document's undoable draft.
/// The document owns acceptance and Save; this widget never builds shaders. GUI thread only.
class RenderGraphPipelineEditor final : public QWidget
{
    Q_OBJECT
public:
    explicit RenderGraphPipelineEditor(const QJsonObject& node, QWidget* parentOrNull = nullptr);

    /// Add shared finite option controls as an authorable pipeline part; ownership transfers to this widget.
    void AddOptionsEditor(QWidget* editor);
    /// Show clickable part cards above the shared controls, keeping selection during edits.
    void ShowParts();

    /// Adopt document-assigned program names without emitting another edit or replacing controls.
    void SetShaderAssignments(const QJsonArray& shaders);
    /// Add shared scene-ray budget controls once, after construction, for a ray pipeline.
    /// Commits propose manifest edits separately from node state; the document owns acceptance.
    void AddSceneQueryBudgets(const RenderGraphSceneQueryBudgets& budgets);

Q_SIGNALS:
    /// A completed edit proposes the complete node; the document may reject unsafe port changes.
    void Edited(const QJsonObject& node);
    /// A completed budget edit proposes the shared technique's limits; no node fields change.
    void SceneQueryBudgetsEdited(const RenderGraphSceneQueryBudgets& budgets);

private:
    [[nodiscard]] QTableWidget*
    createTable(const QString& name, const QStringList& fields, const QStringList& defaults);
    void addProperties(QFormLayout& outForm, const QString& name);
    void addRayStateProperties(QFormLayout& outForm);
    void addSceneComputeStateProperties(QFormLayout& outForm);
    void publishEdit();
    void connectChoices(QTableWidget& inoutTable, foundation::int32 row);
    QJsonObject mNode;
    QTabWidget* mTabs;
    QWidget* mPartsOrNull = nullptr;
    QWidget mRetiredParts;
    QTableWidget* mInputs;
    QTableWidget* mOutputs;
    QTableWidget* mShaders;
    QLabel* mError;
};
} // namespace ludus::editor
