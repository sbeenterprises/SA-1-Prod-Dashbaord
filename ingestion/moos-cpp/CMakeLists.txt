cmake_minimum_required(VERSION 3.16)
project(usv_moos_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(MOOS 10 REQUIRED)
find_package(CURL REQUIRED)

add_executable(moos_bridge moos_bridge.cpp)
target_include_directories(moos_bridge PRIVATE ${MOOS_INCLUDE_DIRS})
target_link_libraries(moos_bridge PRIVATE ${MOOS_LIBRARIES} CURL::libcurl)
target_compile_options(moos_bridge PRIVATE -Wall -Wextra)

install(TARGETS moos_bridge RUNTIME DESTINATION bin)
