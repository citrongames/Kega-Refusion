# Pinned MIT GGPO core from Flycast. See third_party/GGPO-PROVENANCE.md.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/prepare-ggpo.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _kega_ggpo_prepare_result
    ERROR_VARIABLE _kega_ggpo_prepare_error)
if(NOT _kega_ggpo_prepare_result EQUAL 0)
    message(FATAL_ERROR "Cannot prepare pinned GGPO: ${_kega_ggpo_prepare_error}")
endif()

set(_kega_ggpo_root "${CMAKE_SOURCE_DIR}/build/deps/ggpo-src")
set(_kega_ggpo_lib "${_kega_ggpo_root}/lib/ggpo")
add_library(kega_ggpo STATIC
    "${_kega_ggpo_lib}/bitvector.cpp"
    "${_kega_ggpo_lib}/game_input.cpp"
    "${_kega_ggpo_lib}/input_queue.cpp"
    "${_kega_ggpo_lib}/main.cpp"
    "${_kega_ggpo_lib}/platform_linux.cpp"
    "${_kega_ggpo_lib}/platform_windows.cpp"
    "${_kega_ggpo_lib}/poll.cpp"
    "${_kega_ggpo_lib}/sync.cpp"
    "${_kega_ggpo_lib}/timesync.cpp"
    "${_kega_ggpo_lib}/backends/p2p.cpp"
    "${_kega_ggpo_lib}/backends/spectator.cpp"
    "${_kega_ggpo_lib}/backends/synctest.cpp"
    "${_kega_ggpo_lib}/network/udp.cpp"
    "${_kega_ggpo_lib}/network/udp_proto.cpp")
target_compile_features(kega_ggpo PUBLIC cxx_std_17)
target_include_directories(kega_ggpo PUBLIC "${_kega_ggpo_root}/include")
target_include_directories(kega_ggpo PRIVATE "${_kega_ggpo_lib}")
if(WIN32)
    target_link_libraries(kega_ggpo PUBLIC ws2_32 winmm)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(kega_ggpo PUBLIC Threads::Threads)
endif()
add_library(GGPO::GGPO ALIAS kega_ggpo)