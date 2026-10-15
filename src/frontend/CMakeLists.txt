add_library(lizard_frontend STATIC
    compiler.c
    lexer.c)

target_link_libraries(lizard_frontend PUBLIC lizard_core)

lz_enable_warnings(lizard_frontend)
