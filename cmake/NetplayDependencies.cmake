# Direct netplay uses OpenSSL Ed25519 identities. Qt also loads OpenSSL
# dynamically for its TLS-PSK backend on Windows.
if(WIN32)
    set(KEGA_OPENSSL_WINDOWS_ROOT
        "$ENV{HOME}/.cache/kegarefusion-openssl/3.5.8/openssl-3.5.8"
        CACHE PATH "Pinned MinGW OpenSSL 3.5.8 build root")
    if(NOT EXISTS "${KEGA_OPENSSL_WINDOWS_ROOT}/libcrypto.dll.a" OR
       NOT EXISTS "${KEGA_OPENSSL_WINDOWS_ROOT}/include/openssl/evp.h")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare-openssl-windows.py"
            RESULT_VARIABLE _kega_openssl_result
            ERROR_VARIABLE _kega_openssl_error)
        if(NOT _kega_openssl_result EQUAL 0)
            message(FATAL_ERROR "Cannot prepare pinned Windows OpenSSL: ${_kega_openssl_error}")
        endif()
    endif()
    if(NOT TARGET OpenSSL::Crypto)
        add_library(OpenSSL::Crypto SHARED IMPORTED GLOBAL)
        set_target_properties(OpenSSL::Crypto PROPERTIES
            IMPORTED_IMPLIB "${KEGA_OPENSSL_WINDOWS_ROOT}/libcrypto.dll.a"
            IMPORTED_LOCATION "${KEGA_OPENSSL_WINDOWS_ROOT}/libcrypto-3-x64.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${KEGA_OPENSSL_WINDOWS_ROOT}/include")
    endif()
else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
endif()
