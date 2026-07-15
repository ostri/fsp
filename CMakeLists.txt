cmake_minimum_required(VERSION 3.10)
project(fsp LANGUAGES CXX)

#
# for profiling
# cmake -B build-profile -DCMAKE_BUILD_TYPE=Profile
# cmake --build build-profile --target fsp
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
find_package(XercesC REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(magic_enum REQUIRED)
find_package(LibXml2 REQUIRED)

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
        ${CMAKE_SOURCE_DIR}/src/lib
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

set(SOURCES
    ${CMAKE_SOURCE_DIR}/src/lib/x_str.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xerces_mgr.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/handler.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/lock_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xml_segment.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/mmap_file.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xml_processor.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/e_tag.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/error_info.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/mem_buf_holder.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/logger_config.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/processor_config.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xpath_helpers.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/common.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/parsing_util.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/e_tag_wide.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/static_sorted_set.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xml_node.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xml_attr.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xpath_limits.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xpath_el.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/static_xpath_vec.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xpath_limits.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/segment_result.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/logger.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/root_cb.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xml_worker.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/load_grammar.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/process_docs.cpp
)
# --- 5. Define targets ---
set(b_target fsp_lib) # name of the target
# 5.1 fsp_lib (static library)
set(${b_target}_SOURCES
#    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${SOURCES}
)
# Specify additional libraries for fsp_lib (if needed)
set(FSP_TARGET_${b_target}_LIBS
    XercesC::XercesC
    LibXml2::LibXml2
)
add_fsp_target(fsp_lib STATIC ${fsp_lib_SOURCES})
#
# target: fsp
#
set(b_target fsp) # name of the target
set(${b_target}_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.cpp
)
# Additional libraries for main program
set(FSP_TARGET_${PROJECT_NAME}_LIBS
    fsp_lib
    fmt::fmt
    spdlog::spdlog
    magic_enum::magic_enum
    XercesC::XercesC
    LibXml2::LibXml2
)
# If main needs any extra include paths
# set(FSP_TARGET_${PROJECT_NAME}_INCLUDES ${CMAKE_SOURCE_DIR}/src/extra)

add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
#
# --- target: validate --------------------------------------------------
#
set(b_target validate) # name of the target
set(${b_target}_SOURCES
    ${CMAKE_SOURCE_DIR}/src/auxiliary/validate.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xerces_mgr.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/x_str.cpp
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
    ${CMAKE_SOURCE_DIR}/src/lib/parsing_util.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${CMAKE_SOURCE_DIR}/src/lib
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
    ${CMAKE_SOURCE_DIR}/src/lib/parsing_util.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${CMAKE_SOURCE_DIR}/src/lib
  ${CMAKE_SOURCE_DIR}/src/test
  )
set(FSP_TARGET_${b_target}_LIBS
    fmt::fmt
    spdlog::spdlog
    magic_enum::magic_enum
)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
# -----------------------------------------------------------------------------
# --- target: pacs8
# -----------------------------------------------------------------------------
set(b_target pacs8) # name of the target
set(${b_target}_SOURCES
    ${CMAKE_SOURCE_DIR}/src/test/pacs8.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${CMAKE_SOURCE_DIR}/src/lib
  ${CMAKE_SOURCE_DIR}/src/test
  )
set(FSP_TARGET_${b_target}_LIBS
    fsp_lib
    fmt::fmt
    spdlog::spdlog
    magic_enum::magic_enum
)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
# -----------------------------------------------------------------------------
# --- target: validator
# -----------------------------------------------------------------------------
set(b_target validator) # name of the target
set(${b_target}_SOURCES
    ${CMAKE_SOURCE_DIR}/src/auxiliary/validator.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xerces_mgr.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/x_str.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/load_grammar.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/mmap_file.cpp
)
set(FSP_TARGET_${b_target}_INCLUDES
  ${CMAKE_SOURCE_DIR}/src/lib
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
#-------------------------------------------------------------------------------
# --- 6. (Optional) Display built targets ---
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "fsp sources: ${${b_target}_SOURCES}")
# Add a custom target that runs during every build to print the build type
add_custom_target(print_build_type ALL
    COMMAND ${CMAKE_COMMAND} -E echo "Building project in ${CMAKE_BUILD_TYPE} mode."
)