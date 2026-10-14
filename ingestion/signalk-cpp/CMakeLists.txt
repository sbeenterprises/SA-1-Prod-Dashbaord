cmake_minimum_required(VERSION 3.16)
project(usv_signalk_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Engine path mapping unit test (signalk_engine.hpp): no WebSocket, JSON or
# curl dependency, so it builds on any host with a C++17 compiler.
add_executable(signalk_engine_test signalk_engine_test.cpp)
target_compile_options(signalk_engine_test PRIVATE -Wall -Wextra)

# The bridge itself needs IXWebSocket; a host without it (a workstation
# running tools/smoke_test.sh) still gets the unit test.
find_package(ixwebsocket QUIET)
if(NOT ixwebsocket_FOUND)
  message(WARNING "IXWebSocket not found: building signalk_engine_test only, not signalk_bridge")
  return()
endif()
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

add_executable(signalk_bridge signalk_bridge.cpp)
target_link_libraries(signalk_bridge PRIVATE
  ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json
  CURL::libcurl Threads::Threads)
target_compile_options(signalk_bridge PRIVATE -Wall -Wextra)

install(TARGETS signalk_bridge RUNTIME DESTINATION bin)
