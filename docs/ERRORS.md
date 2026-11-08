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
