cmake_minimum_required(VERSION 3.22)
project(nyanroid_android LANGUAGES CXX)

# Engine core + renderer from the repository root.
set(NYAN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(NYAN_BUILD_DESKTOP OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. nyan)

add_library(nyanroid SHARED
  jni_bridge.cpp
  saf_folder.cpp
  aaudio_output.cpp
)
target_link_libraries(nyanroid PRIVATE nyan_render nyan_core aaudio android log)
target_compile_options(nyanroid PRIVATE -Wall -Wextra -Wno-unused-parameter)
# Calls newer than minSdk link weakly and must sit behind __builtin_available (unguarded ones fail
# to compile), so the app still loads on older Android.
target_compile_definitions(nyanroid PRIVATE __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__)
target_compile_options(nyanroid PRIVATE -Werror=unguarded-availability)
