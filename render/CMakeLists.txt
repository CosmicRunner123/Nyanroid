# nyan_render: the Session the front ends drive (session.hpp) and each engine's implementation of
# it, drawn with OpenGL ES 3.0 (CatSystem2: GameSession and the renderer for cs2::Stage). Shared by
# the desktop player and Android.

add_library(nyan_render STATIC
  src/font.cpp
  src/session.cpp
  src/stage_renderer.cpp
  src/game_session.cpp
  src/wa2_session.cpp
  src/wa2/renderer.cpp
  # Diesel Engine
  src/diesel_session.cpp
  src/diesel/renderer.cpp
  src/stb_truetype_impl.cpp
)

target_include_directories(nyan_render
  PUBLIC include ${PROJECT_SOURCE_DIR}/third_party/khronos
  PRIVATE src ${PROJECT_SOURCE_DIR}/third_party)

target_link_libraries(nyan_render PUBLIC nyan_core)

if(ANDROID)
  target_link_libraries(nyan_render PUBLIC GLESv3)
else()
  find_library(GLESV2_LIBRARY NAMES GLESv2 REQUIRED)
  target_link_libraries(nyan_render PUBLIC ${GLESV2_LIBRARY})
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nyan_render PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()
set_source_files_properties(src/stb_truetype_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
