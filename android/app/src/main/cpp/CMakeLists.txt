cmake_minimum_required(VERSION 3.22.1)

project("crossscale")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NATIVE_LIB_DIR ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI})
set(THIRD_PARTY_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/third_party/include)

function(import_static_library target_name library_path)
    if(NOT EXISTS "${library_path}")
        message(FATAL_ERROR "Missing native dependency: ${library_path}\nRun: ./scripts/sync_native_deps.sh")
    endif()
    add_library(${target_name} STATIC IMPORTED)
    set_target_properties(${target_name} PROPERTIES IMPORTED_LOCATION "${library_path}")
endfunction()

function(import_optional_static_library target_name library_path)
    if(EXISTS "${library_path}")
        add_library(${target_name} STATIC IMPORTED)
        set_target_properties(${target_name} PROPERTIES IMPORTED_LOCATION "${library_path}")
    endif()
endfunction()

function(import_shared_library target_name library_path)
    if(NOT EXISTS "${library_path}")
        message(FATAL_ERROR "Missing native dependency: ${library_path}\nRun: ./scripts/sync_native_deps.sh")
    endif()
    add_library(${target_name} SHARED IMPORTED)
    set_target_properties(${target_name} PROPERTIES IMPORTED_LOCATION "${library_path}")
endfunction()

if(NOT EXISTS "${THIRD_PARTY_INCLUDE_DIR}")
    message(FATAL_ERROR "Missing native headers in ${THIRD_PARTY_INCLUDE_DIR}\nRun: ./scripts/sync_native_deps.sh")
endif()

# Native codecs and their transitive static dependencies, synced from xrepo.
import_static_library(codec_png "${NATIVE_LIB_DIR}/libpng.a")
import_static_library(codec_jpeg "${NATIVE_LIB_DIR}/libjpeg.a")
import_static_library(codec_webp "${NATIVE_LIB_DIR}/libwebp.a")
import_static_library(codec_z "${NATIVE_LIB_DIR}/libz.a")

set(CODEC_LIB_TARGETS codec_png codec_jpeg codec_webp codec_z)

import_optional_static_library(codec_webpdecoder "${NATIVE_LIB_DIR}/libwebpdecoder.a")
if(TARGET codec_webpdecoder)
    list(APPEND CODEC_LIB_TARGETS codec_webpdecoder)
endif()

import_optional_static_library(codec_webpdemux "${NATIVE_LIB_DIR}/libwebpdemux.a")
if(TARGET codec_webpdemux)
    list(APPEND CODEC_LIB_TARGETS codec_webpdemux)
endif()

import_optional_static_library(codec_sharpyuv "${NATIVE_LIB_DIR}/libsharpyuv.a")
if(TARGET codec_sharpyuv)
    list(APPEND CODEC_LIB_TARGETS codec_sharpyuv)
endif()

import_optional_static_library(codec_cpufeatures_webp "${NATIVE_LIB_DIR}/libcpufeatures-webp.a")
if(TARGET codec_cpufeatures_webp)
    list(APPEND CODEC_LIB_TARGETS codec_cpufeatures_webp)
endif()

# Find ncnn library and its required transitive dependencies from xrepo.
set(NCNN_STATIC_LIB ${NATIVE_LIB_DIR}/libncnn.a)
set(NCNN_LIB_TARGETS)

if(EXISTS "${NCNN_STATIC_LIB}")
    set(CROSSSCALE_USE_NCNN ON)
    import_static_library(ncnn_static "${NCNN_STATIC_LIB}")
    list(APPEND NCNN_LIB_TARGETS ncnn_static)

    import_static_library(glslang_static "${NATIVE_LIB_DIR}/libglslang.a")
    import_static_library(glslang_default_resource_limits_static "${NATIVE_LIB_DIR}/libglslang-default-resource-limits.a")
    import_static_library(spirv_static "${NATIVE_LIB_DIR}/libSPIRV.a")
    import_static_library(machine_independent_static "${NATIVE_LIB_DIR}/libMachineIndependent.a")
    import_static_library(generic_code_gen_static "${NATIVE_LIB_DIR}/libGenericCodeGen.a")
    import_static_library(os_dependent_static "${NATIVE_LIB_DIR}/libOSDependent.a")
    import_shared_library(omp_shared "${NATIVE_LIB_DIR}/libomp.so")

    list(APPEND NCNN_LIB_TARGETS
        glslang_static
        glslang_default_resource_limits_static
        spirv_static
        machine_independent_static
        generic_code_gen_static
        os_dependent_static
        omp_shared
    )
else()
    set(CROSSSCALE_USE_NCNN OFF)
    message(WARNING "ncnn library not found at ${NATIVE_LIB_DIR}, building with Android stub engine")
endif()

# Vulkan
find_library(vulkan-lib vulkan)

# Android libraries
find_library(android-lib android)
find_library(log-lib log)
find_library(jnigraphics-lib jnigraphics)

# Include directories
include_directories(
    ${THIRD_PARTY_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/../../../../../share
    ${CMAKE_SOURCE_DIR}/../../../../../share/core
    ${CMAKE_SOURCE_DIR}/../../../../../share/utils
    ${CMAKE_SOURCE_DIR}/../../../../../share/core/distributed
)

# Source files from shared core
set(SHARED_SOURCES
    ${CMAKE_SOURCE_DIR}/../../../../../share/core/engine.cpp
    ${CMAKE_SOURCE_DIR}/../../../../../share/core/config.cpp
    ${CMAKE_SOURCE_DIR}/../../../../../share/core/model_manager.cpp
    ${CMAKE_SOURCE_DIR}/../../../../../share/core/distributed/protocol.cpp
    ${CMAKE_SOURCE_DIR}/../../../../../share/core/distributed/scheduler.cpp
    ${CMAKE_SOURCE_DIR}/../../../../../share/core/distributed/worker.cpp
    ${CMAKE_SOURCE_DIR}/../../../../../share/utils/image.cpp
    ${CMAKE_SOURCE_DIR}/../../../../../share/utils/benchmark.cpp
    ${CMAKE_SOURCE_DIR}/../../../../../share/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/../../../../../share/utils/network.cpp
    ${CMAKE_SOURCE_DIR}/../../../../../share/utils/crypto.cpp
)

if(CROSSSCALE_USE_NCNN)
    list(APPEND SHARED_SOURCES
        ${CMAKE_SOURCE_DIR}/../../../../../share/core/ncnn_engine.cpp
    )
else()
    list(APPEND SHARED_SOURCES
        ${CMAKE_SOURCE_DIR}/ncnn_engine_stub_android.cpp
    )
endif()

# JNI bridge source
set(JNI_SOURCES
    ${CMAKE_SOURCE_DIR}/jni_bridge.cpp
)

# Create shared library
add_library(crossscale-native SHARED
    ${SHARED_SOURCES}
    ${JNI_SOURCES}
)

# Compiler flags
target_compile_options(crossscale-native PRIVATE
    -O3
    -fexceptions
    -frtti
    -Wall
    -Wextra
)

# Link libraries
target_link_libraries(crossscale-native
    ${NCNN_LIB_TARGETS}
    ${CODEC_LIB_TARGETS}
    ${vulkan-lib}
    ${android-lib}
    ${log-lib}
    ${jnigraphics-lib}
)

# Export ANativeActivity_onCreate for native activity
set_target_properties(crossscale-native PROPERTIES
    CXX_VISIBILITY_PRESET default
)
