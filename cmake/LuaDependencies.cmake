set(LUA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua/src")
set(LUA_FILES
    lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
    lparser lstate lstring ltable ltm lundump lvm lzio
    lauxlib lbaselib lmathlib lstrlib ltablib lutf8lib)
list(TRANSFORM LUA_FILES PREPEND "${LUA_SOURCE_DIR}/")
list(TRANSFORM LUA_FILES APPEND ".c")
# Lua uses C++ exception unwinding in the protected calls that enter our
# bindings. This keeps Lua errors from jumping over live Qt objects.
set_source_files_properties(${LUA_FILES} PROPERTIES LANGUAGE CXX)
add_library(kega_lua STATIC ${LUA_FILES})
set_target_properties(kega_lua PROPERTIES AUTOMOC OFF AUTORCC OFF)
target_include_directories(kega_lua PUBLIC "${LUA_SOURCE_DIR}")
