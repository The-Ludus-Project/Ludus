#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include "internal/render_preview.h"
#include "internal/world_document.h"

#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QSlider;
class QToolButton;
class QDoubleSpinBox;
QT_END_NAMESPACE
namespace ludus::editor
{
class RenderLoadJob;
class PreviewViewport;
class PlayViewportSource;
class RenderPanel final : public QWidget
{
    Q_OBJECT
public:
    explicit RenderPanel(QWidget* parent = nullptr);
    ~RenderPanel() override;
    void SetWorld(const QString& root, const WorldDocument* world);
    /// Shows a failed scene import in the viewport when there is no previous frame to keep.
    void ShowSceneFailure(const QString& error);
    [[nodiscard]] bool Refresh(bool launch);
    void Stop() noexcept;
    void WorldChanged();
    void PreviewDraft(const WorldDocument& draft, bool shouldStart = false);
    /// Prepare an editor-only terrain snapshot. Cancel invalidates the owned load before publication.
    void PreviewTerrainDraft(const WorldDocument& draft);
    void CancelTerrainDraft() noexcept;
    [[nodiscard]] QString GetLoadError() const;
    [[nodiscard]] QWidget* TakeViewport(QWidget* parent);
    void SetSelection(const QString& id, bool pending);
    [[nodiscard]] RenderPreview& Preview() noexcept
    {
        return Preview_;
    }
    /// Embedded viewport; borrowed for project render-graph binding.
    [[nodiscard]] LUDUS_INLINE PreviewViewport& GetViewport() noexcept
    {
        return *Viewport_;
    }
    /// The "Viewport" menu of resume, pause, reload, tint and camera commands; its owner places it in a header row.
    [[nodiscard]] LUDUS_INLINE QToolButton& GetViewportMenuButton() noexcept
    {
        return *mViewportMenuButton;
    }
    /// @brief Begin Play in the viewport: the game launched with outAddress opens in this viewport.
    ///
    /// Keeps the edit preview's device (the game's frames are imported onto it) and starts a
    /// play source. The viewport keeps the edit image until the game's first frame arrives,
    /// then presents the game and forwards input to it. Native builds only.
    /// @param outAddress The `LUDUS_HOSTED_WINDOW` value for the game, set on success.
    /// @param outReason Why Play cannot open in the viewport, set on failure.
    /// @return True when the game's window will be this viewport.
    [[nodiscard]] bool BeginPlay(QString& outAddress, QString& outReason);
    /// End Play in the viewport: present edit frames again and close the game's window.
    void EndPlay() noexcept;
    /// Close the game's window, as clicking a native window's close button would.
    void RequestPlayClose() noexcept;
    /// Whether a Play-in-the-viewport session is open.
    [[nodiscard]] bool IsPlaying() const noexcept;
    /// Why the last Play-in-the-viewport session ended because the game's images were refused
    /// after launch; empty otherwise. Cleared by the next BeginPlay.
    [[nodiscard]] QString GetPlayUnavailableReason() const;
    /// @brief Label the viewport while a game runs in its own window, or remove the label.
    ///
    /// The label replaces the preview status until it is removed; removing it shows the
    /// preview's own status again, or that the viewport is paused.
    void SetPlayingElsewhere(bool isPlayingElsewhere);
    /// Whether the playing-elsewhere label is shown.
    [[nodiscard]] bool IsPlayingElsewhere() const noexcept;
Q_SIGNALS:
    /// The Play-in-the-viewport session changed state (started, showing the game, or ended).
    void PlayStateChanged();
    void Starting();
    void Loaded(bool success);
    void TintRequested(const QString& id, const QJsonObject& binding);
    /// @brief The viewport reported something the user should see: a failure, a pause or a Play state.
    ///
    /// The panel keeps no visible status line of its own: a wrapped line above the viewport would change the
    /// viewport's height whenever its text changed, and a resize cancels a held drag. Its owner shows the message
    /// in the window's status bar instead. Routine progress (validation, preparation, frame statistics) is not
    /// announced; the "Viewport" menu button's tooltip always holds the latest status.
    /// @param message One or more sentences for the user, including any repair to make.
    /// @param isFailure True when the message reports a refusal or failure, which should stay until replaced.
    void StatusAnnounced(const QString& message, bool isFailure);

private:
    /// How much a status message matters to the user; see StatusAnnounced.
    enum class StatusKind : foundation::uint8
    {
        /// Routine progress, kept in the status label and the menu button's tooltip only.
        Progress,
        /// A state change the user should notice, such as a pause or Play.
        Notice,
        /// A refusal or failure, with its repair.
        Failure,
    };
    /// Replace the panel's status and announce it unless kind is Progress.
    void setStatus(const QString& message, StatusKind kind);
    [[nodiscard]] bool publish(const RenderScene& scene, bool shouldLaunch);
    void navigate();
    void updateNavigationControls();
    void ShowStatus();
    void UpdateTintAction();
    const WorldDocument* World_ = nullptr;
    QString Root_;
    QString Stale_;
    QString Selected_;
    bool Pending_ = false;
    RenderLoadJob* Job_ = nullptr;
    foundation::uint64 Generation_ = 0;
    foundation::uint64 mPublishedWorldGeneration = 0;
    bool Again_ = false;
    bool QueuedLaunch_ = false;
    bool mIsTerrainDraft = false;
    bool mHasPendingTerrainLoad = false;
    /// Whether the next published scene still selects its default camera (set when a World opens).
    bool mShouldUseDefaultCamera = false;
    RenderPreview Preview_;
    PreviewViewport* Viewport_;
    void updatePlay();
    PlayViewportSource* mPlayOrNull = nullptr;
    bool mIsPlaying = false;
    bool mIsPlayingElsewhere = false;
    QString mPlayUnavailableReason;
    WorldDocument PreviewWorld_;
    QLabel* Status_;
    QLabel* Trace_;
    QPushButton* Open_;
    QPushButton* Refresh_;
    QPushButton* Stop_;
    QPushButton* Tint_;
    QToolButton* mViewportMenuButton;
    QComboBox* Cameras_;
    QSlider* Yaw_;
    QSlider* Pitch_;
    QDoubleSpinBox* Zoom_;
    QCheckBox* Reference_;
};
} // namespace ludus::editor
