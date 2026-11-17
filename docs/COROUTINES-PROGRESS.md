# Coroutine checkpoint, November 17, 2026

The implementation stops at execution context storage and switching. There is no coroutine
language feature yet. This checkpoint keeps the completed groundwork reviewable before
starting suspension, object ownership and collector integration.

## Task status

| Work | Status |
| --- | --- |
| 0a, native banner comments | Complete. Removed 28 separator comments from `include/lzapi.h`, `src/core/lzapi.c` and `tests/native/modlib.c`. The other requested native files had no matching banners. |
| 0, signature diagnostics | Complete. Multiple rest parameters report `a signature can have only one rest parameter`. A regular parameter after a rest parameter reports `the rest parameter must be the last one`. Tests cover both with and without an earlier fixed parameter. |
| 1, design document | Written and linked from the documentation index and functions guide. The native guide reserves a future pending-status protocol. Untested language behavior is explicitly identified as planned. |
| 2, execution contexts | Storage, switching, explicit growth, pointer repair and teardown implemented. Main arrays remain embedded. All 109 previous non-benchmark tests and the new context test pass. Automatic reservation inside interpreter operations is deferred to the language implementation. |
| 3, language surface and errors | Not implemented. No coroutine object, builtin, type annotation, resume, yield or status method exists yet. |
| 4, coroutine collection | Not implemented. Detached context teardown is tested, but coroutine tracing, active roots, suspended cycles and VM shutdown with live coroutine objects remain. |
| 5, coroutine tests | Not implemented. Context C tests are registered and `mk/tests.am` regenerated. Two context mutations were detected. The language runner and remaining mutation checks are pending. |
| 6, coroutine fuzzing | Not implemented. Existing fuzz tests pass under UBSan and poison, but they cannot exercise a language feature that does not yet exist. |

`repomix-output.xml` was absent. Decisions were based on the repository sources. No commit,
benchmark measurement, new syntax, event loop, thread API or suspendable native API was made.

## Storage decisions and remaining integration

`LzExecutionContext` holds buffer pointers, capacities, saved counts and the open-upvalue
list. `vm->context` identifies the active context. Active counts and the open-upvalue list
stay directly in the VM. Switching saves them to the old context and loads the new context.
It does not copy values, frames or handlers.

The existing embedded arrays were renamed `main_stack`, `main_frames` and `main_handlers`.
The names `stack`, `frames` and `handlers` now refer to active storage pointers. New fields
sit immediately before Skink. Main initialization needs no additional allocation. The VM
grows by 120 bytes on this AArch64 build.

Detached buffers are initially absent. Explicit reservation starts at 64 values, 8 frames
and 8 handlers, and doubles up to the existing limits. Capacity increments are charged to
the VM heap. Allocation keeps the old block valid until all open-upvalue slot indices are
calculated and their locations repaired. Values are copied without changing reference
counts. Partial allocation failure leaves already-grown buffers valid and charged.

The interpreter still checks the old maximum constants. It does not yet reserve buffers
automatically. A detached context used for interpreter execution must therefore reserve
all maximum capacities first, as the execution test does. Small growing coroutine stacks
are not safe to expose to the language until reservation points and borrowed-pointer
handling are integrated. Native argument slices and C locals pointing into frames need
special attention when callbacks can grow storage.

Context teardown requires a detached context. It closes upvalues before releasing value
slots and buffer charges. Frame closure pointers remain borrowed from their callee slots.
They are not separately retained or released. Coroutine GC must account for that ownership
rather than invent an extra reference for each frame.

Skink currently discovers cycles through closed upvalues. Suspended coroutine cycles can
contain only open upvalues, so coroutine integration needs a way to discover those cycles
as well as an edge visitor. This is still outstanding.

The context functions use the names `lz_vm_context_init`, `lz_vm_context_switch`,
`lz_vm_context_reserve` and `lz_vm_context_free`. They are storage primitives. No alternative
name has been chosen for `LZ_YIELD`, since suspension is not implemented yet.

## Cost and protected paths

These are lexical access counts in the original C source, excluding comments. Each listed
access changed from embedded array addressing to an active pointer load. They are not
dynamic instruction counts, and the compiler can reuse a pointer within a function.

| File | Stack | Frames | Handlers | Used by |
| --- | ---: | ---: | ---: | --- |
| `src/core/run.c` | 135 | 20 | 2 | Reference execution and fallback instructions |
| `src/core/call.c` | 47 | 10 | 2 | Shared calls, callbacks, returns and upvalues |
| `src/core/class.c` | 14 | 2 | 0 | Shared class operations |
| `src/core/module.c` | 0 | 1 | 0 | Shared module execution |
| `src/core/vm.c` | 1 | 0 | 0 | Stack teardown |
| Total | 197 | 33 | 4 | 234 source accesses |

Counts still use their original direct fields. There is no context-pointer lookup before
an active buffer lookup. No growth check or context branch has been inserted into
`push_frame`, `lz_vm_call` or `lz_vm_return`, and their operation order is unchanged.

GE and JIT fast paths use the embedded main arrays directly. Their source changes only
rename those array references. Specifically, `ge_exec.c` keeps direct accesses, the two JIT
trampolines pass `main_stack`, `ge_jit_rt.c` uses the main arrays, and `O_STACK`/`O_FRAMES`
refer to the renamed embedded fields. These changes prevent the new pointer fields from
adding loads to those fast paths. Shared helpers called from GE or JIT still incur the
pointer loads listed above.

Three `_Static_assert` checks verify that each embedded array remains immediately before
its original count field. A separate before/after layout probe found these offsets equal:

| Field | Offset before and after |
| --- | ---: |
| Embedded value stack | 360 |
| `stack_count` | 262504 |
| Embedded frames | 262512 |
| `frame_count` | 360816 |
| `open_upvalues` | 360824 |
| `fuel` | 360832 |
| `instructions` | 360840 |
| Embedded handlers | 372128 |
| `handler_count` | 470432 |
| `lzn` | 472008 |

`LzCallFrame` remains 48 bytes and its declaration is unchanged. Skink moves from 472016
to 472136, after the fields addressed by the JIT. VM size changes from 472800 to 472920.

Compiling HEAD and the changed sources with `cc -O2 -DNDEBUG` on AArch64 produced identical
`.text` bytes for `ge_exec.c`, `ge_jit_rt.c` and `ge_jit_a64.c`. The x86 backend was not
validated by that comparison because it compiles to an empty text section on this host.
This static evidence does not measure runtime cost. The reference and shared-helper loads
mean the requested less-than-1% target remains unproven and needs CI results.

## Verification

Each completed stage passed its build and tests before the next stage began:

* Banner cleanup: 109 passed, 0 failed.
* Signature errors: 109 passed, 0 failed.
* Design documentation: 109 passed, 0 failed, including source-list consistency.
* Context storage: 110 passed, 0 failed.
* Context storage with UBSan and poison: 110 passed, 0 failed.

The ordinary and UBSan suites include native-module tests in reference, GE without JIT and
GE with eager JIT, module CLI tests on those three engines, and the existing fuzz tests.
The new test checks context isolation, handler/frame preservation, global fuel, writes
through an escaped upvalue while a context is inactive, pointer repair through two growth
steps, boundary errors, partial heap-limit failure and zero heap bytes after teardown.
It also runs recursion, a native `map` callback and `try`/`catch` on detached storage with
maximum capacities reserved in advance.

The plain-build script was copied to `/tmp/lizard-coroutine-check.sh` for verification.
Only its source-directory location and test filtering were changed. `bench_scripts` was
excluded because it invokes `bench/measure.sh` on the actual interpreter. The repository
build script and registered test list retain that existing test.

Functional smoke checks compare exit status, stdout and stderr for `examples/*.lz`,
`examples/modules/main.lz`, and copies of every `bench/*.lz` on all three engines. Benchmark
fixture copies use `N = 10` and no timing or performance tool is invoked. The first pass
found that `examples/compound.lz` used the keyword `by` as a parameter name. Renaming it to
`amount` fixes the example without changing its computation. After that fix, all 52
programs passed on all three engines in both the ordinary build and the UBSan/poison build.

ASan plus poison built, but its runtime failed before test execution. The suite reported
108 failures and 2 passes, not a successful sanitizer run. A standalone C program containing
only `int main(void) { return 0; }` reproduces the same ASan initialization failure:

```text
AddressSanitizer: CHECK failed: sanitizer_allocator_primary64.h:131 "((kSpaceBeg)) == ((address_range.Init(TotalSpaceSize, PrimaryAllocatorName, kSpaceBeg)))" (0x500000000000, 0xfffffffffffffff4)
    <empty stack>
```

UBSan with poison was run separately using:

```sh
sh /tmp/lizard-coroutine-check.sh check --builddir=build-context-ubsan --jobs=4 --poison \
  'CFLAGS=-O1 -g -fsanitize=undefined -fno-sanitize-recover=all' \
  'LDFLAGS=-fsanitize=undefined'
```

## Mutation checks

Mutations were compiled from temporary copies of `context.c` and linked ahead of the
ordinary core archive. Repository sources were not mutated.

| Mutation | Result |
| --- | --- |
| Remove open-upvalue location repair during growth | Exit 1. Pointer-location and preserved-value assertions failed. |
| Remove release of occupied value-stack slots during teardown | Exit 1. Reference count, zero-live-object and zero-live-byte assertions failed. |
| Skip context restoration on a coroutine error | Pending. Resume does not exist yet. |
| Remove the native-crossing yield check | Pending. Yield does not exist yet. |

These first two tests verify context primitives, not the full coroutine scenarios requested
for the eventual language tests.

## Diagnostics still to verify

The signature errors are registration diagnostics tested through the C native-module API.
No new coroutine diagnostic can be run through `lizard --color=never` at this checkpoint.
The requested main-yield, native-crossing, invalid-status and coroutine traceback output
must be captured after the language implementation. No such output is claimed here.

## Continuation checkpoint: stack analysis for part A

Part A is in progress. This continuation adds a tested stack-analysis prerequisite, not
automatic growth. Parts B through E have not started. Small detached contexts still require
explicit maximum reservation before execution. The progress document must remain until
the language feature and its verification are complete.

`lz_chunk_stack_plan` in `src/core/stack_plan.c` walks bytecode control flow and reports the
maximum value height above the frame base and maximum number of handlers belonging to
that frame. Entry height includes parameters and the rest slot. The analysis uses
`lz_opcode_length`, validates instruction boundaries, rejects inconsistent joins and models
a catch entry at the saved stack height plus its error value, with its handler removed.
It covers every current opcode, including class operations, imports and exception control
flow. The JIT's existing analysis cannot be reused directly because it rejects several of
those opcodes and depends on translated GE instructions.

The result describes one frame. It does not include another function's frame, dynamic
spread expansion, bound-method receiver insertion or the native callback reserve. Those
must be checked separately at safe call-entry points. The current helper allocates temporary
analysis arrays, returns an error without modifying the result on failure, and is called
only by tests. Caching the result once per function is still pending integration.

`test_stack_plan` is registered in `tests/tests.list`, and `mk/tests.am` is regenerated.
Tests cover branches, back edges, default-argument branches, expression temporary peaks,
spread-call effects, catch entries, nested handlers, malformed/truncated instructions,
operand-byte jump targets, underflow, mismatched joins and maximum stack height. Tests also
compile and run representative recursion, closures, loops, defaults/rest, classes and
exception handlers. An additional compile-only pass analysed all functions and top-level
chunks in `examples/*.lz` and `bench/*.lz`. That pass does not execute those programs.

Two temporary analyzer mutations failed as expected. Removing the catch error value made
catch-height and compiled-function tests fail. Removing peak updates made the expected
height assertions fail. These do not replace the requested pointer-reacquisition and
native-reserve mutations, which remain pending because growth is not integrated yet.

### Borrowed-pointer audit for the next integration step

This list records the call chains that need protection. It is an audit of current code,
not a claim that the protections have been implemented.

| Location | Borrowed data and required treatment |
| --- | --- |
| `call.c: lz_vm_call` | The callee slot pointer must be reacquired after reservation. A copied closure pointer refers to a heap object and survives relocation. Reserve before argument arrangement can write defaults or the rest slot. |
| `call.c: lz_vm_call_ex` | `callee` points into the value stack. Keep `callee_index` and `array_index`, then reacquire after dynamic spread reservation and bound-method expansion. `named[]` is C storage, and the names string and positional array are heap objects. |
| `call.c: unwrap_bound` | Receiver insertion writes one additional slot before a frame exists. Admission must include that slot before the memmove. Heap bound objects survive stack relocation. |
| `call.c: call_native` | `args` borrows the argument slice for the entire native call. Reserve before constructing it, then pin all context buffers until the native returns. Leaf natives need no callback reserve. |
| `call.c: construct_on_stack` | Constructor positional arguments borrow the stack. Class construction can execute defaults and `init`, or call a native constructor. Reserve before passing the slice and keep buffers pinned throughout. |
| `call.c: lz_vm_call_value` | Both `callee` and `argv` may borrow stack storage. Callback arguments must fit before their initial copy, which happens before `push_frame`. Nested callbacks must not replenish a pinned reserve. |
| `call.c: push_frame` | Caller frame pointers must be acquired after reservation. Keep resume locations and callee bases as indices. |
| `call.c: capture_upvalue`, `lz_vm_make_closure`, `lz_vm_close_upvalues`, trace capture | These do not enter Lizard functions. They allocate or release heap objects but must not independently relocate execution buffers. Open-upvalue relocation remains the reservation helper's responsibility. |
| `run.c: CALL` and `CALL_EX` | The loop keeps chunk pointers and an instruction index, then reloads the current frame after a call. The leaf native branch borrows `callee + 1` but cannot call back. |
| `run.c: binary_op`, `equal`, `negate`, `bit_not`, membership and range comparisons | Operand pointers can reach class operators or equality callbacks. Protect the entire operation before passing operands to shared helpers. In particular, ordering may perform a second equality callback after the first comparison. |
| `run.c: get_field_op`, `member_get_op`, field writes and superclass/member lookup | Getter paths can call Lizard code. Several paths copy values to C locals and access stack results again by index, but incoming borrowed arguments still require protection around the complete callback operation. |
| `run.c: class_init_op` and class/error initialization paths | Class constants, default fields and initialization methods can call back without a native CALL opcode. They require the same admission and pinning policy. |
| `run.c: unwind_to_handler`, `try_begin_op` | Unwinding copies the selected handler by value. Opening a handler holds a pointer only while filling it. Reserve the function's maximum handler requirement before execution, so neither needs to grow storage locally. |
| `run.c: describe_subject`, `current_owner`, type checks and closure access | Frame pointers are used synchronously to obtain metadata. They must not become hidden growth points while reporting errors or checking values. |
| `class.c: construct_impl` | `given[]` contains pointers into positional or named argument storage. Supplied fields are stored before default callbacks, but the outer constructor still owns a borrowed argument slice. Protect the full constructor. |
| `class.c: run_operator`, `less_than`, ordering/equality helpers | `run_operator` copies values into C `argv[]`, but its callers retain borrowed operand pointers and may inspect them after the callback. Copying only the immediate argv does not make relocation safe. |
| `class.c: class constants`, getters, `run_inits`, text methods | These enter through `call_closure` and `lz_vm_call_value`. Member pointers refer to heap class data, while receiver arguments may originate on the VM stack. Protect the outer callback-capable operation. |
| `class.c: instance_callee`, `class_callee`, `lz_vm_super_lookup`, `lz_vm_method_lookup` | Some paths already copy receivers and use stack indices. Getter branches still enter callbacks. Reload stack destinations by index and preserve callback pinning. |
| `module.c` | Lookup pointers refer to table entries, and the current frame is inspected synchronously. The actual frame-entry code is elsewhere. |
| `frontend/module_load.c: run_module_code` | This pushes a synthetic callee and frame directly, bypassing `push_frame`, then calls the nested interpreter. It needs module-chunk analysis and admission too. Failure cleanup already reloads the frame by index. |
| `vm.c` | Reset and root release do not push frames. They close upvalues and release values from the active storage. They must not switch or grow buffers during that teardown. |

The central hazard is not limited to `argv`: a caller of a callback helper can retain an
operand pointer even if the helper copied its own arguments. The native reserve must be
established before the outer borrowed slice is exposed. A callback's first stack writes
must also be checked before entering its interpreter loop.

### Remaining part A work and cost

Still required are per-function caching, safe admission at every function/module entry,
value/frame/handler reserve enforcement, context-local pinning, fixed callback budgets,
pointer reacquisition, small-capacity execution tests, and the requested mutation checks.
The main-context path must retain its existing checks without additional context branches.
The selection mechanism for coroutine-specific admission is not implemented or finalized.

This continuation changes no existing interpreter, shared call helper, GE or JIT access.
It adds zero runtime indirections or checks because the analyzer is not yet wired into
execution. The 234 indirect accesses from the previous checkpoint still apply. The new
analyzer does not establish the overall less-than-1% performance target.

ASan remains pending CI as requested. It was not retried in this continuation.

Final verification for this continuation: 111 tests passed in the ordinary build and 111
passed with UBSan plus poison, with zero failures or skips in either filtered suite.
`bench_scripts` remains excluded for the reason recorded above. Compile-only analysis of
all `examples/*.lz` and `bench/*.lz` also passed in both builds. Source-list consistency,
generated test registration, whitespace checks and the no-banner check for new files passed.
No new user-facing diagnostic is installed by this analyzer-only checkpoint.
