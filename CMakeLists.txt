cmake_minimum_required(VERSION 3.10)

# Single source of truth for the release number - bump the top-level VERSION
# file (one line, e.g. "0.2.0") to release a new version; nothing else needs
# to change. See scripts/bump_git_tag.sh.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FSP_VERSION LIMIT_COUNT 1)
string(STRIP "${FSP_VERSION}" FSP_VERSION)

project(fsp VERSION ${FSP_VERSION} LANGUAGES CXX)

#
# for profiling
# cmake -B build-profile -DCMAKE_BUILD_TYPE=Profile
#

# --- 0. Top-level vs. CPM-consumed detection ---
# Whether this is the top-level project (built directly, e.g. `cmake --preset ninja-debug`)
# or pulled in as a dependency of another project (add_subdirectory()/FetchContent/
# CPMAddPackage()). Only the top-level case builds fsp's own executables/tests/coverage
# target -- a consumer of fsp via CPM only wants the fsp::fsp_lib target (see ALIAS below)
# to link against, not fsp's own dev tools. See docs/cpm-integration.md for the consumer side.
set(FSP_IS_TOP_LEVEL_PROJECT OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(FSP_IS_TOP_LEVEL_PROJECT ON)
endif()

# option()'s third argument must be a literal ON/OFF -- passing a variable through (even one
# that itself expands to ON/OFF, like ${FSP_IS_TOP_LEVEL_PROJECT}) is not reliably honored, so
# the default is picked explicitly here instead of handed to option() directly.
if(FSP_IS_TOP_LEVEL_PROJECT)
    option(FSP_BUILD_EXECUTABLES "Build fsp's own executables (validate, gen, pacs8, pacs8-cb, validator)" ON)
    option(FSP_BUILD_TESTS "Build fsp's own test executables (test_pars, t_refl) and unit_tests/coverage" ON)
else()
    option(FSP_BUILD_EXECUTABLES "Build fsp's own executables (validate, gen, pacs8, pacs8-cb, validator)" OFF)
    option(FSP_BUILD_TESTS "Build fsp's own test executables (test_pars, t_refl) and unit_tests/coverage" OFF)
endif()

# BUILD_TESTING is CTest's standard opt-out switch, defaulted to FSP_IS_TOP_LEVEL_PROJECT (see
# above) rather than an unconditional ON -- a project that CPM-consumes fsp only wants fsp_lib,
# not fsp's own Catch2 fetch and unit_tests target. Deliberately set here, BEFORE the first
# CPMAddPackage() call below: option() only takes effect on the first call that sets a given
# cache variable, and ostri/logger (fetched via CPMAddPackage(NAME logger ...) further down)
# also declares its own option(BUILD_TESTING ... ${PROJECT_IS_TOP_LEVEL}) internally -- since
# logger is never top-level when pulled in as fsp's subdirectory, its own default is OFF. If
# logger's add_subdirectory() ran before fsp's own BUILD_TESTING default was set, logger's OFF
# would win the cache race and silently disable fsp's unit_tests too.
if(FSP_IS_TOP_LEVEL_PROJECT)
    option(BUILD_TESTING "Build unit tests" ON)
else()
    option(BUILD_TESTING "Build unit tests" OFF)
endif()

# --- 1. General project settings ---
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose build type" FORCE)
endif()

set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "Profile" "Coverage")

# --- 2. Common compilation and linking parameters ---
if(MSVC)
    set(COMMON_RELEASE_FLAGS "/W4" "/WX" "/O2")
    set(COMMON_DEBUG_FLAGS   "/W4" "/Zi" "/Od")
    set(COMMON_LINK_FLAGS    "")
else()
    set(COMMON_FLAGS
        "-Wall" "-Wextra" "-Wpedantic" "-Werror" "-Wnarrowing" "-Wswitch-enum" "-march=native"
    )
    set(COMMON_RELEASE_FLAGS
        ${COMMON_FLAGS}
        "-O3" "-DNDEBUG"
        "-freflection"
        # "-fcontracts"
        # "-fcontract-evaluation-semantic=ignore"
    )
    set(COMMON_DEBUG_FLAGS
        ${COMMON_FLAGS}
        "-g" "-O0" "-gdwarf-4"
        "-fsanitize=address,undefined,leak"
        "-fno-omit-frame-pointer"
        "-freflection"
        # "-fcontracts"
        # "-fcontract-evaluation-semantic=observe"
    )
    set(COMMON_PROFILE_FLAGS
        ${COMMON_FLAGS}
        "-O3" "-DNDEBUG"
        "-g" "-gdwarf-4"
        "-fno-omit-frame-pointer"
        "-fno-optimize-sibling-calls"
        "-freflection"
        # "-fcontracts"
        # "-fcontract-evaluation-semantic=ignore"
    )
    set(COMMON_LINK_FLAGS "-fsanitize=address,undefined,leak")
    set(COMMON_PROFILE_LINK_FLAGS "")
    # No sanitizers here: ASan/UBSan instrumentation inflates gcov's branch counts with
    # paths that only exist for the sanitizer's own checks, so a Coverage build measures
    # plain -O0 execution instead, kept as close to Debug as possible otherwise.
    set(COMMON_COVERAGE_FLAGS
        ${COMMON_FLAGS}
        "-g" "-O0" "-gdwarf-4"
        "--coverage"
        "-fno-omit-frame-pointer"
        "-freflection"
    )
    set(COMMON_COVERAGE_LINK_FLAGS "--coverage")
endif()

# --- 3. Find required packages ---
# XercesC and LibXml2 stay on find_package(): they are large C libraries with
# their own (non-CPM-trivial) build systems, and the distro packages are
# already what this project builds against. Everything else below comes
# through CPM instead, pinned here rather than taken from whatever the
# distribution happens to ship, so that this file is the whole answer to
# "which versions does fsp build against".
find_package(XercesC REQUIRED)
find_package(LibXml2 REQUIRED)

# CPM_SOURCE_CACHE (see cmake/CPM.cmake) lets every build directory
# (build/debug, build/release, build/profile) share one downloaded copy.
include(cmake/CPM.cmake)

CPMAddPackage(
    NAME fmt
    GITHUB_REPOSITORY fmtlib/fmt
    GIT_TAG 12.2.0
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
    NAME spdlog
    GITHUB_REPOSITORY gabime/spdlog
    GIT_TAG v1.17.0
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
    OPTIONS
        "SPDLOG_FMT_EXTERNAL ON"
        "SPDLOG_BUILD_SHARED OFF"
        "SPDLOG_INSTALL OFF"
        "SPDLOG_BUILD_EXAMPLE OFF"
        "SPDLOG_BUILD_TESTS OFF"
)

CPMAddPackage(
    NAME magic_enum
    GITHUB_REPOSITORY Neargye/magic_enum
    GIT_TAG v0.9.7
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
)

# logger::logger - thin facade over spdlog, replaces the former in-tree
# fsp_logger (src/common/logger, now removed). Not a singleton: every
# consumer takes a logger::Logger& handed down from main(), see
# src/test/pacs8.cpp / pacs8-cb.cpp. Publishes fmt::fmt and stdc++exp
# PUBLIC itself, so nothing below needs to link either just for logging.
# Must come after the fmt/spdlog CPMAddPackage() calls above with matching
# GIT_TAG/OPTIONS -- CPM de-duplicates by NAME, so logger's own internal
# fmt/spdlog fetch becomes a no-op against the ones already resolved here;
# a version/option mismatch would otherwise be an ODR violation at link time.
CPMAddPackage(
    NAME logger
    GITHUB_REPOSITORY ostri/logger
    GIT_TAG v0.1.10
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
)

# --- 3a. Unit testing (Catch2, fetched via CPM) ---
# BUILD_TESTING itself is set at the top of this file (before any CPMAddPackage() call --
# see the comment there for why the ordering matters).
if(BUILD_TESTING AND FSP_BUILD_TESTS)
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        GIT_TAG v3.15.3
        SYSTEM YES
        EXCLUDE_FROM_ALL YES
    )
    # catch_discover_tests lives in Catch2's extras/ and registers one ctest
    # entry per TEST_CASE rather than one for the whole binary.
    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
    include(CTest)
    include(Catch)
endif()

# --- 4. Function to define a target with common settings ---
function(add_fsp_target TARGET_NAME TYPE)
    # TYPE can be STATIC, SHARED, or EXECUTABLE
    if(TYPE STREQUAL "EXECUTABLE")
        add_executable(${TARGET_NAME} ${ARGN})
    else()
        add_library(${TARGET_NAME} ${TYPE} ${ARGN})
    endif()

    # Common include paths (can be extended as needed)
    target_include_directories(${TARGET_NAME} PUBLIC
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/importer
        ${PROJECT_SOURCE_DIR}/src/exporter
        ${PROJECT_SOURCE_DIR}/src/common
        ${PROJECT_SOURCE_DIR}/src/common/lock_queue
        ${PROJECT_SOURCE_DIR}/src/common/mmap
        ${PROJECT_SOURCE_DIR}/src/common/xercesc_helpers
    )

    # Additional include paths provided by the caller via APPEND_INCLUDES
    if(DEFINED FSP_TARGET_${TARGET_NAME}_INCLUDES)
        target_include_directories(${TARGET_NAME} PRIVATE ${FSP_TARGET_${TARGET_NAME}_INCLUDES})
    endif()

    # Compile options based on build type
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(${TARGET_NAME} PRIVATE ${COMMON_RELEASE_FLAGS})
    # Enable Link Time Optimization (LTO) specifically for Release targets
        set_target_properties(${TARGET_NAME} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Profile")
        target_compile_options(${TARGET_NAME} PRIVATE ${COMMON_PROFILE_FLAGS})
        set_target_properties(${TARGET_NAME} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
        if(NOT TYPE STREQUAL "STATIC")
            target_link_options(${TARGET_NAME} PRIVATE ${COMMON_PROFILE_LINK_FLAGS})
        endif()
    elseif(CMAKE_BUILD_TYPE STREQUAL "Coverage")
        target_compile_options(${TARGET_NAME} PRIVATE ${COMMON_COVERAGE_FLAGS})
        # --coverage must be on the link line too (it pulls in libgcov) even for a STATIC
        # library target, unlike the sanitizer flags above which only executables/shared
        # libraries need -- a static lib has no link step of its own, but the final
        # executable that links it still needs every object file's gcov instrumentation.
        target_link_options(${TARGET_NAME} PRIVATE ${COMMON_COVERAGE_LINK_FLAGS})
    else()
        target_compile_options(${TARGET_NAME} PRIVATE ${COMMON_DEBUG_FLAGS})
        if(NOT TYPE STREQUAL "STATIC")  # Only for executable and shared library
            target_link_options(${TARGET_NAME} PRIVATE ${COMMON_LINK_FLAGS})
        endif()
    endif()

    # Macro definitions based on configuration
    target_compile_definitions(${TARGET_NAME} PRIVATE
        $<$<CONFIG:Debug>:DEBUG_MODE>
        $<$<CONFIG:Release>:RELEASE_MODE>
        $<$<CONFIG:Profile>:RELEASE_MODE>
        $<$<CONFIG:Coverage>:DEBUG_MODE>

        $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
        $<$<CONFIG:Release>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
        $<$<CONFIG:Profile>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
        $<$<CONFIG:Coverage>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    )

    # Link with external libraries if provided
    if(DEFINED FSP_TARGET_${TARGET_NAME}_LIBS)
        target_link_libraries(${TARGET_NAME} PRIVATE ${FSP_TARGET_${TARGET_NAME}_LIBS})
    endif()
endfunction()

# Copies whichever ONE of the two checked-in logger config files (config/log.debug.json,
# config/log.release.json) matches this preset's own CMAKE_BUILD_TYPE into the build directory,
# as log.conf -- every fsp executable target lands directly in CMAKE_BINARY_DIR (no per-target
# RUNTIME_OUTPUT_DIRECTORY is set), so one copy right there is reachable from any of them via
# fsp::exe_dir() at runtime. Debug/Coverage (both -O0, no NDEBUG) get log.debug.json;
# Release/Profile (both optimized, NDEBUG defined) get log.release.json -- the same split
# COMMON_*_FLAGS below already draws between these four preset families. The choice is made
# HERE, at configure/build time, deliberately -- NOT at run time by a program picking a name
# out of two present files (the old scheme this replaces): a build's own log.conf must always
# match the binary it was actually compiled with, and a single generic name means
# load_program_logger_config() (pacs8.cpp/pacs8-cb.cpp) needs no build-type logic of its own,
# just fsp::exe_dir() / "log.conf". app_name (used for the log file name and spdlog's %n) is not
# baked into the file -- it is set in code from argv[0] after loading it, since one shared
# config covers every fsp program (see pacs8.cpp's call site comment). configure_file(...,
# COPYONLY) also makes CMake re-run (and thus re-copy) whenever the source file changes, even
# without an explicit `cmake ..`. TARGET_NAME is unused now that the destination no longer
# depends on it, but kept as a parameter so call sites (pacs8, pacs8-cb) don't need to change if
# per-target output directories are introduced later.
function(add_log_config TARGET_NAME)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "Coverage")
        set(log_config_src ${PROJECT_SOURCE_DIR}/config/log.debug.json)
    else() # Release, Profile
        set(log_config_src ${PROJECT_SOURCE_DIR}/config/log.release.json)
    endif()
    configure_file(${log_config_src} ${CMAKE_BINARY_DIR}/log.conf COPYONLY)
endfunction()

# WORKAROUND (remove once GCC 16.2 ships): on this GCC 16.1.1 -freflection branch,
# -fsanitize=undefined breaks consteval evaluation of annotation_text() in
# src/importer/reflection.hpp -- reading a [[= "..."]] annotation back via
# display_string_of()/define_static_array() three consteval calls deep
# (proc_data_of -> classes_of/fields_of -> attr_of/field_attr_of) turns into
# "not a constant expression" only under UBSan. -fsanitize=address and
# -fsanitize=leak are unaffected, so only "undefined" needs suppressing, and only
# for the translation units that actually instantiate proc_data_of<^^Namespace>()
# (or similar) over a reflected schema namespace.
function(disable_ubsan_for_reflection)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=undefined")
    endif()
endfunction()

set(SRC_IMPORTER
    ${PROJECT_SOURCE_DIR}/src/importer/handler.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/xml_segment.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/error_info.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/importer_config.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/xpath_helpers.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/parsing_util.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/e_tag_wide.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/xml_node.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/xml_attr.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/xpath_el.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/segment_result.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/result_values.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/pipeline_hooks.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/xml_worker.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/load_grammar.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/importer.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/stats.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/segment_sax.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/xpath_set.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/doc_dscr.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/doc_set_dscr.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/doc_counters.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/doc_set_counter.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/segment_pool.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/doc_cutter.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/pipeline_worker.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/pipeline.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/doc_validator.cpp
)

set(SRC_EXPORTER
    ${PROJECT_SOURCE_DIR}/src/exporter/xml_writer.cpp
    ${PROJECT_SOURCE_DIR}/src/exporter/exporter_error.cpp
    ${PROJECT_SOURCE_DIR}/src/exporter/exporter_state.cpp
)

set(SRC_COMMON
    ${PROJECT_SOURCE_DIR}/src/common/xercesc_helpers/x_str.cpp
    ${PROJECT_SOURCE_DIR}/src/common/xercesc_helpers/xerces_mgr.cpp
    ${PROJECT_SOURCE_DIR}/src/common/lock_queue/lock_queue.cpp
    ${PROJECT_SOURCE_DIR}/src/common/mmap/mmap_file.cpp
    ${PROJECT_SOURCE_DIR}/src/common/common.cpp
    ${PROJECT_SOURCE_DIR}/src/common/exe_path.cpp
    ${PROJECT_SOURCE_DIR}/src/ach/utility.cpp
)

set(SOURCES
    ${SRC_IMPORTER}
    ${SRC_EXPORTER}
    ${SRC_COMMON}
)
# --- 5. Define targets ---
set(b_target fsp_lib) # name of the target
# 5.1 fsp_lib (static library)
set(${b_target}_SOURCES
#    ${PROJECT_SOURCE_DIR}/src/main.cpp
    ${SOURCES}
)
# Specify additional libraries for fsp_lib (if needed)
# fmt/magic_enum weren't listed here even under find_package(), which
# went unnoticed only because that fmt was consistent between compile
# and link across every consumer (all from the same distro package). Now that
# fmt comes from CPM as a compiled library (not header-only), a fsp_lib
# .cpp that calls fmt::vformat (via error_info.hpp etc.) has an unresolved
# symbol unless fsp_lib links fmt::fmt itself -- so it, and magic_enum, are
# explicit here. logger::logger publishes spdlog and fmt::fmt PUBLIC itself
# (see the CPMAddPackage(logger) comment above), so it is the only thing
# fsp_lib needs to link for logging -- no separate spdlog::spdlog.
set(FSP_TARGET_${b_target}_LIBS
    XercesC::XercesC
    LibXml2::LibXml2
    fmt::fmt
    logger::logger
    magic_enum::magic_enum
)
add_fsp_target(fsp_lib STATIC ${fsp_lib_SOURCES})
# fsp::fsp_lib -- namespaced alias, the stable name a CPM consumer links against
# (target_link_libraries(my_app PRIVATE fsp::fsp_lib)), same convention as logger::logger,
# fmt::fmt, XercesC::XercesC below. See docs/cpm-integration.md.
add_library(fsp::fsp_lib ALIAS fsp_lib)

# --- fsp's own executables/tests: gated behind FSP_BUILD_EXECUTABLES/FSP_BUILD_TESTS ---
# (both default to FSP_IS_TOP_LEVEL_PROJECT, see top of file). A project that CPM-consumes fsp
# only needs fsp_lib itself; none of validate/gen/test_pars/t_refl/pacs8/pacs8-cb/validator/
# unit_tests are part of the library API.
if(FSP_BUILD_EXECUTABLES)
#
# --- target: validate --------------------------------------------------
#
set(b_target validate) # name of the target
set(${b_target}_SOURCES
    ${PROJECT_SOURCE_DIR}/src/auxiliary/validate.cpp
    ${PROJECT_SOURCE_DIR}/src/common/xercesc_helpers/xerces_mgr.cpp
    ${PROJECT_SOURCE_DIR}/src/common/xercesc_helpers/x_str.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES ${PROJECT_SOURCE_DIR}/src/auxiliary)
set(FSP_TARGET_${b_target}_LIBS
    fmt::fmt
    XercesC::XercesC
)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
#
# --- target: gen
#
set(b_target gen) # name of the target
set(${b_target}_SOURCES
    ${PROJECT_SOURCE_DIR}/src/auxiliary/gen.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES ${PROJECT_SOURCE_DIR}/src/auxiliary)
set(FSP_TARGET_${b_target}_LIBS
    fsp_lib
    XercesC::XercesC
)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
endif() # FSP_BUILD_EXECUTABLES
# -----------------------------------------------------------------------------
# --- target: test_pars
# -----------------------------------------------------------------------------
if(FSP_BUILD_TESTS)
set(b_target test_pars) # name of the target
set(${b_target}_SOURCES
    ${PROJECT_SOURCE_DIR}/src/test/test_pars.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/parsing_util.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${PROJECT_SOURCE_DIR}/src/importer
  ${PROJECT_SOURCE_DIR}/src/test
  )
set(FSP_TARGET_${b_target}_LIBS
    fmt::fmt
    spdlog::spdlog
    magic_enum::magic_enum
)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
# -----------------------------------------------------------------------------
# --- target: t_refl
# -----------------------------------------------------------------------------
set(b_target t_refl) # name of the target
set(${b_target}_SOURCES
    ${PROJECT_SOURCE_DIR}/src/test/t_refl.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/parsing_util.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${PROJECT_SOURCE_DIR}/src/importer
  ${PROJECT_SOURCE_DIR}/src/test
  )
set(FSP_TARGET_${b_target}_LIBS
    fmt::fmt
    spdlog::spdlog
    magic_enum::magic_enum
)
disable_ubsan_for_reflection(${PROJECT_SOURCE_DIR}/src/test/t_refl.cpp)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
endif() # FSP_BUILD_TESTS
# -----------------------------------------------------------------------------
# --- target: pacs8
# -----------------------------------------------------------------------------
if(FSP_BUILD_EXECUTABLES)
set(b_target pacs8) # name of the target
set(${b_target}_SOURCES
    ${PROJECT_SOURCE_DIR}/src/test/pacs8.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${PROJECT_SOURCE_DIR}/src/importer
  ${PROJECT_SOURCE_DIR}/src/test
  )
set(FSP_TARGET_${b_target}_LIBS
    fsp_lib
    fmt::fmt
    logger::logger
    magic_enum::magic_enum
)
disable_ubsan_for_reflection(${PROJECT_SOURCE_DIR}/src/test/pacs8.cpp)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
add_log_config(${b_target})
# -----------------------------------------------------------------------------
# --- target: pacs8-cb (same as pacs8, but wires an explicit custom pipeline_hooks class)
# -----------------------------------------------------------------------------
set(b_target pacs8-cb) # name of the target
set(${b_target}_SOURCES
    ${PROJECT_SOURCE_DIR}/src/test/pacs8-cb.cpp
    ${PROJECT_SOURCE_DIR}/src/test/pacs8_cb.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${PROJECT_SOURCE_DIR}/src/importer
  ${PROJECT_SOURCE_DIR}/src/test
  )
set(FSP_TARGET_${b_target}_LIBS
    fsp_lib
    fmt::fmt
    logger::logger
    magic_enum::magic_enum
)
disable_ubsan_for_reflection(${PROJECT_SOURCE_DIR}/src/test/pacs8-cb.cpp)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
add_log_config(${b_target})
# -----------------------------------------------------------------------------
# --- target: validator
# -----------------------------------------------------------------------------
set(b_target validator) # name of the target
set(${b_target}_SOURCES
    ${PROJECT_SOURCE_DIR}/src/auxiliary/validator.cpp
    ${PROJECT_SOURCE_DIR}/src/common/xercesc_helpers/xerces_mgr.cpp
    ${PROJECT_SOURCE_DIR}/src/common/xercesc_helpers/x_str.cpp
    ${PROJECT_SOURCE_DIR}/src/importer/load_grammar.cpp
    ${PROJECT_SOURCE_DIR}/src/common/mmap/mmap_file.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${PROJECT_SOURCE_DIR}/src/importer
  ${PROJECT_SOURCE_DIR}/src/test
  ${XERCESC_INCLUDE_DIR}
  )
set(FSP_TARGET_${b_target}_LIBS PRIVATE
    fmt::fmt
    spdlog::spdlog
    magic_enum::magic_enum
    XercesC::XercesC
)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
endif() # FSP_BUILD_EXECUTABLES
# -----------------------------------------------------------------------------
# --- target: unit_tests
# -----------------------------------------------------------------------------
if(BUILD_TESTING AND FSP_BUILD_TESTS)
    set(b_target unit_tests) # name of the target
    set(${b_target}_SOURCES
        ${PROJECT_SOURCE_DIR}/src/test/test_x_str.cpp
        ${PROJECT_SOURCE_DIR}/src/test/test_xerces_mgr.cpp
        ${PROJECT_SOURCE_DIR}/src/test/test_lock_queue.cpp
        ${PROJECT_SOURCE_DIR}/src/test/test_mmap_file.cpp
        ${PROJECT_SOURCE_DIR}/src/test/test_xml_writer.cpp
        ${PROJECT_SOURCE_DIR}/src/test/test_exporter_error.cpp
        ${PROJECT_SOURCE_DIR}/src/test/test_exporter_state.cpp
        ${PROJECT_SOURCE_DIR}/src/test/test_cb_exporter.cpp
        ${PROJECT_SOURCE_DIR}/src/test/test_exporter.cpp
        ${PROJECT_SOURCE_DIR}/src/test/test_pipeline_stages.cpp
        ${PROJECT_SOURCE_DIR}/src/test/test_value_set_t.cpp
        ${PROJECT_SOURCE_DIR}/src/common/xercesc_helpers/x_str.cpp
        ${PROJECT_SOURCE_DIR}/src/common/xercesc_helpers/xerces_mgr.cpp
        ${PROJECT_SOURCE_DIR}/src/common/mmap/mmap_file.cpp
        ${PROJECT_SOURCE_DIR}/src/exporter/exporter_error.cpp
        ${PROJECT_SOURCE_DIR}/src/exporter/exporter_state.cpp
    )
    set(FSP_TARGET_${b_target}_INCLUDES
        ${PROJECT_SOURCE_DIR}/src/test
    )
    set(FSP_TARGET_${b_target}_LIBS
        Catch2::Catch2WithMain
        fsp_lib
        fmt::fmt
        logger::logger
        magic_enum::magic_enum
        XercesC::XercesC
    )
    add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
    # test_pipeline_stages.cpp instantiates fsp::proc_data_of<^^fsp::work>() (see the same note on
    # pacs8.cpp/pacs8-cb.cpp/t_refl.cpp above disable_ubsan_for_reflection()).
    disable_ubsan_for_reflection(${PROJECT_SOURCE_DIR}/src/test/test_pipeline_stages.cpp)
    # FSP_TEST_SOURCE_DIR: absolute path to the repo root, so test_pipeline_stages.cpp can locate
    # xsd/pacs.008.xsd regardless of which directory ctest/the raw binary is invoked from.
    target_compile_definitions(${b_target} PRIVATE FSP_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    # Catch2's INTERNAL_CATCH_UNIQUE_NAME expands to __COUNTER__, flagged by
    # -Wpedantic as a C2y extension on Clang (silent on GCC); a third-party
    # macro this project doesn't control, so it is silenced rather than
    # allowed to trip -Werror.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${b_target} PRIVATE -Wno-c2y-extensions)
    endif()
    catch_discover_tests(${b_target})

    # --- coverage report (only meaningful for -DCMAKE_BUILD_TYPE=Coverage, see
    # COMMON_COVERAGE_FLAGS/COMMON_COVERAGE_LINK_FLAGS above) ---
    # Runs unit_tests to produce .gcda files next to the .o's, then points gcovr at the
    # build directory: --root/--filter restrict the report to this project's own src/ (excluding
    # CPM-fetched third-party sources, which also get instrumented but aren't ours to cover),
    # and -e src/test/ excludes the test files themselves from being counted as "covered code".
    find_program(GCOVR_EXECUTABLE gcovr)
    if(GCOVR_EXECUTABLE)
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage
            COMMAND $<TARGET_FILE:${b_target}>
            COMMAND ${GCOVR_EXECUTABLE}
                --root ${PROJECT_SOURCE_DIR}
                --filter ${PROJECT_SOURCE_DIR}/src/
                --exclude ${PROJECT_SOURCE_DIR}/src/test/
                --exclude-unreachable-branches
                --exclude-throw-branches
                --print-summary
                --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
                ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS ${b_target}
            COMMENT "Running unit_tests and generating gcovr HTML coverage report in build/coverage/index.html"
            VERBATIM
        )
    else()
        message(STATUS "gcovr not found -- 'coverage' target not available (install gcovr to enable it)")
    endif()
endif()

# ===================================================================
# bump_git_tag - maintainer-only release helper, not a build step: bumps
# this project's own PATCH version by 1 (source of truth is the top-level
# VERSION file), commits, tags, and pushes both to origin - see
# scripts/bump_git_tag.sh for the actual logic (refuses to run on a dirty
# working tree). PROJECT_IS_TOP_LEVEL guarded: a consumer pulling this in
# via add_subdirectory()/CPMAddPackage() has no business tagging/pushing
# this repository's own releases from its build.
# ===================================================================
if(PROJECT_IS_TOP_LEVEL)
    add_custom_target(bump_git_tag
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/bump_git_tag.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Bumping version, committing, tagging, and pushing to origin"
        VERBATIM
        USES_TERMINAL
    )
endif()
#-------------------------------------------------------------------------------
# --- 6. (Optional) Display built targets ---
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "fsp sources: ${${b_target}_SOURCES}")
# Add a custom target that runs during every build to print the build type
add_custom_target(print_build_type ALL
    COMMAND ${CMAKE_COMMAND} -E echo "Building project in ${CMAKE_BUILD_TYPE} mode."
)