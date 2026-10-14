# Skyrim engine module - library target for the TES:SA integration.
#
# Standalone-configurable on purpose: this can be configured and built without
# any parent project, so the module can be developed and tested in isolation.
# At integration time a root CMakeLists will add_subdirectory() this.
#
#     cmake -S . -B build
#     cmake --build build
#
cmake_minimum_required(VERSION 3.16)
project(skyrim_module LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # portable: no compiler-specific syntax

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(skyrim_module STATIC
  src/world/inflate.cpp
  src/world/bsa_archive.cpp
  src/world/form_database.cpp
  src/world/string_table.cpp
  src/world/cell_system.cpp
  src/world/texture.cpp
  src/world/nif.cpp
  src/world/world_render.cpp
  src/world/skyrim_world.cpp
)

target_include_directories(skyrim_module
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# The module is cross-platform C++20 with no third-party dependencies, so the
# only thing it needs is a modern standard library.
target_compile_features(skyrim_module PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(skyrim_module PRIVATE /W4 /permissive-)
  target_compile_definitions(skyrim_module PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(skyrim_module PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(skyrim_module PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# ---------------------------------------------------------------------------
# Standalone host ("Skyrim only" launcher mode)
# ---------------------------------------------------------------------------
# Deliberately a SEPARATE target outside the library sources, so the library
# itself never gains a main() or a dependency on the shell.

add_executable(skyrim_standalone host/main.cpp)
target_link_libraries(skyrim_standalone PRIVATE skyrim_module)

if(MSVC)
  target_compile_options(skyrim_standalone PRIVATE /W4 /permissive-)
else()
  target_compile_options(skyrim_standalone PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

option(SKYRIM_BUILD_TOOLS "Build the module's developer tools" ON)
if(SKYRIM_BUILD_TOOLS)
  add_executable(bsa_probe tools/bsa_probe.cpp)
  target_link_libraries(bsa_probe PRIVATE skyrim_module)
  add_executable(cell_demo tools/cell_demo.cpp)
  target_link_libraries(cell_demo PRIVATE skyrim_module)
  add_executable(mesh_probe tools/mesh_probe.cpp)
  target_link_libraries(mesh_probe PRIVATE skyrim_module)
endif()

option(SKYRIM_BUILD_TESTS "Build the module's unit tests" ON)
if(SKYRIM_BUILD_TESTS)
  enable_testing()
  add_executable(inflate_test tests/inflate_test.cpp)
  target_link_libraries(inflate_test PRIVATE skyrim_module)
  target_include_directories(inflate_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME inflate
           COMMAND inflate_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data)

  add_executable(bsa_test tests/bsa_test.cpp)
  target_link_libraries(bsa_test PRIVATE skyrim_module)
  add_test(NAME bsa COMMAND bsa_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/bsa_fixture)

  add_executable(strings_test tests/strings_test.cpp)
  target_link_libraries(strings_test PRIVATE skyrim_module)
  add_test(NAME strings COMMAND strings_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/strings/Skyrim_English.STRINGS)

  add_executable(cell_test tests/cell_test.cpp)
  target_link_libraries(cell_test PRIVATE skyrim_module)
  add_test(NAME cell COMMAND cell_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cell_fixture)

  add_executable(texture_test tests/texture_test.cpp)
  target_link_libraries(texture_test PRIVATE skyrim_module)
  add_test(NAME texture COMMAND texture_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/dds_fixture)

  add_executable(nif_test tests/nif_test.cpp)
  target_link_libraries(nif_test PRIVATE skyrim_module)
  add_test(NAME nif COMMAND nif_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/nif_fixture)

  add_executable(world_render_test tests/world_render_test.cpp)
  target_link_libraries(world_render_test PRIVATE skyrim_module)
  add_test(NAME world_render
           COMMAND world_render_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/worldrender_fixture)
endif()
