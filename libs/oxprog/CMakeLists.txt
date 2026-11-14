# OpenOX programmer protocol (docs/07-console-protocol.md).
#
#   oxprog        definitions and CRC-32, for both ends
#   oxprogclient  the host side (console, PC tools)
#   oxprogc       the host side for C (oxprog/oxprog.h); a shared library with
#                 BUILD_SHARED_LIBS, for bindings from other languages
#
# Inside an ESP-IDF build this folder is a component providing oxprog only.

if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/oxprog.cpp"
        INCLUDE_DIRS "include"
    )
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
    return()
endif()

cmake_minimum_required(VERSION 3.16)
project(oxprog LANGUAGES C CXX)

add_library(oxprog STATIC "${CMAKE_CURRENT_LIST_DIR}/src/oxprog.cpp")
target_include_directories(oxprog PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(oxprog PUBLIC cxx_std_20)

add_library(oxprogclient STATIC "${CMAKE_CURRENT_LIST_DIR}/src/client.cpp")
target_link_libraries(oxprogclient PUBLIC oxprog)
set_target_properties(oxprog oxprogclient PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(oxprogc "${CMAKE_CURRENT_LIST_DIR}/src/oxprog_c.cpp")
target_link_libraries(oxprogc PRIVATE oxprogclient)
target_include_directories(oxprogc PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
if(BUILD_SHARED_LIBS)
    target_compile_definitions(oxprogc PUBLIC OXPROG_SHARED)
    set_target_properties(oxprogc PROPERTIES CXX_VISIBILITY_PRESET hidden)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_executable(oxprog_c_test "${CMAKE_CURRENT_LIST_DIR}/tests/c_api_test.c")
    target_link_libraries(oxprog_c_test PRIVATE oxprogc)
    add_test(NAME oxprog_c_test COMMAND oxprog_c_test)
endif()
