# Standalone from Cemu's own (huge) CMakeLists.txt on purpose -- see
# test_finlink_messages.cpp's own top comment. Only needs finlink_core's
# json.c (what FinlinkMessages.cpp itself links against) and handshake.c
# (what this test's own round-trip assertions call) -- not the rest of
# finlink_core, and none of Cemu proper.
cmake_minimum_required(VERSION 3.13)
project(finlink_messages_test CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 99)

set(FINLINK_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../dependencies/finlink/core")

add_executable(finlink_messages_test
    test_finlink_messages.cpp
    ../FinlinkMessages.cpp
    "${FINLINK_CORE_DIR}/src/json.c"
    "${FINLINK_CORE_DIR}/src/handshake.c"
)
target_include_directories(finlink_messages_test PRIVATE "${FINLINK_CORE_DIR}/include")

enable_testing()
add_test(NAME finlink_messages COMMAND finlink_messages_test)
