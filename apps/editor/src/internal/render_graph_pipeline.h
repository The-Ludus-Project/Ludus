#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_graph_pipeline_node.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QFormLayout;
class QLabel;
class QTableWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Edits read inputs and paired writable resources, program references, state and invocation.
/// Each writable row creates both ports and serializes their target association automatically.
/// Completed control edits emit a candidate for the document's undoable draft.
/// The document owns acceptance and Save; this widget never builds shaders. GUI thread only.
class RenderGraphPipelineEditor final : public QWidget
{
    Q_OBJECT
public:
    explicit RenderGraphPipelineEditor(const QJsonObject& node, QWidget* parentOrNull = nullptr);

    /// Adopt document-assigned program names without emitting another edit or replacing controls.
    void SetShaderAssignments(const QJsonArray& shaders);

Q_SIGNALS:
    /// A completed edit proposes the complete node; the document may reject unsafe port changes.
    void Edited(const QJsonObject& node);

private:
    [[nodiscard]] QTableWidget*
    createTable(const QString& name, const QStringList& fields, const QStringList& defaults);
    void addProperties(QFormLayout& outForm, const QString& name);
    void addRayStateProperties(QFormLayout& outForm);
    void publishEdit();
    void connectChoices(QTableWidget& inoutTable, foundation::int32 row);
    QJsonObject mNode;
    QTableWidget* mInputs;
    QTableWidget* mOutputs;
    QTableWidget* mShaders;
    QLabel* mError;
};
} // namespace ludus::editor
