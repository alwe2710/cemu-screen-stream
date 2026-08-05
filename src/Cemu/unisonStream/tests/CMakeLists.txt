# Standalone from Cemu's own (huge) CMakeLists.txt on purpose -- see
# test_unison_messages.cpp's own top comment. Only needs unison_core's
# json.c (what UnisonMessages.cpp itself links against) and handshake.c
# (what this test's own round-trip assertions call) -- not the rest of
# unison_core, and none of Cemu proper.
cmake_minimum_required(VERSION 3.13)
project(unison_messages_test CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 99)

set(UNISON_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../dependencies/unison/core")

add_executable(unison_messages_test
    test_unison_messages.cpp
    ../UnisonMessages.cpp
    "${UNISON_CORE_DIR}/src/json.c"
    "${UNISON_CORE_DIR}/src/handshake.c"
)
target_include_directories(unison_messages_test PRIVATE "${UNISON_CORE_DIR}/include")

enable_testing()
add_test(NAME unison_messages COMMAND unison_messages_test)
