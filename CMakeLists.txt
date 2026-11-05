cmake_minimum_required(VERSION 3.22)
project(Nyanroid LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(NYAN_BUILD_TOOLS "Build desktop command-line tools" ON)
option(NYAN_BUILD_RENDER "Build the OpenGL ES renderer" ON)
option(NYAN_BUILD_DESKTOP "Build the SDL3 desktop player" ON)

add_subdirectory(core)
if(NYAN_BUILD_RENDER)
  add_subdirectory(render)
endif()
if(NYAN_BUILD_DESKTOP AND NOT ANDROID)
  add_subdirectory(apps/desktop)
endif()

if(NYAN_BUILD_TOOLS)
  add_subdirectory(tools/nyan-cli)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(nyan_core_tests tests/core_tests.cpp)
  target_link_libraries(nyan_core_tests PRIVATE nyan_core)
  target_include_directories(nyan_core_tests PRIVATE third_party)  # stb_truetype (testCff2Font)
  add_test(NAME nyan_core_tests COMMAND nyan_core_tests)

  # White Album 2 (WA2) and the Diesel Engine, on made-up data.
  add_executable(nyan_wa2_tests tests/wa2_tests.cpp)
  target_link_libraries(nyan_wa2_tests PRIVATE nyan_core)
  add_test(NAME nyan_wa2_tests COMMAND nyan_wa2_tests)

  add_executable(nyan_diesel_tests tests/diesel_tests.cpp)
  target_link_libraries(nyan_diesel_tests PRIVATE nyan_core)
  add_test(NAME nyan_diesel_tests COMMAND nyan_diesel_tests)
endif()

# A companion folder of your own, git-ignored: tests and tools that need the games themselves.
if(EXISTS "${PROJECT_SOURCE_DIR}/private/CMakeLists.txt")
  add_subdirectory(private)
endif()
