# FFmpeg is already shipped with Qt Multimedia on Windows. Reuse those DLLs
# through the matching, checksum-pinned public headers and generated import libs.
add_library(kega_video_deps INTERFACE)
if(WIN32)
    set(KEGA_VIDEO_ROOT "${CMAKE_SOURCE_DIR}/build/deps/video/windows"
        CACHE PATH "FFmpeg development files prepared by prepare-video-windows.py")
    get_filename_component(_video_qt_root "${Qt6_DIR}/../../.." ABSOLUTE)
    set(KEGA_VIDEO_RUNTIME_DIR "${_video_qt_root}/bin"
        CACHE PATH "Directory containing Qt's FFmpeg 7 runtime DLLs")
    foreach(_pair IN ITEMS "avformat;61" "avcodec;61" "avutil;59")
        list(GET _pair 0 _name)
        list(GET _pair 1 _major)
        set(_implib "${KEGA_VIDEO_ROOT}/lib/lib${_name}.dll.a")
        set(_runtime "${KEGA_VIDEO_RUNTIME_DIR}/${_name}-${_major}.dll")
        if(NOT EXISTS "${_implib}" OR NOT EXISTS "${KEGA_VIDEO_ROOT}/include/lib${_name}/version_major.h")
            message(FATAL_ERROR
                "Missing FFmpeg headers/import libraries. Run: python3 tools/prepare-video-windows.py --qt-root '${_video_qt_root}'")
        endif()
        if(NOT EXISTS "${_runtime}")
            message(FATAL_ERROR "Missing Qt FFmpeg runtime: ${_runtime}")
        endif()
        add_library(kega_video_${_name} SHARED IMPORTED)
        set_target_properties(kega_video_${_name} PROPERTIES
            IMPORTED_IMPLIB "${_implib}"
            IMPORTED_LOCATION "${_runtime}"
            INTERFACE_INCLUDE_DIRECTORIES "${KEGA_VIDEO_ROOT}/include")
        target_link_libraries(kega_video_deps INTERFACE kega_video_${_name})
    endforeach()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(KEGA_VIDEO_FFMPEG REQUIRED IMPORTED_TARGET
        libavformat>=59 libavcodec>=59 libavutil>=57)
    target_link_libraries(kega_video_deps INTERFACE PkgConfig::KEGA_VIDEO_FFMPEG)
endif()
