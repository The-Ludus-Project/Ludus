#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/controller.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
QT_END_NAMESPACE
namespace ludus::editor
{
// Project-owned source/sequence drafts. Process and play ownership stay in the
// controller. Loading only reads bounded files; cooking/activation are explicit.
class ScriptWorkspace final : public QWidget
{
    Q_OBJECT
public:
    explicit ScriptWorkspace(EditorController* controller, QWidget* parent = nullptr);
    void SetProject(const QString& root, const QString& source, const QString& preset);
    void Refresh();
    /// Absolute source path of the loaded draft; empty when no program is selected.
    [[nodiscard]] QString GetPath() const;
    [[nodiscard]] bool ConfirmDiscard();
    [[nodiscard]] bool Dirty() const noexcept
    {
        return Dirty_;
    }
    [[nodiscard]] bool Save();
    [[nodiscard]] bool CanSave() const noexcept;
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    void Undo();
    void Redo();
    void Send(const QString& action);

Q_SIGNALS:
    void DocumentChanged();

private:
    void CommitPending();
    void OpenSelected();
    void RenderGraph();
    void ChangeNode(foundation::int32 row, foundation::int32 column);
    void Commit(const QByteArray& bytes);
    void History(bool redo);
    void Breakpoint(bool enabled = true);
    [[nodiscard]] bool MapsMatch() const;
    void Highlight();
    [[nodiscard]] QJsonObject Cursor(const QString& action) const;
    EditorController* Controller_;
    QString Root_;
    QString Source_;
    QString Preset_;
    QString Path_;
    QByteArray Disk_;
    QByteArray Baseline_;
    bool Graph_ = false;
    bool Dirty_ = false;
    bool Rendering_ = false;
    foundation::int32 Selected_ = -1;
    QString HighlightStop_;
    QJsonObject Program_;
    QJsonObject Sidecar_;
    QJsonObject Evidence_;
    QList<QByteArray> Undo_;
    QList<QByteArray> Redo_;
    QList<QPushButton*> DebugControls_;
    QComboBox* Assets_;
    QPlainTextEdit* Text_;
    QTableWidget* Nodes_;
    QPlainTextEdit* Debug_;
    QLabel* Status_;
    QPushButton* Cook_;
    QPushButton* Reload_;
    QPushButton* Break_;
    QPushButton* ClearBreak_;
    QSpinBox* Amount_;
};
} // namespace ludus::editor
