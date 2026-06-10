cmake_minimum_required(VERSION 3.15)
project(suAPI LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_executable(suAPI main.cpp)
target_link_libraries(suAPI PRIVATE CURL::libcurl Threads::Threads)

if(WIN32)
    target_link_libraries(suAPI PRIVATE ws2_32)
endif()

message(STATUS "Found libcurl ${CURL_VERSION_STRING}")