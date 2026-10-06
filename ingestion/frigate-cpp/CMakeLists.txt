cmake_minimum_required(VERSION 3.16)
project(usv_frigate_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

enable_testing()
add_executable(frigate_logic_test frigate_logic_test.cpp)
target_link_libraries(frigate_logic_test PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(frigate_logic_test PRIVATE -Wall -Wextra)
add_test(NAME frigate_logic COMMAND frigate_logic_test)

add_executable(frigate_bridge frigate_bridge.cpp)
target_link_libraries(frigate_bridge PRIVATE nlohmann_json::nlohmann_json
                      CURL::libcurl Threads::Threads)
target_compile_options(frigate_bridge PRIVATE -Wall -Wextra)

install(TARGETS frigate_bridge RUNTIME DESTINATION bin)
