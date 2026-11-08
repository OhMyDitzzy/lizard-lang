# Error reports

Every error answers three questions: **what happened**, **where** (carets under the exact part of
the line), and, when there is a real fix that can be written as code, **what to do** (`help:`).
A help is optional. A fatal error or one with no clear fix (out of memory, `instruction limit
exceeded`) is just a message; a vague help is worse than none.

## The parts of a report

```
error: cannot store: this would create a reference cycle      <- message: what happened
 --> class.lz:8:6                                             <- file:line:column
  |
8 | anak.relation = ibu
  |      ^^^^^^^^ `Person.relation` would own an object ...   <- carets + label: where and why
  |
  = note: an object owns what its fields hold, ...            <- note: context
help: make the field a link: it points at an object ...       <- help with a visual fix (LzFix)
 --> class.lz:3:9
  |
3 |     pub link relation = nil
  |         +++++                                              <- `+` added, `~` replaced, `-` removed
```

* **label**: short text next to the carets. **note**: `= note:` context. **help**: `= help:` text
  (a line break starts a line of code to copy).
* A **fix** (`LzFix`, `report.h`) is drawn instead of the help text: the corrected line with markers
  under what changed. A whole new line is drawn with `+` in the margin. If the place of the fix
  cannot be determined with certainty, the report falls back to the help text; it never guesses.
* The carets of a run-time error cover the whole identifier at the error offset (a member name).
* `--engine=ref`, `ge` and the JIT print the same report: help is set in shared code, never per engine.

## Adding a help

* **Run time:** after `lz_vm_set_error`, call `lz_vm_set_help(vm, "...")`, `lz_vm_set_note`,
  `lz_vm_set_label`, or `lz_vm_set_fix(vm, headline, offset, delete_length, text)`. All four are
  cleared by the next `lz_vm_set_error`. Inside `class.c`, `fail(vm, status, help, ...)` does it.
* **Compile time:** set the extras just before the call that reports the error, in `compiler.c`:
  `diag_label`, `diag_note`, `diag_help`, `diag_fix`, `diag_fix_line`. `record_full` stores them in
  the `LzDiagnostic`. Help that is the same every time lives in the `STATIC_HELP` table.
* Quality bar: one short sentence, code in backticks, a concrete example. Do not repeat the message.
  `example_value_for_type` and `conversion_for` build examples (`return "text"`, `str(5)`).

## Golden tests

`tests/reports/NAME.lz` + `NAME.expected` hold the full stderr (`--color=never`; files named
`color_*` use `--color=always`). `tests/reports_check.sh` runs each on `--engine=ref`,
`--engine=ge --jit=off` and `--engine=ge --jit=eager`; all three must match. After changing a
report, rewrite the files with

    LZ_UPDATE_REPORTS=1 sh tests/reports_check.sh build/bin/lizard

and **read every changed `.expected`** before committing. It runs in ctest as `reports_cli`.

## `try`, `catch`, `finally`, `throw`

```lizard
try {
    ...
} catch RangeError as e {        # this kind, and every kind below it
    println(e.message)
} catch ValueError as _ {        # `_` discards the error: nothing is declared
    println("bad value")
} catch e {                      # the same as `catch Error as e`
    println(e)                   # RuntimeError: ...
} catch {                        # the same as `catch Error as _`
    println("something failed")
} finally {
    println("always")
}

throw RuntimeError("message")
throw e                          # throws an error you already have, keeping its trace
```

`try` is a statement and needs at least one `catch` or a `finally`. `catch` and `finally`
follow the `}` before them like `else` does. The clauses are tried from the top and the
first that matches wins; a `catch` with no type (`catch e`, `catch { }`) must be the last.
These are compile errors: a `catch` that an earlier one already covers (`unreachable catch:
'IOError' is already caught by 'Error' above`), a type that does not exist (`unknown error
type 'X'`, with a "did you mean" suggestion), and `catch NotFound { }`, which would only
name the error `NotFound`, so write `catch NotFound as _`. The variable of a `catch`
belongs to its block, shadows a variable of the same name outside without changing it, and
cannot be assigned.

### Kinds

One chain of inheritance, only for errors:

```
Error
+- RuntimeError
|  +- TypeError          a value of the wrong type
|  +- ValueError         a bad value: a failed conversion, an argument out of its domain
|  +- RangeError         an index out of range, a missing key, a shift out of range
|  +- ArithmeticError    division by zero, overflow
|  +- ArityError         the wrong number of arguments
+- IOError               for the input/output builtins
```

Every built-in failure has the kind that fits it, and anything not listed is a
`RuntimeError`. Each kind is a function that makes an error: `RangeError("message")`; the
message must be a string.

### Your own kinds

`error` declares a kind, with a parent that is a kind already known (a built-in one, or one
declared above it in the file); without a parent it is a child of `Error`:

```lizard
error NotFound : IOError
error ParseError : ValueError
error AppError                      # a child of Error
```

`error` is a keyword only at the start of a statement at the top level, followed by a name;
anywhere else it is a name like any other (`let error = 1`, `error = 2` and `error + 1`
still work, `x = error` too). Mistakes are compile errors: a declaration inside a function,
block or loop (`an 'error' declaration is only allowed at the top level`), a name that is
already a kind (`duplicate error type 'X'`), a name that a top-level variable or function
has (`'X' is already used by a variable or function`), an unknown parent (`unknown error
type 'X'`, with a "did you mean" suggestion), and more than 256 kinds in all. There is one
parent, no body and no fields, and a kind below `Error` only; the limits of the sandbox are
no kind and cannot be caught.

Hoisting: all the declarations of a file are found and made before any statement runs, in
the order they are written, so a kind can be caught and called above its declaration. A
parent has to be written above its child. A kind is a function that makes an error, so its
name may not be a variable or function of the file either, and `throw NotFound` is a compile
error (`an error type must be called to make an error: NotFound("message")`).

### `cause` and `data`

Every kind, built-in or yours, is made as `Kind(message)`, with two optional named
arguments:

```lizard
throw ParseError("line 3 is broken", cause: e, data: {"line": 3})
```

`cause` is an error or `nil` (anything else is a `TypeError`); `data` is any value. `cause =
e` is written the same way as the named arguments of other functions and works too. Another
named argument, or a second positional one, is an error (`unexpected named argument` at
compile time, or an `ArityError` when the call goes through a variable). The fields are
`e.cause` (an error or `nil`) and `e.data` (`nil` when none was given), read only like the
others; a container in `data` is shared, not copied. An error and what it carries follow the
rule of arrays and dicts: a store that would make a cycle through an error, such as putting
the error in the dict that is its `data`, is refused (`a value cannot contain itself; for a
pointer back to the owner, use a link field`, a `ValueError`).

### Errors are values

`e.message` and `e.kind` are strings and `e.trace` is an array of strings
`name (file:line:column)`, innermost first, as many frames as an uncaught error reports
(16). These fields are read only (`e.message = "x"` is `cannot assign to a field of an
error`). `.name` without parentheses is a field; with parentheses it is a method as always.
`println(e)`, `e.to_string()` and `{e}` in a template give `Kind: message`. Two errors are
equal only if they are the same object, and an error cannot be a dict key. `throw` takes only
an error: `throw "oops"` is `only an error can be thrown (write RuntimeError("oops"))`. An
error that is thrown again keeps the trace of where it first was.

### `finally`

It runs after the `try` body, after a `catch` body, when an error that nothing catches goes
on, and when `return`, `break` or `continue` leaves the `try` or a `catch` (a `return` value
is kept first; with several nested, the innermost `finally` runs first). A `return`, `break`
or `continue` in a `finally` body is an error (`cannot leave a 'finally' block with
'return'`), but a `throw` there is allowed and its error replaces the one in flight. The body
is compiled again at every way out of its `try`; it sees the names visible where it is
written.

### Limits cannot be caught

Running out of instructions (fuel), heap, memory, or call, stack or callback depth stops the
program at once: no `catch` and no `finally` block runs. Only the errors a script can cause
(all the kinds above) are caught.

### An uncaught error

An uncaught error stops the program with the same report as before (message, caret, trace).
The report of an error made with `throw` is titled `error: Kind: message`; the title of the
built-in errors is unchanged. When the error has a `cause`, the report ends with one line for
each error of the chain, nearest first, at most 8 and then `...`:

```
error: ParseError: cannot read the configuration
 --> app.lz:14:5
   ...
  = caused by: NotFound: config.lz is missing
  = caused by: IOError: permission denied
```

## Error messages

Errors show the source line, the offending span, and, when a name does not exist, a "did you
mean" suggestion drawn as the corrected line (`+` letters to add, `~` letters to change, `-`
letters to remove):

```
error: undefined function 'printj'
 --> hello.lz:2:1
  |
2 | printj("Hello World!")
  | ^^^^^^ not found in this scope
  |
help: did you mean `print`?
  |
2 | print("Hello World!")
  |      - remove `j`
  |
help: or maybe `println`?
  |
2 | println("Hello World!")
  |      ~~
```

Candidates are the builtins, the functions and variables of the script that are in scope at
that point, type names (`let x: intt`) and parameter names (`f(hieght = 1)`). Names that
later come from `import` need no extra work: once they are symbols or globals of the VM they
are offered too. A call is only matched with things that can be called.
