#pragma once

#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include <QWidget>

namespace ludus::editor
{
/// Shared manifest-setting controls for Properties and canvas dialogs. GUI thread only.
/// Emits draft edit requests; the workspace's document owns undo, save and rebuild.
class RenderGraphSettingsEditor final : public QWidget
{
    Q_OBJECT
public:
    /// Displays bounded typed constants. Malformed declarations show a repair message.
    explicit RenderGraphSettingsEditor(const QJsonArray& settings, QWidget* parentOrNull = nullptr);
Q_SIGNALS:
    /// Requests one setting value change; no files are written by this widget.
    void SettingEdited(const QString& identity, const QJsonValue& value);
};
} // namespace ludus::editor
