#include "internal/captured_image_view.h"

#include <ludus/foundation/base/types.h>

#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMenu>
#include <QPixmap>
#include <QResizeEvent>
#include <QSaveFile>
#include <QStandardPaths>

namespace ludus::editor
{
namespace
{
constexpr foundation::int32 MINIMUM_IMAGE_EXTENT = 160;
} // namespace

bool WritePngAtomically(const QString& path, const QImage& image, QString& outError)
{
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) == false || image.save(&file, "PNG") == false || file.commit() == false)
    {
        outError = QStringLiteral("Could not write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

CapturedImageView::CapturedImageView(const QString& fileStem, QWidget* parentOrNull)
    : QLabel(parentOrNull), mFileStem(fileStem),
      mSaveDirectory(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)),
      mCopyAction(new QAction(QStringLiteral("Copy image"), this)),
      mSaveAction(new QAction(QStringLiteral("Save image as…"), this))
{
    setAlignment(Qt::AlignCenter);
    setMinimumSize(MINIMUM_IMAGE_EXTENT, MINIMUM_IMAGE_EXTENT);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    mCopyAction->setObjectName(QStringLiteral("capturedImageCopy"));
    mCopyAction->setEnabled(false);
    mSaveAction->setObjectName(QStringLiteral("capturedImageSave"));
    mSaveAction->setEnabled(false);
    connect(mCopyAction, &QAction::triggered, this, [this]() { static_cast<void>(CopyImage()); });
    connect(mSaveAction, &QAction::triggered, this, &CapturedImageView::saveWithDialog);
}

void CapturedImageView::SetImage(const QImage& image)
{
    // Progressive refreshes hand the same image back; rescaling it would only cost time.
    if (image.cacheKey() == mImage.cacheKey())
    {
        return;
    }
    mImage = image;
    mCopyAction->setEnabled(mImage.isNull() == false);
    mSaveAction->setEnabled(mImage.isNull() == false);
    showScaled();
}

bool CapturedImageView::CopyImage() const
{
    if (mImage.isNull() == true)
    {
        return false;
    }
    QGuiApplication::clipboard()->setImage(mImage);
    return true;
}

bool CapturedImageView::SaveImage(const QString& path, QString& outError) const
{
    if (mImage.isNull() == true)
    {
        outError = QStringLiteral("There is no image to save yet.");
        return false;
    }
    return WritePngAtomically(path, mImage, outError);
}

void CapturedImageView::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("capturedImageMenu"));
    menu.addAction(mCopyAction);
    menu.addAction(mSaveAction);
    menu.exec(event->globalPos());
    event->accept();
}

void CapturedImageView::resizeEvent(QResizeEvent* event)
{
    QLabel::resizeEvent(event);
    showScaled();
}

void CapturedImageView::saveWithDialog()
{
    QString path = QFileDialog::getSaveFileName(this,
                                                QStringLiteral("Save image"),
                                                QDir(mSaveDirectory).filePath(mFileStem + QStringLiteral(".png")),
                                                QStringLiteral("PNG image (*.png)"));
    if (path.isEmpty() == true)
    {
        return;
    }
    // The file is always PNG, so its name says so even when the dialog returns another or no suffix.
    if (QFileInfo(path).suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) != 0)
    {
        path += QStringLiteral(".png");
    }
    QString error;
    if (SaveImage(path, error) == false)
    {
        Q_EMIT SaveReported(error);
        return;
    }
    mSaveDirectory = QFileInfo(path).absolutePath();
    Q_EMIT SaveReported(QStringLiteral("Saved %1 (%2 × %3).").arg(path).arg(mImage.width()).arg(mImage.height()));
}

void CapturedImageView::showScaled()
{
    if (mImage.isNull() == true)
    {
        clear();
        return;
    }
    setPixmap(QPixmap::fromImage(mImage).scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
} // namespace ludus::editor
