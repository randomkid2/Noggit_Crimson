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

# MySQL Connector/C++ is optional here. Without it the schema model is still fully tested
# against fixtures; with it, the live introspector and connection layer are tested too. Same
# hint locations the root CMakeLists uses, one directory deeper.
set(NOGGIT_LIBS_HINT "${CMAKE_CURRENT_SOURCE_DIR}/../../Noggit3libs/mysql")

find_path(MYSQLCPPCONN_INCLUDE NAMES cppconn/driver.h HINTS "${NOGGIT_LIBS_HINT}/connector")
find_library(MYSQLCPPCONN_LIBRARY NAMES mysqlcppconn HINTS "${NOGGIT_LIBS_HINT}/connector")

set(NOGGIT_DB_SOURCES "${NOGGIT_SRC_DIR}/noggit/database/SchemaModel.cpp")
set(NOGGIT_DB_TESTS SchemaModelTests.cpp)

if(MYSQLCPPCONN_INCLUDE AND MYSQLCPPCONN_LIBRARY)
  message(STATUS "Connector/C++ found - building live database tests")
  list(APPEND NOGGIT_DB_SOURCES
    "${NOGGIT_SRC_DIR}/noggit/database/WorldDatabaseConnection.cpp"
    "${NOGGIT_SRC_DIR}/noggit/database/SchemaIntrospector.cpp"
  )
  list(APPEND NOGGIT_DB_TESTS DatabaseIntegrationTests.cpp)
else()
  message(STATUS "Connector/C++ not found - schema model tests only. "
                 "Set MYSQLCPPCONN_INCLUDE and MYSQLCPPCONN_LIBRARY to enable the rest.")
endif()

add_executable(noggit_schema_tests
  ${NOGGIT_DB_TESTS}
  FixtureLoader.cpp
  ${NOGGIT_DB_SOURCES}
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

if(MYSQLCPPCONN_INCLUDE AND MYSQLCPPCONN_LIBRARY)
  target_include_directories(noggit_schema_tests SYSTEM PRIVATE "${MYSQLCPPCONN_INCLUDE}")
  target_link_libraries(noggit_schema_tests PRIVATE "${MYSQLCPPCONN_LIBRARY}")

  # The connector's runtime DLL lives one level ABOVE the vs14/ directory holding the import
  # library, which is easy to miss and produces a delay-load failure at first run rather than a
  # link error.
  file(GLOB MYSQLCPPCONN_RUNTIME
    "${NOGGIT_LIBS_HINT}/connector/mysqlcppconn-*.dll"
    "${NOGGIT_LIBS_HINT}/*.dll"
  )
  foreach(dll ${MYSQLCPPCONN_RUNTIME})
    add_custom_command(TARGET noggit_schema_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${dll}" "$<TARGET_FILE_DIR:noggit_schema_tests>"
    )
  endforeach()
endif()

if(MSVC)
  target_compile_options(noggit_schema_tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(noggit_schema_tests PRIVATE -Wall -Wextra)
endif()

enable_testing()

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

# Catch2 exits 4 when a test case skips. Without this, ctest reports every skipped integration
# test as a failure on a machine with no database, which defeats the point of skipping.
catch_discover_tests(noggit_schema_tests PROPERTIES SKIP_RETURN_CODE 4)
