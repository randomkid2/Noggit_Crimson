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

# Sources are added only when present, so a partially-implemented tree still configures and
# the modules that do exist stay under test.
function(noggit_add_if_present list_var)
  foreach(candidate ${ARGN})
    if(EXISTS "${candidate}")
      list(APPEND ${list_var} "${candidate}")
    else()
      message(STATUS "  skipping absent source: ${candidate}")
    endif()
  endforeach()
  set(${list_var} "${${list_var}}" PARENT_SCOPE)
endfunction()

# Pure logic: no Qt, no database client. This is what makes the suite runnable on a bare
# machine, and it is deliberately the larger half of the layer.
# SpawnQueryDetail holds every SQL builder and row decoder. It is listed here, not in the
# connector group, precisely because it links no database client -- which is what lets the
# spawn-query tests run on a machine with no MySQL at all. DoctorReport is split the same way:
# the checks that need a connection live in DoctorConnectionChecks.cpp.
set(NOGGIT_DB_SOURCES "")
noggit_add_if_present(NOGGIT_DB_SOURCES
  "${NOGGIT_SRC_DIR}/noggit/database/SchemaModel.cpp"
  "${NOGGIT_SRC_DIR}/noggit/database/TileCoordinates.cpp"
  "${NOGGIT_SRC_DIR}/noggit/database/SpawnTypes.cpp"
  "${NOGGIT_SRC_DIR}/noggit/database/ChangesetBuilder.cpp"
  "${NOGGIT_SRC_DIR}/noggit/database/SpawnQueryDetail.cpp"
  "${NOGGIT_SRC_DIR}/noggit/database/ChunkTransform.cpp"
  "${NOGGIT_SRC_DIR}/noggit/database/GmCommands.cpp"
  "${NOGGIT_SRC_DIR}/noggit/database/ChangesetArchive.cpp"
  "${NOGGIT_SRC_DIR}/noggit/database/DoctorReport.cpp"
  "${NOGGIT_SRC_DIR}/noggit/database/SpawnPlacement.cpp"
  # ModelPathFixup belongs here and not with DisplayResolver, which is the point of the split:
  # the resolver needs DBC.h, ClientData and a client install, while the path rewrite is pure
  # string work and is the half that can be tested exhaustively on a bare machine.
  "${NOGGIT_SRC_DIR}/noggit/database/ModelPathFixup.cpp"
  "${NOGGIT_SRC_DIR}/noggit/database/GameTeleBuilder.cpp"
  # Under src/noggit/, not src/noggit/database/, unlike everything else here: both are
  # pure STL with no Qt, no archive and no database, so they belong in the group that
  # builds on a bare machine even though they do not live in the database directory.
  "${NOGGIT_SRC_DIR}/noggit/AssetScan.cpp"
  "${NOGGIT_SRC_DIR}/noggit/UidCollisionLog.cpp"
  "${NOGGIT_SRC_DIR}/noggit/TerrainRules.cpp"
  "${NOGGIT_SRC_DIR}/noggit/ErosionKernel.cpp"
  "${NOGGIT_SRC_DIR}/noggit/AmbientOcclusion.cpp"
  "${NOGGIT_SRC_DIR}/noggit/AlphaIntegrity.cpp"
)

set(NOGGIT_DB_TESTS "")
noggit_add_if_present(NOGGIT_DB_TESTS
  "${CMAKE_CURRENT_SOURCE_DIR}/SchemaModelTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TileCoordinatesTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/SpawnTypesTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ChangesetBuilderTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/SpawnQueryTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ChunkTransformTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GmCommandsTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ChangesetArchiveTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/DoctorReportTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/SpawnPlacementTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ModelPathFixupTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GameTeleBuilderTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/AssetScanTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/UidCollisionLogTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/TerrainRulesTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ErosionKernelTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/AmbientOcclusionTests.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/AlphaIntegrityTests.cpp"
)

if(MYSQLCPPCONN_INCLUDE AND MYSQLCPPCONN_LIBRARY)
  message(STATUS "Connector/C++ found - building live database tests")
  noggit_add_if_present(NOGGIT_DB_SOURCES
    "${NOGGIT_SRC_DIR}/noggit/database/WorldDatabaseConnection.cpp"
    "${NOGGIT_SRC_DIR}/noggit/database/SchemaIntrospector.cpp"
    "${NOGGIT_SRC_DIR}/noggit/database/SpawnQuery.cpp"
    "${NOGGIT_SRC_DIR}/noggit/database/DoctorConnectionChecks.cpp"
  )
  noggit_add_if_present(NOGGIT_DB_TESTS
    "${CMAKE_CURRENT_SOURCE_DIR}/DatabaseIntegrationTests.cpp"
  )
else()
  message(STATUS "Connector/C++ not found - pure logic tests only. "
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

# Registered by tag rather than with catch_discover_tests.
#
# catch_discover_tests parses `--list-tests` output into a CMake list, so a test NAME containing
# a comma silently corrupts the generated script: every case collapses into one entry whose
# name is all 98 names concatenated, and it reports as failed even when the binary passes.
# Several names here legitimately contain commas ("[0, 2pi)", "tile (49,31)"). Tags are ours to
# control and contain no separators, so grouping by tag is robust.
#
# The executable still reports per-case results; this only affects ctest's granularity.
#
# SKIP_RETURN_CODE 4 is what Catch2 returns when every selected case skips, which is exactly
# what happens to [integration] on a machine with no database. Without it that reads as failure.
# Tags carried by files that are ALWAYS compiled.
set(NOGGIT_TEST_TAGS
  schema drift tile spawn validation changeset sqlformat safety
  spawnquery sql chunk gm archive doctor recovery roundtrip placement frame
  modelpath display creature gameobject parse adt count tele assetscan uid
  terrainrules erosion ao alpha
)

# Tags carried ONLY by connector-gated files. Registering these unconditionally is a trap:
# without the connector those translation units are never compiled, the filter matches zero
# cases, and Catch2 exits 2 for "no tests ran" -- which is NOT the configured SKIP_RETURN_CODE
# of 4, so ctest reports a failure on a machine that is behaving exactly as intended. Verified
# empirically: a filter matching nothing returns 2. Registering them alongside their sources
# keeps a zero match a genuine error rather than an expected state.
# spawnquery and sql are no longer gated: SpawnQueryDetail.cpp links no database client, so
# those cases compile and run with no MySQL present.

enable_testing()

foreach(tag ${NOGGIT_TEST_TAGS})
  add_test(NAME "tag.${tag}"
           COMMAND noggit_schema_tests "[${tag}]" --reporter compact)
  set_tests_properties("tag.${tag}" PROPERTIES SKIP_RETURN_CODE 4)
endforeach()

# Integration is separated so it can be excluded with -LE, and so a database-less machine sees
# one clear skip rather than several. Also gated: without the connector the file does not exist.
if(MYSQLCPPCONN_INCLUDE AND MYSQLCPPCONN_LIBRARY)
  foreach(tag db integration)
    add_test(NAME "tag.${tag}"
             COMMAND noggit_schema_tests "[${tag}]" --reporter compact)
    set_tests_properties("tag.${tag}" PROPERTIES SKIP_RETURN_CODE 4 LABELS "needs-database")
  endforeach()
endif()

# Backstop: the whole suite in one run, so nothing can be missed by a tag that no case carries.
add_test(NAME "all" COMMAND noggit_schema_tests)
set_tests_properties("all" PROPERTIES SKIP_RETURN_CODE 4)
