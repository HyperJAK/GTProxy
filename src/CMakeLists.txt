project(Proxy)

file(GLOB GTPROXY_INCLUDE_FILES
    *.hpp
    command/*.hpp
    command/commands/*.hpp
    control/*.hpp
    core/*.hpp
    core/handlers/*.hpp
    event/*.hpp
    item/*.hpp
    network/*.hpp
    packet/*.hpp
    packet/game/*.hpp
    packet/message/*.hpp
    plugin/*.hpp
    player/*.hpp
    scripting/*.hpp
    scripting/bindings/*.hpp
    utils/*.hpp
    utils/formatter/*.hpp
    world/*.hpp)

file(GLOB GTPROXY_SOURCE_FILES
    command/*.cpp
    control/*.cpp
    core/*.cpp
    core/handlers/*.cpp
    item/*.cpp
    network/*.cpp
    packet/*.cpp
    plugin/*.cpp
    scripting/*.cpp
    scripting/bindings/*.cpp
    utils/*.cpp
    world/*.cpp)

# Everything except main.cpp lives in a static library so that the unit and
# integration tests can link the real proxy code.
add_library(GTProxyLib STATIC
    ${GTPROXY_INCLUDE_FILES}
    ${GTPROXY_SOURCE_FILES})

add_executable(${PROJECT_NAME} main.cpp)

find_package(eventpp REQUIRED)
find_package(fmt REQUIRED)
find_package(glaze REQUIRED)
find_package(glm REQUIRED)
#find_package(httplib REQUIRED)
find_package(magic_enum REQUIRED)
find_package(pcg-cpp REQUIRED)
find_package(LibreSSL REQUIRED)
find_package(lua REQUIRED)
find_package(sol2 REQUIRED)
find_package(spdlog REQUIRED)
find_package(ZLIB REQUIRED)

target_link_libraries(GTProxyLib PUBLIC
    enet
    eventpp::eventpp
    fmt::fmt
    glaze::glaze
    glm::glm
    httplib::httplib
    LibreSSL::TLS
    LibreSSL::Crypto
    lua::lua
    magic_enum::magic_enum
    pcg-cpp::pcg-cpp
    sol2::sol2
    spdlog::spdlog
    ZLIB::ZLIB)

target_link_libraries(${PROJECT_NAME} PRIVATE GTProxyLib)

target_include_directories(GTProxyLib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/lib/enet/include)

target_include_directories(GTProxyLib PUBLIC
    ${CMAKE_SOURCE_DIR}/lib/libressl/include)

if (MSVC)
    target_compile_options(GTProxyLib PUBLIC
        /EHsc
        /bigobj)
else ()
    target_compile_options(GTProxyLib PUBLIC
        -fexceptions)
endif ()

target_compile_definitions(GTProxyLib PUBLIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    SPDLOG_FMT_EXTERNAL
    CPPHTTPLIB_OPENSSL_SUPPORT
    GTPROXY_VERSION_MAJOR=${CMAKE_PROJECT_VERSION_MAJOR}
    GTPROXY_VERSION_MINOR=${CMAKE_PROJECT_VERSION_MINOR}
    GTPROXY_VERSION_PATCH=${CMAKE_PROJECT_VERSION_PATCH})

if (NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "" OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(GTProxyLib PUBLIC
        GTPROXY_DEBUG)
endif ()

add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/resources"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/resources"
    COMMENT "Copying ${CMAKE_SOURCE_DIR}/resources to $<TARGET_FILE_DIR:${PROJECT_NAME}>/resources.")

file(GLOB SCRIPT_FILES "${CMAKE_SOURCE_DIR}/scripts/*.lua")
file(GLOB_RECURSE SCRIPT_SUBDIR_FILES "${CMAKE_SOURCE_DIR}/scripts/**/*.lua")

list(FILTER SCRIPT_FILES EXCLUDE REGEX "test_.*\\.lua$")
list(FILTER SCRIPT_SUBDIR_FILES EXCLUDE REGEX "test_.*\\.lua$")

foreach (script_file ${SCRIPT_FILES} ${SCRIPT_SUBDIR_FILES})
    get_filename_component(script_name "${script_file}" NAME)
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${script_file}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/scripts/${script_name}"
        COMMENT "Copying ${script_name} to $<TARGET_FILE_DIR:${PROJECT_NAME}>/scripts/")
endforeach ()
