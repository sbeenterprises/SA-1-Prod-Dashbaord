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

# mayara_bridge conversion tests (need nlohmann-json; skipped without it).
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
  add_executable(mayara_logic_test mayara_logic_test.cpp)
  target_link_libraries(mayara_logic_test PRIVATE nlohmann_json::nlohmann_json)
  target_compile_options(mayara_logic_test PRIVATE -Wall -Wextra)
  add_test(NAME mayara_logic COMMAND mayara_logic_test)
endif()

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

  # mayara-server radar targets -> RADAR_TARGET (systemd service
  # usv-mayara-bridge, display only; see mayara_bridge.cpp).
  find_package(nlohmann_json REQUIRED)
  add_executable(mayara_bridge mayara_bridge.cpp)
  target_include_directories(mayara_bridge PRIVATE ${MOOS_INCLUDE_DIRS})
  target_link_libraries(mayara_bridge PRIVATE ${MOOS_LIBRARIES} CURL::libcurl
                        nlohmann_json::nlohmann_json Threads::Threads)
  target_compile_options(mayara_bridge PRIVATE -Wall -Wextra)

  install(TARGETS moos_bridge moos_poke mayara_bridge RUNTIME DESTINATION bin)
endif()
