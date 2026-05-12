cmake_minimum_required(VERSION 3.10)
project(fsp LANGUAGES CXX)

# --- 1. Splošne nastavitve projekta ---
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose build type" FORCE)
endif()

# --- 2. Definicija skupnih parametrov za prevajanje in linkanje ---
# Te liste uporabimo za vse tarče, da zagotovimo identične pogoje gradnje.

if(MSVC)
    set(COMMON_RELEASE_FLAGS "/W4" "/WX" "/O2")
    set(COMMON_DEBUG_FLAGS   "/W4" "/Zi" "/Od")
    set(COMMON_LINK_FLAGS    "")
else()
    # Zastavice za Release (vse tarče)
    set(COMMON_RELEASE_FLAGS
        "-Wall" "-Wextra" "-Wpedantic" "-Werror"
        "-Wnarrowing" "-Wswitch-enum" "-O3"
    )

    # Zastavice za Debug (vključno s sanitizerji)
    set(COMMON_DEBUG_FLAGS
        "-Wall" "-Wextra" "-Wpedantic" "-Werror"
        "-Wnarrowing" "-Wswitch-enum" "-g3" "-O1"
        "-fsanitize=address,undefined,leak"
        "-fno-omit-frame-pointer"
    )

    # Zastavice za povezovanje (linkanje) - nujno za sanitizerje
    set(COMMON_LINK_FLAGS "-fsanitize=address,undefined,leak")
endif()

# --- 3. Iskanje paketov ---
find_package(XercesC REQUIRED)
find_package(pugixml REQUIRED)

# --- 4. fsp_lib tarča ---
set(FSP_CPP_SOURCES
  ${CMAKE_SOURCE_DIR}/src/lib/x_str.cpp
  ${CMAKE_SOURCE_DIR}/src/lib/handler.cpp
  ${CMAKE_SOURCE_DIR}/src/lib/queue.cpp
  ${CMAKE_SOURCE_DIR}/src/lib/xml_segment.cpp
)
add_library(fsp_lib STATIC ${FSP_CPP_SOURCES})

# --- 5. main executable tarča ---
add_executable(${PROJECT_NAME} ${CMAKE_SOURCE_DIR}/src/main.cpp)

# --- 6. Aplikacija parametrov na vse tarče ---
# Uporabimo funkcijo, da se izognemo podvajanju kode
set(ALL_TARGETS fsp_lib ${PROJECT_NAME})

foreach(target ${ALL_TARGETS})
    # Nastavitev vključnih poti
    target_include_directories(${target} PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/lib)

    # Nastavitev compile opcij glede na build type
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(${target} PRIVATE ${COMMON_RELEASE_FLAGS})
    else()
        target_compile_options(${target} PRIVATE ${COMMON_DEBUG_FLAGS})
        # Link opcije dodamo samo, če gre za executable ali če knjižnica to zahteva
        target_link_options(${target} PRIVATE ${COMMON_LINK_FLAGS})
    endif()
endforeach()

# --- 7. Povezovanje knjižnic ---
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        XercesC::XercesC
        pugixml
        fsp_lib
)

# --- 8. Makro definicije ---
target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:DEBUG_MODE>
    $<$<CONFIG:Release>:RELEASE_MODE>
)