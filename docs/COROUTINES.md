# Coroutines

Coroutines use fixed execution stacks, the reference interpreter and the existing native
method table. This document records their language contract and implementation.

A coroutine runs a function on its own value stack. It can stop inside that function or
inside any Lizard function it calls, then continue with its locals, closures and open `try`
blocks intact. Calls are asymmetric: `resume` transfers control to a coroutine and `yield`
returns control to that particular caller. There is no scheduler and no new syntax.

## Values and calls

`coroutine(fn, depth = 1000)` accepts a Lizard function or closure and creates a suspended
coroutine. The signature shows the default, but arguments are positional. Ordinary builtins
do not accept named arguments. `depth` must be an `int` from 1 through `LZ_FRAMES_MAX`
(2048), inclusive. Other types raise `TypeError` and out-of-range integers raise `ValueError`.
Both diagnostics include the accepted range in `help:`.
Native functions, classes and other values are rejected with a type error. Creation retains
the function but allocates no execution stacks. `typeof(co)` returns `"coroutine"`, and
`coroutine` is also a type annotation. The text representation is
`<coroutine suspended>`, with the current status in place of `suspended`.

The first `co.resume(args...)` passes its arguments to the function. Arity checks, default
expressions and the rest array use the existing call machinery. Subsequent calls pass their
arguments back as the result of the pending `yield` call. Arguments are positional, as they
are for other native methods. The usual spread syntax can supply a variable number.

Both directions use the same packing rule:

| Number of values | Value received |
| --- | --- |
| Zero | `nil` |
| One | That value, unchanged |
| Two or more | A new array containing the values in order |

Thus `yield([1, 2])` and `yield(1, 2)` both deliver an array. Packing does not wrap a single
array in another array. A successful function return delivers its ordinary return value,
including an implicit `nil`. Use `co.status()` to distinguish a yielded `nil` from completion.

For example:

```lizard
let co = coroutine(fn(start = 1, *extra) {
    let reply = yield(start, extra); return reply
})
println(co.status())
println(co.resume(4, 5))
println(co.status())
println(co.resume(8, 9))
println(co.status())
```

`coroutine` and `yield` are ordinary builtins.

| Status | Meaning |
| --- | --- |
| `suspended` | Created but not started, or waiting at `yield` |
| `running` | Currently executing |
| `normal` | Waiting for a coroutine it resumed to yield or return |
| `done` | The function returned successfully |
| `failed` | An error escaped the coroutine |

Only a suspended coroutine can be resumed. Invalid transitions report
`cannot resume a coroutine that is running`, `cannot resume a coroutine that is normal`,
`cannot resume a coroutine that is done`, or `cannot resume a coroutine that is failed`.
A rejected resume does not change the target's status.

## Execution contexts

The context holds the value stack, frame stack, handler stack, their counts and capacities,
and the list of open upvalues. Fuel, instruction accounting, heap accounting, module tables
and the collector belong to the VM and are shared by every context.

The main program keeps the existing embedded arrays. Their layout remains available to GE
and the JIT, which only execute the main context. The reference interpreter and shared VM
helpers use active stack pointers, with counts still directly in the VM. Context switches
save those fields into the outgoing context and load the incoming context. This avoids an
extra context-pointer dereference for every stack access. The existing reference-engine
pointer-load counts and capacity checks are detailed under Implementation notes.

Each coroutine allocates fixed buffers on its first resume. For a requested depth `d`,
the capacities are:

| Buffer | Capacity | Default (`d = 1000`) | Small (`d = 50`) |
| --- | --- | --- | --- |
| Call frames | `d` | 1000 | 50 |
| Values | `min(8*d, LZ_STACK_MAX)` | 8000 | 400 |
| Open handlers | `min(d/4 + 16, LZ_HANDLERS_MAX)` | 266 | 28 |

Integer division rounds down. The main program still has 2048 frames, 16384 values and
4096 handlers. Frame capacity includes the internal caller frame used by resume, as well
as the entry function. Thus `depth = 1` is accepted at creation but cannot start a function.
Each buffer has its own limit. A function with many locals can exhaust values first, and
recursion with an open `try` in every call can exhaust handlers first.

The ratio of eight values per frame leaves room for parameters, callee slots, locals and
expression temporaries. A functional audit adapted the factorial and mutual recursion in
`examples/functions.lz` and Fibonacci in `bench/fib.lz` to yield at the base case. No timing
was collected. The suspended snapshots held 151 values in 52 frames for factorial,
52 in 27 for Fibonacci's first descent, and 104 in 53 for mutual recursion. The deep-upvalue
test held 4506 values in 903 frames, about five per frame. Fibonacci can also retain a left
result while computing its right branch. Eight allows room beyond these common cases
without imposing the main program's value budget on every small coroutine. This is a
budget, not a guarantee for arbitrary locals or temporary argument lists.

The handler ratio allows a quarter of the frames to keep one `try` open, with 16 extra
entries for shallow nesting. The ordinary recursion examples and bench programs above
keep no handlers open. Code that holds one per recursive frame needs a larger depth or
fewer simultaneously open handlers. At the maximum depth, the handler budget is 528.

The buffers never move, so borrowed native `argv` slices, class operator operands and open
upvalue locations stay valid through callbacks and suspension. There is no stack growth.

The full capacity counts against `--max-heap` after the first resume, including while
suspended, so the heap limit also bounds the number of live started coroutines. Buffers
are allocated with `malloc` and only live slots are initialized in ordinary builds.
Untouched pages need not consume physical memory. Poison builds may fill all storage.
Allocation failure rolls back all partial allocations and reports the ordinary heap-limit
or out-of-memory diagnostic. Completion, failure and destruction release the buffers.
The main program retains its embedded arrays and original capacities.

On this 64-bit build, a value is 16 bytes, a frame is 48 bytes, a handler is 24 bytes and
a coroutine object is 192 bytes. Buffer cost is
`16*min(8*d, 16384) + 48*d + 24*min(d/4 + 16, 4096)` bytes:

| Depth | Buffer bytes | Buffer KiB | Object plus buffers |
| --- | --- | --- | --- |
| 8 | 1840 | 1.80 | 2032 bytes |
| 50 | 9472 | 9.25 | 9664 bytes |
| 1000 (default) | 182384 | 178.11 | 182576 bytes |
| 2048 | 373120 | 364.38 | 373312 bytes |

These figures exclude retained closures, user values, container storage and allocator
metadata. Structure sizes can differ on other targets. An unstarted coroutine pays only
for its object and retained entry function. A started suspended coroutine pays for the
entire buffer capacity in `--max-heap`, even if most pages remain untouched. Returning,
failing or being collected releases the buffers and removes their charge immediately.
A coroutine object retained after completion still counts until it is released.

For thousands of small suspended tasks, select a shallow budget explicitly. This example
is executed as `tests/coroutines/small_many.lz`:

```lizard
let held = []
for i in range(3000) {
    let co = coroutine(fn(n) => yield(n), 50)
    co.resume(i)
    held.push(co)
}
println(len(held))
println(held[0].status(), held[2999].status())
for co in held { co.resume() }
println(held[0].status(), held[2999].status())
```

```text
3000
suspended suspended
done done
```

The default handles 900 recursive levels, including suspension at the deepest call.
This example is executed as `tests/coroutines/default_deep.lz`:

```lizard
fn down(n) {
    if n == 0 { return yield(19) }
    return down(n - 1)
}
let co = coroutine(fn() => down(900))
println(co.resume())
println(co.resume(23))
println(co.status())
```

```text
19
23
done
```


`resume` is a native function. It retains the target as an execution root, switches contexts
and calls the nested reference interpreter. `yield` returns an internal `LZ_YIELD` status.
The loop saves the instruction pointer immediately after the call and returns without
unwinding frames or handlers. The next resume replaces the pending call result with the
packed resume arguments. A yield is control flow and must never enter error handling.

Every exit from resume restores the caller's context, including errors during argument
arrangement or execution. Native call-site metadata must also be restored. Error information
is deliberately propagated to the caller. The caller's GE/JIT state must see the same stack
addresses, counts and frame state that it had before entering the native call.

Nested resumes use the existing `LZ_NATIVE_DEPTH_MAX` and the C-stack byte budget used by
`lz_vm_call_value`. The depth saved at entry to resume is also the yield boundary. A nested
callback raises `native_depth`, so yield can reject crossing that callback without confusing
it with a legal yield in a separately resumed child coroutine.

## Deliberate limits

| Situation | Behavior and reason |
| --- | --- |
| Running coroutine code with GE or JIT selected | The body still runs in the reference interpreter. A GE coroutine body is future work. |
| `yield` in the main program | `cannot yield from the main program` |
| `yield` through a native callback | `cannot yield across a native call` |
| `map`, `filter`, `sort`, a getter, `init`, or any nested `lz_vm_call_value` callback | The callback must finish before its native caller returns. Its C stack cannot be suspended. |
| `lzn_call` inside a coroutine | The call works synchronously, with the same prohibition on yielding from the callback. |
| Threads and scheduling | No threads, event loop, cancellation or automatic scheduling are added. |

A Lizard function called directly by another Lizard function inside the coroutine may yield
at any depth. The restriction concerns an intervening native callback, not an ordinary
Lizard call. A callback can also resume a different coroutine, whose own yield returns to
that nested resume without crossing the callback's C frame.

## Errors and limits

A `try` inside a coroutine remains open across yield. It catches errors only in that
context. If an error escapes, the coroutine becomes failed and releases its execution
storage. The error leaves `resume` with its original kind and message, so the caller can
catch it normally. The trace records coroutine frames first, then the caller frames at
resume. Native functions themselves do not gain synthetic frames.

Record the coroutine trace before dropping its frames. Preserve the innermost source and
caret offset when appending the caller trace, including source files belonging to imported
modules. An error in a resumed child can be caught by its parent coroutine.

Fuel and `vm->instructions` are global. Resuming another coroutine never replenishes fuel.
The heap limit covers coroutine objects and their buffer capacities, not just live slots.
Value, frame and handler limits apply separately to each context. Coroutine overflows
identify the exhausted buffer and that coroutine's limit. Their `help:` suggests
`coroutine(fn, depth)` with a larger depth up to 2048. At depth 2048, it instead explains
that the maximum is already selected. Main-program diagnostics remain unchanged. Native nesting additionally
uses the existing native-depth and C-stack limits, with the existing overflow diagnostics.

## Ownership and collection

A coroutine retains its entry closure until execution no longer needs it. Each occupied
value slot owns one reference. The open-upvalue list owns one reference per upvalue. Existing
`LzCallFrame.closure` pointers borrow the callee reference already on the value stack.
That ownership must stay unchanged for ordinary calls. Coroutine tracing must reach frame
closures through those callee slots without subtracting a second, nonexistent reference.
If an additional retained frame reference is introduced for coroutine frames, tracing and
teardown must both account for it explicitly.

Before a stack is freed, its open upvalues close over their current values. An escaped
closure therefore remains valid even if its coroutine is discarded. While the coroutine is
suspended, an escaped closure still points to the actual local slot. Calling it from outside
can change that local, and the resumed function must observe the change.

Skink enumerates the entry closure, occupied stack slots and open-upvalue list as owned
edges. Frame closures are reached through callee slots, so there is no duplicate owned edge.
Open upvalues also lead to the value in their slot as a borrowed edge. Discovery follows
that edge without subtracting a reference, while marking follows it to keep a live escaped
closure's value intact. Coroutine seeds make open-only cycles discoverable without adding
owner metadata or a store to ordinary upvalue capture.

Full collection seeds all coroutine objects from the heap when the coroutine count is
nonzero. This discovers cycles containing only open upvalues, including cycles with no
closed-upvalue watch entry. Active and normal coroutines have retained execution roots in
`resume`. For an active context, tracing reads current VM counts instead of saved counts.

Discarding a suspended coroutine closes upvalues before releasing occupied slots and buffers.
Collection retains all dead nodes first and clears coroutine contexts before their other
children, because closing moves slot references into upvalues. A collection guard prevents
teardown from modifying the pending-suspect list while a trial is using it. Normal return
and uncaught failure release execution storage immediately. An unstarted coroutine has no
buffers. Both VM root release and VM destruction handle suspended contexts, including open
`try` blocks and escaped upvalues.

## Async and native modules

Coroutine suspension is the execution mechanism for future async libraries. A native module
registered through [lzapi.h](NATIVE.md) will provide an event loop. A library written in Lizard
will manage ready coroutines and resume them when work completes. Future `async` and `await`
syntax can be sugar over that protocol.

No suspendable native-call ABI is introduced here. A future protocol may let native code
return a pending status and let an event loop wake the waiting coroutine. Such a protocol
must define ownership and completion without keeping native handles or a C stack frame alive.
For now `lzn_call` remains synchronous, and coroutine values passed through native handles
follow the normal handle lifetime rules.

## Implementation notes

The public status is `LZ_YIELD`. Only the nested reference interpreter propagates it to
`resume`, saving the IP after CALL or CALL_EX without unwinding handlers or frames.
`call_native` replaces the yield call and its arguments with an owned packed result.
`resume` transfers that result to its caller and leaves a nil placeholder in the suspended
stack. The next resume replaces that placeholder with the packed reply. An internal caller
frame lets coroutine entry use ordinary argument arrangement and return machinery without
changing `LzCallFrame`. It is omitted from traces.

Resume retains the coroutine before allocation or switching, marks the parent normal,
checks the existing native-depth and C-stack budgets, and always restores the caller's
storage, counts and open-upvalue list. It also restores `native_depth`, `current_native`,
`native_call_offset` and the C-stack floor. Fuel and instruction counts remain global.
The innermost error source is kept while caller frames are appended to its trace.

### Borrowed-pointer audit

Fixed addresses protect all of these borrowed references. No reserve, pinning or pointer
repair is needed while calling back into the interpreter.

| Location | Borrowed reference |
| --- | --- |
| `call.c`, `lz_vm_call`, `lz_vm_call_ex`, `unwrap_bound` | Callee slots, arguments, receiver insertion and spread destinations |
| `call.c`, `call_native`, `construct_on_stack`, `lz_vm_call_value` | Native argv slices, constructor arguments and callback arguments |
| `call.c`, `push_frame`, capture, close and trace helpers | Caller frames and open-upvalue locations |
| `run.c`, CALL and CALL_EX | Callee and argument slices |
| `run.c`, arithmetic, ordering, equality, membership and ranges | Operator operands retained across class callbacks |
| `run.c`, field, member, superclass and initialization operations | Receivers, getter operands and class constants |
| `run.c`, handlers, type checking and subject descriptions | Handler entries, frame metadata and local slots |
| `class.c`, `construct_impl` | Given field pointers into positional and named arguments |
| `class.c`, operators, constants, getters, inits and text methods | Receivers and outer operand pointers, including after nested callbacks |
| `class.c`, callee, superclass and method lookup | Receiver slots and callback destinations |
| `module.c` and `frontend/module_load.c`, `run_module_code` | Frame metadata and synthetic module frame storage |
| `vm.c`, reset and root release | Active slots during closing and teardown |

### Capacity audit

Every maximum-constant use under `src/` has the following disposition. The declarations of
embedded arrays in `vm.h` retain the old constants.

| File and operation | Limit source |
| --- | --- |
| `run.c`, push, member keep, CONSTANT fast path | `vm->stack_cap`, replacing three constant comparisons |
| `run.c`, TRY_BEGIN | `vm->handler_cap`, replacing one constant comparison |
| `call.c`, push_frame | `vm->frame_cap`, replacing the comparison and cold diagnostic arguments |
| `call.c`, arrange, unwrap_bound, call_value, call_ex, make_closure | `vm->stack_cap`, replacing five comparisons |
| `frontend/module_load.c`, module frame entry | `vm->frame_cap` and `vm->stack_cap`, replacing two comparisons |
| `context.c`, explicit allocation admission | Active capacities, only on first resume or explicit C reservation |
| `vm.c`, main initialization | Original stack, frame and handler constants for capacities and limits |
| `ge_exec.c`, GE_ROOM, CONSTANT, scalar pushes, GET_LOCAL, GET_GLOBAL, closure calls, GET_UPVALUE and member keep | Original stack/frame constants, GE executes main storage only |
| `ge_jit_a64.c`, entry depth, closure call and compiled peak admission | Original stack/frame constants |
| `ge_jit_x64.c`, entry depth, closure call and compiled peak admission | Original stack/frame constants |

The native-depth limit remains `LZ_NATIVE_DEPTH_MAX` and the C-stack byte budgets are
unchanged. The stack analyzer and its growth-only tests were removed. Both build paths
read `mk/sources.am`, and the generated test list is checked by `tools/check-sync.sh`.

### Cost

The earlier context refactor changed embedded addressing to active VM pointer loads at
234 lexical source accesses. These counts are source occurrences, not executed loads, and
compilers can reuse a pointer.

| File | Stack | Frames | Handlers | Engines affected |
| --- | ---: | ---: | ---: | --- |
| `run.c` | 135 | 20 | 2 | Reference and instructions delegated by GE/JIT |
| `call.c` | 47 | 10 | 2 | Shared helpers on all engines |
| `class.c` | 14 | 2 | 0 | Shared class helpers on all engines |
| `module.c` | 0 | 1 | 0 | Shared module helpers |
| `vm.c` | 1 | 0 | 0 | Stack teardown |

Fixed-capacity execution replaces twelve existing constant comparisons with direct VM
capacity loads, four in reference operations, six in shared calls and two in module entry.
Main execution gains no new admission check or context branch at these comparisons.
Active counts remain direct VM fields. Context switches save/load pointers and counts
without copying stack values. Allocation and capacity checks happen only at first resume.
GE/JIT fast paths continue addressing embedded main arrays and constant bounds directly.
Their generated stack/frame offsets are protected by static assertions. Shared fallback
helpers incur the pointer and capacity loads listed above.

The native call's existing non-success branch now distinguishes yield. The nested reference
loop distinguishes yield only on its existing non-success path. Collector work follows
coroutine stack and borrowed upvalue edges, uses an active snapshot test while visiting a
coroutine, and checks a coroutine count at allocation ticks.
When that count is zero it skips heap enumeration for coroutine seeds. Entry closure and
frame references keep their existing ownership conventions. The extra root retain/release,
context switching, packing, trace concatenation and buffer allocation occur in coroutine
operations. The coroutine resume path checks native depth before switching. The existing
`lz_vm_call_value` callback path keeps its original inline depth checks. This is a source audit,
with no runtime overhead percentage claimed. No
benchmark was run. The earlier checkpoint's AArch64 code comparison found unchanged GE and
JIT fast-path text after the storage refactor. It did not validate x86 generated execution.
The current build host is AArch64 Linux. The added capacity loads are in the reference and
shared helper paths. The GE interpreter and AArch64 JIT fast paths continue using main-array
constants. The x64 JIT source also retains those constants but was not executed on this host.

### Verification

The ordinary and UBSan plus poison suites passed 115 tests each. The `bench_scripts` test
was excluded because it invokes timing measurements. Verification used a local copy of
`tools/build-plain.sh` with only that test filtered out, with `TMPDIR` set to Termux's
writable temporary directory. UBSan used the following flags together with `--poison`:

```text
-O1 -g -fsanitize=undefined -fno-sanitize=function -fno-sanitize-recover=all -fno-omit-frame-pointer
```

Clang 21's `function` instrumentation reads metadata before the generated JIT trampoline,
which can cross its mapping boundary. The unfiltered sanitizer build segfaulted in six
JIT tests, while `LZ_JIT=off test_engine` passed. Disabling that one check in a fresh build
keeps the other undefined-behavior checks and all three execution modes enabled.
ASan remains pending CI because it cannot run in this Termux environment.
The final build-script summaries were:

```text
=> 115 tests: 115 passed, 0 skipped, 0 failed (logs: /data/data/com.termux/files/home/lizard-lang/build-depth/test-logs)
=> 115 tests: 115 passed, 0 skipped, 0 failed (logs: /data/data/com.termux/files/home/lizard-lang/build-depth-ubsan-fixed/test-logs)
```

All 30 top-level examples, 21 reduced fixture copies of `bench/*.lz` with `N=10`, and
`examples/modules/main.lz` completed identically in reference, GE without JIT and eager JIT
on ordinary and UBSan/poison builds. These were functional checks with no timings. Existing module and native suites run on all three
engines. The coroutine runner compares exact stdout, stderr and exit status, including
module-created coroutines and diagnostic source/caret/trace fixtures.

Capacity tests additionally cover every accepted depth from 1 through 2048, rejected depth
values and error kinds, default recursion to 900, failure at 1100, failure beyond a depth
of 50, deep open upvalues, exact buffer charges before and after completion or failure, and 20000
started coroutines at depth 8 within 64 MiB. Depth 2048 fails cleanly within a 4 MiB heap.
The CLI fixtures compare exit status and all diagnostic text across all three engines.

The default-depth heap refusal test now uses a 4 MiB cap instead of 512 MiB, so it
verifies refusal without retaining thousands of larger default buffers. The small-cycle
stress test explicitly requests depth 50 to keep its collection workload small. Old value,
frame and handler diagnostic fixtures now expect limits 8000, 1000 and 266. The new depth
50 fixtures expect 400 values, 50 frames and 28 handlers. The ordinary call path retains
the same successful comparisons and stores. Only overflow branches call the new context
aware diagnostic helpers. Depth validation and budget derivation run during coroutine
creation. No new coroutine check runs on a successful ordinary instruction or call.

The checkout also contained an unregistered stack-plan source/test and progress document.
They are now listed in the build manifests so the source-synchronization check passes.
This adds no stack planning to execution.

The C tests cover lazy allocation, full-capacity charging, partial-allocation rollback,
small value/frame/handler limits, stable upvalues, value exchange, defaults/rest/spread,
status transitions, deep yields, alias mutation while suspended, escaped closures after
context destruction, try/catch across yield, native map/sort crossings, nested resumes,
fuel, heap-bounded attempts to suspend 20000 default-depth coroutines, and an open-only cycle where a
suspended coroutine holds itself in a stack slot captured by an open upvalue. The test runs
Skink after its last external root is gone and requires a cycle deletion. It also covers
direct VM destruction with suspended try/upvalues. Native module tests call
`lzn_call` from a coroutine and reject callback yield. Fuzzing adds random coroutine values,
fuel and heap limits to differential engine checks, including poison runs.

| Mutation in temporary source copies | Observed test result |
| --- | --- |
| Remove a context value-bound comparison | Exit 1 |
| Skip context buffer teardown | Exit 1 |
| Remove a release of occupied stack slots | Exit 1 |
| Skip restoration on coroutine error | Signal 11 |
| Remove native-crossing detection | Exit 1 |
| Omit coroutine stack edges in Skink | Exit 1 |
| Remove the reference value-push bound check | Exit 1 |
| Calculate value capacity as `7*d` instead of `8*d` | Exit 1 |

### Diagnostic transcripts

These are actual `--color=never` reference-engine outputs. The runner requires identical
output and exit status on all three engines. The heap failure uses `--max-heap=4M`.
Call carets use the same source offsets as existing calls.

`bad_arg_bool.lz`

```text
error: coroutine(): depth must be an int, got bool
 --> bad_arg_bool.lz:1:10
  |
1 | coroutine(fn() => nil, true)
  |          ^
  |
  = help: use coroutine(fn, depth) with an int depth from 1 to 2048 (default 1000)
```

`bad_arg_float.lz`

```text
error: coroutine(): depth must be an int, got float
 --> bad_arg_float.lz:1:10
  |
1 | coroutine(fn() => nil, 1.0)
  |          ^
  |
  = help: use coroutine(fn, depth) with an int depth from 1 to 2048 (default 1000)
```

`bad_arg_high.lz`

```text
error: coroutine(): depth must be between 1 and 2048, got 2049
 --> bad_arg_high.lz:1:10
  |
1 | coroutine(fn() => nil, 2049)
  |          ^
  |
  = help: use coroutine(fn, depth) with an int depth from 1 to 2048 (default 1000)
```

`bad_arg_named.lz`

```text
error: builtin functions do not take named arguments
 --> bad_arg_named.lz:1:24
  |
1 | coroutine(fn() => nil, depth = 50)
  |                        ^^^^^

error: aborting due to 1 previous error
```

`bad_arg_negative.lz`

```text
error: coroutine(): depth must be between 1 and 2048, got -1
 --> bad_arg_negative.lz:1:10
  |
1 | coroutine(fn() => nil, -1)
  |          ^
  |
  = help: use coroutine(fn, depth) with an int depth from 1 to 2048 (default 1000)
```

`bad_arg_string.lz`

```text
error: coroutine(): depth must be an int, got string
 --> bad_arg_string.lz:1:10
  |
1 | coroutine(fn() => nil, "50")
  |          ^
  |
  = help: use coroutine(fn, depth) with an int depth from 1 to 2048 (default 1000)
```

`bad_arg_zero.lz`

```text
error: coroutine(): depth must be between 1 and 2048, got 0
 --> bad_arg_zero.lz:1:10
  |
1 | coroutine(fn() => nil, 0)
  |          ^
  |
  = help: use coroutine(fn, depth) with an int depth from 1 to 2048 (default 1000)
```

`bad_depth.lz`

```text
error: stack overflow: too many nested calls in a coroutine (limit 1000)
 --> bad_depth.lz:3:16
  |
3 |     return down(n - 1)
  |                ^
  |
  = help: use coroutine(fn, depth) with a larger depth (up to 2048) to raise this limit
  = stack trace: innermost call first
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      at down (bad_depth.lz:3:16)
      ... 984 more frames
```

`bad_depth_1.lz`

```text
error: stack overflow: too many nested calls in a coroutine (limit 1)
 --> bad_depth_1.lz:2:10
  |
2 | co.resume()
  |          ^
  |
  = help: use coroutine(fn, depth) with a larger depth (up to 2048) to raise this limit
  = stack trace: innermost call first
      at <script> (bad_depth_1.lz:2:10)
```

`bad_depth_50.lz`

```text
error: stack overflow: too many nested calls in a coroutine (limit 50)
 --> bad_depth_50.lz:3:16
  |
3 |     return down(n - 1)
  |                ^
  |
  = help: use coroutine(fn, depth) with a larger depth (up to 2048) to raise this limit
  = stack trace: innermost call first
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      at down (bad_depth_50.lz:3:16)
      ... 34 more frames
```

`bad_done.lz`

```text
error: cannot resume a coroutine that is done
 --> bad_done.lz:3:10
  |
3 | co.resume()
  |          ^
```

`bad_failed.lz`

```text
error: cannot resume a coroutine that is failed
 --> bad_failed.lz:4:10
  |
4 | co.resume()
  |          ^
```

`bad_handlers.lz`

```text
error: stack overflow: too many open try blocks in a coroutine (limit 266)
 --> bad_handlers.lz:1:16
  |
1 | fn recurse() { try { return recurse() } catch e { return nil } }
  |                ^^^
  |
  = help: use coroutine(fn, depth) with a larger depth (up to 2048) to raise this limit
  = stack trace: innermost call first
      at recurse (bad_handlers.lz:1:16)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      at recurse (bad_handlers.lz:1:36)
      ... 252 more frames
```

`bad_handlers_50.lz`

```text
error: stack overflow: too many open try blocks in a coroutine (limit 28)
 --> bad_handlers_50.lz:1:16
  |
1 | fn recurse() { try { return recurse() } catch e { return nil } }
  |                ^^^
  |
  = help: use coroutine(fn, depth) with a larger depth (up to 2048) to raise this limit
  = stack trace: innermost call first
      at recurse (bad_handlers_50.lz:1:16)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      at recurse (bad_handlers_50.lz:1:36)
      ... 14 more frames
```

`bad_heap_large.lz`

```text
error: heap limit of 4194304 bytes exceeded
 --> bad_heap_large.lz:4:14
  |
4 |     co.resume()
  |              ^
  |
  = stack trace: innermost call first
      at <script> (bad_heap_large.lz:4:14)
```

`bad_main.lz`

```text
error: cannot yield from the main program
 --> bad_main.lz:1:6
  |
1 | yield()
  |      ^
```

`bad_max_values.lz`

```text
error: stack overflow: too many values in a coroutine (limit 16384)
 --> bad_max_values.lz:1:33
  |
1 | let co = coroutine(fn() => print(*range(17000)), 2048)
  |                                 ^
  |
  = help: coroutine(fn, depth) is already at the maximum depth of 2048. Reduce live values, calls or try blocks
  = stack trace: innermost call first
      at <fn> (bad_max_values.lz:1:33)
      at <script> (bad_max_values.lz:2:10)
```

`bad_native.lz`

```text
error: cannot yield across a native call
 --> bad_native.lz:1:20
  |
1 | fn step(x) => yield(x)
  |                    ^
  |
  = stack trace: innermost call first
      at step (bad_native.lz:1:20)
      at <fn> (bad_native.lz:2:35)
      at <script> (bad_native.lz:3:10)
```

`bad_normal.lz`

```text
error: cannot resume a coroutine that is normal
 --> bad_normal.lz:2:44
  |
2 | let child = coroutine(fn() => box[0].resume())
  |                                            ^
  |
  = stack trace: innermost call first
      at <fn> (bad_normal.lz:2:44)
      at <fn> (bad_normal.lz:3:40)
      at <script> (bad_normal.lz:4:14)
```

`bad_running.lz`

```text
error: cannot resume a coroutine that is running
 --> bad_running.lz:2:41
  |
2 | box[0] = coroutine(fn() => box[0].resume())
  |                                         ^
  |
  = stack trace: innermost call first
      at <fn> (bad_running.lz:2:41)
      at <script> (bad_running.lz:3:14)
```

`bad_trace.lz`

```text
error: Error: broken
 --> bad_trace.lz:1:13
  |
1 | fn fail() { throw Error("broken") }
  |             ^^^^^
  |
  = stack trace: innermost call first
      at fail (bad_trace.lz:1:13)
      at body (bad_trace.lz:2:24)
      at drive (bad_trace.lz:4:23)
      at <script> (bad_trace.lz:5:6)
```

`bad_values.lz`

```text
error: stack overflow: too many values in a coroutine (limit 8000)
 --> bad_values.lz:1:58
  |
1 | fn recurse(n) { let a = n; let b = n; let c = n; let d = n; let e = n; let f = n; let g = n; let h = n; let i = n; let j = n; let k = n; let capture = fn() => a; return recurse(n + 1) }
  |                                                          ^
  |
  = help: use coroutine(fn, depth) with a larger depth (up to 2048) to raise this limit
  = stack trace: innermost call first
      at recurse (bad_values.lz:1:58)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      at recurse (bad_values.lz:1:177)
      ... 558 more frames
```

`bad_values_spread.lz`

```text
error: stack overflow: too many values in a coroutine (limit 400)
 --> bad_values_spread.lz:1:33
  |
1 | let co = coroutine(fn() => print(*range(500)), 50)
  |                                 ^
  |
  = help: use coroutine(fn, depth) with a larger depth (up to 2048) to raise this limit
  = stack trace: innermost call first
      at <fn> (bad_values_spread.lz:1:33)
      at <script> (bad_values_spread.lz:2:10)
```
