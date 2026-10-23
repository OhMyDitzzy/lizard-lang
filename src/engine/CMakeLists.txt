# The Gecko Engine: an alternative, faster execution engine for the same bytecode.
# It depends on the core only; the reference interpreter stays in lizard_core.
add_library(lizard_engine STATIC
    engine.c
    ge_exec.c
    ge_translate.c
    ge_jit.c)

target_include_directories(lizard_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(lizard_engine PUBLIC lizard_core)

# The JIT emits x86-64 machine code (Linux, macOS). Elsewhere, or when switched off, the
# same sources build a stub and every program runs on the GE interpreter.
if(NOT LZ_ENABLE_JIT)
    target_compile_definitions(lizard_engine PRIVATE LZ_NO_JIT)
endif()

lz_enable_warnings(lizard_engine)
