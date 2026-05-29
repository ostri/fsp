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
    set(COMMON_RELEASE_FLAGS
        "-Wall" "-Wextra" "-Wpedantic" "-Werror"
        "-Wnarrowing" "-Wswitch-enum" "-O3"
    )
    set(COMMON_DEBUG_FLAGS
        "-Wall" "-Wextra" "-Wpedantic" "-Werror"
        "-Wnarrowing" "-Wswitch-enum" "-g" "-O0" "-gdwarf-4"
        "-fsanitize=address,undefined,leak"
        "-fno-omit-frame-pointer"
    )
    set(COMMON_LINK_FLAGS "-fsanitize=address,undefined,leak")
    set(COMMON_PROFILE_FLAGS
    "-Wall" "-Wextra" "-Wpedantic"
    "-Wnarrowing" "-Wswitch-enum"
    "-O2"
    "-g" "-gdwarf-4"
    "-fno-omit-frame-pointer"
    "-pg"
    )
    set(COMMON_PROFILE_LINK_FLAGS "-pg")
endif()

# --- 3. Find required packages ---
find_package(XercesC REQUIRED)
#find_package(pugixml REQUIRED)
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
    elseif(CMAKE_BUILD_TYPE STREQUAL "Profile")
        target_compile_options(${TARGET_NAME} PRIVATE ${COMMON_PROFILE_FLAGS})
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
    )

    # Link with external libraries if provided
    if(DEFINED FSP_TARGET_${TARGET_NAME}_LIBS)
        target_link_libraries(${TARGET_NAME} PRIVATE ${FSP_TARGET_${TARGET_NAME}_LIBS})
    endif()
endfunction()

# --- 5. Define targets ---

# 5.1 fsp_lib (static library)
set(FSP_LIB_SOURCES
    ${CMAKE_SOURCE_DIR}/src/lib/x_str.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/handler.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/queue.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xml_segment.cpp
)
# Specify additional libraries for fsp_lib (if needed)
set(FSP_TARGET_fsp_lib_LIBS
    XercesC::XercesC
    LibXml2::LibXml2
)
add_fsp_target(fsp_lib STATIC ${FSP_LIB_SOURCES})
#
# target: fsp
#
set(b_target fsp) # name of the target
set(${b_target}_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/x_str.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/xerces_mgr.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/handler.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/queue.cpp
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
    ${CMAKE_SOURCE_DIR}/src/lib/dom_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/dom_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/lib/parsing_util.cpp
)
# Additional libraries for main program
set(FSP_TARGET_${PROJECT_NAME}_LIBS
#    fsp_lib
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
# set(FSP_TARGET_${b_target}_LIBS
#     XercesC::XercesC
#)
add_fsp_target(${b_target} EXECUTABLE ${${b_target}_SOURCES})
#
# --- target: test_pars
#
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

# --- 6. (Optional) Display built targets ---
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "fsp sources: ${${b_target}_SOURCES}")