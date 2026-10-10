cmake_minimum_required(VERSION 3.22.1)
project(assrender C)

# AnimeStream needs the direct libass bridge only, not the FFmpeg-based
# stream extraction path. Keep the latter optional for other consumers.
option(ASSRENDER_ENABLE_FFMPEG "Build FFmpeg subtitle extraction bridge" OFF)
set(PREBUILT_DIR "${CMAKE_SOURCE_DIR}/../../../../prebuilt/${ANDROID_ABI}")

add_library(ass SHARED IMPORTED)
set_target_properties(ass PROPERTIES IMPORTED_LOCATION "${PREBUILT_DIR}/lib/libass.so")

add_library(freetype SHARED IMPORTED)
set_target_properties(freetype PROPERTIES IMPORTED_LOCATION "${PREBUILT_DIR}/lib/libfreetype.so")

add_library(fribidi SHARED IMPORTED)
set_target_properties(fribidi PROPERTIES IMPORTED_LOCATION "${PREBUILT_DIR}/lib/libfribidi.so")

add_library(harfbuzz SHARED IMPORTED)
set_target_properties(harfbuzz PROPERTIES IMPORTED_LOCATION "${PREBUILT_DIR}/lib/libharfbuzz.so")

include_directories("${PREBUILT_DIR}/include")

set(ASSRENDER_SOURCES ass_direct.c ass_direct_jni.c)
set(ASSRENDER_LINK_LIBS ass freetype fribidi harfbuzz)

if(ASSRENDER_ENABLE_FFMPEG)
    foreach(lib avformat avcodec avutil swresample)
        add_library(${lib} SHARED IMPORTED)
        set_target_properties(${lib} PROPERTIES IMPORTED_LOCATION "${PREBUILT_DIR}/lib/lib${lib}.so")
    endforeach()
    list(APPEND ASSRENDER_SOURCES assrender_jni.c subtitle_pipeline.c)
    list(APPEND ASSRENDER_LINK_LIBS avformat avcodec avutil swresample)
endif()

add_library(assrender SHARED ${ASSRENDER_SOURCES})
target_link_libraries(assrender ${ASSRENDER_LINK_LIBS} android jnigraphics log)
