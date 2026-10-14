cmake_minimum_required(VERSION 3.16)
project(usv_can_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_executable(can_bridge main.cpp)
target_link_libraries(can_bridge PRIVATE CURL::libcurl Threads::Threads)
target_compile_options(can_bridge PRIVATE -Wall -Wextra)

add_executable(usv722_can_test usv722_can_test.cpp)
target_compile_options(usv722_can_test PRIVATE -Wall -Wextra)

# Per-vessel engine profile (cpp-common), shared by all bridges; tested here
# because this is the one bridge build that needs no MOOS or WebSocket library.
add_executable(engine_profile_test ../cpp-common/engine_profile_test.cpp)
target_compile_options(engine_profile_test PRIVATE -Wall -Wextra)
add_executable(j1939_faults_test ../cpp-common/j1939_faults_test.cpp)
target_compile_options(j1939_faults_test PRIVATE -Wall -Wextra)

install(TARGETS can_bridge RUNTIME DESTINATION bin)
