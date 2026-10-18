# stb_image

Vendored [stb_image](https://github.com/nothings/stb) 2.30 by Sean Barrett,
pinned in `SOURCE-LOCK.json`. Ludus::SceneCook uses it privately to decode PNG,
JPEG and Radiance HDR images (and its zlib inflater for OpenEXR ZIP chunks) for
the [best-effort scene importers](../../docs/development/best-effort-scene-import.md).
`stb_image_config.h` selects the decoders; `stb_image_allocator.c` bounds every
decoder allocation so oversized images fail as out-of-memory.
