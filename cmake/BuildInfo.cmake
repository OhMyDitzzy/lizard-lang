# Build information: the commit, whether the tree was dirty, the kind of build, and whether it
# claims to be an official (signed) one. See cmake/GenerateBuildInfo.cmake for the rules.
#
#   -DLZ_BUILD_CHANNEL=local|official   (default local: unsigned)
#   -DLZ_SIGNING_KEY=<fingerprint>      (required by official; build.sh --official fills it in)

set(LZ_BUILD_CHANNEL "local" CACHE STRING "local (unsigned) or official (signed package, clean tree)")
set_property(CACHE LZ_BUILD_CHANNEL PROPERTY STRINGS local official)
set(LZ_SIGNING_KEY "" CACHE STRING "Fingerprint of the key that signs an official build's package")

function(lz_setup_buildinfo)
    find_package(Git QUIET)

    # the compiler's own idea of the target: x86_64-linux-gnu, aarch64-apple-darwin23.0.0, ...
    set(triple "")
    if(CMAKE_C_COMPILER_TARGET)
        set(triple "${CMAKE_C_COMPILER_TARGET}")
    elseif(NOT MSVC)
        execute_process(COMMAND ${CMAKE_C_COMPILER} -dumpmachine
            OUTPUT_VARIABLE triple ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    if(triple STREQUAL "")
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_SYSTEM_NAME}" triple)
    endif()

    set(compiler "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")

    # The JIT exists for x86-64 and AArch64 on Linux and macOS only (src/engine/ge_jit.h).
    set(jit OFF)
    if(LZ_ENABLE_JIT AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|aarch64|arm64|ARM64)$"
       AND CMAKE_SYSTEM_NAME MATCHES "^(Linux|Android|Darwin)$")
        set(jit ON)
    endif()

    set(sanitizers OFF)
    if(LZ_ENABLE_SANITIZERS AND NOT MSVC)
        set(sanitizers ON)
    endif()
    set(lto OFF)
    if(CMAKE_INTERPROCEDURAL_OPTIMIZATION)
        set(lto ON)
    endif()
    set(static OFF)
    if(LZ_STATIC)
        set(static ON)
    endif()

    set(generated ${PROJECT_BINARY_DIR}/generated)
    set(arguments
        -DSRC_DIR=${PROJECT_SOURCE_DIR}
        -DBIN_DIR=${generated}
        -DBUILD_DIR=${PROJECT_BINARY_DIR}
        -DTEMPLATE=${PROJECT_SOURCE_DIR}/include/lizard/version.h.in
        -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
        -DPROJECT_VERSION=${PROJECT_VERSION}
        -DPROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        -DPROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        -DPROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        -DCHANNEL=${LZ_BUILD_CHANNEL}
        -DSIGNING_KEY=${LZ_SIGNING_KEY}
        "-DCONFIG=${CMAKE_BUILD_TYPE}"
        "-DTARGET=${triple}"
        "-DCOMPILER=${compiler}"
        -DJIT=${jit}
        -DLTO=${lto}
        -DSANITIZERS=${sanitizers}
        -DSTATIC=${static}
        -P ${PROJECT_SOURCE_DIR}/cmake/GenerateBuildInfo.cmake)

    # Now, so that the header exists after configuring and a bad official request fails early...
    execute_process(COMMAND ${CMAKE_COMMAND} ${arguments} RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "generating the build information failed")
    endif()

    # ...and on every build, because the commit and the dirty state change without a reconfigure.
    add_custom_target(lizard_buildinfo
        COMMAND ${CMAKE_COMMAND} ${arguments}
        BYPRODUCTS ${generated}/lizard/version.h ${generated}/BUILDINFO
        COMMENT "Generating build information"
        VERBATIM)

    set(LZ_BUILDINFO_FILE ${generated}/BUILDINFO PARENT_SCOPE)
endfunction()
