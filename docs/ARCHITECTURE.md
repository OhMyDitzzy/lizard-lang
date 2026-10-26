# Architecture

## Layout

| Path | Responsibility |
| --- | --- |
| `include/lizard/` | Public headers of the core (values, heap, VM, writer) |
| `src/core/` | Core implementation, no knowledge of any builtin (`array.c` holds the array operations) |
| `src/frontend/` | Lexer and the single-pass compiler |
| `src/engine/` | The Gecko Engine, a faster execution engine for the same bytecode ([ENGINE.md](ENGINE.md)) |
| `src/cli/` | The `lizard` binary: argument parsing and process entry point |
| `builtin/` | Builtin functions and their registry, one subfolder per area (`io/`, ...) |
| `tests/` | Unit tests, registered with CTest |
| `cmake/` | Build helper modules |

Dependency direction: `cli` -> `engine` -> `core` and `cli` -> `builtin` ->
`core`. The core never includes anything from `builtin/` or `engine/`.

## Memory model

Every non-scalar value is an `LzObject` allocated through `LzHeap`. The heap
tracks live objects and bytes, counts allocations, and records the peak.
Ownership is by reference count (`lz_object_retain` / `lz_object_release`).
Objects still alive when the VM shuts down are visible through
`--heap-stats` and `--trace-memory`.

Arrays (`LzArray`) own a separately allocated item buffer. The buffer is
charged to the heap (`lz_heap_charge` / `lz_heap_uncharge`), so `--max-heap`
covers it and `live_bytes` returns to zero when everything is released. Items
are retained when stored and released when removed or when the array dies.
Releasing is iterative: a release that happens while another object is being
finalized is queued, so freeing an array nested a million levels deep cannot
overflow the C stack.

Reference counting cannot free a cycle, so storing an array inside itself,
directly or through other arrays, is a runtime error. Each array counts how many
arrays hold it (`holders`); an array held by nobody cannot be inside the value
being stored, so the check is free in the common case, and otherwise walks only
arrays that contain arrays, once each (`mark` plus a heap epoch).

A cycle can still be built through a closure and one of its upvalues (an array
that holds a closure which captured that array, a closure that captures the
variable it is stored in). **Skink** (`skink.h`, `core/skink.c`) frees those. It
never scans the heap: every upvalue that `lz_vm_close_upvalues` closes while its
value may take part in a cycle (a closure, or an array that could reach one) is
reported with a held reference, and once the returning frame's
slots are gone (`lz_vm_return`, `OP_CLOSE_UPVALUE`) each suspect gets a local trial
deletion: the objects reachable from it are visited, the references between them
are subtracted from their counts, and whatever has no outside reference left is a
dead cycle and is freed on the spot. A trial has a fixed budget
(`LZ_SKINK_BUDGET`); a subgraph that does not fit is left alone, so nothing is
ever freed on a guess and the cost of one frame exit is bounded by a constant. The
hooks live in C code every engine reaches, so the engines and both JIT back-ends
are untouched, and `--skink=off` turns it off.

An array can reach a closure unless its item type rules it out, however deep: `int[]`,
`string[]`, `(int | string)[]`, `int?[]` and also `int[][]` and `float[][][]` cannot (every
cycle runs through a closure, and these hold only scalars, strings and arrays of the same),
while `any[]`, `any[][]`, `fn`-valued items and any unannotated array can. Such a typed
array is a leaf for Skink: never a suspect, never a node of a trial. That rests on the item
type only narrowing (an array stored under `int[][]` has its rows restricted to `int[]`, so
nothing can push a closure into a row through an alias), and a wrong answer could only make
Skink miss a cycle, never free a live object, because a reference it does not follow counts
as an outside reference.

A suspect that survives its trial goes on a watch list (three more levels, due
every 4th, 16th and 64th tick, so a live structure is looked at ever more rarely).
That finds a cycle whose last outside reference is dropped after the frame exit,
and one that was completed after its upvalue closed (a closure pushed into the array
it captured). The lists are intrusive and circular, hold no references, and an
upvalue unlinks itself when it is freed (`lz_object_finalize`). A tick is a return or
block exit that closed at least one upvalue and examines at most two watched
upvalues; it is defined by the closing, not by the call of `lz_vm_return`, because
the reference interpreter returns through it every time and the Gecko Engine only
when it has something to close, and the engines must agree on the heap statistics
(`test_skink_engines`). `lz_vm_release_roots` ends with one sweep over the watch
list, so a cycle that was only waiting for the program to finish is freed as well.
Before an allocation is refused because of `--max-heap`, the heap asks Skink for
room: `lz_skink_reclaim` runs the same passes over the whole watch list with
`LZ_SKINK_EMERGENCY_BUDGET` instead of `LZ_SKINK_BUDGET`, so a cycle that holds a
big array, which no ordinary trial can afford, is freed exactly when the memory is
needed. More suspects than the pending list holds are put on the watch list without
a trial instead of being dropped.
An upvalue that held a number, a string or nil when its frame ended was never
reported, and a closure or array assigned to it later can still close a cycle
(`slot = self`). `SET_UPVALUE` therefore tells Skink (`lz_skink_note_store`) when it
stores an object into a closed upvalue that is not watched yet, and the upvalue goes
on the watch list then. The implementation is shared: the GE interpreter delegates
`SET_UPVALUE` to the reference one in `run.c`, and the JIT back-ends store inline
everything that cannot need the barrier (a number, bool or nil; a string; any store
into an upvalue that is already watched or still open) and send the rest to the same
code through their slow path.

A tick does not need an upvalue to close. Every `LZ_SKINK_ALLOC_PERIOD` (128) object
allocations the heap calls `heap->on_alloc` (`lz_skink_alloc_tick`), which is one
more tick of the same clock whenever a watch list is not empty, so a program that builds
cycles, drops them and goes on allocating is still polled. Allocations are counted by the
program, not by the clock, so every engine ticks at the same allocations. A poll that
frees something is refunded (up to 30 per tick): garbage tends to die together, and
a burst of dead cycles is cleared in a few ticks instead of two per tick. Like the
emergency pass, a tick can run at any allocation; it is safe because it frees only
objects whose every reference comes from inside the dead set. The build option
`-DLZ_SKINK_ALLOC_PERIOD=1` polls at every allocation and is how that claim was
tested (the whole test suite, the fuzzers and the example programs under ASan).

Not covered: a cycle that dies in a program that neither closes an upvalue nor
allocates again (nothing ticks until the heap limit or the end of the program asks).

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

`run.c` is the *reference* interpreter: it defines what every instruction does.
Faster engines reuse it through two extension points, `lz_vm_exec_one` (run one
instruction, no accounting) and `lz_vm_eval_with` (supply the instruction loop),
and must behave identically down to the fuel spent. See [ENGINE.md](ENGINE.md).

## Functions and call frames

Each `fn` compiles to its own chunk, held by an `LzFunction` (`function.h`) that
lives only in the constant table of the code that defines it. At run time a
function value is an `LzClosure`: the function plus its captured variables
(`LzUpvalue`). An upvalue is *open* while the variable is still on the stack
(it points into the stack; the VM keeps the list, highest slot first) and
*closed* once the frame returns (the value is moved into the upvalue).

`vm->frames` (limit `LZ_FRAMES_MAX`, 2048) holds one `LzCallFrame` per active
call: closure, chunk, saved `ip`, `base`, a `supplied` mask and the offset of
the call for stack traces. Locals are `stack[base + slot]`; the callee sits at
`base - 1`, followed by the parameters, the rest array, then other locals.
Frame 0 is the script itself.

Calls (`call.c`): `OP_CALL` for plain positional calls, `OP_CALL_EX` when the
call uses `*spread` or `name = value`. Missing trailing arguments are not filled
by the caller: the callee's prologue evaluates each default
(`OP_JUMP_IF_SUPPLIED` skips it when the argument was given), so a default can
use earlier parameters. `*rest` collects surplus arguments into an array.
`OP_CHECK_PARAM` / `OP_CHECK_RETURN` enforce annotations at run time; when the
callee is known at compile time, the compiler checks arity, names and types
itself (`FuncSig`). Recursive local functions refer to themselves through
`OP_GET_SELF` rather than capturing their own variable, which would be a
reference cycle that reference counting never frees.

Errors keep the failing frames in place so the offset comes from the right
chunk and `lz_vm_capture_trace` can record the call stack (`vm->trace`, the 16
innermost frames) before the stack is reset.

## Source positions and diagnostics

Anything that points into program text stores a byte offset into an `LzSource`:
tokens, and every byte of a chunk (`chunk->offsets`). Lines and columns are
computed only when an error is shown (`lz_source_locate`), and
`lz_source_write_diagnostic` prints `name:line:col: severity: message` with the
source line and a caret underline. Columns count UTF-8 code points. A runtime
error reports `vm->error_offset`, the offset of the failing instruction
(`LZ_NO_OFFSET` when there is none). Sources are limited to 64 MiB (a file of exactly
64 MiB is accepted), and `lz_source_read_file` drops a leading UTF-8 byte order
mark. A long file name or message never truncates a diagnostic.

## Lexer

`lz_lexer_next` never fails: bad input becomes an `ERROR` token with a static
message and scanning resumes after it, so one pass reports every error. The
lexical rules are decided; the grammar built on them is not.

| Item | Rule |
| --- | --- |
| Comments | `#` to end of line |
| Identifiers | ASCII letters, digits and `_`, not starting with a digit |
| Keywords | `and`, `or`, `true`, `false`, `nil` (one table in `lexer.c`) |
| Integers | decimal, `0x`, `0b`, `0o`; `_` between digits; a literal is a magnitude (the sign is a unary minus), so above 2^63 is an error and 2^63 itself is accepted only right after a unary minus, which makes `-9223372036854775808` writable |
| Floats | `1.5`, `1e3`, `1.5e-2`; a `.` needs a digit after it, so `1.foo` is `1`, `.`, `foo`; out-of-range is an error |
| Strings | `"..."` or `'...'` on one line; escapes `\n \t \r \0 \\ \" \' \xHH` (00 to 7F) `\u{H..H}` |
| Operators | `+ - * / // % ! != = == < <= > >= \| ?` and `( ) [ ] { } , . : ;` |
| Newlines | a token; blank or comment-only lines collapse into one, none at the file start, one is added before EOF, and line breaks inside `( )` and `[ ]` are ignored |

`strtod` is locale dependent, so float literals swap in the locale's decimal
point before parsing; this will be revisited with the determinism work.

## Syntax and compiler

`lz_compile` turns a script into one chunk in a single pass (Pratt parser, no
AST). It collects up to `LZ_MAX_DIAGNOSTICS` errors per run, recovering at
statement boundaries, and the CLI prints them with source lines.

```
statement  = let | const | if | while | block | assignment | index_assign | expression
let        = "let" IDENT [ ":" type ] [ "=" expr ]
const      = "const" IDENT [ ":" type ] "=" expr
typedecl   = "type" IDENT "=" type     "type" is only a keyword when a name follows
assignment = IDENT "=" expr            a statement, never an expression
index_assign = postfix "[" expr "]" "=" expr   a statement; the target must end in an index
if         = "if" expr block [ "else" ( if | block ) ]
while      = "while" expr block
block      = "{" statement* "}"
```

A statement ends at a newline or `;`, or just before `}` or the end of the
file. A newline after a binary operator, `and`, `or` or `=` is ignored, and
`else` may sit on the line after `}`. Expressions, lowest to highest
precedence: `or`, `and`, `== !=`, `< <= > >=`, `+ -`, `* / // %`, unary `! -`,
call and subscript (`f(x)`, `a[i]`, `a[i:j]`). `[a, b]` at the start of an expression is an array literal (a trailing comma is allowed). `and` and `or` short-circuit and return the deciding operand.

**Names.** Every name must be declared before use, and an unknown name is a
compile error. Declaring a name twice in one scope is an error; an inner
scope may shadow. `let` at the top level makes a global (`DEFINE_GLOBAL`);
inside a block it is a stack slot (`GET_LOCAL`/`SET_LOCAL`) popped when the
block ends. Builtins and host-defined globals are visible but cannot be
assigned to.

**`const`.** If the initializer is made only of literals, other constants and
pure operators, the compiler runs that code on the real VM
(`lz_vm_eval`), throws the code away and remembers the value. The constant has
no storage at all: each use compiles to one `OP_CONSTANT`, and the value lives
once in the chunk's constant table. An error while evaluating it, such as
`1 / 0`, is a compile error. Otherwise (a call, a variable) the constant is an
ordinary global or slot that the compiler refuses to assign to.

**Arrays.** A literal compiles to `ARRAY_NEW` and one `ARRAY_PUSH` per item, so
its size is not limited by the value stack. `a[i]` is `INDEX`, `a[i:j]` is
`SLICE` (an omitted bound is `nil`), and `a[i] = v` turns the trailing `INDEX`
into `SET_INDEX`. A literal is never folded into a `const`: each evaluation must
build a fresh array, so `const xs = [1]` is an ordinary variable that cannot be
reassigned (its items can change). Arrays are limited to `LZ_ARRAY_MAX_LENGTH`
(16,777,216) items.

**Constants are shared.** Identical constants of a chunk are stored once (names by
identity, numbers by value, floats by their bits, strings by content), so a
chunk holds up to 65536 *distinct* constants however often they are used.

**Jumps.** `JUMP`, `JUMP_IF_FALSE`, `JUMP_IF_TRUE` and `LOOP` take a 16-bit
distance, so one block body is limited to 65535 bytes. The VM bounds-checks
every jump, local slot and operand, so malformed bytecode fails cleanly
instead of crashing; fuel stops infinite loops.

**Limits.** 255 call arguments, 256 locals, 512 declared names, and 128
levels of expression or block nesting. Chains are not nesting: `a and b and c`,
`a or b or c` and `if ... else if ...` are parsed in a loop, so a long chain
(or a hostile script) cannot overflow the C stack. A long `else if` chain is
bounded by the 16-bit jump distance instead, because its first branch jumps
over the whole chain.

**Exit codes.** `lizard file.lz` exits 65 on compile errors, 66 if the file
cannot be read, and 70 on a runtime error or an exhausted budget.

## Types

Types are optional and gradual. Code with no annotations behaves exactly as
before: an unannotated `let` has the type `any`.

```
type      = postfix ( "|" postfix )*
postfix   = primary "?"*                     T? is T | nil
postfix   = primary ( "?" | "[" "]" )*     T[] is an array of T
primary   = int | float | bool | string | number | any | never | nil
          | alias | "string" | integer | "-" integer | true | false | "(" type ")"
```

`number` is `int | float`, `any` accepts every value and `never` none. A literal
is a type of its own, so `"r" | "w"` or `-1 | 0 | 1` act as small enums (at most
8 literals per union). Float literal types are not supported. Names such as
`int` are only types in a type position, so `let int = 5` still works. An alias
(`type Id = int | string`) is lexically scoped, must be declared before use,
and cannot reuse a builtin type name.

**How a value is checked.** Every expression gets a static type and the
compiler compares it with the type the value is stored under (an annotated
`let` or `const`, or an assignment to one):

| Source type vs. target | Result |
| --- | --- |
| every value of the source fits | nothing is emitted |
| none fits (`let a: int = "x"`) | compile error |
| some fit (`any`, a union, a wider type) | `OP_CHECK_TYPE`, a run-time check |

A failed run-time check stops the script with `LZ_ERR_TYPE` and the offset of
the declaration or assignment. The check looks only at the value's kind and, for
literal types, its value, so it costs one instruction. A `const` the compiler
folds is checked against its exact value at compile time.

**Static rules.** `int op int` is `int`, anything with a `float` is `float`,
`/` is always `float`, `"a" + "b"` is `string`, comparisons and `!` are `bool`.
An operator whose operands can never work (`"a" - 1`, `1 + "a"`, `-"x"`,
`nil < 1`) is a compile error, and so is calling a value that can never be a
function. When the compiler cannot tell (an `any` operand) it does not guess:
the operation is still checked when it runs.

There is no implicit conversion, so `let x: float = 1` is an error. Convert
explicitly with the builtins `int()`, `float()`, `string()` and `bool()`; their
result types are known to the compiler, so `let n: int = int("12")` needs no
run-time check. A host native gets the result type `any` unless it sets
`result_kinds`.

**Arrays.** `T[]` is an array whose items all match `T`; `int[]?` is `int[] | nil`
and `int?[]` an array of `int | nil`. `T` can be any type, including another
array type. Item types are interned in the VM (`lz_vm_array_of`), so equal item
types are the same pointer. Assignability looks inside: `int[]` fits `number[]`
and `any[]`, `(int | string)[]` to `int[]` needs a run-time check, and `int[]` to
`string[]` is a compile error. A union of two different array types such as
`int[] | string[]` is rejected in written types (write `(int | string)[]`); in the
type of an expression it widens to `any[]`.

An array object carries the item type it was last stored under. Storing an
array under `T[]` always emits `CHECK_TYPE`, even when it fits statically: the
check scans the items once (skipped when the array's own type already fits) and
then restricts the array, and from then on `push`, `insert`, `a[i] = v` and
nested stores check against it, so the annotation holds after the declaration.
Rows of an `int[][]` are restricted too. An array already restricted to an
unrelated type cannot be stored under another (`int[]` as `string[]` fails even
when empty); restricting to a wider type is harmless and to a narrower one
narrows the array. `+` and slices of arrays keep the type when both sides agree.
Unannotated arrays hold anything (`any`), and `sort`, `sum` and the like check
their items when they run.

**Not yet.** Narrowing (`if x != nil { ... }` does not refine `x`, so `int?`
used as `int` costs a run-time check), `unknown`, `bytes`, collection and
function types, generics, and parameter and return annotations (they arrive
with `fn`). Type literals hold interned strings, which live as long as the VM.

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

An allocation that would cross the limit first calls `heap->reclaim` once and then
checks again. The VM installs Skink there (`lz_skink_reclaim`, see Memory model): an
emergency pass over its watch list that frees dead cycles which were only waiting for
their next poll, with a budget far bigger than a tick may spend. The hook is never
re-entered, can run at any allocation, and frees only objects nothing references; its
scratch memory is malloc'd outside the heap, so it does not count against the limit.

Fuel costs one unit per instruction and persists across `lz_vm_run` calls, so
the host sets it before each run; `vm->instructions` counts executed
instructions even when metering is off. A native that does heavy work calls
`lz_vm_charge_fuel(vm, cost)` so a single call cannot hide unbounded effort;
string concatenation and `print`/`println` charge one unit per 64 bytes they
copy or write. Array work charges one unit per 8 items (copying, slicing,
`+`, searching, sorting with its passes, scanning for a type check, comparing);
an `==` on arrays stops at the fuel that is left, so a hostile diamond-shaped
structure cannot run forever.
Call depth is limited to `LZ_FRAMES_MAX` frames (a "stack overflow" error).

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
| `+` on two arrays builds a new array; arrays compare by content, other objects by identity; `[]` is truthy | done |
| NaN prints as `nan` and the infinities as `inf` and `-inf` on every platform | done |
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

The array builtins (`len`, `push`, `sort`, `range`, ...) live in
`builtin/array/`. Natives receive their arguments on the VM stack, so a native
that mutates an array mutates the caller's object; every store goes through
`lz_array_push` / `lz_array_check_store`, which apply the item type, the cycle
check and the fuel charge.

To add a builtin: implement it under `builtin/<area>/`, declare it in that
area's header, add one row to the table in `registry.c`, and add the source to
`builtin/CMakeLists.txt`.

## Not decided yet

`for` (so `range` is only useful with `while` for now), `break`/`continue`, maps and other collections, indexing and slicing strings, narrowing, the error model, and classes. Removing entries from `LzTable`.
Skink: a trial that can cross a subgraph bigger than `LZ_SKINK_EMERGENCY_NODES`; a run-time "holds no closure" fact for unannotated arrays (they are `any` today, so a captured array of numbers is still a suspect).
