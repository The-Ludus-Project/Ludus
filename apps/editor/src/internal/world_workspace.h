#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/identity/uuid.hpp>
#include <ludus/platform/entropy.h>

#include "internal/world_document.h"

#include <QMap>
#include <QTimer>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;
class QSplitter;
class QToolButton;
QT_END_NAMESPACE
namespace ludus::editor
{
// Desktop, project-owned schema UI. No runtime pools or project code are loaded.
class RenderPanel;
class RenderPreview;
class LevelWorkspace;
struct ToolingPaths;
class WorldWorkspace final : public QWidget
{
    Q_OBJECT
public:
    explicit WorldWorkspace(QWidget* parent = nullptr);
    WorldWorkspace(const ToolingPaths& tooling, QWidget* parent);
    [[nodiscard]] bool ImportScene(const QString& source);
    /// Dry-run source (a .pbrt, .gltf, .glb or .obj file, or a reference directory) and show the
    /// preview panel; ConfirmImportPreview then imports the previewed source.
    [[nodiscard]] bool PreviewImportScene(const QString& source);
    /// Import the source shown in the preview panel.
    [[nodiscard]] bool ConfirmImportPreview();
    [[nodiscard]] QString Path() const
    {
        return Path_;
    }
    /// Renderer of the viewport this World shows, whose device and extraction feed the
    /// render-graph preview: the scene viewport of an imported-scene World, otherwise the
    /// render panel, which draws schema-bound and World-demo Worlds alike. RenderPreviewChanged reports a change of
    /// choice. The result is owned by this workspace; it never starts a device on the caller's behalf.
    [[nodiscard]] RenderPreview& GetRenderPreview() noexcept;
    void SetProject(const QString& root, foundation::uint64 epoch);
    [[nodiscard]] bool Open(const QString& path);
    [[nodiscard]] bool Create(const QString& path);
    [[nodiscard]] bool Save();
    [[nodiscard]] bool ConfirmDiscard();
    [[nodiscard]] bool Dirty() const noexcept
    {
        return Pending_ || Unsaved_ || Document_.Dirty();
    }
    [[nodiscard]] bool CanSave() const noexcept
    {
        return Document_.Loaded() && SceneBusy() == false;
    }
    [[nodiscard]] bool CanUndo() const noexcept
    {
        return Pending_ == false && SceneBusy() == false && Document_.CanUndo();
    }
    [[nodiscard]] bool CanRedo() const noexcept
    {
        return Pending_ == false && SceneBusy() == false && Document_.CanRedo();
    }
    void StopPreview() noexcept;
    void ResumePreview();
    [[nodiscard]] bool PreviewRunning() const noexcept;
    [[nodiscard]] QJsonObject Session() const;
    void RestoreSession(const QJsonObject& state);
    void Undo();
    void Redo();
    [[nodiscard]] bool Apply();
    [[nodiscard]] bool Recover();
    [[nodiscard]] const WorldDocument& Document() const noexcept
    {
        return Document_;
    }
    /// Move the entity inspector (name, component fields, Add Component, Apply and Reset)
    /// out of this view into newParent, for a shell Properties tool window. The panel keeps
    /// editing this workspace's draft; this workspace keeps owning its state. Call once.
    /// @return The moved panel; it lives as long as newParent keeps it.
    [[nodiscard]] QWidget* TakePropertiesPanel(QWidget* newParent);
    /// Replace the entropy source that mints World, entity and scene-level IDs,
    /// including the embedded 3D level workspace's. Production keeps
    /// platform::GetSystemEntropySource(); tests inject a controlled or failing source.
    /// @pre No scene operation is running.
    void SetEntropySource(const foundation::identity::EntropySource& entropy) noexcept;
Q_SIGNALS:
    void DocumentChanged();
    void PreviewStarting();
    void PlayRequested();
    /// GetRenderPreview now returns a different viewport renderer (the shown view changed).
    void RenderPreviewChanged();

private:
    [[nodiscard]] bool LoadProjectSchema(WorldDocument& candidate, QByteArray& schema, bool worldDemo = false);
    [[nodiscard]] bool Inside(const QString& path, bool existing) const;
    [[nodiscard]] bool SceneBusy() const noexcept;
    void RefreshScene();
    void SceneCompleted(bool accepted);
    void SceneVerified(bool accepted);
    void Render();
    void RenderInspector();
    void Changed(const QString& message = {});
    void Pending();
    void PreviewDraft();
    [[nodiscard]] bool ReadDraft(WorldDocument& document, QString& error, QWidget*& invalid);
    void PickTint(const QString& id, const QJsonObject& binding);
    void Snapshot();
    [[nodiscard]] QByteArray RecoveryHeader() const;
    /// Record which viewport renderer the World shows and emit RenderPreviewChanged on change.
    void updateShownRenderPreview(bool isSceneShown);
    RenderPreview* mShownRenderPreviewOrNull = nullptr;
    /// Entity inspector column; TakePropertiesPanel moves it into a shell tool window.
    QWidget* mPropertiesPanel;
    WorldDocument Document_;
    foundation::identity::EntropySource mEntropy = platform::GetSystemEntropySource();
    QPushButton* PlayButton_ = nullptr;
    RenderPanel* Render_ = nullptr;
    LevelWorkspace* Scene_ = nullptr;
    QWidget* Viewport_ = nullptr;
    QWidget* BoundViewport_ = nullptr;
    QSplitter* Split_ = nullptr;
    QTimer DraftTimer_;
    bool DraftPreview_ = false;
    QPushButton* Import_ = nullptr;
    QPushButton* CancelScene_ = nullptr;
    /// Dry-run preview of a pending import, with confirm and discard actions.
    QPlainTextEdit* ImportPreview_ = nullptr;
    QPushButton* ConfirmImport_ = nullptr;
    QPushButton* DiscardImport_ = nullptr;
    QString PreviewedSource_;
    /// Stored import reports of the World's scene.
    QPlainTextEdit* ImportReport_ = nullptr;
    QToolButton* mImportReportToggleOrNull = nullptr;
    void ShowImportPreview(bool isAccepted);
    void ShowImportReports();
    /// Import source into the World; isPreviewed requires it to match the approved preview.
    [[nodiscard]] bool StartSceneImport(const QString& source, bool isPreviewed);
    /// Drop the previewed import and the report panel when the live World is replaced.
    void ClearImportState();
    bool Importing_ = false;
    bool SavePending_ = false;
    bool SaveValidated_ = false;
    QJsonObject ValidatedScene_;
    QByteArray ValidatedWorld_;
    QString SceneError_;
    QString ConversionPath_;
    QByteArray ConversionSchema_;
    foundation::uint64 SceneGeneration_ = 0;

    QString Root_;
    QString Path_;
    QString Selected_;
    QByteArray Disk_;
    QByteArray SchemaDisk_;
    foundation::uint64 Epoch_ = ~foundation::uint64{0};
    foundation::uint64 DocumentGeneration_ = 0;
    bool Pending_ = false;
    bool Unsaved_ = false;
    bool Rendering_ = false;
    bool RecoveryFailed_ = false;
    QMap<QString, QWidget*> Fields_;
    QListWidget* Entities_;
    QLabel* Status_;
    QLabel* PathLabel_;
    QLineEdit* Name_ = nullptr;
    QComboBox* Components_;
    QPushButton* Attach_;
    QPushButton* Add_;
    QPushButton* Duplicate_;
    QPushButton* Remove_;
    QPushButton* Apply_;
    QPushButton* Reset_;
    QPushButton* Recovery_;
    QVBoxLayout* Inspector_;
};
} // namespace ludus::editor
