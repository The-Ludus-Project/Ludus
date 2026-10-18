// libFuzzer entry point for the glTF/GLB and OBJ front-ends (without file
// access), the OpenEXR/Radiance/JPEG decoders and tangent generation: any input
// must be accepted or rejected without crashing, leaking or reading out of bounds.

#include "importers/gltf/gltf_reader.h"
#include "importers/image/exr_reader.h"
#include "importers/image/image_reader.h"
#include "importers/obj/obj_reader.h"
#include "importers/tangent_generation.h"

extern "C" int LLVMFuzzerTestOneInput(const ludus::foundation::core::uint8* data, ludus::foundation::core::usize size)
{
    using namespace ludus::content::scene_cook::importers;
    const std::span<const ludus::foundation::core::uint8> input(data, size);
    MemorySourceResolver resolver({});
    SceneIr gltfScene;
    DiagnosticLog gltfLog;
    if (gltf::ReadScene(input, "fuzz.gltf", resolver, gltfScene, gltfLog) == ludus::content::Status::Ok)
    {
        for (IrMesh& mesh : gltfScene.Meshes)
        {
            (void)GenerateTangents(mesh);
        }
    }
    MemoryByteSource source(input);
    SceneIr wavefrontScene;
    DiagnosticLog wavefrontLog;
    (void)obj::ReadScene(source, "fuzz.obj", resolver, wavefrontScene, wavefrontLog);
    std::string_view message;
    DecodedLightingImage lighting;
    (void)DecodeExr(input, lighting, message);
    (void)DecodeLightingImage("fuzz.hdr", {data, size}, lighting, message);
    DecodedImage image;
    (void)DecodeImage("fuzz.jpg", input, image, message);
    return 0;
}
