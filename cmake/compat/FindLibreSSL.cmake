# Compatibility shim: map the Conan LibreSSL targets onto system OpenSSL.
find_package(OpenSSL REQUIRED)
if (NOT TARGET LibreSSL::TLS)
    add_library(LibreSSL::TLS INTERFACE IMPORTED)
    target_link_libraries(LibreSSL::TLS INTERFACE OpenSSL::SSL)
    add_library(LibreSSL::Crypto INTERFACE IMPORTED)
    target_link_libraries(LibreSSL::Crypto INTERFACE OpenSSL::Crypto)
endif ()
set(LibreSSL_FOUND TRUE)
