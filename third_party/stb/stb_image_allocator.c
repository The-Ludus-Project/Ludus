/* Bounded allocator for the vendored stb_image decoders.
 *
 * Each block carries a header with its payload size so realloc and free can keep
 * a per-thread count of live decoder bytes. Requests that overflow size_t or push
 * the count past LUDUS_STBI_MAX_LIVE_BYTES return NULL, which stb_image reports
 * as "outofmem"; callers map that to an explicit out-of-memory status. Blocks are
 * released on the thread that allocated them (stb decodes synchronously). */
#include "stb_image_config.h"

#include <stdlib.h>

/* max_align_t-sized header keeps the payload suitably aligned for any type. */
typedef union LudusStbiHeader
{
    size_t Size;
    max_align_t Alignment;
} LudusStbiHeader;

static _Thread_local size_t ludus_stbi_live_bytes = 0;

static int ludus_stbi_admit(size_t size)
{
    return size <= LUDUS_STBI_MAX_LIVE_BYTES - sizeof(LudusStbiHeader) &&
           ludus_stbi_live_bytes <= LUDUS_STBI_MAX_LIVE_BYTES - size;
}

void* ludus_stbi_malloc(size_t size)
{
    LudusStbiHeader* header;
    if (!ludus_stbi_admit(size))
    {
        return NULL;
    }
    header = (LudusStbiHeader*)malloc(sizeof(LudusStbiHeader) + size);
    if (header == NULL)
    {
        return NULL;
    }
    header->Size = size;
    ludus_stbi_live_bytes += size;
    return header + 1;
}

void* ludus_stbi_realloc(void* pointer, size_t size)
{
    LudusStbiHeader* header;
    LudusStbiHeader* grown;
    size_t previous;
    if (pointer == NULL)
    {
        return ludus_stbi_malloc(size);
    }
    header = (LudusStbiHeader*)pointer - 1;
    previous = header->Size;
    ludus_stbi_live_bytes -= previous;
    if (!ludus_stbi_admit(size))
    {
        ludus_stbi_live_bytes += previous;
        return NULL;
    }
    grown = (LudusStbiHeader*)realloc(header, sizeof(LudusStbiHeader) + size);
    if (grown == NULL)
    {
        ludus_stbi_live_bytes += previous;
        return NULL;
    }
    grown->Size = size;
    ludus_stbi_live_bytes += size;
    return grown + 1;
}

void ludus_stbi_free(void* pointer)
{
    LudusStbiHeader* header;
    if (pointer == NULL)
    {
        return;
    }
    header = (LudusStbiHeader*)pointer - 1;
    ludus_stbi_live_bytes -= header->Size;
    free(header);
}
