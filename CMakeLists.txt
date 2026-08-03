cmake_minimum_required(VERSION 3.10)
project(fsp LANGUAGES CXX)

#
# for profiling
# cmake -B build-profile -DCMAKE_BUILD_TYPE=Profile
#

# --- 1. General project settings ---
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose build type" FORCE)
endif()

set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "Profile")

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

# --- 3a. Unit testing (Catch2, fetched via CPM) ---
# BUILD_TESTING is CTest's standard opt-out switch, defaulted ON here since
# this project has no separate packaging/consumer mode to skip it for.
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
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
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/importer
        ${CMAKE_SOURCE_DIR}/src/exporter
        ${CMAKE_SOURCE_DIR}/src/common
        ${CMAKE_SOURCE_DIR}/src/common/lock_queue
        ${CMAKE_SOURCE_DIR}/src/common/mmap
        ${CMAKE_SOURCE_DIR}/src/common/logger
        ${CMAKE_SOURCE_DIR}/src/common/xercesc_helpers
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

        $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
        $<$<CONFIG:Release>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
        $<$<CONFIG:Profile>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
    )

    # Link with external libraries if provided
    if(DEFINED FSP_TARGET_${TARGET_NAME}_LIBS)
        target_link_libraries(${TARGET_NAME} PRIVATE ${FSP_TARGET_${TARGET_NAME}_LIBS})
    endif()
endfunction()

# Generates TARGET_NAME's logger config from the checked-in template
# (config/log_debug.conf.in or config/log_release.conf.in, picked by the current
# CMAKE_BUILD_TYPE), substituting @PROGRAM_NAME@ for the target's own name, into
# log_<TARGET_NAME>_<debug|release>.conf in the build directory -- see
# fsp::load_logger_config() in src/common/logger/logger.cpp for the matching runtime lookup.
# configure_file() also makes CMake re-run (and thus regenerate this file) whenever the
# template changes, even without an explicit `cmake ..`.
function(add_log_config TARGET_NAME)
    set(PROGRAM_NAME ${TARGET_NAME})
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(FSP_LOG_TEMPLATE ${CMAKE_SOURCE_DIR}/config/log_debug.conf.in)
        set(FSP_LOG_KIND debug)
    else()
        set(FSP_LOG_TEMPLATE ${CMAKE_SOURCE_DIR}/config/log_release.conf.in)
        set(FSP_LOG_KIND release)
    endif()
    configure_file(${FSP_LOG_TEMPLATE} ${CMAKE_BINARY_DIR}/log_${TARGET_NAME}_${FSP_LOG_KIND}.conf @ONLY)
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
    ${CMAKE_SOURCE_DIR}/src/importer/handler.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/xml_segment.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/error_info.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/processor_config.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/xpath_helpers.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/parsing_util.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/e_tag_wide.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/xml_node.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/xml_attr.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/xpath_el.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/segment_result.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/result_values.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/pipeline_hooks.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/xml_worker.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/load_grammar.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/process_docs.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/stats.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/segment_sax.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/xpath_set.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/doc_dscr.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/doc_set_dscr.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/doc_counters.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/doc_set_counter.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/segment_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/doc_cutter.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/pipeline_worker.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/doc_validator.cpp
)

set(SRC_EXPORTER
    ${CMAKE_SOURCE_DIR}/src/exporter/xml_writer.cpp
)

set(SRC_COMMON
    ${CMAKE_SOURCE_DIR}/src/common/xercesc_helpers/x_str.cpp
    ${CMAKE_SOURCE_DIR}/src/common/xercesc_helpers/xerces_mgr.cpp
    ${CMAKE_SOURCE_DIR}/src/common/lock_queue/lock_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/common/mmap/mmap_file.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logger/logger_config.cpp
    ${CMAKE_SOURCE_DIR}/src/common/common.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logger/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/common/logger/logger_impl.cpp
    ${CMAKE_SOURCE_DIR}/src/ach/utility.cpp
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
#    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${SOURCES}
)
# Specify additional libraries for fsp_lib (if needed)
# fmt/spdlog/magic_enum weren't listed here even under find_package(), which
# went unnoticed only because that fmt/spdlog were consistent between compile
# and link across every consumer (all from the same distro package). Now that
# fmt/spdlog come from CPM as compiled libraries (not header-only), a fsp_lib
# .cpp that calls fmt::vformat (via error_info.hpp etc.) has an unresolved
# symbol unless fsp_lib links fmt::fmt itself -- so all three are explicit here.
set(FSP_TARGET_${b_target}_LIBS
    XercesC::XercesC
    LibXml2::LibXml2
    fmt::fmt
    spdlog::spdlog
    magic_enum::magic_enum
)
add_fsp_target(fsp_lib STATIC ${fsp_lib_SOURCES})
#
# --- target: validate --------------------------------------------------
#
set(b_target validate) # name of the target
set(${b_target}_SOURCES
    ${CMAKE_SOURCE_DIR}/src/auxiliary/validate.cpp
    ${CMAKE_SOURCE_DIR}/src/common/xercesc_helpers/xerces_mgr.cpp
    ${CMAKE_SOURCE_DIR}/src/common/xercesc_helpers/x_str.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES ${CMAKE_SOURCE_DIR}/src/auxiliary)
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
    ${CMAKE_SOURCE_DIR}/src/auxiliary/gen.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES ${CMAKE_SOURCE_DIR}/src/auxiliary)
set(FSP_TARGET_${b_target}_LIBS
    fsp_lib
    XercesC::XercesC
)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
# -----------------------------------------------------------------------------
# --- target: test_pars
# -----------------------------------------------------------------------------
set(b_target test_pars) # name of the target
set(${b_target}_SOURCES
    ${CMAKE_SOURCE_DIR}/src/test/test_pars.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/parsing_util.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${CMAKE_SOURCE_DIR}/src/importer
  ${CMAKE_SOURCE_DIR}/src/test
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
    ${CMAKE_SOURCE_DIR}/src/test/t_refl.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/parsing_util.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${CMAKE_SOURCE_DIR}/src/importer
  ${CMAKE_SOURCE_DIR}/src/test
  )
set(FSP_TARGET_${b_target}_LIBS
    fmt::fmt
    spdlog::spdlog
    magic_enum::magic_enum
)
disable_ubsan_for_reflection(${CMAKE_SOURCE_DIR}/src/test/t_refl.cpp)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
# -----------------------------------------------------------------------------
# --- target: pacs8
# -----------------------------------------------------------------------------
set(b_target pacs8) # name of the target
set(${b_target}_SOURCES
    ${CMAKE_SOURCE_DIR}/src/test/pacs8.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${CMAKE_SOURCE_DIR}/src/importer
  ${CMAKE_SOURCE_DIR}/src/test
  )
set(FSP_TARGET_${b_target}_LIBS
    fsp_lib
    fmt::fmt
    spdlog::spdlog
    magic_enum::magic_enum
)
disable_ubsan_for_reflection(${CMAKE_SOURCE_DIR}/src/test/pacs8.cpp)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
add_log_config(${b_target})
# -----------------------------------------------------------------------------
# --- target: pacs8-cb (same as pacs8, but wires an explicit custom pipeline_hooks class)
# -----------------------------------------------------------------------------
set(b_target pacs8-cb) # name of the target
set(${b_target}_SOURCES
    ${CMAKE_SOURCE_DIR}/src/test/pacs8-cb.cpp
    ${CMAKE_SOURCE_DIR}/src/test/pacs8_cb.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${CMAKE_SOURCE_DIR}/src/importer
  ${CMAKE_SOURCE_DIR}/src/test
  )
set(FSP_TARGET_${b_target}_LIBS
    fsp_lib
    fmt::fmt
    spdlog::spdlog
    magic_enum::magic_enum
)
disable_ubsan_for_reflection(${CMAKE_SOURCE_DIR}/src/test/pacs8-cb.cpp)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
add_log_config(${b_target})
# -----------------------------------------------------------------------------
# --- target: validator
# -----------------------------------------------------------------------------
set(b_target validator) # name of the target
set(${b_target}_SOURCES
    ${CMAKE_SOURCE_DIR}/src/auxiliary/validator.cpp
    ${CMAKE_SOURCE_DIR}/src/common/xercesc_helpers/xerces_mgr.cpp
    ${CMAKE_SOURCE_DIR}/src/common/xercesc_helpers/x_str.cpp
    ${CMAKE_SOURCE_DIR}/src/importer/load_grammar.cpp
    ${CMAKE_SOURCE_DIR}/src/common/mmap/mmap_file.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${CMAKE_SOURCE_DIR}/src/importer
  ${CMAKE_SOURCE_DIR}/src/test
  ${XERCESC_INCLUDE_DIR}
  )
set(FSP_TARGET_${b_target}_LIBS PRIVATE
    fmt::fmt
    spdlog::spdlog
    magic_enum::magic_enum
    XercesC::XercesC
)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
# -----------------------------------------------------------------------------
# --- target: unit_tests
# -----------------------------------------------------------------------------
if(BUILD_TESTING)
    set(b_target unit_tests) # name of the target
    set(${b_target}_SOURCES
        ${CMAKE_SOURCE_DIR}/src/test/test_x_str.cpp
        ${CMAKE_SOURCE_DIR}/src/test/test_xerces_mgr.cpp
        ${CMAKE_SOURCE_DIR}/src/test/test_lock_queue.cpp
        ${CMAKE_SOURCE_DIR}/src/test/test_mmap_file.cpp
        ${CMAKE_SOURCE_DIR}/src/test/test_logger.cpp
        ${CMAKE_SOURCE_DIR}/src/common/xercesc_helpers/x_str.cpp
        ${CMAKE_SOURCE_DIR}/src/common/xercesc_helpers/xerces_mgr.cpp
        ${CMAKE_SOURCE_DIR}/src/common/mmap/mmap_file.cpp
        ${CMAKE_SOURCE_DIR}/src/common/logger/logger.cpp
        ${CMAKE_SOURCE_DIR}/src/common/logger/logger_impl.cpp
        ${CMAKE_SOURCE_DIR}/src/common/logger/logger_config.cpp
        ${CMAKE_SOURCE_DIR}/src/importer/error_info.cpp
    )
    set(FSP_TARGET_${b_target}_LIBS
        Catch2::Catch2WithMain
        fmt::fmt
        spdlog::spdlog
        magic_enum::magic_enum
        XercesC::XercesC
    )
    add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
    # Catch2's INTERNAL_CATCH_UNIQUE_NAME expands to __COUNTER__, flagged by
    # -Wpedantic as a C2y extension on Clang (silent on GCC); a third-party
    # macro this project doesn't control, so it is silenced rather than
    # allowed to trip -Werror.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${b_target} PRIVATE -Wno-c2y-extensions)
    endif()
    catch_discover_tests(${b_target})
endif()
#-------------------------------------------------------------------------------
# --- 6. (Optional) Display built targets ---
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "fsp sources: ${${b_target}_SOURCES}")
# Add a custom target that runs during every build to print the build type
add_custom_target(print_build_type ALL
    COMMAND ${CMAKE_COMMAND} -E echo "Building project in ${CMAKE_BUILD_TYPE} mode."
)