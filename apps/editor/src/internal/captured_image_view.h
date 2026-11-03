#pragma once

#include <ludus/foundation/base/compiler.h>

#include <QImage>
#include <QLabel>
#include <QString>

class QAction;
class QContextMenuEvent;
class QResizeEvent;

namespace ludus::editor
{
/// Write image as PNG to path through a QSaveFile, so a failed write leaves any existing file unchanged.
/// False reports the path and the file error in outError.
[[nodiscard]] bool WritePngAtomically(const QString& path, const QImage& image, QString& outError);

/// Label that shows one captured image scaled to fit while keeping the full-resolution image for the clipboard
/// and for files. Right-click offers Copy image and Save image as…; both are disabled until an image is set.
/// UI-thread only.
class CapturedImageView final : public QLabel
{
    Q_OBJECT
public:
    /// fileStem names the default file of Save image as…, such as "reference" for "reference.png".
    explicit CapturedImageView(const QString& fileStem, QWidget* parentOrNull = nullptr);

    /// Show image scaled to the view; a null image clears the view and disables its actions.
    void SetImage(const QImage& image);
    /// The full-resolution image as set; null when none is shown.
    [[nodiscard]] LUDUS_INLINE const QImage& GetImage() const noexcept
    {
        return mImage;
    }
    /// Put the full-resolution image on the system clipboard. False when no image is shown.
    bool CopyImage() const;
    /// Write the full-resolution image to path as PNG. False when no image is shown or the write fails, with
    /// the reason in outError; an existing file is then unchanged.
    [[nodiscard]] bool SaveImage(const QString& path, QString& outError) const;

Q_SIGNALS:
    /// Save image as… finished; message names the written file or why it was not written.
    void SaveReported(const QString& message);

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void saveWithDialog();
    void showScaled();

    QImage mImage;
    QString mFileStem;
    /// Folder Save image as… opens in: the Pictures folder, then the folder of the last saved image.
    QString mSaveDirectory;
    QAction* const mCopyAction;
    QAction* const mSaveAction;
};
} // namespace ludus::editor
