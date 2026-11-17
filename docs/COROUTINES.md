# Coroutines

Coroutines use fixed execution stacks, the reference interpreter and the existing native
method table. This document records their language contract and implementation.

A coroutine runs a function on its own value stack. It can stop inside that function or
inside any Lizard function it calls, then continue with its locals, closures and open `try`
blocks intact. Calls are asymmetric: `resume` transfers control to a coroutine and `yield`
returns control to that particular caller. There is no scheduler and no new syntax.

## Values and calls

`coroutine(fn)` accepts a Lizard function or closure and creates a suspended coroutine.
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
extra context-pointer dereference for every stack access. The additional reference-engine
pointer-load counts and capacity checks are detailed under Implementation notes.

Each coroutine allocates fixed buffers on its first resume, with `LZ_CO_STACK_MAX` set to
2048 values, `LZ_CO_FRAMES_MAX` to 200 frames and `LZ_CO_HANDLERS_MAX` to 64 handlers.
The buffers never move, so borrowed native `argv` slices, class operator operands and open
upvalue locations stay valid through callbacks and suspension. Automatic growth was
cancelled because these borrowed pointers occur throughout callback-capable helpers.
Growth can be reconsidered with evidence if fixed capacities become limiting. A future
optional argument to `coroutine(fn, ...)` could configure capacities, but this version does
not expose it.

The full capacity counts against `--max-heap` after the first resume, including while
suspended, so the heap limit also bounds the number of live started coroutines. Buffers
are allocated with `malloc` and only live slots are initialized in ordinary builds.
Untouched pages need not consume physical memory. Poison builds may fill all storage.
Allocation failure rolls back all partial allocations and reports the ordinary heap-limit
or out-of-memory diagnostic. Completion, failure and destruction release the buffers.
The main program retains its embedded arrays and original capacities.

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
Value, frame and handler limits apply separately to each context. Native nesting additionally
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
shared helper paths; the GE interpreter and AArch64 JIT fast paths continue using main-array
constants. The x64 JIT source also retains those constants but was not executed on this host.

### Verification

The complete ordinary and UBSan plus poison suites passed 114 tests each. The bench timing
test was excluded. ASan is pending CI. It was not retried here because an empty C program
already reproduced the AArch64 sanitizer allocator initialization failure in this environment.

All 52 examples and reduced fixture copies of `bench/*.lz` with `N=10` completed identically
in reference, GE without JIT and eager JIT on ordinary and UBSan/poison builds. These were
functional checks with no timings. Existing module and native suites run on all three
engines. The coroutine runner compares exact stdout, stderr and exit status, including
module-created coroutines and diagnostic source/caret/trace fixtures.

The C tests cover lazy allocation, full-capacity charging, partial-allocation rollback,
small value/frame/handler limits, stable upvalues, value exchange, defaults/rest/spread,
status transitions, deep yields, alias mutation while suspended, escaped closures after
context destruction, try/catch across yield, native map/sort crossings, nested resumes,
fuel, heap-bounded attempts to suspend 20000 coroutines, and an open-only cycle where a
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

### Diagnostic transcripts

These are actual `--color=never` reference-engine outputs. The runner requires identical
output on all three engines. Call carets use the same source offsets as existing calls.

`bad_depth.lz`

```text
error: stack overflow: too many nested calls (limit 200)
 --> bad_depth.lz:1:57
  |
1 | fn recurse(n) { let captured = fn() => n; return recurse(n + 1) }
  |                                                         ^
  |
  = help: a function calls itself without a base case, or the recursion is too deep (limit 200)
  = stack trace: innermost call first
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      at recurse (bad_depth.lz:1:57)
      ... 184 more frames
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
error: stack overflow: too many open try blocks
 --> bad_handlers.lz:1:16
  |
1 | fn recurse() { try { return recurse() } catch e { return nil } }
  |                ^^^
  |
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
      ... 50 more frames
```

`bad_main.lz`

```text
error: cannot yield from the main program
 --> bad_main.lz:1:6
  |
1 | yield()
  |      ^
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
error: stack overflow
 --> bad_values.lz:1:36
  |
1 | fn recurse(n) { let a = n; let b = n; let c = n; let d = n; let e = n; let f = n; let g = n; let h = n; let i = n; let j = n; let k = n; let capture = fn() => a; return recurse(n + 1) }
  |                                    ^
  |
  = stack trace: innermost call first
      at recurse (bad_values.lz:1:36)
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
      ... 133 more frames
```

