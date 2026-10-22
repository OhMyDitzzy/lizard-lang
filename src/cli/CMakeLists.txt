add_executable(lizard main.c options.c)

target_link_libraries(lizard PRIVATE lizard_engine lizard_frontend lizard_builtin lizard_core)

lz_enable_warnings(lizard)
