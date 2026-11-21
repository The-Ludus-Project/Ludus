// Mobile M0 probe: a JNI translation unit for the Android shell boundary.
//
// It compiles against the NDK's jni.h and exports one native method and
// JNI_OnLoad, with exceptions disabled: no JNI call here can throw into C++.
// See docs/development/mobile-support-plan.md#acceptance-probes.
#include <jni.h>

#include <ludus/foundation/base/types.h>

using namespace ludus::foundation::core;

namespace
{
constexpr jint PROBE_RESULT_OK = 0;
constexpr jint PROBE_RESULT_ABI_MISMATCH = 1;
} // namespace

/// Reports whether native pointers and 64-bit integers match the Java side.
extern "C" JNIEXPORT jint JNICALL Java_org_ludus_probe_NativeProbe_nativeAbiProbe(JNIEnv* environment, jobject thiz)
{
    static_cast<void>(environment);
    static_cast<void>(thiz);
    const bool isAbiMatching = sizeof(usize) == sizeof(void*) && sizeof(jlong) == sizeof(int64);
    return isAbiMatching == true ? PROBE_RESULT_OK : PROBE_RESULT_ABI_MISMATCH;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* machine, void* reserved)
{
    static_cast<void>(machine);
    static_cast<void>(reserved);
    return JNI_VERSION_1_6;
}
