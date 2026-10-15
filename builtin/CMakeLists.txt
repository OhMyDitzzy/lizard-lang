add_library(lizard_builtin STATIC
    registry.c
    io/print.c)

target_include_directories(lizard_builtin
    PUBLIC include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(lizard_builtin PUBLIC lizard_core)

lz_enable_warnings(lizard_builtin)
