cmake_minimum_required(VERSION 3.16)

project(Lizard
    VERSION 0.1.0
    DESCRIPTION "Lizard, an interpreted programming language written in C"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LZ_BUILD_TESTS "Build the test suite" ON)
option(LZ_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(LZ_ENABLE_SANITIZERS "Build with AddressSanitizer and UBSan" OFF)
option(LZ_ENABLE_LTO "Enable link-time optimization when supported" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

if(LZ_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lz_ipo_supported OUTPUT lz_ipo_message)
    if(lz_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO requested but not supported: ${lz_ipo_message}")
    endif()
endif()

configure_file(
    include/lizard/version.h.in
    ${CMAKE_BINARY_DIR}/generated/lizard/version.h
    @ONLY)

add_subdirectory(src)
add_subdirectory(builtin)

if(LZ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(UNIX)
    # cmake --build build --target bench: every bench/*.lz on both engines
    add_custom_target(bench
        COMMAND ${PROJECT_SOURCE_DIR}/bench/run.sh $<TARGET_FILE:lizard> 3
        DEPENDS lizard
        USES_TERMINAL
        COMMENT "Running bench/*.lz on both engines")
endif()

install(TARGETS lizard RUNTIME DESTINATION bin)

message(STATUS "Lizard ${PROJECT_VERSION} (${CMAKE_BUILD_TYPE})")
message(STATUS "  tests: ${LZ_BUILD_TESTS}  sanitizers: ${LZ_ENABLE_SANITIZERS}  werror: ${LZ_WARNINGS_AS_ERRORS}  lto: ${LZ_ENABLE_LTO}")
