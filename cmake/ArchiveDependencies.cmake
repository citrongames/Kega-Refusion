# Static libraries prepared by tools/prepare-archive.py. No archive runtime DLLs.
if(WIN32)
    set(_archive_platform windows)
else()
    set(_archive_platform linux)
endif()
set(KEGA_ARCHIVE_ROOT "${CMAKE_SOURCE_DIR}/build/deps/archive/${_archive_platform}"
    CACHE PATH "Pinned static archive dependency prefix")
foreach(_component archive lzma z)
    set(_archive_file "lib${_component}.a")
    if(WIN32 AND _component STREQUAL "z")
        set(_archive_file libzs.a)
    endif()
    if(NOT EXISTS "${KEGA_ARCHIVE_ROOT}/lib/${_archive_file}")
        message(FATAL_ERROR "Missing pinned archive dependency. Run: python3 tools/prepare-archive.py --platform ${_archive_platform}")
    endif()
    add_library(kega_${_component} STATIC IMPORTED)
    set_target_properties(kega_${_component} PROPERTIES
        IMPORTED_LOCATION "${KEGA_ARCHIVE_ROOT}/lib/${_archive_file}"
        INTERFACE_INCLUDE_DIRECTORIES "${KEGA_ARCHIVE_ROOT}/include")
endforeach()
set_target_properties(kega_archive PROPERTIES
    INTERFACE_COMPILE_DEFINITIONS LIBARCHIVE_STATIC
    INTERFACE_LINK_LIBRARIES "kega_lzma;kega_z")
set_target_properties(kega_lzma PROPERTIES INTERFACE_COMPILE_DEFINITIONS LZMA_API_STATIC)
if(WIN32)
    # libarchive's built-in random generator uses the Windows system API.
    set_property(TARGET kega_archive APPEND PROPERTY INTERFACE_LINK_LIBRARIES bcrypt)
endif()
