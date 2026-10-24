#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/scalar.hpp>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QShortcut>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

namespace ludus::editor
{
using namespace foundation;
namespace
{
const QString NODE_MIME = QStringLiteral("application/x-ludus-render-graph-nodes");
}

void RendererWorkspace::populateNodeNameFields(QFormLayout& outFields,
                                               QWidget& inoutParent,
                                               QDialog* closeOnRenameOrNull)
{
    QWidget* row = new QWidget(&inoutParent);
    QHBoxLayout* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    QLineEdit* name = new QLineEdit(mSelectedNode, row);
    name->setObjectName(QStringLiteral("renderGraphNodeName"));
    name->setProperty("renderGraphField", QStringLiteral("id"));
    name->setAccessibleName(QStringLiteral("Node name"));
    name->setToolTip(QStringLiteral("Unique ASCII identifier starting with a letter; 'input' is reserved."));
    QPushButton* rename = new QPushButton(QStringLiteral("Rename"), row);
    rename->setObjectName(QStringLiteral("renderGraphRenameNode"));
    rename->setAutoDefault(false);
    layout->addWidget(name);
    layout->addWidget(rename);
    QLabel* error = new QLabel(&inoutParent);
    error->setObjectName(QStringLiteral("renderGraphNodeNameError"));
    error->setWordWrap(true);
    const auto apply = [this, name, error, closeOnRenameOrNull]() {
        QString message;
        if (renameSelectedNode(name->text(), message) == false)
        {
            error->setText(message);
            name->setFocus();
        }
        else if (closeOnRenameOrNull != nullptr)
        {
            closeOnRenameOrNull->accept();
        }
    };
    connect(rename, &QPushButton::clicked, this, apply);
    connect(name, &QLineEdit::returnPressed, this, apply);
    connect(name, &QLineEdit::textChanged, error, &QLabel::clear);
    outFields.addRow(QStringLiteral("Name"), row);
    outFields.addRow(error);
}

bool RendererWorkspace::renameSelectedNode(const QString& name, QString& outError)
{
    if (CanCreateSubgraph() == false || selectedNode().isEmpty() == true || mSelectedNodes.size() != 1)
    {
        outError = QStringLiteral("Select one node and resolve source conflicts before renaming it.");
        mStatus->setText(outError);
        return false;
    }
    const QString previous = mSelectedNode;
    const bool isExpanded = mCanvas->IsSceneBundleExpanded(previous);
    const RenderGraphDocumentStatus status =
        mDocument.RenameNode(mSelectedFile, mSelectedConfiguration, previous, name, outError);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(outError);
        return false;
    }
    if (previous == name)
    {
        return true;
    }
    mSelectedNode = name;
    mSelectedNodes = {name};
    if (isExpanded == true)
    {
        mCanvas->SetSceneBundleExpanded(name, true);
    }
    documentEdited(status);
    return true;
}

void RendererWorkspace::promptRenameNode()
{
    if (selectedNode().isEmpty() == true || CanCreateSubgraph() == false)
    {
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("renderGraphRenameNodeDialog"));
    dialog.setWindowTitle(QStringLiteral("Rename %1").arg(mSelectedNode));
    QFormLayout* form = new QFormLayout(&dialog);
    populateNodeNameFields(*form, dialog, &dialog);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    QLineEdit* name = dialog.findChild<QLineEdit*>(QStringLiteral("renderGraphNodeName"));
    LUDUS_REQUIRE(name != nullptr);
    name->selectAll();
    name->setFocus();
    dialog.exec();
}

void RendererWorkspace::setupGraphSelection()
{
    connect(mTree, &QTreeWidget::itemSelectionChanged, this, &RendererWorkspace::syncTreeSelection);
    connect(mCanvas, &RenderGraphCanvas::NodesSelected, this, &RendererWorkspace::syncCanvasSelection);
    connect(mCanvas, &RenderGraphCanvas::CopyRequested, this, [this]() { static_cast<void>(CopySelectedNodes()); });
    connect(mCanvas, &RenderGraphCanvas::PasteRequested, this, [this]() {
        static_cast<void>(PasteNodes(mCanvas->mapToScene(mCanvas->viewport()->rect().center())));
    });
    connect(mCanvas, &RenderGraphCanvas::RemoveNodesRequested, this, [this]() { RemoveSelectedNodes(); });
    QShortcut* copy = new QShortcut(QKeySequence::Copy, mTree);
    copy->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copy, &QShortcut::activated, this, [this]() { static_cast<void>(CopySelectedNodes()); });
    QShortcut* paste = new QShortcut(QKeySequence::Paste, mTree);
    paste->setContext(Qt::WidgetWithChildrenShortcut);
    connect(paste, &QShortcut::activated, this, [this]() {
        static_cast<void>(PasteNodes(mCanvas->mapToScene(mCanvas->viewport()->rect().center())));
    });
    QShortcut* remove = new QShortcut(QKeySequence(Qt::Key_Delete), mTree);
    remove->setContext(Qt::WidgetWithChildrenShortcut);
    connect(remove, &QShortcut::activated, this, [this]() {
        const QList<QTreeWidgetItem*> selected = mTree->selectedItems();
        if (selected.size() == 1 && selected.first()->data(0, CONFIGURATION_ROLE).toString().isEmpty() == false &&
            selected.first()->data(0, NODE_ROLE).toString().isEmpty() == true)
        {
            static_cast<void>(RemoveSelectedConfiguration());
            return;
        }
        RemoveSelectedNodes();
    });
}

void RendererWorkspace::syncTreeSelection()
{
    if (mIsRendering == true)
    {
        return;
    }
    const QTreeWidgetItem* currentOrNull = mTree->currentItem();
    if (currentOrNull != nullptr)
    {
        mSelectedFile = currentOrNull->data(0, FILE_ROLE).toString();
        mSelectedConfiguration = currentOrNull->data(0, CONFIGURATION_ROLE).toString();
        mSelectedNode = currentOrNull->data(0, NODE_ROLE).toString();
    }
    mSelectedNodes.clear();
    for (const QTreeWidgetItem* item : mTree->selectedItems())
    {
        if (item->data(0, FILE_ROLE).toString() != mSelectedFile ||
            item->data(0, CONFIGURATION_ROLE).toString() != mSelectedConfiguration ||
            item->data(0, NODE_ROLE).toString().isEmpty() == true)
        {
            // Mixed file/configuration rows remain visibly selected, but are not a node group.
            mSelectedNodes.clear();
            break;
        }
        mSelectedNodes.append(item->data(0, NODE_ROLE).toString());
    }
    if (mSelectedNodes.isEmpty() == true)
    {
        mSelectedNode.clear();
    }
    renderInspector();
    renderEdges();
    renderCanvas();
    updateActions();
}

void RendererWorkspace::syncCanvasSelection(const QStringList& nodes)
{
    mSelectedFile = mCanvas->GetFile();
    mSelectedConfiguration = mCanvas->GetConfiguration();
    mSelectedNodes = nodes;
    mSelectedNode = nodes.isEmpty() == true ? QString() : nodes.first();
    mIsRendering = true;
    for (QTreeWidgetItemIterator item(mTree); *item != nullptr; ++item)
    {
        QTreeWidgetItem* row = *item;
        const bool isSelected = row->data(0, FILE_ROLE).toString() == mSelectedFile &&
                                row->data(0, CONFIGURATION_ROLE).toString() == mSelectedConfiguration &&
                                nodes.contains(row->data(0, NODE_ROLE).toString()) == true;
        row->setSelected(isSelected);
        if (isSelected == true && row->data(0, NODE_ROLE).toString() == mSelectedNode)
        {
            mTree->setCurrentItem(row, 0, QItemSelectionModel::NoUpdate);
        }
    }
    mIsRendering = false;
    renderInspector();
    renderEdges();
    updateActions();
}

bool RendererWorkspace::CreateConfiguration(const QString& name)
{
    if (mDocument.IsLoaded() == false || mSourceProblem.isEmpty() == false)
    {
        mStatus->setText(
            QStringLiteral("Open a graph and resolve its external source conflict before adding a configuration."));
        return false;
    }
    const RenderGraphDocumentStatus status = mDocument.AddConfiguration(mSelectedFile, name);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(
            QStringLiteral("Cannot add configuration: use a unique name beginning with a letter, "
                           "up to 63 ASCII letters, digits, '_' or '-'. A file supports eight configurations."));
        return false;
    }
    mSelectedConfiguration = name;
    mSelectedNode.clear();
    mSelectedNodes.clear();
    documentEdited(status);
    mViews->setCurrentWidget(mCanvas);
    return true;
}

bool RendererWorkspace::ApplyRestirTemplate(const QString& configuration)
{
    return ApplyConfigurationTemplate(configuration, QStringLiteral("restir-di"));
}

bool RendererWorkspace::ApplyConfigurationTemplate(const QString& configuration,
                                                   const QString& templateIdentity,
                                                   RenderGraphTemplateBudgetIncrease* inoutBudgetOrNull)
{
    if (CanCreateSubgraph() == false)
    {
        mStatus->setText(
            QStringLiteral("Open a graph and resolve external source conflicts before applying a template."));
        return false;
    }
    QString error;
    const RenderGraphDocumentStatus status = mDocument.ApplyConfigurationTemplate(mSelectedFile,
                                                                                  configuration,
                                                                                  templateIdentity,
                                                                                  error,
                                                                                  mSelectedConfiguration,
                                                                                  inoutBudgetOrNull);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(error);
        return false;
    }
    if (mSelectedFile == mDocument.GetRootGraph())
    {
        mSelectedFile = configuration + QStringLiteral(".lurender.json");
    }
    mSelectedConfiguration = configuration;
    mSelectedNode.clear();
    mSelectedNodes.clear();
    documentEdited(status);
    mViews->setCurrentWidget(mCanvas);
    mStatus->setText(QStringLiteral("Created %1#%2. Connect the Subgraph inputs and route its outputs. "
                                    "Save publishes the graph and shader sources.")
                         .arg(mSelectedFile, configuration));
    return true;
}

void RendererWorkspace::promptConfigurationTemplate()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("renderGraphTemplateDialog"));
    dialog.setWindowTitle(QStringLiteral("Apply Configuration Template"));
    QFormLayout* fields = new QFormLayout(&dialog);
    QComboBox* choice = new QComboBox(&dialog);
    choice->setObjectName(QStringLiteral("renderGraphConfigurationTemplate"));
    choice->addItem(QStringLiteral("ReSTIR DI — mesh emitter lighting"), QStringLiteral("restir-di"));
    choice->addItem(QStringLiteral("Visibility buffer + depth"), QStringLiteral("visibility-buffer"));
    choice->addItem(QStringLiteral("Visibility + secondary rays"), QStringLiteral("visibility-secondary-rays"));
    fields->addRow(QStringLiteral("Template"), choice);
    QLineEdit* name = new QLineEdit(mSelectedConfiguration, &dialog);
    name->setObjectName(QStringLiteral("renderGraphTemplateConfiguration"));
    QJsonObject graph;
    if (mDocument.GetGraph(mSelectedFile, graph) == true)
    {
        for (const QJsonValueConstRef value : graph.value(QStringLiteral("configurations")).toArray())
        {
            const QJsonObject configuration = value.toObject();
            if (configuration.value(QStringLiteral("id")).toString() == mSelectedConfiguration &&
                (configuration.value(QStringLiteral("nodes")).toArray().isEmpty() == false ||
                 configuration.value(QStringLiteral("inputs")).toArray().isEmpty() == false ||
                 configuration.value(QStringLiteral("outputs")).toArray().isEmpty() == false))
            {
                name->setText(QStringLiteral("RestirDi"));
            }
        }
    }
    if (mSelectedFile == mDocument.GetRootGraph())
    {
        name->setText(QStringLiteral("RestirDi"));
    }
    fields->addRow(QStringLiteral("Configuration"), name);
    QLabel* description = new QLabel(&dialog);
    description->setWordWrap(true);
    fields->addRow(description);
    const auto describe = [choice, name, description, &dialog]() {
        const QString identity = choice->currentData().toString();
        QString defaultName = QStringLiteral("RestirDi");
        QString text =
            QStringLiteral("Three ray passes with two automatically sized reservoirs and Graph settings. "
                           "Connect scene, view and color; requires native ray tracing and emissive meshes.");
        if (identity == QStringLiteral("visibility-buffer"))
        {
            defaultName = QStringLiteral("VisibilityBuffer");
            text = QStringLiteral("Write RGBA8 primitive/instance IDs and depth. Connect scene, view, visibility "
                                  "and depth; both attachments are exposed as outputs. Uses opaque static triangles.");
        }
        else if (identity == QStringLiteral("visibility-secondary-rays"))
        {
            defaultName = QStringLiteral("VisibilitySecondaryRays");
            text = QStringLiteral(
                "Rasterize visibility/depth, reconstruct the surface and trace one reflected ray. "
                "Connect scene, view and color; outputs color, visibility and depth. Requires native ray tracing.");
        }
        if (name->text() == QStringLiteral("RestirDi") || name->text() == QStringLiteral("VisibilityBuffer") ||
            name->text() == QStringLiteral("VisibilitySecondaryRays"))
        {
            name->setText(defaultName);
        }
        description->setText(QStringLiteral("Create a library configuration or fill an empty one. On the root canvas, "
                                            "also insert a Subgraph node. ") +
                             text);
        dialog.adjustSize();
    };
    connect(choice, &QComboBox::currentIndexChanged, &dialog, describe);
    describe();
    QLabel* error = new QLabel(&dialog);
    error->setObjectName(QStringLiteral("renderGraphTemplateError"));
    error->setWordWrap(true);
    fields->addRow(error);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
    fields->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply),
            &QPushButton::clicked,
            &dialog,
            [this, name, choice, error, &dialog]() {
                const QString configuration = name->text().trimmed();
                const QString templateIdentity = choice->currentData().toString();
                RenderGraphTemplateBudgetIncrease budget;
                bool isApplied = ApplyConfigurationTemplate(configuration, templateIdentity, &budget);
                if (isApplied == false && budget.RequiredPrograms > budget.CurrentPrograms)
                {
                    QMessageBox confirmation(QMessageBox::Question,
                                             QStringLiteral("Increase shader program budget?"),
                                             QStringLiteral("This template needs %1 shader programs, but the current "
                                                            "limit is %2. Increase budgets.programs from %2 to %1 "
                                                            "and apply the template?\n\n"
                                                            "The increase and template can be undone together. "
                                                            "Save publishes both changes.")
                                                 .arg(budget.RequiredPrograms)
                                                 .arg(budget.CurrentPrograms),
                                             QMessageBox::Cancel,
                                             &dialog);
                    confirmation.setObjectName(QStringLiteral("renderGraphTemplateBudgetConfirmation"));
                    QPushButton* increase =
                        confirmation.addButton(QStringLiteral("Increase and apply"), QMessageBox::AcceptRole);
                    confirmation.setDefaultButton(QMessageBox::Cancel);
                    confirmation.exec();
                    if (confirmation.clickedButton() == increase)
                    {
                        budget.ApprovedPrograms = budget.RequiredPrograms;
                        isApplied = ApplyConfigurationTemplate(configuration, templateIdentity, &budget);
                    }
                }
                if (isApplied == true)
                {
                    dialog.accept();
                }
                else
                {
                    error->setText(mStatus->text());
                    dialog.adjustSize();
                }
            });
    dialog.exec();
}

bool RendererWorkspace::RemoveSelectedConfiguration()
{
    if (CanCreateSubgraph() == false || selectedConfiguration().isEmpty() == true)
    {
        mStatus->setText(
            QStringLiteral("Select a configuration and resolve external source conflicts before deleting it."));
        return false;
    }
    const QString file = mSelectedFile;
    const QString configuration = mSelectedConfiguration;
    QString error;
    const RenderGraphDocumentStatus status = mDocument.RemoveConfiguration(file, configuration, error);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(error);
        return false;
    }
    mSelectedNode.clear();
    mSelectedNodes.clear();
    documentEdited(status);
    mStatus->setText(
        QStringLiteral("Deleted %1#%2. Undo restores it; Save publishes the deletion.").arg(file, configuration));
    return true;
}

void RendererWorkspace::promptCreateConfiguration()
{
    bool isAccepted = false;
    const QString name = QInputDialog::getText(this,
                                               QStringLiteral("Add Configuration"),
                                               QStringLiteral("Configuration name"),
                                               QLineEdit::Normal,
                                               QStringLiteral("NewConfiguration"),
                                               &isAccepted);
    if (isAccepted == true)
    {
        static_cast<void>(CreateConfiguration(name));
    }
}

bool RendererWorkspace::CopySelectedNodes()
{
    if (mSourceProblem.isEmpty() == false)
    {
        mStatus->setText(QStringLiteral("Resolve the external source conflict before copying nodes."));
        return false;
    }
    QVector<RenderGraphNodePosition> positions;
    for (const QString& identity : mSelectedNodes)
    {
        const QPointF position = mCanvas->GetNodePosition(identity);
        positions.append({identity, qRound(position.x()), qRound(position.y())});
    }
    QJsonObject payload;
    QString error;
    const RenderGraphDocumentStatus status =
        mDocument.CopyNodes(mSelectedFile, mCanvas->GetConfiguration(), mSelectedNodes, positions, payload, error);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(error);
        return false;
    }
    const QByteArray bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QMimeData* data = new QMimeData;
    data->setData(NODE_MIME, bytes);
    QApplication::clipboard()->setMimeData(data);
    mStatus->setText(QStringLiteral("Copied %1 nodes and their internal connections.").arg(mSelectedNodes.size()));
    return true;
}

bool RendererWorkspace::PasteNodes(QPointF position)
{
    if (math::IsFinite(position.x()) == false || math::IsFinite(position.y()) == false ||
        position.x() < -RenderGraphDocument::MAX_COORDINATE || position.x() > RenderGraphDocument::MAX_COORDINATE ||
        position.y() < -RenderGraphDocument::MAX_COORDINATE || position.y() > RenderGraphDocument::MAX_COORDINATE)
    {
        mStatus->setText(QStringLiteral("Paste needs a position inside the canvas coordinate limits."));
        return false;
    }
    if (CanCreateSubgraph() == false)
    {
        mStatus->setText(
            QStringLiteral("Select a configuration and resolve external source conflicts before pasting."));
        return false;
    }
    const QMimeData* dataOrNull = QApplication::clipboard()->mimeData();
    if (dataOrNull == nullptr || dataOrNull->hasFormat(NODE_MIME) == false)
    {
        mStatus->setText(QStringLiteral("Copy a render-graph node selection before pasting."));
        return false;
    }
    const QByteArray bytes = dataOrNull->data(NODE_MIME);
    if (bytes.size() > RenderGraphDocument::MAX_FILE_BYTES)
    {
        mStatus->setText(QStringLiteral("The clipboard node group exceeds the supported source size."));
        return false;
    }
    QStringList created;
    QString error;
    const RenderGraphDocumentStatus status = mDocument.PasteNodes(mCanvas->GetFile(),
                                                                  mCanvas->GetConfiguration(),
                                                                  QJsonDocument::fromJson(bytes).object(),
                                                                  qRound(position.x()),
                                                                  qRound(position.y()),
                                                                  created,
                                                                  error);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(error);
        return false;
    }
    mSelectedFile = mCanvas->GetFile();
    mSelectedConfiguration = mCanvas->GetConfiguration();
    mSelectedNodes = created;
    mSelectedNode = created.first();
    documentEdited(status);
    mCanvas->SelectNodes(created);
    mCanvas->RevealNode(created.first());
    mViews->setCurrentWidget(mCanvas);
    return true;
}

void RendererWorkspace::RemoveSelectedNodes()
{
    if (mSourceProblem.isEmpty() == false)
    {
        return;
    }
    const RenderGraphDocumentStatus status =
        mDocument.RemoveNodes(mSelectedFile, mCanvas->GetConfiguration(), mSelectedNodes);
    if (status == RenderGraphDocumentStatus::Ok)
    {
        mSelectedNode.clear();
        mSelectedNodes.clear();
    }
    documentEdited(status);
}
} // namespace ludus::editor
