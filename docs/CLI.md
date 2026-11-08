# Command line

```
lizard [options] <script.lz> [script arguments]
```

| Option | Effect |
| --- | --- |
| `-h`, `--help` | Show help |
| `-V`, `--version` | Show the version |
| `--heap-stats` | Print heap statistics when the program ends |
| `--trace-memory` | Log every heap allocation and release to stderr |
| `--dump-tokens` | Print the tokens of the script and exit |
| `--max-heap=N` | Limit live heap bytes (suffix `K`, `M` or `G`) |
| `--fuel=N` | Limit the number of executed instructions |
| `--engine=E` | Execution engine: `ge` (default, the fast one) or `ref` |
| `--engine-stats` | Print what the engine did when the program ends |
| `--jit=M` | Machine code for hot code: `on` (default), `off` or `eager` |
| `--skink=M` | Cycle collector for closures: `on` (default) or `off` |
| `--color=W` | Colour in error messages: `auto` (default: only on a terminal, `NO_COLOR` turns it off), `always` or `never` |
| `--` | Stop option parsing |

`--max-heap` refuses an allocation when the live heap would exceed the limit; dead
closure cycles are collected first. `--fuel` counts executed instructions; running
out stops the program at once and cannot be caught. `--engine=ref` selects the plain
reference interpreter, which defines the language. `--color=never` disables colour
even on a terminal; `NO_COLOR` also disables it.

| Code | Meaning |
| --- | --- |
| 0 | Success |
| 64 | Usage error (bad options or arguments) |
| 65 | Data error |
| 66 | No input (missing script) |
| 70 | Run-time error |

Errors show the source line, the offending span, and a `help:` line when there is a
clear fix — see [Errors](ERRORS.md) for how reports work.

    lizard hello.lz
    lizard --engine=ref --fuel=1000 script.lz
    lizard --heap-stats --color=always program.lz arg1 arg2

`ref` versus the Gecko Engine (`ge`) and the JIT are in [Engine](ENGINE.md).
`--heap-stats` and the skink collector are in [Memory](MEMORY.md).
