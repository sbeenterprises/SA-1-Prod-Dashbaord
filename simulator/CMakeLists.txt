cmake_minimum_required(VERSION 3.16)
project(usv_simulator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_executable(usv_simulator usv_simulator.cpp)
target_link_libraries(usv_simulator PRIVATE CURL::libcurl Threads::Threads)
target_compile_options(usv_simulator PRIVATE -Wall -Wextra)

install(TARGETS usv_simulator RUNTIME DESTINATION bin)
