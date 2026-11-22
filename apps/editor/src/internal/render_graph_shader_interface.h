#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QMap>
#include <QString>

class QPlainTextEdit;
class QWidget;

namespace ludus::editor
{
/// Describe every program using sourceFile in the draft build manifest, including
/// graph resource ports supplied by the shader context.
/// Ray declarations come from the compiler's bundled shader sources. sources supplies
/// draft payload files by package-relative path; missing payloads are reported explicitly.
/// Invalid manifests and unregistered sources produce an explicit unavailable message.
[[nodiscard]] QString DescribeShaderInterface(const QByteArray& manifest,
                                              const QString& sourceFile,
                                              const QMap<QString, QByteArray>& sources = {},
                                              const QJsonArray& resources = {});
/// Create the searchable, read-only code reference around the caller's text view.
/// Qt parents the view and returned widget to parent; GUI thread only.
[[nodiscard]] QWidget* CreateShaderInterfaceView(QPlainTextEdit* textView, QWidget* parent);
/// Update declarations only when changed; preserve position while refreshing the same source.
void UpdateShaderInterfaceView(QPlainTextEdit& inoutTextView, const QString& source, const QString& declarations);
} // namespace ludus::editor
