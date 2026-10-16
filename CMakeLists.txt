# skyrim-cpp - build for the portable library and its playable host.
#
# Standalone: configure and build this directory on its own.
#
#     cmake -S . -B build
#     cmake --build build --config Release
#     ctest --test-dir build -C Release
#
cmake_minimum_required(VERSION 3.20)
project(skyrim_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # portable: no compiler-specific syntax

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Library - portable C++20, no renderer, no main(), standard library only.
# ---------------------------------------------------------------------------

add_library(skyrim_module STATIC
  src/world/inflate.cpp
  src/world/bsa_archive.cpp
  src/world/form_database.cpp
  src/world/string_table.cpp
  src/world/cell_system.cpp
  src/world/land.cpp
  src/world/texture.cpp
  src/world/nif.cpp
  src/world/world_render.cpp
  src/world/collision.cpp
  src/world/skyrim_world.cpp
)

target_include_directories(skyrim_module
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

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
# Host - the playable program built on the library.
# ---------------------------------------------------------------------------
# A separate target so the library never gains a main() or a renderer. The
# Direct3D 9 renderer and Win32 window live under host/engine/ and are thus
# Windows-only; the library builds everywhere.

add_executable(skyrim_standalone host/main.cpp host/engine/install.cpp)
target_link_libraries(skyrim_standalone PRIVATE skyrim_module)

if(WIN32)
  target_sources(skyrim_standalone PRIVATE
    host/engine/d3d9_renderer.cpp
    host/engine/game.cpp
    host/engine/engine.cpp
  )
  target_link_libraries(skyrim_standalone PRIVATE d3d9 shell32 ole32 advapi32)
else()
  target_sources(skyrim_standalone PRIVATE host/engine/engine_stub.cpp)
endif()

if(MSVC)
  target_compile_options(skyrim_standalone PRIVATE /W4 /permissive-)
  target_compile_definitions(skyrim_standalone PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(skyrim_standalone PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Developer tools
# ---------------------------------------------------------------------------

option(SKYRIM_BUILD_TOOLS "Build skyrim-cpp's developer tools" ON)
if(SKYRIM_BUILD_TOOLS)
  add_executable(bsa_probe tools/bsa_probe.cpp)
  target_link_libraries(bsa_probe PRIVATE skyrim_module)
  add_executable(cell_demo tools/cell_demo.cpp)
  target_link_libraries(cell_demo PRIVATE skyrim_module)
  add_executable(mesh_probe tools/mesh_probe.cpp)
  target_link_libraries(mesh_probe PRIVATE skyrim_module)
  add_executable(world_probe tools/world_probe.cpp)
  target_link_libraries(world_probe PRIVATE skyrim_module)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
# The repository contains no binary files. The synthetic fixtures the tests
# read are generated from tests/make_*.py into the build tree, so a checkout is
# pure source and `ctest` needs no game install. Python is required only to
# generate the fixtures; if it is missing the tests are skipped.

option(SKYRIM_BUILD_TESTS "Build skyrim-cpp's unit tests" ON)
if(SKYRIM_BUILD_TESTS)
  enable_testing()
  find_program(SKYRIM_PYTHON NAMES python python3 py)

  if(NOT SKYRIM_PYTHON)
    message(WARNING
      "Python not found: unit-test fixtures cannot be generated, so the tests "
      "are disabled. Install Python 3 and re-run CMake to enable them.")
  else()
    set(SKYRIM_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")

    # skyrim_fixture(<name> <script> [extra args...]) generates one fixture
    # directory and exposes it as a target the test executable depends on.
    function(skyrim_fixture name script)
      set(dir "${SKYRIM_FIXTURE_DIR}/${name}")
      add_custom_command(
        OUTPUT "${dir}/.stamp"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${dir}"
        COMMAND "${SKYRIM_PYTHON}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/${script}" "${dir}" ${ARGN}
        COMMAND "${CMAKE_COMMAND}" -E touch "${dir}/.stamp"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${script}"
        COMMENT "Generating ${name} test fixtures"
        VERBATIM
      )
      add_custom_target(skyrim_fixture_${name} ALL DEPENDS "${dir}/.stamp")
    endfunction()

    skyrim_fixture(inflate     make_fixtures.py)
    skyrim_fixture(bsa         make_bsa_fixture.py)
    skyrim_fixture(strings     make_strings_fixture.py)
    skyrim_fixture(cell        make_cell_fixture.py)
    skyrim_fixture(dds         make_dds_fixture.py)
    skyrim_fixture(nif         make_nif_fixture.py)
    skyrim_fixture(worldrender make_worldrender_fixture.py)

    function(skyrim_test name target fixture_dir)
      add_test(NAME ${name} COMMAND ${target} "${fixture_dir}")
      add_dependencies(${target} skyrim_fixture_${name})
    endfunction()

    add_executable(inflate_test tests/inflate_test.cpp)
    target_link_libraries(inflate_test PRIVATE skyrim_module)
    target_include_directories(inflate_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    skyrim_test(inflate inflate_test "${SKYRIM_FIXTURE_DIR}/inflate")

    add_executable(bsa_test tests/bsa_test.cpp)
    target_link_libraries(bsa_test PRIVATE skyrim_module)
    skyrim_test(bsa bsa_test "${SKYRIM_FIXTURE_DIR}/bsa")

    add_executable(strings_test tests/strings_test.cpp)
    target_link_libraries(strings_test PRIVATE skyrim_module)
    add_test(NAME strings
             COMMAND strings_test "${SKYRIM_FIXTURE_DIR}/strings/Skyrim_English.STRINGS")
    add_dependencies(strings_test skyrim_fixture_strings)

    add_executable(cell_test tests/cell_test.cpp)
    target_link_libraries(cell_test PRIVATE skyrim_module)
    skyrim_test(cell cell_test "${SKYRIM_FIXTURE_DIR}/cell")

    add_executable(texture_test tests/texture_test.cpp)
    target_link_libraries(texture_test PRIVATE skyrim_module)
    skyrim_test(dds texture_test "${SKYRIM_FIXTURE_DIR}/dds")

    add_executable(nif_test tests/nif_test.cpp)
    target_link_libraries(nif_test PRIVATE skyrim_module)
    skyrim_test(nif nif_test "${SKYRIM_FIXTURE_DIR}/nif")

    add_executable(world_render_test tests/world_render_test.cpp)
    target_link_libraries(world_render_test PRIVATE skyrim_module)
    skyrim_test(worldrender world_render_test "${SKYRIM_FIXTURE_DIR}/worldrender")

    # land_test builds its LAND payload in memory, so it needs no fixture.
    add_executable(land_test tests/land_test.cpp)
    target_link_libraries(land_test PRIVATE skyrim_module)
    add_test(NAME land COMMAND land_test)

    # Like land_test, collision_test builds its meshes in memory.
    add_executable(collision_test tests/collision_test.cpp)
    target_link_libraries(collision_test PRIVATE skyrim_module)
    add_test(NAME collision COMMAND collision_test)
  endif()
endif()
