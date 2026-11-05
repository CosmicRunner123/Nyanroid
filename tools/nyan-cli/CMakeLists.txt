find_package(Threads REQUIRED)

# main.cpp dispatches to one command file per engine.
add_executable(nyan-cli main.cpp cs2_commands.cpp wa2_commands.cpp diesel_commands.cpp)
target_include_directories(nyan-cli PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
target_link_libraries(nyan-cli PRIVATE nyan_core Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nyan-cli PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
