set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies come from Conan by default (Windows / macOS / Linux).
# Configure with -DGTPROXY_USE_CONAN=OFF to use system packages instead
# (see cmake/compat and docs/BUILDING.md); useful in offline / CI sandboxes.
option(GTPROXY_USE_CONAN "Fetch dependencies with Conan" ON)

if (GTPROXY_USE_CONAN)
    # CMake Conan requires this to be set or passed in via the command line
    set(CMAKE_PROJECT_TOP_LEVEL_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake-conan/conan_provider.cmake)
else ()
    list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/compat)
endif ()

cmake_minimum_required(VERSION 3.24)
project(GTProxy VERSION 3.0.0)

add_subdirectory(lib)
add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
