# Tests for the database schema layer.
#
# This is a STANDALONE CMake project on purpose. It links neither Qt nor Noggit, and compiles
# only the dependency-free sources under src/noggit/database. That is what allows it to be
# built and run before Qt is available, which is most of the reason the schema layer was
# written first.
#
#   cmake -S tests -B build-tests -G "Visual Studio 17 2022" -A x64
#   cmake --build build-tests --config Release
#   ctest --test-dir build-tests -C Release --output-on-failure
#
# It can also be pulled into the main build later with add_subdirectory(tests), since
# add_test/enable_testing work either way.

cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(noggit_schema_tests CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

set(NOGGIT_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src")

add_executable(noggit_schema_tests
  SchemaModelTests.cpp
  FixtureLoader.cpp
  "${NOGGIT_SRC_DIR}/noggit/database/SchemaModel.cpp"
)

target_include_directories(noggit_schema_tests PRIVATE
  "${NOGGIT_SRC_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

# Fixtures are read from the source tree rather than copied, so a stale copy can never be
# tested against a fresh model.
target_compile_definitions(noggit_schema_tests PRIVATE
  NOGGIT_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(noggit_schema_tests PRIVATE Catch2::Catch2WithMain)

if(MSVC)
  target_compile_options(noggit_schema_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(noggit_schema_tests PRIVATE -Wall -Wextra)
endif()

enable_testing()

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(noggit_schema_tests)
