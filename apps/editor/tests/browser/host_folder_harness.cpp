// Test-only WebAssembly harness for apps/editor/browser/host_folder.js. It drives
// the host-folder mount through the same POSIX calls Qt and Ludus make (open,
// read, write, fsync, rename, unlink), without Qt. host_folder_fs.mjs supplies
// mock folder handles and checks the folder on disk. Built and run by
// `python scripts/build-web-editor --test-host-folder`.

#include <ludus/foundation/base/types.h>

#include <emscripten/emscripten.h>
#include <emscripten/val.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using ludus::foundation::float64;
using ludus::foundation::int32;
using ludus::foundation::int64;
using ludus::foundation::isize;
using ludus::foundation::usize;

extern "C" void LudusHostPickFolder(emscripten::EM_VAL target, int32 intent, const char* options);
extern "C" int32
LudusHostMountSelection(int32 selection, const char* mountPoint, float64 budgetBytes, const char* options);

namespace
{
constexpr const char* OPTIONS = R"({"skip":["out",".git"],"maximumEntries":64})";

int32 Failure() noexcept
{
    return -errno;
}
} // namespace

extern "C" {
/// Start a pick; the test listens on globalThis.harnessTarget.
EMSCRIPTEN_KEEPALIVE void HarnessPick(int32 intent)
{
    emscripten::val target = emscripten::val::global("EventTarget").new_();
    emscripten::val::global().set("harnessTarget", target);
    LudusHostPickFolder(target.as_handle(), intent, OPTIONS);
}

EMSCRIPTEN_KEEPALIVE int32 HarnessMount(int32 selection, const char* mountPoint, float64 budgetBytes)
{
    return LudusHostMountSelection(selection, mountPoint, budgetBytes, OPTIONS);
}

/// Read a whole file into `destination`; returns the byte count or -errno.
EMSCRIPTEN_KEEPALIVE int32 HarnessRead(const char* path, char* destination, int32 capacity)
{
    const int32 descriptor = open(path, O_RDONLY);
    if (descriptor < 0)
    {
        return Failure();
    }
    int32 total = 0;
    while (total < capacity)
    {
        const isize count = read(descriptor, destination + total, static_cast<usize>(capacity - total));
        if (count < 0)
        {
            const int32 error = Failure();
            close(descriptor);
            return error;
        }
        if (count == 0)
        {
            break;
        }
        total += static_cast<int32>(count);
    }
    close(descriptor);
    return total;
}

EMSCRIPTEN_KEEPALIVE int64 HarnessSize(const char* path)
{
    struct stat status = {};
    return stat(path, &status) == 0 ? static_cast<int64>(status.st_size) : Failure();
}

/// Write with plain open/write/close (QFile without QSaveFile).
EMSCRIPTEN_KEEPALIVE int32 HarnessWrite(const char* path, const char* bytes, int32 size)
{
    const int32 descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0)
    {
        return Failure();
    }
    const isize written = write(descriptor, bytes, static_cast<usize>(size));
    const int32 closed = close(descriptor);
    return written == size && closed == 0 ? 0 : Failure();
}

/// Save like QSaveFile and Ludus persistence: temporary file, fsync, rename.
EMSCRIPTEN_KEEPALIVE int32 HarnessSave(const char* path, const char* bytes, int32 size)
{
    constexpr usize MAXIMUM_PATH_BYTES = 1024;
    char temporary[MAXIMUM_PATH_BYTES] = {};
    const usize length = std::strlen(path);
    if (length + sizeof(".save") > MAXIMUM_PATH_BYTES)
    {
        return -ENAMETOOLONG;
    }
    std::memcpy(temporary, path, length);
    std::memcpy(temporary + length, ".save", sizeof(".save"));
    const int32 descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (descriptor < 0)
    {
        return Failure();
    }
    const isize written = write(descriptor, bytes, static_cast<usize>(size));
    const int32 synced = fsync(descriptor);
    close(descriptor);
    if (written != size || synced != 0)
    {
        unlink(temporary);
        return -EIO;
    }
    return rename(temporary, path) == 0 ? 0 : Failure();
}

/// Retry pending folder writes through the existing file, without rewriting it.
EMSCRIPTEN_KEEPALIVE int32 HarnessFlush(const char* path)
{
    const int32 descriptor = open(path, O_RDONLY);
    if (descriptor < 0)
    {
        return Failure();
    }
    const int32 result = fsync(descriptor) == 0 ? 0 : Failure();
    close(descriptor);
    return result;
}

EMSCRIPTEN_KEEPALIVE int32 HarnessRename(const char* source, const char* destination)
{
    return rename(source, destination) == 0 ? 0 : Failure();
}

EMSCRIPTEN_KEEPALIVE int32 HarnessUnlink(const char* path)
{
    return unlink(path) == 0 ? 0 : Failure();
}

EMSCRIPTEN_KEEPALIVE int32 HarnessMakeDirectory(const char* path)
{
    return mkdir(path, 0755) == 0 ? 0 : Failure();
}
}
