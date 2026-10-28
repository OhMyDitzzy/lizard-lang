add_library(lizard_utils STATIC
    argparse.c)

target_include_directories(lizard_utils PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}/generated)

lz_enable_warnings(lizard_utils)