function(lz_add_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE lizard_frontend lizard_builtin lizard_core)
    lz_enable_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Tests of the Gecko Engine link it as well.
function(lz_add_engine_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE lizard_engine lizard_frontend lizard_builtin lizard_core)
    lz_enable_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# The same test sources again, with every call to the reference interpreter routed
# through the Gecko Engine (engine_redirect.h): the existing suite becomes a
# conformance test for GE, with the same assertions and expected results.
function(lz_add_ge_variant name)
    add_executable(${name}_ge ${name}.c)
    target_link_libraries(${name}_ge PRIVATE lizard_engine lizard_frontend lizard_builtin lizard_core)
    if(MSVC)
        target_compile_options(${name}_ge PRIVATE /FI${CMAKE_CURRENT_SOURCE_DIR}/engine_redirect.h)
    else()
        target_compile_options(${name}_ge PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/engine_redirect.h)
    endif()
    lz_enable_warnings(${name}_ge)
    add_test(NAME ${name}_ge COMMAND ${name}_ge)
endfunction()

lz_add_test(test_memory)
lz_add_test(test_table)
lz_add_test(test_native)
lz_add_test(test_chunk)
lz_add_test(test_type)
lz_add_test(test_convert)
lz_add_test(test_debug)
lz_add_test(test_run)
lz_add_test(test_arith)
lz_add_test(test_compare)
lz_add_test(test_limits)
lz_add_test(test_source)
lz_add_test(test_suggest)
lz_add_test(test_lexer)
lz_add_test(test_compiler)
lz_add_test(test_array)
lz_add_test(test_higher)
lz_add_test(test_methods)
lz_add_test(test_strings)
lz_add_test(test_bits)
lz_add_test(test_loops)
lz_add_test(test_skink)
lz_add_test(fuzz_compile)
lz_add_test(fuzz_run)
lz_add_test(fuzz_lexer)

add_executable(test_argparse test_argparse.c)
target_link_libraries(test_argparse PRIVATE lizard_utils)
lz_enable_warnings(test_argparse)
add_test(NAME test_argparse COMMAND test_argparse test.lz)
lz_add_test(test_print)

lz_add_engine_test(test_engine)
lz_add_engine_test(fuzz_engine)
lz_add_engine_test(test_jit)
lz_add_engine_test(test_skink_engines)

lz_add_ge_variant(test_run)
lz_add_ge_variant(test_skink)
lz_add_ge_variant(test_limits)
lz_add_ge_variant(test_array)
lz_add_ge_variant(test_higher)
lz_add_ge_variant(test_methods)
lz_add_ge_variant(test_strings)
lz_add_ge_variant(test_bits)
lz_add_ge_variant(test_loops)
lz_add_ge_variant(test_compiler)
lz_add_ge_variant(fuzz_run)
lz_add_ge_variant(fuzz_compile)

# The Gecko JIT: the whole engine and conformance suite again with every program compiled
# to machine code on first sight (LZ_JIT=eager). Without a JIT on the platform these are
# plain GE runs.
foreach(name test_engine fuzz_engine test_bits_ge test_run_ge test_limits_ge test_array_ge test_compiler_ge
        test_loops_ge fuzz_run_ge fuzz_compile_ge test_skink_ge)
    add_test(NAME ${name}_jit COMMAND ${name})
    set_tests_properties(${name}_jit PROPERTIES ENVIRONMENT "LZ_JIT=eager")
endforeach()

if(UNIX)
    # Skink through the reference interpreter, the Gecko interpreter and the JIT: the output,
    # the statistics and what is left alive must agree (tests/skink_check.sh, which also fails
    # when the JIT did not run).
    add_test(NAME skink_cli
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/skink_check.sh $<TARGET_FILE:lizard>)

    # `lizard --version`: the version, the kind of build, the channel, all consistent with BUILDINFO.
    add_test(NAME version_cli
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/version_check.sh $<TARGET_FILE:lizard>
                ${PROJECT_BINARY_DIR}/generated/BUILDINFO)

    # build.sh: usage text, option errors, and that it detects this machine.
    find_program(LZ_BASH bash)
    if(LZ_BASH)
        add_test(NAME build_script
            COMMAND ${LZ_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/build_script_check.sh ${PROJECT_SOURCE_DIR}/build.sh)
    endif()
endif()
