cmake_minimum_required(VERSION 3.22.1)

project("crossscale")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find ncnn library
set(NCNN_DIR ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI})
find_library(ncnn-lib ncnn PATHS ${NCNN_DIR} NO_DEFAULT_PATH)

if(NOT ncnn-lib)
    message(WARNING "ncnn library not found at ${NCNN_DIR}, using stub")
    add_library(ncnn STATIC IMPORTED)
    set_target_properties(ncnn PROPERTIES IMPORTED_LOCATION ${NCNN_DIR}/libncnn.a)
else()
    add_library(ncnn STATIC IMPORTED)
    set_target_properties(ncnn PROPERTIES IMPORTED_LOCATION ${ncnn-lib})
endif()

# Vulkan
find_library(vulkan-lib vulkan)

# Android libraries
find_library(android-lib android)
find_library(log-lib log)
find_library(jnigraphics-lib jnigraphics)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/../../../../share
    ${CMAKE_SOURCE_DIR}/../../../../share/core
    ${CMAKE_SOURCE_DIR}/../../../../share/utils
    ${CMAKE_SOURCE_DIR}/../../../../share/core/distributed
)

# Source files from shared core
set(SHARED_SOURCES
    ${CMAKE_SOURCE_DIR}/../../../../share/core/engine.cpp
    ${CMAKE_SOURCE_DIR}/../../../../share/core/ncnn_engine.cpp
    ${CMAKE_SOURCE_DIR}/../../../../share/core/model_manager.cpp
    ${CMAKE_SOURCE_DIR}/../../../../share/utils/image.cpp
    ${CMAKE_SOURCE_DIR}/../../../../share/utils/logger.cpp
    ${CMAKE_SOURCE_DIR}/../../../../share/utils/network.cpp
    ${CMAKE_SOURCE_DIR}/../../../../share/utils/crypto.cpp
    ${CMAKE_SOURCE_DIR}/../../../../share/core/distributed/protocol.cpp
    ${CMAKE_SOURCE_DIR}/../../../../share/core/distributed/scheduler.cpp
    ${CMAKE_SOURCE_DIR}/../../../../share/core/distributed/worker.cpp
)

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
    ncnn
    ${vulkan-lib}
    ${android-lib}
    ${log-lib}
    ${jnigraphics-lib}
)

# Export ANativeActivity_onCreate for native activity
set_target_properties(crossscale-native PROPERTIES
    CXX_VISIBILITY_PRESET default
)
