cmake_minimum_required(VERSION 3.16)
project(usv_moos_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The mission-logic unit tests build anywhere — no MOOS required:
#   cmake -S . -B build -DMISSION_LOGIC_TESTS_ONLY=ON
#   cmake --build build && ctest --test-dir build
option(MISSION_LOGIC_TESTS_ONLY
       "Build only the mission-logic unit tests (no MOOS needed)" OFF)

enable_testing()
add_executable(mission_logic_test mission_logic_test.cpp)
target_compile_options(mission_logic_test PRIVATE -Wall -Wextra)
add_test(NAME mission_logic COMMAND mission_logic_test)

if(NOT MISSION_LOGIC_TESTS_ONLY)
  find_package(MOOS 10 REQUIRED)
  find_package(CURL REQUIRED)
  find_package(Threads REQUIRED)

  add_executable(moos_bridge moos_bridge.cpp)
  target_include_directories(moos_bridge PRIVATE ${MOOS_INCLUDE_DIRS})
  target_link_libraries(moos_bridge PRIVATE ${MOOS_LIBRARIES} CURL::libcurl Threads::Threads)
  target_compile_options(moos_bridge PRIVATE -Wall -Wextra)

  # Test/commissioning poke utility (not installed as a service; used by
  # tools/smoke_test.sh and handy for manual variable pokes).
  add_executable(moos_poke moos_poke.cpp)
  target_include_directories(moos_poke PRIVATE ${MOOS_INCLUDE_DIRS})
  target_link_libraries(moos_poke PRIVATE ${MOOS_LIBRARIES})
  target_compile_options(moos_poke PRIVATE -Wall -Wextra)

  install(TARGETS moos_bridge moos_poke RUNTIME DESTINATION bin)
endif()
