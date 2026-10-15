# Architecture

## Layout

| Path | Responsibility |
| --- | --- |
| `include/lizard/` | Public headers of the core (values, heap, VM, writer) |
| `src/core/` | Core implementation, no knowledge of any builtin |
| `src/frontend/` | Lexer (parser and compiler will join it) |
| `src/cli/` | The `lizard` binary: argument parsing and process entry point |
| `builtin/` | Builtin functions and their registry, one subfolder per area (`io/`, ...) |
| `tests/` | Unit tests, registered with CTest |
| `cmake/` | Build helper modules |

Dependency direction: `cli` -> `builtin` -> `core`. The core never includes
anything from `builtin/`.

## Memory model

Every non-scalar value is an `LzObject` allocated through `LzHeap`. The heap
tracks live objects and bytes, counts allocations, and records the peak.
Ownership is by reference count (`lz_object_retain` / `lz_object_release`).
Objects still alive when the VM shuts down are visible through
`--heap-stats` and `--trace-memory`.

Scalars (nil, bool, int, float) live inline in `LzValue` and never touch the
heap.

## Execution

Programs run as bytecode on a stack VM. A chunk (`LzChunk`) is a byte stream:
a one-byte opcode followed by operands, 16-bit operands big-endian, plus a
constant table (holding references) and one source line per byte. The opcode
set lives in `include/lizard/chunk.h`.

`lz_vm_run` executes a chunk until `OP_RETURN`. The value stack owns a
reference to everything on it: push retains, pop releases. `OP_CALL` pops the
arguments and callee, then pushes the native's result, which is a new
reference owned by the caller. Failures return a status, set `vm->error`, and
record the failing source line in `vm->error_line`; the stack is always empty
afterwards. `lz_disassemble_chunk` (`debug.h`) prints a chunk for debugging.

There are no call frames yet: nothing can be called except natives. Frames
arrive together with user-defined functions.

## Source positions and diagnostics

Anything that points into program text stores a byte offset into an `LzSource`:
tokens, and every byte of a chunk (`chunk->offsets`). Lines and columns are
computed only when an error is shown (`lz_source_locate`), and
`lz_source_write_diagnostic` prints `name:line:col: severity: message` with the
source line and a caret underline. Columns count UTF-8 code points. A runtime
error reports `vm->error_offset`, the offset of the failing instruction
(`LZ_NO_OFFSET` when there is none). Sources are limited to 64 MiB.

## Lexer

`lz_lexer_next` never fails: bad input becomes an `ERROR` token with a static
message and scanning resumes after it, so one pass reports every error. The
lexical rules are decided; the grammar built on them is not.

| Item | Rule |
| --- | --- |
| Comments | `#` to end of line |
| Identifiers | ASCII letters, digits and `_`, not starting with a digit |
| Keywords | `and`, `or`, `true`, `false`, `nil` (one table in `lexer.c`) |
| Integers | decimal, `0x`, `0b`, `0o`; `_` between digits; above `INT64_MAX` is an error |
| Floats | `1.5`, `1e3`, `1.5e-2`; a `.` needs a digit after it, so `1.foo` is `1`, `.`, `foo`; out-of-range is an error |
| Strings | `"..."` or `'...'` on one line; escapes `\n \t \r \0 \\ \" \' \xHH` (00 to 7F) `\u{H..H}` |
| Operators | `+ - * / // % ! != = == < <= > >=` and `( ) [ ] { } , . : ;` |
| Newlines | a token; blank or comment-only lines collapse into one, none at the file start, one is added before EOF, and line breaks inside `( )` and `[ ]` are ignored |

`strtod` is locale dependent, so float literals swap in the locale's decimal
point before parsing; this will be revisited with the determinism work.

## Resource limits

Hosts bound what a script can consume, and exceeding a budget is reported with
its own status so it can be told apart from a bug in the script:

| Status | Budget | How to set it |
| --- | --- | --- |
| `LZ_ERR_HEAP_LIMIT` | live heap bytes | `lz_vm_set_heap_limit`, CLI `--max-heap=N` |
| `LZ_ERR_FUEL` | executed instructions | `lz_vm_set_fuel`, CLI `--fuel=N` |
| `LZ_ERR_STACK_OVERFLOW` | value stack depth (`LZ_STACK_MAX`) | fixed at build time |

Both budgets default to unlimited. The heap limit is checked in `lz_heap_alloc`
and counts heap objects only (strings, natives, future containers); tables and
chunks owned by the VM or the program are not counted, and neither are the few
hundred bytes of builtins when the CLI applies the limit after installing them.
A native that fails to allocate returns `lz_vm_out_of_memory(vm)`, which reports
`LZ_ERR_HEAP_LIMIT` when the limit was the cause and `LZ_ERR_MEMORY` otherwise.

Fuel costs one unit per instruction and persists across `lz_vm_run` calls, so
the host sets it before each run; `vm->instructions` counts executed
instructions even when metering is off. A native that does heavy work calls
`lz_vm_charge_fuel(vm, cost)` so a single call cannot hide unbounded effort.
Call depth gets its own limit when call frames arrive.

## Language semantics

Decided rules, implemented in stages.

| Rule | Status |
| --- | --- |
| Numbers are `int` (int64) and `float` (double); mixing them gives a float | done |
| `/` always produces a float | done |
| int64 overflow is a runtime error, never a silent wrap | done |
| Division by zero is a runtime error, for ints and floats | done |
| Only `nil` and `false` are falsy | done |
| `1 == 1.0` is true, strings compare by content, other mixed types are unequal | done |
| `<` and `>` order numbers only; int against float is compared exactly, and anything involving NaN is false | done |
| `+` on two strings concatenates; there is no implicit conversion (`"a" + 1` is an error) | done |
| `//` rounds toward negative infinity and `%` takes the sign of the divisor (as in Python); with a float operand both give floats; a zero divisor is an error | done |

Equality, truthiness and exact number ordering live in `value.c`
(`lz_value_equal`, `lz_value_is_truthy`, `lz_number_order`); the `<` and `>`
operators are in `compare.c`. A future compiler must not turn `a <= b` into
`!(a > b)`: with NaN that gives the wrong answer, so `<=` and `>=` need
their own opcodes (or a NaN-aware sequence). `!=` is safe as `!(a == b)`.

Arithmetic lives in `src/core/arith.c` so `run.c` only dispatches. `OP_ADD`
accepts two numbers or two strings. Concatenation allocates a new string, so
it returns a new reference like any native. Float `//` and `%` use `fmod`, so
`lizard_core` links the math library when one exists. Overflow detection uses the GCC/Clang
builtins and falls back to portable checks elsewhere; define
`LZ_PORTABLE_OVERFLOW` to force the fallback (the test suite passes on both).

## Names and builtins

Names are interned strings: `lz_vm_intern` returns one unique `LzString` per
distinct text, so comparing two names is a pointer comparison and `LzTable`
hashes on identity. The intern table holds a reference for the life of the VM,
so only identifiers should be interned, not arbitrary runtime strings.

The VM has two scopes: `globals` and `builtins`. `lz_vm_lookup` checks globals
first, so user definitions shadow builtins. Local scopes will sit in front of
these once the evaluator exists.

A builtin is a plain C function:

    LzStatus fn(LzVM *vm, int argc, const LzValue *argv, LzValue *result);

declared in `builtin/registry.c` with a name and an arity range.
`lz_builtin_install` runs once after `lz_vm_init` (the CLI calls it), wraps each
entry in an `LzNative` heap object and defines it in the builtin scope. The
core never includes `builtin/`; it only sees the resulting objects.

Builtins write through `vm->out` / `vm->err` (`LzWriter`), never through
`stdout`, so tests can capture output. Call one with `lz_native_call`, which
checks arity first.

To add a builtin: implement it under `builtin/<area>/`, declare it in that
area's header, add one row to the table in `registry.c`, and add the source to
`builtin/CMakeLists.txt`.

## Not decided yet

Syntax and the lexer/parser/compiler. Local scopes, user functions and call frames, arithmetic and control-flow opcodes. Removing entries from `LzTable`.
Garbage collection beyond reference counting (cycles).
