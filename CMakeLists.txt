cmake_minimum_required(VERSION 3.5)
project(fixclient CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(fixclient_core
    src/socket.cpp
    src/config_parser.cpp
    src/application.cpp
    src/fix_parser.cpp
)
target_include_directories(fixclient_core PUBLIC include)

add_executable(fixclient
    src/main.cpp
)
target_link_libraries(fixclient fixclient_core)
