#include "internal/script_workspace.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/sort.hpp>

#include <utility>

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QVBoxLayout>

namespace ludus::editor
{
namespace
{
QByteArray Read(const QString& path)
{
    QFile file(path);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > 131072)
    {
        return {};
    }
    return file.readAll();
}
QString Within(const QString& root, const QString& relative)
{
    if (relative.isEmpty() || QDir::isAbsolutePath(relative))
    {
        return {};
    }
    const QFileInfo info(QDir(root).filePath(relative));
    const auto path = info.canonicalFilePath();
    const auto base = QFileInfo(root).canonicalFilePath();
    return !info.isSymLink() && path.startsWith(base + QLatin1Char('/')) ? path : QString();
}
QJsonObject Object(const QByteArray& bytes)
{
    return QJsonDocument::fromJson(bytes).object();
}
} // namespace
ScriptWorkspace::ScriptWorkspace(EditorController* controller, QWidget* parent)
    : QWidget(parent), Controller_(controller)
{
    setObjectName(QStringLiteral("scriptWorkspace"));
    auto* layout = new QVBoxLayout(this);
    Assets_ = new QComboBox(this);
    Assets_->setObjectName(QStringLiteral("scriptAssets"));
    layout->addWidget(Assets_);
    auto* actions = new QHBoxLayout;
    auto button = [this, actions](const QString& text, auto callback) {
        auto* value = new QPushButton(text, this);
        actions->addWidget(value);
        connect(value, &QPushButton::clicked, this, callback);
        return value;
    };
    button(QStringLiteral("Save asset"), [this]() { (void)Save(); });
    button(QStringLiteral("Reload asset"), [this]() { OpenSelected(); });
    button(QStringLiteral("Undo"), [this]() { History(false); });
    button(QStringLiteral("Redo"), [this]() { History(true); });
    Cook_ = button(QStringLiteral("Cook scripts"), [this]() {
        if (!Dirty_)
        {
            Controller_->CookScripts();
        }
    });
    Reload_ = button(QStringLiteral("Build / Reload"), [this]() {
        if (!Dirty_)
        {
            if (Controller_->CanBuildReload())
            {
                Controller_->BuildReload();
            }
            else if (Controller_->Caps().CanBuild)
            {
                Controller_->Build();
            }
        }
    });
    layout->addLayout(actions);
    Text_ = new QPlainTextEdit(this);
    Text_->setObjectName(QStringLiteral("scriptText"));
    Text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    layout->addWidget(Text_);
    Nodes_ = new QTableWidget(this);
    Nodes_->setObjectName(QStringLiteral("scriptNodes"));
    Nodes_->setColumnCount(4);
    Nodes_->setHorizontalHeaderLabels({QStringLiteral("Node ID"),
                                       QStringLiteral("Kind"),
                                       QStringLiteral("Value"),
                                       QStringLiteral("Ordered children")});
    Nodes_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    Nodes_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    layout->addWidget(Nodes_);
    auto* debugActions = new QHBoxLayout;
    layout->addLayout(debugActions);
    auto control = [this](QHBoxLayout* row, const std::pair<QString, QString>& spec) {
        const auto& [name, action] = spec;
        auto* value = new QPushButton(name, this);
        row->addWidget(value);
        value->setProperty("scriptAction", action);
        DebugControls_.append(value);
        connect(value, &QPushButton::clicked, this, [this, action]() { Send(action); });
        return value;
    };
    control(debugActions, {QStringLiteral("Inspect"), QStringLiteral("inspect")});
    Amount_ = new QSpinBox(this);
    Amount_->setRange(1, 10);
    debugActions->addWidget(Amount_);
    control(debugActions, {QStringLiteral("Interact"), QStringLiteral("interact")});
    Break_ = new QPushButton(QStringLiteral("Set breakpoint"), this);
    debugActions->addWidget(Break_);
    connect(Break_, &QPushButton::clicked, this, [this]() { Breakpoint(); });
    ClearBreak_ = new QPushButton(QStringLiteral("Clear breakpoint"), this);
    debugActions->addWidget(ClearBreak_);
    connect(ClearBreak_, &QPushButton::clicked, this, [this]() { Breakpoint(false); });
    auto* steps = new QHBoxLayout;
    layout->addLayout(steps);
    for (const auto& pair : {std::pair{QStringLiteral("Continue"), QStringLiteral("continue")},
                             std::pair{QStringLiteral("Into"), QStringLiteral("into")},
                             std::pair{QStringLiteral("Over"), QStringLiteral("over")},
                             std::pair{QStringLiteral("Out"), QStringLiteral("out")},
                             std::pair{QStringLiteral("Restart behavior"), QStringLiteral("restart")}})
    {
        control(steps, pair);
    }
    Debug_ = new QPlainTextEdit(this);
    Debug_->setObjectName(QStringLiteral("scriptDebug"));
    Debug_->setReadOnly(true);
    Debug_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(Debug_);
    Status_ = new QLabel(QStringLiteral("Open a project with ludus.scripts.json."), this);
    Status_->setWordWrap(true);
    layout->addWidget(Status_);
    connect(Assets_, &QComboBox::currentIndexChanged, this, [this]() {
        if (!Rendering_)
        {
            OpenSelected();
        }
    });
    connect(Text_, &QPlainTextEdit::textChanged, this, [this]() {
        if (!Rendering_ && !Graph_)
        {
            Dirty_ = Text_->toPlainText().toUtf8() != Baseline_;
            Refresh();
        }
    });
    connect(Nodes_, &QTableWidget::cellChanged, this, &ScriptWorkspace::ChangeNode);
    connect(Controller_, &EditorController::StateChanged, this, &ScriptWorkspace::Refresh);
}
void ScriptWorkspace::SetProject(const QString& root, const QString& source, const QString& preset)
{
    if (Root_ == root && Source_ == source)
    {
        if (Preset_ != preset)
        {
            Preset_ = preset;
            HighlightStop_.clear();
            Refresh();
        }
        return;
    }
    Root_ = root;
    Source_ = source;
    Preset_ = preset;
    Path_.clear();
    Selected_ = -1;
    HighlightStop_.clear();
    Disk_.clear();
    Baseline_.clear();
    Dirty_ = false;
    Sidecar_ = Object(Read(QDir(root).filePath(QStringLiteral("ludus.scripts.json"))));
    const auto name = Sidecar_.value(QStringLiteral("name")).toString();
    const bool nameValid = !name.isEmpty() && name.size() <= 64 && name.at(0).isLetter() && [&name]() {
        for (QChar c : name)
        {
            if (c.unicode() >= 128 || (!c.isLetterOrNumber() && c != QLatin1Char('_')))
            {
                return false;
            }
        }
        return true;
    }();
    if (Sidecar_.size() != 4 || Sidecar_.value(QStringLiteral("version")).toInt() != 1 || !nameValid ||
        Within(root, Sidecar_.value(QStringLiteral("contract")).toString()).isEmpty())
    {
        Sidecar_ = {};
    }
    Rendering_ = true;
    Assets_->clear();
    const auto packagePath = Within(root, Sidecar_.value(QStringLiteral("package")).toString());
    const auto package = Object(Read(packagePath));
    const auto programs = package.value(QStringLiteral("programs")).toArray();
    for (const auto& entry : programs.size() <= 8 ? programs : QJsonArray{})
    {
        const auto program = entry.toObject();
        const auto path = Within(
            QFileInfo(packagePath).absolutePath(),
            program
                .value(program.contains(QStringLiteral("graph")) ? QStringLiteral("graph") : QStringLiteral("source"))
                .toString());
        if (!path.isEmpty())
        {
            Assets_->addItem(QFileInfo(path).fileName(), program);
        }
    }
    Rendering_ = false;
    OpenSelected();
}
void ScriptWorkspace::OpenSelected()
{
    if (Dirty_ && !ConfirmDiscard())
    {
        const QSignalBlocker blocker(Assets_);
        Assets_->setCurrentIndex(Selected_);
        return;
    }
    Selected_ = Assets_->currentIndex();
    HighlightStop_.clear();
    Program_ = Assets_->currentData().toJsonObject();
    Graph_ = Program_.contains(QStringLiteral("graph"));
    const auto package = Within(Root_, Sidecar_.value(QStringLiteral("package")).toString());
    Path_ = Within(QFileInfo(package).absolutePath(),
                   Program_.value(Graph_ ? QStringLiteral("graph") : QStringLiteral("source")).toString());
    Disk_ = Read(Path_);
    Undo_.clear();
    Redo_.clear();
    Dirty_ = false;
    Rendering_ = true;
    Text_->setPlainText(QString::fromUtf8(Disk_));
    Baseline_ = Text_->toPlainText().toUtf8();
    Text_->setReadOnly(Graph_ || Path_.isEmpty());
    Rendering_ = false;
    RenderGraph();
    Refresh();
}
void ScriptWorkspace::Commit(const QByteArray& bytes)
{
    Undo_.append(Text_->toPlainText().toUtf8());
    if (Undo_.size() > 64)
    {
        Undo_.removeFirst();
    }
    Redo_.clear();
    Rendering_ = true;
    Text_->setPlainText(QString::fromUtf8(bytes));
    Rendering_ = false;
    Dirty_ = bytes != Baseline_;
    RenderGraph();
    Refresh();
}
void ScriptWorkspace::History(bool redo)
{
    if (!Graph_)
    {
        if (redo)
        {
            Text_->redo();
        }
        else
        {
            Text_->undo();
        }
        return;
    }
    auto& from = redo ? Redo_ : Undo_;
    auto& to = redo ? Undo_ : Redo_;
    if (from.isEmpty())
    {
        return;
    }
    to.append(Text_->toPlainText().toUtf8());
    const auto bytes = from.takeLast();
    Rendering_ = true;
    Text_->setPlainText(QString::fromUtf8(bytes));
    Rendering_ = false;
    Dirty_ = bytes != Baseline_;
    RenderGraph();
    Refresh();
}
void ScriptWorkspace::RenderGraph()
{
    Text_->setVisible(!Graph_);
    Nodes_->setVisible(Graph_);
    if (!Graph_)
    {
        return;
    }
    Rendering_ = true;
    const auto nodes = Object(Text_->toPlainText().toUtf8()).value(QStringLiteral("nodes")).toArray();
    if (nodes.size() > 64)
    {
        Nodes_->setRowCount(0);
        Rendering_ = false;
        return;
    }
    Nodes_->setRowCount(static_cast<foundation::int32>(nodes.size()));
    for (foundation::int32 row = 0; row < nodes.size(); ++row)
    {
        const auto node = nodes.at(row).toObject();
        const auto kind = node.value(QStringLiteral("kind")).toString();
        const QString key = kind == QStringLiteral("Increment") ? QStringLiteral("amount")
                            : kind == QStringLiteral("If")      ? QStringLiteral("threshold")
                            : kind == QStringLiteral("Repeat")  ? QStringLiteral("count")
                                                                : QString();
        const auto value = node.value(key);
        QStringList children;
        for (const auto& child : node.value(QStringLiteral("children")).toArray())
        {
            children.append(child.toString());
        }
        const QStringList columns{node.value(QStringLiteral("id")).toString(),
                                  kind,
                                  value.isString() ? value.toString()
                                  : key.isEmpty()  ? QString()
                                                   : QString::number(value.toInt()),
                                  children.join(QLatin1Char(','))};
        for (foundation::int32 col = 0; col < 4; ++col)
        {
            auto* item = new QTableWidgetItem(columns.at(col));
            if (col != 2 || key.isEmpty())
            {
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            }
            Nodes_->setItem(row, col, item);
        }
    }
    Rendering_ = false;
}
void ScriptWorkspace::ChangeNode(foundation::int32 row, foundation::int32 column)
{
    if (Rendering_ || column != 2 || row < 0)
    {
        return;
    }
    auto document = Object(Text_->toPlainText().toUtf8());
    auto nodes = document.value(QStringLiteral("nodes")).toArray();
    if (row >= nodes.size())
    {
        return;
    }
    auto node = nodes.at(row).toObject();
    const auto kind = node.value(QStringLiteral("kind")).toString();
    const QString key = kind == QStringLiteral("Increment") ? QStringLiteral("amount")
                        : kind == QStringLiteral("If")      ? QStringLiteral("threshold")
                        : kind == QStringLiteral("Repeat")  ? QStringLiteral("count")
                                                            : QString();
    const auto text = Nodes_->item(row, column)->text();
    bool ok = false;
    const foundation::int32 number = text.toInt(&ok);
    if (key.isEmpty() || (!ok && !(key == QStringLiteral("amount") && text == QStringLiteral("event"))) ||
        (ok && (number < 1 || number > (key == QStringLiteral("count") ? 4 : 10))))
    {
        RenderGraph();
        return;
    }
    node.insert(key, ok ? QJsonValue(number) : QJsonValue(text));
    nodes[row] = node;
    document.insert(QStringLiteral("nodes"), nodes);
    Commit(QJsonDocument(document).toJson(QJsonDocument::Indented));
}
bool ScriptWorkspace::CanSave() const noexcept
{
    return !Path_.isEmpty();
}
bool ScriptWorkspace::CanUndo() const
{
    return Graph_ ? !Undo_.isEmpty() : Text_->document()->isUndoAvailable();
}
bool ScriptWorkspace::CanRedo() const
{
    return Graph_ ? !Redo_.isEmpty() : Text_->document()->isRedoAvailable();
}
void ScriptWorkspace::Undo()
{
    History(false);
}
void ScriptWorkspace::Redo()
{
    History(true);
}
void ScriptWorkspace::CommitPending()
{
    auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
    if (edit != nullptr && Nodes_->isAncestorOf(edit) && Nodes_->currentIndex().isValid())
    {
        Nodes_->itemDelegate()->setModelData(edit, Nodes_->model(), Nodes_->currentIndex());
    }
}
bool ScriptWorkspace::Save()
{
    CommitPending();
    if (Path_.isEmpty() || !QFileInfo(Path_).isFile() || QFileInfo(Path_).canonicalFilePath() != Path_ ||
        Read(Path_) != Disk_ || QFileInfo(Path_).isSymLink())
    {
        Status_->setText(QStringLiteral("Asset changed on disk; reopen or merge explicitly."));
        return false;
    }
    const auto bytes = Text_->toPlainText().toUtf8();
    if (bytes.size() > 131072)
    {
        return false;
    }
    QSaveFile file(Path_);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    {
        Status_->setText(QStringLiteral("Asset save failed."));
        return false;
    }
    Disk_ = bytes;
    Baseline_ = bytes;
    Dirty_ = false;
    Refresh();
    return true;
}
bool ScriptWorkspace::ConfirmDiscard()
{
    CommitPending();
    if (!Dirty_)
    {
        return true;
    }
    const auto answer = QMessageBox::question(this,
                                              QStringLiteral("Unsaved script asset"),
                                              QStringLiteral("Save this asset before continuing?"),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                              QMessageBox::Cancel);
    if (answer == QMessageBox::Save)
    {
        return Save();
    }
    if (answer == QMessageBox::Discard)
    {
        Rendering_ = true;
        Text_->setPlainText(QString::fromUtf8(Baseline_));
        Rendering_ = false;
        Undo_.clear();
        Redo_.clear();
        HighlightStop_.clear();
        Dirty_ = false;
        RenderGraph();
        Refresh();
        return true;
    }
    return false;
}
QJsonObject ScriptWorkspace::Cursor(const QString& action) const
{
    QJsonObject command{{QStringLiteral("version"), 1}, {QStringLiteral("action"), action}};
    if (action != QStringLiteral("inspect"))
    {
        const auto& status = Controller_->PlayState().ScriptStatus;
        for (const auto& key : {QStringLiteral("session"), QStringLiteral("execution"), QStringLiteral("stop")})
        {
            command.insert(key, status.value(key));
        }
    }
    return command;
}
void ScriptWorkspace::Send(const QString& action)
{
    auto command = Cursor(action);
    if (action == QStringLiteral("interact"))
    {
        command.insert(QStringLiteral("asset"), Program_.value(QStringLiteral("asset")));
        command.insert(QStringLiteral("amount"), Amount_->value());
    }
    Controller_->ScriptCommand(command);
}
void ScriptWorkspace::Breakpoint(bool enabled)
{
    const auto& status = Controller_->PlayState().ScriptStatus;
    if (!MapsMatch() || status.value(QStringLiteral("package")) != Evidence_.value(QStringLiteral("key")))
    {
        return;
    }
    for (const auto& value : Evidence_.value(QStringLiteral("programs")).toArray())
    {
        const auto program = value.toObject();
        if (program.value(QStringLiteral("asset")) != Program_.value(QStringLiteral("asset")))
        {
            continue;
        }
        foundation::int32 line =
            Text_->textCursor().blockNumber() + 1 + program.value(QStringLiteral("first_line")).toInt() - 2;
        if (Graph_)
        {
            const foundation::int32 row = Nodes_->currentRow();
            if (row < 0 || Nodes_->item(row, 0) == nullptr)
            {
                return;
            }
            line = 0;
            for (const auto& entry : program.value(QStringLiteral("maps")).toArray())
            {
                const auto span = entry.toObject();
                if (span.value(QStringLiteral("node")).toString() == Nodes_->item(row, 0)->text())
                {
                    line = span.value(QStringLiteral("compiled_line")).toInt();
                    break;
                }
            }
            if (line == 0)
            {
                return;
            }
        }
        auto command = Cursor(QStringLiteral("breakpoint"));
        command.insert(QStringLiteral("asset"), Program_.value(QStringLiteral("asset")));
        command.insert(QStringLiteral("line"), line);
        command.insert(QStringLiteral("enabled"), enabled);
        Controller_->ScriptCommand(command);
        return;
    }
}
bool ScriptWorkspace::MapsMatch() const
{
    if (Dirty_ || Path_.isEmpty())
    {
        return false;
    }
    auto bytes = Text_->toPlainText().toUtf8();
    if (Graph_)
    {
        auto value = Object(bytes);
        value.remove(QStringLiteral("layout"));
        for (const auto& key : {QStringLiteral("nodes"), QStringLiteral("variables")})
        {
            auto entries = value.value(key).toArray().toVariantList();
            foundation::Sort(entries.data(),
                             static_cast<foundation::usize>(entries.size()),
                             [](const QVariant& a, const QVariant& b) {
                                 return a.toMap().value(QStringLiteral("id")).toString() <
                                        b.toMap().value(QStringLiteral("id")).toString();
                             });
            value.insert(key, QJsonArray::fromVariantList(entries));
        }
        bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    }
    const auto hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    for (const auto& entry : Evidence_.value(QStringLiteral("programs")).toArray())
    {
        const auto program = entry.toObject();
        if (program.value(QStringLiteral("asset")) == Program_.value(QStringLiteral("asset")))
        {
            return program.value(QStringLiteral("authored")).toString() == hash;
        }
    }
    return false;
}
void ScriptWorkspace::Highlight()
{
    const auto& status = Controller_->PlayState().ScriptStatus;
    const auto stop = status.value(QStringLiteral("stop")).toString();
    if (!status.value(QStringLiteral("partial")).toBool() || !MapsMatch() ||
        status.value(QStringLiteral("package")) != Evidence_.value(QStringLiteral("key")))
    {
        Text_->setExtraSelections({});
        HighlightStop_.clear();
        return;
    }
    if (stop == HighlightStop_)
    {
        return;
    }
    const auto frames = status.value(QStringLiteral("frames")).toArray();
    if (frames.isEmpty())
    {
        return;
    }
    const auto frame = frames.first().toObject();
    const auto source = frame.value(QStringLiteral("source")).toString();
    if (!source.endsWith(QStringLiteral("asset/") + Program_.value(QStringLiteral("asset")).toString()))
    {
        return;
    }
    HighlightStop_ = stop;
    if (Graph_)
    {
        for (foundation::int32 row = 0; row < Nodes_->rowCount(); ++row)
        {
            if (Nodes_->item(row, 0)->text() == frame.value(QStringLiteral("node")).toString())
            {
                Nodes_->selectRow(row);
                Nodes_->scrollToItem(Nodes_->item(row, 0));
                return;
            }
        }
        return;
    }
    for (const auto& entry : Evidence_.value(QStringLiteral("programs")).toArray())
    {
        const auto program = entry.toObject();
        if (program.value(QStringLiteral("asset")) != Program_.value(QStringLiteral("asset")))
        {
            continue;
        }
        const foundation::int32 line =
            frame.value(QStringLiteral("line")).toInt() - program.value(QStringLiteral("first_line")).toInt() + 2;
        if (line <= 0)
        {
            return;
        }
        auto cursor = Text_->textCursor();
        cursor.movePosition(QTextCursor::Start);
        cursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor, line - 1);
        Text_->setTextCursor(cursor);
        Text_->ensureCursorVisible();
        QTextEdit::ExtraSelection selection;
        selection.cursor = foundation::Move(cursor);
        selection.format.setBackground(QColor(Qt::yellow));
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        Text_->setExtraSelections({selection});
        return;
    }
}
void ScriptWorkspace::Refresh()
{
    const auto output = QDir(Source_).filePath(QStringLiteral("out/build/") + Preset_ + QLatin1Char('/') +
                                               Sidecar_.value(QStringLiteral("name")).toString());
    const auto pointer = Object(Read(QDir(output).filePath(QStringLiteral("current.json"))));
    const auto key = pointer.value(QStringLiteral("key")).toString();
    Evidence_ = {};
    if (key.size() == 64 && [&key]() {
            for (QChar c : key)
            {
                if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                      (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
                {
                    return false;
                }
            }
            return true;
        }())
    {
        Evidence_ = Object(Read(Within(output, key + QStringLiteral("/evidence.json"))));
    }
    const auto& play = Controller_->PlayState();
    Debug_->setPlainText(play.ScriptMessage + QLatin1Char('\n') +
                         QString::fromUtf8(QJsonDocument(play.ScriptStatus).toJson(QJsonDocument::Indented)));
    Cook_->setEnabled(!Path_.isEmpty() && !Sidecar_.isEmpty() && !Dirty_ && !Controller_->State().Busy());
    Reload_->setEnabled(!Dirty_ && (Controller_->CanBuildReload() || Controller_->Caps().CanBuild));
    const bool ready = play.Phase == PlayPhase::Running && !play.ScriptBusy && !play.DebuggerStopped;
    for (auto* control : DebugControls_)
    {
        const auto action = control->property("scriptAction").toString();
        control->setEnabled(ready &&
                            (action == QStringLiteral("inspect") ||
                             (!play.ScriptStatus.isEmpty() &&
                              (action == QStringLiteral("restart") ||
                               (action == QStringLiteral("interact") ? !play.ScriptPaused : play.ScriptPaused)))));
    }
    Break_->setEnabled(ready && MapsMatch() && !play.ScriptStatus.isEmpty() &&
                       play.ScriptStatus.value(QStringLiteral("package")) == Evidence_.value(QStringLiteral("key")));
    ClearBreak_->setEnabled(Break_->isEnabled());
    Highlight();
#if defined(Q_OS_WASM)
    Cook_->setEnabled(false);
    Reload_->setEnabled(false);
    Break_->setEnabled(false);
    ClearBreak_->setEnabled(false);
#endif
    Status_->setText(
        Dirty_         ? QStringLiteral("Unsaved asset draft; save before cooking.")
        : !MapsMatch() ? QStringLiteral("Source maps are stale; cook, then build/reload before setting breakpoints.")
        : play.ScriptPaused
            ? QStringLiteral(
                  "Paused script: state/effects are unpublished. Continue/step or restart before native reload.")
            : QStringLiteral(
                  "Cook prepares a candidate. Build / Reload activates it. Inspect obtains a fresh debugger cursor."));
    Q_EMIT DocumentChanged();
}
} // namespace ludus::editor
