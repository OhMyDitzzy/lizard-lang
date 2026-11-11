# Documentation

Read in order, from the most basic to the most advanced:

- [VISION.md](VISION.md) — the design: Simple. Strict. Kind.
- [SYNTAX.md](SYNTAX.md) — programs, comments, `let`/`const`, blocks, operators, precedence, `in`/`not in`/`??`, `switch`, `if` expressions
- [TYPES.md](TYPES.md) — type system: `int`, `float`, `bool`, `nil`, `string`, optionals, unions, literals, aliases
- [CONTROL_FLOW.md](CONTROL_FLOW.md) — `if`, `while`, `for`, `break`, `continue`
- [FUNCTIONS.md](FUNCTIONS.md) — functions, parameters, named arguments, variadics, closures
- [COLLECTIONS.md](COLLECTIONS.md) — arrays and dicts
- [STRINGS.md](STRINGS.md) — literals, templates, format specs, debug form
- [ERRORS.md](ERRORS.md) — `try`/`catch`/`finally`/`throw`, error kinds, error reports
- [CLASSES.md](CLASSES.md) — classes, inheritance, traits, operators, `extend`
- [MODULES.md](MODULES.md) — modules: a global table per module, private `extend`, resolvers, loading from C
- [METHODS.md](METHODS.md) — methods and callbacks on values
- [CLI.md](CLI.md) — `lizard` command-line options and exit codes
- [MEMORY.md](MEMORY.md) — ownership, reference cycles, links, `--heap-stats`
- [ENGINE.md](ENGINE.md) — Gecko Engine, reference interpreter, JIT
- [DEVELOPING.md](DEVELOPING.md) — tests, sanitizers, poison, fuzz, contributing

## Internals

- [ARCHITECTURE.md](ARCHITECTURE.md) — repository layout, memory model, how to add a builtin (internal; not linked from the README)

## Build

- [building/README.md](building/README.md) — how to build from source (Autotools, or plain `sh` without them)
