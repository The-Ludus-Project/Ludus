/* Feature selection shared by the stb_image implementation and its callers.
 * Decoders: PNG, baseline/progressive JPEG and Radiance RGBE (.hdr). Every heap
 * allocation goes through the bounded allocator in stb_image_allocator.c. */
#ifndef LUDUS_STB_IMAGE_CONFIG_H
#define LUDUS_STB_IMAGE_CONFIG_H
#include <stddef.h>
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_HDR
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 16384
/* Live decoder bytes allowed per thread (output plus scratch); a request beyond it
 * fails like an exhausted heap, so stbi_failure_reason() reports "outofmem". */
#define LUDUS_STBI_MAX_LIVE_BYTES ((size_t)2048 * 1024 * 1024)
void* ludus_stbi_malloc(size_t size);
void* ludus_stbi_realloc(void* pointer, size_t size);
void ludus_stbi_free(void* pointer);
#define STBI_MALLOC(size) ludus_stbi_malloc(size)
#define STBI_REALLOC(pointer, size) ludus_stbi_realloc(pointer, size)
#define STBI_FREE(pointer) ludus_stbi_free(pointer)
#endif
