# Lizard

An interpreted programming language written in C. Source files use the `.lz`
extension.

Status: early. The memory model, bytecode VM, builtin registry, `print`/`println`
and the lexer exist and are tested; there is no parser or compiler yet, so the
binary cannot run scripts.

## Build

Requires a C11 compiler and CMake 3.16 or newer.

    cmake -S . -B build
    cmake --build build
    ctest --test-dir build --output-on-failure

The binary is written to `build/bin/lizard`.

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Release` | `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel` |
| `LZ_BUILD_TESTS` | `ON` | Build the test suite |
| `LZ_WARNINGS_AS_ERRORS` | `OFF` | Compile with `-Werror` |
| `LZ_ENABLE_SANITIZERS` | `OFF` | AddressSanitizer and UBSan |
| `LZ_ENABLE_LTO` | `OFF` | Link-time optimization when supported |

Example: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLZ_ENABLE_SANITIZERS=ON`

## Usage

    lizard [options] <script.lz> [script arguments]

| Option | Effect |
| --- | --- |
| `-h`, `--help` | Show help |
| `-V`, `--version` | Show the version |
| `--heap-stats` | Print heap statistics when the program ends |
| `--trace-memory` | Log every heap allocation and release to stderr |
| `--dump-tokens` | Print the tokens of the script and exit (the only thing the front-end does so far) |
| `--max-heap=N` | Limit live heap bytes (suffix `K`, `M` or `G`) |
| `--fuel=N` | Limit the number of executed instructions |
| `--` | Stop option parsing |

## Development

`.clangd` reads `build/compile_commands.json`, so configure once before
opening the project in an editor. `.clang-tidy` and `.clang-format` are at the
repository root.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the layout, the memory
model, and how to add a builtin.
