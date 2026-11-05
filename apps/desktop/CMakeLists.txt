# nyan-desktop: SDL3 front end used to develop and test the engine on Linux.
# SDL3 is built from source (static) so no system -dev packages are needed.

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)    # audio goes through PulseAudio (see pulse_output.cpp)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.2.8
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

add_executable(nyan-desktop main.cpp pulse_output.cpp)
# Fonts bundled with Nyanroid (not in git; see fonts/README.md).
target_compile_definitions(nyan-desktop PRIVATE NYAN_BUNDLED_FONT_DIR="${CMAKE_SOURCE_DIR}/fonts")
target_include_directories(nyan-desktop PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
target_link_libraries(nyan-desktop PRIVATE nyan_render SDL3::SDL3-static ${CMAKE_DL_LIBS})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nyan-desktop PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()
