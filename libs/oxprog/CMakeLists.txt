# OpenOX programmer protocol (docs/07-console-protocol.md).
#
#   oxprog        definitions and CRC-32, for both ends
#   oxprogclient  the host side (console, PC tools)
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
project(oxprog LANGUAGES CXX)

add_library(oxprog STATIC "${CMAKE_CURRENT_LIST_DIR}/src/oxprog.cpp")
target_include_directories(oxprog PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_features(oxprog PUBLIC cxx_std_20)

add_library(oxprogclient STATIC "${CMAKE_CURRENT_LIST_DIR}/src/client.cpp")
target_link_libraries(oxprogclient PUBLIC oxprog)
