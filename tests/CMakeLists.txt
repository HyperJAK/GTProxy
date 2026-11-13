project(GTProxy_tests)

find_package(GTest REQUIRED)

add_executable(GTProxy_tests
    utils/test_text_parse.cpp
    utils/test_byte_stream.cpp
    control/test_codec.cpp
    control/test_flow.cpp
    control/test_api.cpp
    control/test_integration.cpp
    control/test_enet_loopback.cpp
    test_config.cpp)

target_include_directories(GTProxy_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)

target_link_libraries(GTProxy_tests PRIVATE GTProxyLib GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(GTProxy_tests DISCOVERY_TIMEOUT 30)
