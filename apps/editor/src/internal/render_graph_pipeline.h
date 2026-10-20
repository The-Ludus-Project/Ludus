#pragma once

#include "internal/render_graph_pipeline_node.h"

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
/// Apply emits a complete candidate; the document owns undo/save and acceptance.
/// Unapplied controls do not mutate the document. GUI thread only.
class RenderGraphPipelineEditor final : public QWidget
{
    Q_OBJECT
public:
    explicit RenderGraphPipelineEditor(const QJsonObject& node, QWidget* parentOrNull = nullptr);

Q_SIGNALS:
    void Applied(const QJsonObject& node);

private:
    [[nodiscard]] QTableWidget*
    createTable(const QString& name, const QStringList& fields, const QStringList& defaults);
    void addProperties(QFormLayout& outForm, const QString& name);
    void apply();
    QJsonObject mNode;
    QTableWidget* mInputs;
    QTableWidget* mOutputs;
    QTableWidget* mShaders;
    QLabel* mError;
};
} // namespace ludus::editor
