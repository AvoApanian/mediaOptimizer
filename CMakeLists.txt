cmake_minimum_required(VERSION 3.20)

project(mediaOptimizer
    VERSION 1.0.0
    DESCRIPTION "Cross-platform media optimization engine"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

option(MEDIAOPT_BUILD_CLI "Build the standalone desktop video CLI" ON)
option(MEDIAOPT_BUILD_MANUAL_TESTS "Build machine-specific integration tests" OFF)
option(MEDIAOPT_REQUIRE_FFMPEG "Fail configuration if FFmpeg and FFprobe are missing" OFF)

if(ANDROID)
    include(ExternalProject)

    # All third-party Android dependencies are built as static PIC libraries.
    set(MEDIAOPT_THIRD_PARTY_INSTALL_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/third_party"
    )

    set(MEDIAOPT_JPEG_INSTALL_DIR
        "${MEDIAOPT_THIRD_PARTY_INSTALL_DIR}/libjpeg-turbo-install"
    )
    set(MEDIAOPT_PNG_INSTALL_DIR
        "${MEDIAOPT_THIRD_PARTY_INSTALL_DIR}/libpng-install"
    )
    set(MEDIAOPT_WEBP_INSTALL_DIR
        "${MEDIAOPT_THIRD_PARTY_INSTALL_DIR}/libwebp-install"
    )

    file(MAKE_DIRECTORY
        "${MEDIAOPT_JPEG_INSTALL_DIR}/include"
        "${MEDIAOPT_PNG_INSTALL_DIR}/include"
        "${MEDIAOPT_WEBP_INSTALL_DIR}/include"
    )

    # JPEG: libjpeg-turbo
    ExternalProject_Add(mediaopt_libjpeg_turbo_external
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libjpeg-turbo"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        INSTALL_DIR "${MEDIAOPT_JPEG_INSTALL_DIR}"

        CMAKE_ARGS
            "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}"
            "-DANDROID_ABI:STRING=${ANDROID_ABI}"
            "-DANDROID_PLATFORM:STRING=${ANDROID_PLATFORM}"
            "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
            "-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>"
            "-DCMAKE_INSTALL_LIBDIR:STRING=lib"
            "-DCMAKE_INSTALL_INCLUDEDIR:STRING=include"
            "-DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON"
            "-DENABLE_SHARED:BOOL=OFF"
            "-DENABLE_STATIC:BOOL=ON"
            "-DWITH_SIMD:BOOL=ON"
            "-DWITH_TURBOJPEG:BOOL=OFF"
            "-DWITH_TOOLS:BOOL=OFF"
            "-DWITH_TESTS:BOOL=OFF"
            "-DWITH_FUZZ:BOOL=OFF"
            "-DWITH_PROFILE:BOOL=OFF"

        BUILD_BYPRODUCTS
            "${MEDIAOPT_JPEG_INSTALL_DIR}/lib/libjpeg.a"
    )

    # PNG: libpng, using the Android NDK zlib.
    ExternalProject_Add(mediaopt_libpng_external
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libpng"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        INSTALL_DIR "${MEDIAOPT_PNG_INSTALL_DIR}"

        CMAKE_ARGS
            "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}"
            "-DANDROID_ABI:STRING=${ANDROID_ABI}"
            "-DANDROID_PLATFORM:STRING=${ANDROID_PLATFORM}"
            "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
            "-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>"
            "-DCMAKE_INSTALL_LIBDIR:STRING=lib"
            "-DCMAKE_INSTALL_INCLUDEDIR:STRING=include"
            "-DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON"
            "-DPNG_SHARED:BOOL=OFF"
            "-DPNG_STATIC:BOOL=ON"
            "-DPNG_TESTS:BOOL=OFF"
            "-DPNG_TOOLS:BOOL=OFF"
            "-DPNG_FRAMEWORK:BOOL=OFF"
            "-DPNG_HARDWARE_OPTIMIZATIONS:BOOL=ON"

        BUILD_BYPRODUCTS
            "${MEDIAOPT_PNG_INSTALL_DIR}/lib/libpng16.a"
    )

    # WebP: encoder, decoder, demux, mux and SharpYUV.
    ExternalProject_Add(mediaopt_libwebp_external
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libwebp"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        INSTALL_DIR "${MEDIAOPT_WEBP_INSTALL_DIR}"

        CMAKE_ARGS
            "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}"
            "-DANDROID_ABI:STRING=${ANDROID_ABI}"
            "-DANDROID_PLATFORM:STRING=${ANDROID_PLATFORM}"
            "-DANDROID_NDK:PATH=${CMAKE_ANDROID_NDK}"
            "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
            "-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>"
            "-DCMAKE_INSTALL_LIBDIR:STRING=lib"
            "-DCMAKE_INSTALL_INCLUDEDIR:STRING=include"
            "-DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON"
            "-DBUILD_SHARED_LIBS:BOOL=OFF"
            "-DWEBP_LINK_STATIC:BOOL=ON"
            "-DWEBP_ENABLE_SIMD:BOOL=ON"
            "-DWEBP_BUILD_ANIM_UTILS:BOOL=OFF"
            "-DWEBP_BUILD_CWEBP:BOOL=OFF"
            "-DWEBP_BUILD_DWEBP:BOOL=OFF"
            "-DWEBP_BUILD_GIF2WEBP:BOOL=OFF"
            "-DWEBP_BUILD_IMG2WEBP:BOOL=OFF"
            "-DWEBP_BUILD_VWEBP:BOOL=OFF"
            "-DWEBP_BUILD_WEBPINFO:BOOL=OFF"
            "-DWEBP_BUILD_LIBWEBPMUX:BOOL=ON"
            "-DWEBP_BUILD_WEBPMUX:BOOL=OFF"
            "-DWEBP_BUILD_EXTRAS:BOOL=OFF"
            "-DWEBP_BUILD_FUZZTEST:BOOL=OFF"

        BUILD_BYPRODUCTS
            "${MEDIAOPT_WEBP_INSTALL_DIR}/lib/libwebp.a"
            "${MEDIAOPT_WEBP_INSTALL_DIR}/lib/libwebpdemux.a"
            "${MEDIAOPT_WEBP_INSTALL_DIR}/lib/libwebpmux.a"
            "${MEDIAOPT_WEBP_INSTALL_DIR}/lib/libsharpyuv.a"
            "${MEDIAOPT_WEBP_INSTALL_DIR}/lib/libcpufeatures-webp.a"
    )

    # Imported JPEG target.
    add_library(mediaopt_jpeg STATIC IMPORTED GLOBAL)
    set_target_properties(mediaopt_jpeg PROPERTIES
        IMPORTED_LOCATION
            "${MEDIAOPT_JPEG_INSTALL_DIR}/lib/libjpeg.a"
        INTERFACE_INCLUDE_DIRECTORIES
            "${MEDIAOPT_JPEG_INSTALL_DIR}/include"
    )

    # Imported PNG target.
    add_library(mediaopt_png STATIC IMPORTED GLOBAL)
    set_target_properties(mediaopt_png PROPERTIES
        IMPORTED_LOCATION
            "${MEDIAOPT_PNG_INSTALL_DIR}/lib/libpng16.a"
        INTERFACE_INCLUDE_DIRECTORIES
            "${MEDIAOPT_PNG_INSTALL_DIR}/include"
        INTERFACE_LINK_LIBRARIES z
    )

    # Imported WebP dependencies.
    add_library(mediaopt_cpufeatures_webp STATIC IMPORTED GLOBAL)
    set_target_properties(mediaopt_cpufeatures_webp PROPERTIES
        IMPORTED_LOCATION
            "${MEDIAOPT_WEBP_INSTALL_DIR}/lib/libcpufeatures-webp.a"
    )

    add_library(mediaopt_sharpyuv STATIC IMPORTED GLOBAL)
    set_target_properties(mediaopt_sharpyuv PROPERTIES
        IMPORTED_LOCATION
            "${MEDIAOPT_WEBP_INSTALL_DIR}/lib/libsharpyuv.a"
        INTERFACE_INCLUDE_DIRECTORIES
            "${MEDIAOPT_WEBP_INSTALL_DIR}/include"
        INTERFACE_LINK_LIBRARIES
            "mediaopt_cpufeatures_webp;dl"
    )

    add_library(mediaopt_webp STATIC IMPORTED GLOBAL)
    set_target_properties(mediaopt_webp PROPERTIES
        IMPORTED_LOCATION
            "${MEDIAOPT_WEBP_INSTALL_DIR}/lib/libwebp.a"
        INTERFACE_INCLUDE_DIRECTORIES
            "${MEDIAOPT_WEBP_INSTALL_DIR}/include"
        INTERFACE_LINK_LIBRARIES
            "mediaopt_sharpyuv;mediaopt_cpufeatures_webp;dl"
    )

    add_library(mediaopt_webpdemux STATIC IMPORTED GLOBAL)
    set_target_properties(mediaopt_webpdemux PROPERTIES
        IMPORTED_LOCATION
            "${MEDIAOPT_WEBP_INSTALL_DIR}/lib/libwebpdemux.a"
        INTERFACE_INCLUDE_DIRECTORIES
            "${MEDIAOPT_WEBP_INSTALL_DIR}/include"
        INTERFACE_LINK_LIBRARIES mediaopt_webp
    )

    add_library(mediaopt_webpmux STATIC IMPORTED GLOBAL)
    set_target_properties(mediaopt_webpmux PROPERTIES
        IMPORTED_LOCATION
            "${MEDIAOPT_WEBP_INSTALL_DIR}/lib/libwebpmux.a"
        INTERFACE_INCLUDE_DIRECTORIES
            "${MEDIAOPT_WEBP_INSTALL_DIR}/include"
        INTERFACE_LINK_LIBRARIES mediaopt_webp
    )
endif()

if(ANDROID)
    message(STATUS "Target platform: Android")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(STATUS "Target platform: iOS")
elseif(EMSCRIPTEN)
    message(STATUS "Target platform: WebAssembly")
elseif(WIN32)
    message(STATUS "Target platform: Windows")
elseif(UNIX)
    message(STATUS "Target platform: Unix-like desktop")
else()
    message(FATAL_ERROR "Unsupported target platform")
endif()



if(ANDROID)
    add_library(mediaOptimizer SHARED
        src/core/dispatcher/media_optimizer.cpp
        platforms/android/src/image_optimizer.cpp
        platforms/android/src/video_optimizer.cpp
        platforms/android/src/audio_optimizer.cpp
    )
else()
    add_library(mediaOptimizer STATIC
        src/core/dispatcher/media_optimizer.cpp
        platforms/linux/src/image_optimizer.cpp
        platforms/linux/src/video_optimizer.cpp
        platforms/linux/src/audio_optimizer.cpp
    )
endif()

add_library(mediaOptimizer::mediaOptimizer ALIAS mediaOptimizer)

if(ANDROID)
    add_dependencies(mediaOptimizer
        mediaopt_libjpeg_turbo_external
        mediaopt_libpng_external
        mediaopt_libwebp_external
    )

    target_link_libraries(mediaOptimizer PRIVATE
        mediaopt_jpeg
        mediaopt_png
        mediaopt_webp
        mediaopt_webpdemux
        mediaopt_webpmux
        z
        dl
    )
endif()

target_include_directories(mediaOptimizer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(mediaOptimizer PUBLIC cxx_std_17)

set_target_properties(mediaOptimizer PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

function(mediaopt_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

mediaopt_enable_warnings(mediaOptimizer)

if(MEDIAOPT_BUILD_CLI AND NOT ANDROID)
    add_executable(mediaoptimizer-video-cli
        cli/main.cpp
    )

    target_link_libraries(mediaoptimizer-video-cli
        PRIVATE mediaOptimizer::mediaOptimizer
    )

    target_compile_features(mediaoptimizer-video-cli PRIVATE cxx_std_17)
    mediaopt_enable_warnings(mediaoptimizer-video-cli)
endif()

if(MEDIAOPT_BUILD_MANUAL_TESTS AND NOT ANDROID)
    add_executable(test_image_api
        tests/test_image_api.cpp
    )

    target_link_libraries(test_image_api
        PRIVATE mediaOptimizer::mediaOptimizer
    )

    add_executable(test_media_api
        tests/test_media_api.cpp
    )

    target_link_libraries(test_media_api
        PRIVATE mediaOptimizer::mediaOptimizer
    )

    mediaopt_enable_warnings(test_image_api)
    mediaopt_enable_warnings(test_media_api)
endif()

if(NOT ANDROID)
    find_program(FFMPEG_EXECUTABLE ffmpeg)
    find_program(FFPROBE_EXECUTABLE ffprobe)

    if(FFMPEG_EXECUTABLE AND FFPROBE_EXECUTABLE)
        message(STATUS "FFmpeg: ${FFMPEG_EXECUTABLE}")
        message(STATUS "FFprobe: ${FFPROBE_EXECUTABLE}")
    else()
        if(MEDIAOPT_REQUIRE_FFMPEG)
            message(FATAL_ERROR "FFmpeg and FFprobe are required but were not found.")
        else()
            message(WARNING "FFmpeg or FFprobe missing. Runtime optimization may fail.")
        endif()
    endif()
endif()

install(
    TARGETS mediaOptimizer
    EXPORT mediaOptimizerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(
    DIRECTORY include/mediaOptimizer
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    EXPORT mediaOptimizerTargets
    FILE mediaOptimizerTargets.cmake
    NAMESPACE mediaOptimizer::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mediaOptimizer
)
