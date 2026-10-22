#include "internal/renderer_workspace.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/scalar.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
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
    connect(remove, &QShortcut::activated, this, &RendererWorkspace::RemoveSelectedNodes);
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
