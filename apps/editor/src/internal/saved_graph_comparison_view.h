#pragma once

class QString;
class QWidget;

namespace ludus::editor
{
/// Read a Compare graphs manifest and create a modeless overview of its saved images and metrics.
/// The caller owns the returned window through parentOrNull or by closing it. Failure reports outError.
[[nodiscard]] QWidget*
CreateSavedGraphComparisonView(const QString& manifestPath, QWidget* parentOrNull, QString& outError);

/// Choose a saved Compare graphs manifest and show its overview. The optional directory starts the picker.
void OpenSavedGraphComparison(QWidget& inoutParent, const QString& directory);
} // namespace ludus::editor
