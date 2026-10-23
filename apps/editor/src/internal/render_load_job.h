#pragma once

#include <ludus/foundation/base/types.h>

#include "internal/render_scene.h"
#include "internal/world_document.h"

#include <QObject>
#include <QThread>

namespace ludus::editor
{
// One bounded CPU extraction on an owned worker. Results are read only after
// Finished on the GUI thread. No device/widget or live document crosses threads.
class RenderLoadJob final : public QObject
{
    Q_OBJECT
public:
    RenderLoadJob(const QString& root,
                  const WorldDocument& world,
                  foundation::uint64 generation,
                  bool launch,
                  QObject* parent);
    ~RenderLoadJob() override;
    [[nodiscard]] foundation::uint64 Generation() const noexcept
    {
        return Generation_;
    }
    [[nodiscard]] bool Launch() const noexcept
    {
        return Launch_;
    }
    [[nodiscard]] bool Success() const noexcept
    {
        return Success_;
    }
    [[nodiscard]] const RenderScene& Scene() const noexcept
    {
        return Scene_;
    }
    [[nodiscard]] QString Error() const
    {
        return Error_;
    }
Q_SIGNALS:
    void Finished();

private:
    QThread Thread_;
    QString Root_;
    WorldDocument World_;
    RenderScene Scene_;
    QString Error_;
    foundation::uint64 Generation_;
    bool Launch_;
    bool Success_ = false;
};
} // namespace ludus::editor
