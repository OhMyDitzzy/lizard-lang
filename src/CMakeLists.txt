add_library(lizard_core STATIC
    core/arith.c
    core/chunk.c
    core/compare.c
    core/debug.c
    core/memory.c
    core/number.c
    core/native.c
    core/run.c
    core/source.c
    core/table.c
    core/type.c
    core/value.c
    core/vm.c
    core/writer.c)

target_include_directories(lizard_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}/generated)

find_library(LZ_MATH_LIBRARY m)
if(LZ_MATH_LIBRARY)
    target_link_libraries(lizard_core PUBLIC ${LZ_MATH_LIBRARY})
endif()

lz_enable_warnings(lizard_core)

add_subdirectory(frontend)
add_subdirectory(cli)
