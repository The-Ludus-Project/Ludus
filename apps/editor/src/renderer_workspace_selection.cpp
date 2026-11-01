#include "internal/renderer_workspace.h"

#include "internal/render_graph_template_registry.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/scalar.hpp>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
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

/// First of `<base>Copy`, `<base>Copy2`, ... that is not in taken (compared case-insensitively).
QString SuggestCopyName(const QString& base, const QStringList& taken)
{
    const QString stem = base + QStringLiteral("Copy");
    QString candidate = stem;
    for (int32 i = 2; taken.contains(candidate, Qt::CaseInsensitive) == true; ++i)
    {
        candidate = stem + QString::number(i);
    }
    return candidate;
}
} // namespace

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

bool RendererWorkspace::DuplicateSelectedConfiguration(const QString& name)
{
    if (CanCreateSubgraph() == false || selectedConfiguration().isEmpty() == true)
    {
        mStatus->setText(
            QStringLiteral("Select a configuration and resolve external source conflicts before duplicating it."));
        return false;
    }
    const QString file = mSelectedFile;
    const QString configuration = mSelectedConfiguration;
    QString error;
    const RenderGraphDocumentStatus status = mDocument.DuplicateConfiguration(file, configuration, name, error);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(error);
        return false;
    }
    mSelectedConfiguration = name;
    mSelectedNode.clear();
    mSelectedNodes.clear();
    documentEdited(status);
    mViews->setCurrentWidget(mCanvas);
    mStatus->setText(QStringLiteral("Duplicated %1#%2 as %1#%3. Undo removes the copy; Save publishes it.")
                         .arg(file, configuration, name));
    return true;
}

bool RendererWorkspace::DuplicateSelectedGraph(const QString& name)
{
    QJsonObject graph;
    if (CanCreateSubgraph() == false || mDocument.GetGraph(mSelectedFile, graph) == false)
    {
        mStatus->setText(
            QStringLiteral("Select a graph file and resolve external source conflicts before duplicating it."));
        return false;
    }
    const QString file = mSelectedFile;
    QString asset;
    QString error;
    const RenderGraphDocumentStatus status = mDocument.DuplicateGraph(file, name, asset, error);
    if (status != RenderGraphDocumentStatus::Ok)
    {
        mStatus->setText(error);
        return false;
    }
    mSelectedFile = asset;
    mSelectedConfiguration = graph.value(QStringLiteral("default")).toString();
    mSelectedNode.clear();
    mSelectedNodes.clear();
    documentEdited(status);
    mSourceFile->setCurrentText(asset);
    mViews->setCurrentWidget(mCanvas);
    mStatus->setText(QStringLiteral("Duplicated %1 as %2. Quick Add inserts it as a Subgraph; Undo removes the copy "
                                    "and Save publishes it.")
                         .arg(file, asset));
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

void RendererWorkspace::promptConfigurationTemplate(const QString& templateIdentity)
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("renderGraphTemplateDialog"));
    dialog.setWindowTitle(QStringLiteral("Apply Configuration Template"));
    QFormLayout* fields = new QFormLayout(&dialog);
    QComboBox* choice = new QComboBox(&dialog);
    choice->setObjectName(QStringLiteral("renderGraphConfigurationTemplate"));
    // Each item carries its snippet's proposed configuration name and description beside its identity.
    constexpr int32 DEFAULT_CONFIGURATION_ROLE = Qt::UserRole + 1;
    constexpr int32 DESCRIPTION_ROLE = Qt::UserRole + 2;
    QStringList defaultConfigurations;
    for (const RenderGraphTemplate& entry :
         RenderGraphTemplateRegistry::GetBuiltIn().GetTemplates(RenderGraphTemplateKind::Snippet))
    {
        choice->addItem(entry.Label, entry.Identity);
        choice->setItemData(choice->count() - 1, entry.DefaultConfiguration, DEFAULT_CONFIGURATION_ROLE);
        choice->setItemData(choice->count() - 1, entry.Description, DESCRIPTION_ROLE);
        defaultConfigurations.append(entry.DefaultConfiguration);
    }
    const QString firstConfiguration =
        defaultConfigurations.isEmpty() == true ? QString{} : defaultConfigurations.first();
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
                name->setText(firstConfiguration);
            }
        }
    }
    if (mSelectedFile == mDocument.GetRootGraph())
    {
        name->setText(firstConfiguration);
    }
    fields->addRow(QStringLiteral("Configuration"), name);
    QLabel* description = new QLabel(&dialog);
    description->setWordWrap(true);
    fields->addRow(description);
    const auto describe = [choice, name, description, defaultConfigurations, &dialog]() {
        if (defaultConfigurations.contains(name->text()) == true)
        {
            name->setText(choice->currentData(DEFAULT_CONFIGURATION_ROLE).toString());
        }
        description->setText(QStringLiteral("Create a library configuration or fill an empty one. On the root canvas, "
                                            "also insert a Subgraph node. ") +
                             choice->currentData(DESCRIPTION_ROLE).toString());
        dialog.adjustSize();
    };
    if (templateIdentity.isEmpty() == false && choice->findData(templateIdentity) >= 0)
    {
        choice->setCurrentIndex(choice->findData(templateIdentity));
    }
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
                // An attempt reports at most one repair, so a program increase can reveal a settings
                // removal on the retry. Each repair is offered once.
                constexpr int32 MAX_TEMPLATE_REPAIRS = 2;
                for (int32 repair = 0; repair < MAX_TEMPLATE_REPAIRS && isApplied == false; ++repair)
                {
                    if (confirmTemplateRepair(budget, &dialog) == false)
                    {
                        break;
                    }
                    isApplied = ApplyConfigurationTemplate(configuration, templateIdentity, &budget);
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

bool RendererWorkspace::confirmTemplateRepair(RenderGraphTemplateBudgetIncrease& inoutBudget, QWidget* parent)
{
    const bool shouldIncreasePrograms = inoutBudget.RequiredPrograms > inoutBudget.CurrentPrograms &&
                                        inoutBudget.ApprovedPrograms < inoutBudget.RequiredPrograms;
    bool shouldRemoveSettings = false;
    for (const QString& setting : inoutBudget.UnusedSettings)
    {
        shouldRemoveSettings = shouldRemoveSettings || inoutBudget.ApprovedSettingRemovals.contains(setting) == false;
    }
    if (shouldIncreasePrograms == false && shouldRemoveSettings == false)
    {
        return false;
    }
    QMessageBox confirmation(QMessageBox::Question, QString{}, QString{}, QMessageBox::Cancel, parent);
    QPushButton* accept = confirmation.addButton(shouldIncreasePrograms == true ? QStringLiteral("Increase and apply")
                                                                                : QStringLiteral("Remove and apply"),
                                                 QMessageBox::AcceptRole);
    if (shouldIncreasePrograms == true)
    {
        confirmation.setWindowTitle(QStringLiteral("Increase shader program budget?"));
        confirmation.setText(QStringLiteral("This template needs %1 shader programs, but the current limit is %2. "
                                            "Increase budgets.programs from %2 to %1 and apply the template?\n\n"
                                            "The increase and template can be undone together. "
                                            "Save publishes both changes.")
                                 .arg(inoutBudget.RequiredPrograms)
                                 .arg(inoutBudget.CurrentPrograms));
        confirmation.setObjectName(QStringLiteral("renderGraphTemplateBudgetConfirmation"));
    }
    else
    {
        confirmation.setWindowTitle(QStringLiteral("Remove unused settings?"));
        confirmation.setText(QStringLiteral("This template needs %1 settings, more than the editor supports. "
                                            "No shader in this package uses these %2 settings:\n\n%3\n\n"
                                            "Remove them and apply the template?\n\n"
                                            "The removal and template can be undone together. "
                                            "Save publishes both changes.")
                                 .arg(inoutBudget.RequiredSettings)
                                 .arg(inoutBudget.UnusedSettings.size())
                                 .arg(inoutBudget.UnusedSettings.join(QStringLiteral(", "))));
        confirmation.setObjectName(QStringLiteral("renderGraphTemplateSettingsConfirmation"));
    }
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != accept)
    {
        return false;
    }
    if (shouldIncreasePrograms == true)
    {
        inoutBudget.ApprovedPrograms = inoutBudget.RequiredPrograms;
    }
    else
    {
        inoutBudget.ApprovedSettingRemovals = inoutBudget.UnusedSettings;
    }
    return true;
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

void RendererWorkspace::promptDuplicateConfiguration()
{
    QStringList taken;
    QJsonObject graph;
    if (mDocument.GetGraph(mSelectedFile, graph) == true)
    {
        for (const QJsonValueConstRef value : graph.value(QStringLiteral("configurations")).toArray())
        {
            taken.append(value.toObject().value(QStringLiteral("id")).toString());
        }
    }
    bool isAccepted = false;
    const QString name =
        QInputDialog::getText(this,
                              QStringLiteral("Duplicate Configuration"),
                              QStringLiteral("Copy %1#%2 as:").arg(mSelectedFile, mSelectedConfiguration),
                              QLineEdit::Normal,
                              SuggestCopyName(mSelectedConfiguration, taken),
                              &isAccepted)
            .trimmed();
    if (isAccepted == true)
    {
        static_cast<void>(DuplicateSelectedConfiguration(name));
    }
}

void RendererWorkspace::promptDuplicateGraph()
{
    const QString suffix = QStringLiteral(".lurender.json");
    QStringList taken;
    for (const QString& source : mDocument.GetFiles())
    {
        if (source.endsWith(suffix) == false)
        {
            continue;
        }
        taken.append(source.chopped(suffix.size()));
        QJsonObject existing;
        if (mDocument.GetGraph(source, existing) == true)
        {
            taken.append(existing.value(QStringLiteral("id")).toString());
        }
    }
    QJsonObject graph;
    static_cast<void>(mDocument.GetGraph(mSelectedFile, graph));
    QString base = graph.value(QStringLiteral("id")).toString();
    if (base.isEmpty() == true)
    {
        base = QFileInfo(mSelectedFile).fileName().chopped(suffix.size());
    }
    bool isAccepted = false;
    const QString name =
        QInputDialog::getText(this,
                              QStringLiteral("Duplicate Graph"),
                              QStringLiteral("Copy %1 with every configuration.\nName (creates <name>.lurender.json):")
                                  .arg(mSelectedFile),
                              QLineEdit::Normal,
                              SuggestCopyName(base, taken),
                              &isAccepted)
            .trimmed();
    if (isAccepted == true)
    {
        static_cast<void>(DuplicateSelectedGraph(name));
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
