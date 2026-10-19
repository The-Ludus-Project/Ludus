#pragma once

#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace ludus::editor
{
/// Native launch arguments; each path remains one argument, with no shell expansion.
struct ExternalCodeEditorCommand final
{
    QString Program;
    QStringList Arguments;
};

/// Validate an installed application and existing source file; preserve output on failure.
[[nodiscard]] bool
MakeExternalCodeEditorCommand(const QString& application, const QString& file, ExternalCodeEditorCommand& outCommand);

/// Native application selection/launch only; owns no source bytes or document state.
class ExternalCodeEditor final
{
public:
    /// Choose an application using the platform file picker; cancel preserves preferences.
    [[nodiscard]] bool Configure(QWidget* ownerOrNull);
    /// Open a saved file, choosing an application on first use. No source writes.
    /// Reports cancellation/launch failure through outError. Unavailable in the browser.
    [[nodiscard]] bool Open(QWidget* ownerOrNull, const QString& file, QString& outError);
};
} // namespace ludus::editor
