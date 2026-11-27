#pragma once

#include <QWidget>

namespace ludus::editor
{
/// Tab in Reference Capture that compares three complete, matched raw captures on a planar receiver.
class ReferencePowerView final : public QWidget
{
public:
    explicit ReferencePowerView(QWidget* parentOrNull = nullptr);
};
} // namespace ludus::editor
