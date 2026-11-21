#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum
{
    MAX_READS = 4096,
    ENTRY_CAPACITY = 256
};
struct Read
{
    uint64_t thread;
    uint64_t start;
    uint64_t end;
    off_t offset;
    size_t request;
    ssize_t received;
    char entry[ENTRY_CAPACITY];
};
static struct Read reads[MAX_READS];
static _Atomic size_t count;
static atomic_bool published[MAX_READS];

// These diagnostic passes run separately from replay timing. Filter exact
// pack paths or the flat fixture root. Keep formatting out of the read path.
static const char* GetEntryOrNull(int descriptor, char path[PATH_MAX])
{
    const char* packOrNull = getenv("FS_R2_PACK_PATH");
    const char* rootOrNull = getenv("FS_R2_CAPTURE_ROOT");
    if ((packOrNull == NULL && rootOrNull == NULL) || fcntl(descriptor, F_GETPATH, path) != 0)
    {
        return NULL;
    }
    if (packOrNull != NULL && strcmp(path, packOrNull) == 0)
    {
        const char* separatorOrNull = strrchr(path, '/');
        return separatorOrNull == NULL ? path : separatorOrNull + 1;
    }
    if (rootOrNull == NULL)
    {
        return NULL;
    }
    const size_t prefix = strlen(rootOrNull);
    if (prefix == 0 || prefix >= PATH_MAX - 1 || strncmp(path, rootOrNull, prefix) != 0 || path[prefix] != '/')
    {
        return NULL;
    }
    const char* entry = path + prefix + 1;
    return strchr(entry, '/') == NULL ? entry : NULL;
}

static ssize_t CaptureRead(int descriptor, void* destination, size_t length, off_t offset)
{
    const int entryError = errno;
    char path[PATH_MAX];
    const char* entryOrNull = GetEntryOrNull(descriptor, path);
    if (entryOrNull == NULL || strlen(entryOrNull) >= ENTRY_CAPACITY)
    {
        errno = entryError;
        return pread(descriptor, destination, length, offset);
    }
    uint64_t thread = 0;
    (void)pthread_threadid_np(NULL, &thread);
    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    errno = entryError;
    const ssize_t received = pread(descriptor, destination, length, offset);
    const int readError = errno;
    const uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const size_t index = atomic_fetch_add_explicit(&count, 1, memory_order_relaxed);
    if (index < MAX_READS)
    {
        struct Read* event = &reads[index];
        *event = (struct Read)
        {
            .thread = thread,
            .start = start,
            .end = end,
            .offset = offset,
            .request = length,
            .received = received,
        };
        memcpy(event->entry, entryOrNull, strlen(entryOrNull) + 1);
        atomic_store_explicit(&published[index], true, memory_order_release);
    }
    errno = readError;
    return received;
}

__attribute__((destructor)) static void DumpReads(void)
{
    const size_t total = atomic_load_explicit(&count, memory_order_relaxed);
    fprintf(stderr, "read_count %zu\n", total);
    for (size_t index = 0; index < total && index < MAX_READS; ++index)
    {
        if (atomic_load_explicit(&published[index], memory_order_acquire) == false)
        {
            continue;
        }
        const struct Read* event = &reads[index];
        fprintf(stderr,
                "pread %llu %llu %llu %lld %zu %zd %s\n",
                (unsigned long long)event->thread,
                (unsigned long long)event->start,
                (unsigned long long)event->end,
                (long long)event->offset,
                event->request,
                event->received,
                event->entry);
    }
}

__attribute__((used, section("__DATA,__interpose"))) static const struct
{
    const void* replacement;
    const void* original;
} interpose = {(const void*)CaptureRead, (const void*)pread};
