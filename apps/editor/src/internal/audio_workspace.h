#pragma once
#include <ludus/foundation/base/compiler.h>

#include "internal/audio_preview.h"
#include <ludus/content/content.h>

#include <QWidget>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class QListWidget;
class QLabel;
QT_END_NAMESPACE
namespace ludus::editor
{
class AudioWorkspace final : public QWidget
{
    Q_OBJECT
public:
    explicit AudioWorkspace(QWidget* parent = nullptr);
    void SetRoot(const QString& root);
    void StopPreview();
    void ShutdownPreview();
    void ResetPreview();
    [[nodiscard]] bool PreviewFinished() const noexcept;
    [[nodiscard]] bool ConfirmDiscard();
    void Save();
    [[nodiscard]] LUDUS_INLINE bool IsDirty() const noexcept
    {
        return Dirty_;
    }
    void RefreshCatalog();
    /// Prompt for an ID and create a definition using the selected imported audio source.
    void PromptCreateDefinition(bool isMusic);
    [[nodiscard]] bool OpenResource(const QString& id);
    [[nodiscard]] bool CanSave() const noexcept
    {
        return HasDocument_ && isEnabled();
    }

Q_SIGNALS:
    void DocumentChanged();
    void ImportRequested();

private:
    [[nodiscard]] bool ReadDraft(audio::content::Sound& sound, audio::content::Music& music);
    void Reload();
    void Open();
    void Import();
    void Create(bool music);
    void Render();
    void Edit();
    void Play();
    QString Root_;
    QString Path_;
    bool Dirty_ = false;
    bool Rendering_ = false;
    bool IsMusic_ = false;
    bool HasDocument_ = false;
    ludus::content::Catalog Catalog_;
    ludus::content::Digest Digest_;
    audio::content::Sound Sound_;
    audio::content::Music Music_;
    AudioPreview* Preview_ = nullptr;
    QListWidget* List_ = nullptr;
    QLabel* Message_ = nullptr;
    QWidget* Wave_ = nullptr;
    QLineEdit *Id_ = nullptr, *Bus_ = nullptr, *Group_ = nullptr, *Sources_ = nullptr, *Begin_ = nullptr,
              *End_ = nullptr;
    QSpinBox *Priority_ = nullptr, *Cooldown_ = nullptr;
    QDoubleSpinBox *Gain_ = nullptr, *Rate_ = nullptr, *Min_ = nullptr, *Max_ = nullptr;
    QCheckBox *Loop_ = nullptr, *Spatial_ = nullptr, *Suppress_ = nullptr;
};
} // namespace ludus::editor
