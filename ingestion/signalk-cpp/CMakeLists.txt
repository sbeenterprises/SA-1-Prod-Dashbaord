cmake_minimum_required(VERSION 3.16)
project(usv_signalk_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CURL REQUIRED)
find_package(ixwebsocket REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

add_executable(signalk_bridge signalk_bridge.cpp)
target_link_libraries(signalk_bridge PRIVATE
  ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json
  CURL::libcurl Threads::Threads)
target_compile_options(signalk_bridge PRIVATE -Wall -Wextra)

install(TARGETS signalk_bridge RUNTIME DESTINATION bin)
