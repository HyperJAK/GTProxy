# Compatibility shim: header-only pcg-cpp from system include path.
find_path(PCG_INCLUDE_DIR pcg_random.hpp REQUIRED)
if (NOT TARGET pcg-cpp::pcg-cpp)
    add_library(pcg-cpp::pcg-cpp INTERFACE IMPORTED)
    target_include_directories(pcg-cpp::pcg-cpp INTERFACE ${PCG_INCLUDE_DIR})
endif ()
set(pcg-cpp_FOUND TRUE)
