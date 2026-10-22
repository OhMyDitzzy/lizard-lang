# The Gecko Engine: an alternative, faster execution engine for the same bytecode.
# It depends on the core only; the reference interpreter stays in lizard_core.
add_library(lizard_engine STATIC
    engine.c
    ge_exec.c
    ge_translate.c)

target_include_directories(lizard_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(lizard_engine PUBLIC lizard_core)

lz_enable_warnings(lizard_engine)
