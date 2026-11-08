# Functions

In Lizard, a function is a value. You can bind it to a variable, pass it as an
argument, return it from another function, or call it through any name that holds it.
A `fn` whose body is a single expression can be written with `=>`:
`fn(x) => expression` means the same as `fn(x) { return expression }`.

Parameters and the result may carry types. A typed call that is wrong is a
compile-time error when the compiler knows the callee, and a run-time error
otherwise. A function declared with `-> T` must return on every path — a missing
return is an error.

Top-level functions can call each other regardless of order, and a function nested
inside another captures the variables it uses, so closures keep those variables
alive.

```lizard
fn area(w: float, h: float = 1.0) -> float { return w * h }
let twice = fn(f, x) { return f(f(x)) }

println(area(3.0, h = 2.0))
println(twice(fn(n) => n * 3, 2))
```

```text
6
18
```

Positional arguments come first, then named ones — `area(3.0, h = 2.0)` works in any
order for the named parameters. A default may use earlier parameters:
`fn k(a, b = a * 2)`.

A rest parameter collects surplus arguments into an array: `fn sum(*xs)` makes `xs`
an array. The `*` operator spreads an array back into arguments:
`sum(1, 2, *[3, 4])`.

If you give a typed call the wrong kind of value, Lizard stops you:

```lizard fail
fn square(x: int) -> int { return x * x }
println(square("x"))
```

```
error: TypeError: square expects int, got string
```

Pass an `int`, or change the parameter type.

If a function promises `-> int` but does not return on every path:

```lizard fail
fn f(x: int) -> int {
  if x > 0 { return x }
}
println(f(0))
```

```
error: missing return
```

Add a `return` on every path, or change the result type.

If you pass the wrong number of arguments:

```lizard fail
fn add(a, b) { return a + b }
println(add(1))
```

```
error: ArityError: add takes 2 arguments, got 1
```

Pass the declared number of arguments, or use defaults or a rest parameter.

Calls nest up to 2048 deep, and an error inside a function prints a stack trace.
Closures run on the reference interpreter and may nest 128 deep per callback.

Parameter and result type annotations are in [Types](TYPES.md). Calling a function
as a method — `receiver.name(args)` — is in [Methods](METHODS.md). Methods are
functions whose first parameter is `self`, covered in [Classes](CLASSES.md).
