set(RCHEEVOS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rcheevos")
file(GLOB RCHEEVOS_RUNTIME CONFIGURE_DEPENDS "${RCHEEVOS_ROOT}/src/rcheevos/*.c")
file(GLOB RCHEEVOS_API CONFIGURE_DEPENDS "${RCHEEVOS_ROOT}/src/rapi/*.c")
file(GLOB RCHEEVOS_HASH CONFIGURE_DEPENDS "${RCHEEVOS_ROOT}/src/rhash/*.c")
add_library(rcheevos STATIC ${RCHEEVOS_RUNTIME} ${RCHEEVOS_API} ${RCHEEVOS_HASH}
    "${RCHEEVOS_ROOT}/src/rc_client.c" "${RCHEEVOS_ROOT}/src/rc_compat.c"
    "${RCHEEVOS_ROOT}/src/rc_libretro.c" "${RCHEEVOS_ROOT}/src/rc_util.c"
    "${RCHEEVOS_ROOT}/src/rc_version.c")
target_include_directories(rcheevos PUBLIC "${RCHEEVOS_ROOT}/include" "${RCHEEVOS_ROOT}/src"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libretro")
target_compile_definitions(rcheevos PRIVATE RC_DISABLE_LUA RC_CLIENT_SUPPORTS_HASH)
if(UNIX)
    target_link_libraries(rcheevos PUBLIC m)
endif()
