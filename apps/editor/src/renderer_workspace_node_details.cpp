#include "internal/render_graph_node_details.h"
#include "internal/render_graph_options.h"
#include "internal/render_graph_pipeline_node.h"
#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/core.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 NODE_EDITOR_WIDTH = 900;
constexpr int32 NODE_EDITOR_HEIGHT = 700;
constexpr int32 MAX_COORDINATE = RenderGraphDocument::MAX_COORDINATE;
} // namespace
void RendererWorkspace::editNodeOnCanvas(const QString& diagnosticField)
{
    if (selectedNode().isEmpty() == true || CanCreateSubgraph() == false)
    {
        return;
    }
    RenderGraphNodeDetails dialog(this);
    connect(&dialog, &RenderGraphNodeDetails::PopulateRequested, this, [this](QFormLayout* outFields, QWidget* parent) {
        LUDUS_REQUIRE(outFields != nullptr && parent != nullptr);
        populateNodeFields(*outFields, *parent);
        for (const bool isX : {true, false})
        {
            QSpinBox* coordinate = new QSpinBox(parent);
            coordinate->setObjectName(isX == true ? QStringLiteral("canvasLayoutX") : QStringLiteral("canvasLayoutY"));
            coordinate->setRange(-MAX_COORDINATE, MAX_COORDINATE);
            coordinate->setValue(isX == true ? mLayoutX->value() : mLayoutY->value());
            outFields->addRow(isX == true ? QStringLiteral("Layout x") : QStringLiteral("Layout y"), coordinate);
            connect(coordinate, &QSpinBox::editingFinished, this, [this, coordinate, isX]() {
                if (coordinate->value() == (isX == true ? mLayoutX->value() : mLayoutY->value()))
                {
                    return;
                }
                documentEdited(mDocument.MoveNode(mSelectedFile,
                                                  mSelectedConfiguration,
                                                  mSelectedNode,
                                                  isX == true ? coordinate->value() : mLayoutX->value(),
                                                  isX == true ? mLayoutY->value() : coordinate->value()));
            });
        }
    });
    const auto refresh = [this, &dialog]() {
        dialog.Refresh(QStringLiteral("Details — %1").arg(mSelectedNode),
                       QStringLiteral("%1 / %2 / %3").arg(mSelectedFile, mSelectedConfiguration, mSelectedNode));
    };
    refresh();
    dialog.RevealDiagnostic(diagnosticField, mStatus->text());
    connect(this, &RendererWorkspace::NodeFieldsChanged, &dialog, [&dialog, refresh](QWidget* editOriginOrNull) {
        if (dialog.ContainsControl(editOriginOrNull) == false)
        {
            refresh();
        }
    });
    static_cast<void>(dialog.exec());
}

void RendererWorkspace::openNodeShader(const QString& shaderId, QWidget* editor)
{
    LUDUS_REQUIRE(editor != nullptr);
    QJsonObject effective;
    QString error;
    if (ResolveRenderGraphPipelineOptions(selectedNode(), effective, error) == false)
    {
        mStatus->setText(error);
        return;
    }
    const QJsonObject manifest = QJsonDocument::fromJson(mDocument.GetDraft(manifestFile())).object();
    const QJsonArray programs = manifest.value(QStringLiteral("programs")).toArray();
    for (const QJsonValueConstRef shader : effective.value(QStringLiteral("shaders")).toArray())
    {
        if (shader.toObject().value(QStringLiteral("id")).toString() != shaderId)
        {
            continue;
        }
        for (const QJsonValueConstRef program : programs)
        {
            const QJsonObject record = program.toObject();
            const QString source = record.value(QStringLiteral("source")).toString();
            if (record.value(QStringLiteral("id")) == shader.toObject().value(QStringLiteral("program")) &&
                mDocument.GetFiles().contains(source) == true)
            {
                QDialog* dialogOrNull = qobject_cast<QDialog*>(editor->window());
                if (dialogOrNull != nullptr)
                {
                    dialogOrNull->accept();
                }
                openSourceLine(source, 1);
                return;
            }
        }
    }
    mStatus->setText(QStringLiteral("No source is mapped to shader '%1' in the package manifest.").arg(shaderId));
}

QWidget* RendererWorkspace::createNodeOptionsEditor(QWidget& inoutParent)
{
    const QJsonObject node = selectedNode();
    RenderGraphOptionsEditor* editor =
        new RenderGraphOptionsEditor(GetRenderGraphNodeOptions(mDocument, node),
                                     node.value(QStringLiteral("overrides")).toObject(),
                                     GetRenderGraphOptionTargets(node),
                                     IsPipelineNode(node.value(QStringLiteral("kind")).toString()),
                                     &inoutParent);
    connect(editor, &RenderGraphOptionsEditor::OverridesEdited, this, [this](const QJsonObject& overrides) {
        QJsonObject candidate = selectedNode();
        if (overrides.isEmpty() == true)
        {
            candidate.remove(QStringLiteral("overrides"));
        }
        else
        {
            candidate.insert(QStringLiteral("overrides"), overrides);
        }
        replaceSelectedNode(candidate);
    });
    connect(editor, &RenderGraphOptionsEditor::DeclarationsEdited, this, [this](const QJsonArray& options) {
        QJsonObject candidate = selectedNode();
        if (options.isEmpty() == true)
        {
            candidate.remove(QStringLiteral("options"));
        }
        else
        {
            candidate.insert(QStringLiteral("options"), options);
        }
        replaceSelectedNode(candidate);
    });
    return editor;
}

void RendererWorkspace::editExposedOptionsOnCanvas()
{
    const QString file = mCanvas->GetFile();
    const QString configuration = mCanvas->GetConfiguration();
    static_cast<void>(SelectNode(file, configuration, {}));
    const QJsonObject record = selectedConfiguration();
    if (record.isEmpty() == true || CanCreateSubgraph() == false)
    {
        return;
    }
    QStringList targets;
    for (const QJsonValueConstRef entry : record.value(QStringLiteral("nodes")).toArray())
    {
        const QJsonObject node = entry.toObject();
        for (const QJsonValueConstRef option : GetRenderGraphNodeOptions(mDocument, node))
        {
            targets.append(node.value(QStringLiteral("id")).toString() + QLatin1Char('.') +
                           option.toObject().value(QStringLiteral("id")).toString());
        }
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("renderGraphExposedOptionsDialog"));
    dialog.setWindowTitle(QStringLiteral("Exposed Options — %1").arg(configuration));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    RenderGraphOptionsEditor* editor =
        new RenderGraphOptionsEditor(record.value(QStringLiteral("options")).toArray(), {}, targets, true, &dialog);
    layout->addWidget(editor);
    QLabel* error = new QLabel(&dialog);
    error->setWordWrap(true);
    layout->addWidget(error);
    connect(editor,
            &RenderGraphOptionsEditor::DeclarationsEdited,
            &dialog,
            [this, file, configuration, &dialog, error](const QJsonArray& options) {
                QString message;
                const RenderGraphDocumentStatus status =
                    mDocument.SetConfigurationOptions(file, configuration, options, message);
                if (status != RenderGraphDocumentStatus::Ok)
                {
                    error->setText(message);
                    return;
                }
                documentEdited(status);
                dialog.accept();
            });
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(NODE_EDITOR_WIDTH, NODE_EDITOR_HEIGHT);
    static_cast<void>(dialog.exec());
}
} // namespace ludus::editor
