// Optional FS-R2 measurement shim. Run separately from startup timing passes.
// Filter to one exact pack path so unrelated game and OS reads stay out of the capture.
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static ssize_t CapturePackRead(int descriptor, void* destination, size_t length, off_t offset)
{
    const int entryError = errno;
    char path[PATH_MAX];
    const char* targetOrNull = getenv("FS_R2_PACK_PATH");
    const int isPack =
        targetOrNull != NULL && fcntl(descriptor, F_GETPATH, path) == 0 && strcmp(path, targetOrNull) == 0;
    errno = entryError;
    const uint64_t start = isPack ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
    const ssize_t received = pread(descriptor, destination, length, offset);
    const int readError = errno;
    if (isPack)
    {
        const uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        fprintf(stderr,
                "pack_pread offset=%lld requested=%zu received=%zd elapsed_ns=%llu\n",
                (long long)offset,
                length,
                received,
                (unsigned long long)(end - start));
    }
    errno = readError;
    return received;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct
{
    const void* replacement;
    const void* original;
} readInterpose = {(const void*)CapturePackRead, (const void*)pread};
