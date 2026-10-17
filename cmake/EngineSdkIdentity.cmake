# Derives the installable SDK identity and redistributable dependency inventory
# from the ACTUAL build inputs (requirements P02/P03/P11). Nothing here is
# inferred from a version string: the triple, runtime ABI tag and distro baseline
# come from the configured compiler and host, and the component/dependency lists
# are accumulated by the module CMakeLists as they are added.
#
# Produced cache/normal variables (consumed by LudusSdkManifest.json.in):
#   LUDUS_TARGET_TRIPLE, LUDUS_CXX_RUNTIME_ABI, LUDUS_DISTRO_BASELINE
#   LUDUS_SDK_FEATURES_JSON, LUDUS_SDK_COMPONENTS_JSON,
#   LUDUS_SDK_DEPENDENCIES_JSON, LUDUS_SDK_SYSTEM_PREREQS_JSON
#
# Modules register themselves with ludus_sdk_register_component() /
# ludus_sdk_register_dependency() / ludus_sdk_register_feature() /
# ludus_sdk_register_system_prerequisite().

include_guard(GLOBAL)

# --- Target triple -----------------------------------------------------------
if(EMSCRIPTEN)
    set(LUDUS_TARGET_TRIPLE "wasm32-unknown-emscripten")
else()
    # Normalize the architecture to a canonical token.
    set(_ludus_arch "${CMAKE_SYSTEM_PROCESSOR}")
    if(_ludus_arch STREQUAL "" OR _ludus_arch STREQUAL "AMD64")
        set(_ludus_arch "x86_64")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(LUDUS_TARGET_TRIPLE "${_ludus_arch}-apple-darwin")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(LUDUS_TARGET_TRIPLE "${_ludus_arch}-pc-windows-msvc")
    else()
        string(TOLOWER "${CMAKE_SYSTEM_NAME}" _ludus_os)
        # The GNU/libstdc++ ABI tag participates in the triple so a libc++ build is
        # never treated as compatible with a libstdc++ build of the same compiler.
        set(LUDUS_TARGET_TRIPLE "${_ludus_arch}-${_ludus_os}-gnu")
    endif()
endif()

# --- C++ runtime / ABI tag ---------------------------------------------------
# Record which standard library and ABI the libraries were built against. We do
# not claim portability from the compiler major alone; this is the actual
# standard-library family observed at configure time.
set(LUDUS_CXX_RUNTIME_ABI "unknown")
if(EMSCRIPTEN)
    set(LUDUS_CXX_RUNTIME_ABI "emscripten-libc++")
else()
    # Clang on Linux defaults to libstdc++ unless -stdlib=libc++ is in the flags.
    set(_ludus_cxx_flags "${CMAKE_CXX_FLAGS}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        # Clang targeting MSVC uses the Microsoft STL and dynamic CRT.
        set(LUDUS_CXX_RUNTIME_ABI "msvc-stl")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR _ludus_cxx_flags MATCHES "libc\\+\\+")
        set(LUDUS_CXX_RUNTIME_ABI "libc++")
    else()
        set(LUDUS_CXX_RUNTIME_ABI "libstdc++-cxx11")
    endif()
endif()

# --- Distro / platform baseline ----------------------------------------------
set(LUDUS_DISTRO_BASELINE "unknown")
if(EMSCRIPTEN)
    set(LUDUS_DISTRO_BASELINE "emscripten")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(LUDUS_DISTRO_BASELINE "macos-${CMAKE_OSX_DEPLOYMENT_TARGET}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(LUDUS_DISTRO_BASELINE "windows-${CMAKE_SYSTEM_VERSION}")
elseif(EXISTS "/etc/os-release")
    file(STRINGS "/etc/os-release" _ludus_os_release)
    set(_ludus_distro_id "")
    set(_ludus_distro_ver "")
    foreach(_line IN LISTS _ludus_os_release)
        if(_line MATCHES "^ID=\"?([^\"]+)\"?")
            set(_ludus_distro_id "${CMAKE_MATCH_1}")
        elseif(_line MATCHES "^VERSION_ID=\"?([^\"]+)\"?")
            set(_ludus_distro_ver "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(_ludus_distro_id)
        set(LUDUS_DISTRO_BASELINE "${_ludus_distro_id}-${_ludus_distro_ver}")
    endif()
endif()

# --- Accumulators (global properties, serialized to JSON at install config) --
# Note: GLOBAL properties are appended directly; define_property is intentionally
# not used so this module stays usable from `cmake -P` test harnesses as well as
# a real configure.
function(ludus_sdk_register_component name)
    set_property(GLOBAL APPEND PROPERTY LUDUS_SDK_COMPONENTS "${name}")
endfunction()

function(ludus_sdk_register_feature name)
    set_property(GLOBAL APPEND PROPERTY LUDUS_SDK_FEATURES "${name}")
endfunction()

# A redistributable dependency entry: "name|version|licenses|kind".
# kind is bundled (static lib shipped in the SDK) or system (must be present).
function(ludus_sdk_register_dependency name version licenses kind)
    set_property(GLOBAL APPEND PROPERTY LUDUS_SDK_DEPENDENCIES "${name}|${version}|${licenses}|${kind}")
endfunction()

function(ludus_sdk_register_system_prerequisite name)
    set_property(GLOBAL APPEND PROPERTY LUDUS_SDK_SYSTEM_PREREQS "${name}")
endfunction()

# Serialize the accumulated inventory into the @VAR@ placeholders used by the
# manifest template. Call AFTER all add_subdirectory() module registrations.
function(ludus_sdk_finalize_identity)
    get_property(_components GLOBAL PROPERTY LUDUS_SDK_COMPONENTS)
    get_property(_features GLOBAL PROPERTY LUDUS_SDK_FEATURES)
    get_property(_deps GLOBAL PROPERTY LUDUS_SDK_DEPENDENCIES)
    get_property(_prereqs GLOBAL PROPERTY LUDUS_SDK_SYSTEM_PREREQS)

    if(_components)
        list(SORT _components)
        list(REMOVE_DUPLICATES _components)
    endif()
    if(_features)
        list(SORT _features)
        list(REMOVE_DUPLICATES _features)
    endif()
    if(_prereqs)
        list(SORT _prereqs)
        list(REMOVE_DUPLICATES _prereqs)
    endif()

    _ludus_json_string_array("${_components}" _components_json)
    _ludus_json_string_array("${_features}" _features_json)
    _ludus_json_string_array("${_prereqs}" _prereqs_json)

    set(_dep_objs "")
    foreach(_entry IN LISTS _deps)
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _dname)
        list(GET _parts 1 _dver)
        list(GET _parts 2 _dlic)
        list(GET _parts 3 _dkind)
        set(_obj "{\"name\": \"${_dname}\", \"version\": \"${_dver}\", \"licenses\": \"${_dlic}\", \"kind\": \"${_dkind}\"}")
        if(_dep_objs STREQUAL "")
            set(_dep_objs "${_obj}")
        else()
            set(_dep_objs "${_dep_objs}, ${_obj}")
        endif()
    endforeach()

    set(LUDUS_SDK_COMPONENTS_JSON "${_components_json}" PARENT_SCOPE)
    set(LUDUS_SDK_FEATURES_JSON "${_features_json}" PARENT_SCOPE)
    set(LUDUS_SDK_SYSTEM_PREREQS_JSON "${_prereqs_json}" PARENT_SCOPE)
    set(LUDUS_SDK_DEPENDENCIES_JSON "${_dep_objs}" PARENT_SCOPE)
endfunction()

function(_ludus_json_string_array items out)
    set(_result "")
    foreach(_item IN LISTS items)
        if(_result STREQUAL "")
            set(_result "\"${_item}\"")
        else()
            set(_result "${_result}, \"${_item}\"")
        endif()
    endforeach()
    set(${out} "${_result}" PARENT_SCOPE)
endfunction()
