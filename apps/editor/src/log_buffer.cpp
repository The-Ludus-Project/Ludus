#include "internal/log_buffer.h"

namespace ludus::editor
{

void LogBuffer::Append(OutputStream /*stream*/, const QString& text)
{
    if (text.isEmpty())
    {
        return;
    }
    // A single chunk larger than the entire byte budget is itself truncated so
    // one giant line (no newline) cannot blow the bound. We keep the tail, which
    // is usually the most recent/relevant, and count the dropped prefix.
    QString chunk = text;
    const usize chunkBytes = static_cast<usize>(chunk.toUtf8().size());
    if (chunkBytes > MaxBytes)
    {
        // Keep roughly the last MaxBytes worth of characters. toUtf8 size is an
        // upper bound on characters, so trimming by characters is safe.
        const int keep = static_cast<int>(MaxBytes);
        DroppedBytes_ += static_cast<uint64>(chunkBytes) - MaxBytes;
        chunk = chunk.right(keep);
    }

    Blocks_.append(chunk);
    RetainedBytes_ += static_cast<usize>(chunk.toUtf8().size());
    EnforceBounds();
}

void LogBuffer::EnforceBounds()
{
    while (Blocks_.isEmpty() == false && (RetainedBytes_ > MaxBytes || static_cast<usize>(Blocks_.size()) > MaxBlocks))
    {
        const QString oldest = Blocks_.takeFirst();
        const usize bytes = static_cast<usize>(oldest.toUtf8().size());
        RetainedBytes_ = RetainedBytes_ >= bytes ? RetainedBytes_ - bytes : 0;
        DroppedBlocks_ += 1;
        DroppedBytes_ += static_cast<uint64>(bytes);
    }
}

void LogBuffer::Clear()
{
    Blocks_.clear();
    RetainedBytes_ = 0;
    DroppedBlocks_ = 0;
    DroppedBytes_ = 0;
}

QString LogBuffer::Text() const
{
    QString out;
    if (DroppedBlocks_ != 0 || DroppedBytes_ != 0)
    {
        out += QStringLiteral("[... %1 earlier output block(s), %2 byte(s) omitted ...]\n")
                   .arg(DroppedBlocks_)
                   .arg(DroppedBytes_);
    }
    out += Blocks_.join(QString());
    return out;
}

} // namespace ludus::editor
