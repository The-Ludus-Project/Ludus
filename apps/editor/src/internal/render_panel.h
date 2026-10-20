#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include "internal/render_preview.h"

#include <QWidget>

QT_BEGIN_NAMESPACE
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QSlider;
class QDoubleSpinBox;
QT_END_NAMESPACE
namespace ludus::editor
{
class RenderLoadJob;
class PreviewViewport;
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
    void PreviewDraft(const WorldDocument& draft);
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
Q_SIGNALS:
    void Starting();
    void Loaded(bool success);
    void TintRequested(const QString& id, const QJsonObject& binding);

private:
    void Publish(const RenderScene& scene, bool launch);
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
    bool Again_ = false;
    bool QueuedLaunch_ = false;
    /// Whether the next published scene still selects its default camera (set when a World opens).
    bool mShouldUseDefaultCamera = false;
    RenderPreview Preview_;
    PreviewViewport* Viewport_;
    WorldDocument PreviewWorld_;
    QLabel* Status_;
    QLabel* Trace_;
    QPushButton* Open_;
    QPushButton* Refresh_;
    QPushButton* Stop_;
    QPushButton* Tint_;
    QComboBox* Cameras_;
    QSlider* Yaw_;
    QSlider* Pitch_;
    QDoubleSpinBox* Zoom_;
    QCheckBox* Reference_;
};
} // namespace ludus::editor
