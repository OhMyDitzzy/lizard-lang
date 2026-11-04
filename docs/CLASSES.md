# Classes

A class is a kind of value you define yourself. There is no `new`: you call the class like a
function.

```lizard
class Point {
    pub x: int
    pub y: int = 0                      # a default

    pub fn sum(self) => self.x + self.y
    pub fn move(self, dx, dy) {
        self.x += dx
        self.y += dy
    }
}

let p = Point(3, 4)
p.move(1, 1)
println(p)                              # Point(x: 4, y: 5)
println(Point(x: 3))                    # Point(x: 3, y: 0)
```

See `examples/classes.lz` for a longer tour.

## Syntax

```
class_decl  := "class" Name "{" member* "}"
member      := field | method | property | constant
field       := ["pub" ["read"]] name [":" type] ["=" expr]
method      := ["pub"] "fn" name "(" params ")" ( block | "=>" expr )
property    := ["pub"] "get" name "(" "self" ")" ( block | "=>" expr )
constant    := ["pub"] "const" NAME "=" expr
```

Members are separated by new lines. `class`, `pub`, `read` and `get` are keywords only in these
positions; everywhere else they are ordinary names. In a class body `pub read x: int` makes
`read` a modifier only when an identifier follows it; `pub read: int` is a field named `read`.

A class can only be declared at the top level of a file. The name is bound when the `class`
statement runs (like every statement in Lizard), so a method may call `Point(...)` freely.

## Fields

* The order of declaration is the order of the constructor's positional arguments.
* A default is evaluated **every time an object is made**, so `items: int[] = []` gives each
  object its own array. It runs in the global scope and cannot use `self`
  (`a field default cannot use 'self'; set it in init() instead`).
* Field types are checked on construction and on every assignment:
  `TypeError: Point.x expects int, got string`.
* A field without a default that `init()` does not fill is an error (see below).
* At most 64 fields per class.

## The constructor

`Point(...)` takes **every `pub` field** (including `pub read`) in declaration order. Private
fields are not parameters. Arguments are positional first, then named (`Point(x: 3)`, the same
mechanism as `Error("msg", cause: e)`).

Order of work: fill the fields from the arguments, evaluate the remaining defaults in
declaration order, run `init`, then check that every field has a value.

| Mistake | Error |
| --- | --- |
| missing | `ArityError: Point needs 'x'` |
| too many | `ArityError: Point takes at most 2 arguments, got 3` |
| unknown name | `ArityError: Point has no field 'zzzz' (did you mean ...?)` |
| private field | `ArityError: 'count' is private to Account, so it is not an argument of Account(...)` |
| twice | `ArityError: 'x' was given twice` |
| wrong type | `TypeError: Point.x expects int, got string` |

## `init`

```lizard
class Temperature {
    pub celsius: float
    kelvin: float = 0.0
    fn init(self) {
        if self.celsius < -273.15 { throw ValueError("below absolute zero") }
        self.kelvin = self.celsius + 273.15
    }
}
```

`init(self)` takes only `self`, runs after the fields are filled, and its result is ignored. It
may throw: the object is dropped and the error goes to the caller. It runs automatically and only
from the constructor and `with`; `x.init()` is an error. A field still without a value when it
ends is `RuntimeError: field 'kelvin' of Temperature has no value yet (assign it in init())`.
Reading such a field while `init` runs is the same error.

## `self`, methods and static functions

A `fn` whose **first parameter is named `self`** is a method; any other `fn` is a static
function, called through the class: `Color.gray(128)`. Members are reached through `self.`
(there are no bare field names). `self` anywhere but first gives `'self' must be the first
parameter of a method`; in a static function it is an undefined name with a hint.

Stack traces and reports name methods `Point.move`.

## Constants

`const NAME = expr` belongs to the class and is read as `Point.NAME`, from inside the class too.
It is evaluated once, right after the class name is bound, in declaration order, so it may use
its own class (`const WHITE = Color(255, 255, 255)`). It cannot be assigned.

## Visibility

| Declaration | Outside the class | Inside |
| --- | --- | --- |
| `x`, `fn f`, `const C` | not visible | read, write, call |
| `pub x`, `pub fn f`, `pub const C` | read, write, call | read, write, call |
| `pub read x` | read only | read, write |
| `pub get p(self)` | read only | read |

"Inside the class" is **lexical**: any code written in the class body, including lambdas inside
methods. It includes the private members of *other objects of the same class*. A violation is a
`RuntimeError` with a help line:

```
error: 'count' is private to Account
 = help: mark it 'pub' (or 'pub read') to use it from outside
error: 'balance' is read-only outside Account
 = help: change it inside a method such as deposit()
```

Changing a `pub read` field's content in place (`a.items[0] = 1`, `a.items[0] += 1`) is refused
outside the class too. Calling a mutating method on it (`a.items.push(1)`) is not checked.

## Properties

`get name(self) => expr` is read like a field (`r.area`) and computed on every read. Assigning is
`'area' is a computed property and cannot be assigned`; calling it as `r.area()` is
`'area' is a property; write r.area without parentheses`.

## Members and assignment

`value.name` reads a field, property, constant (through the class) or bound method. Targets of
assignment: `obj.name = v`, `obj.name += v` (every compound operator), `obj.items[i] = v`,
`a.b.c = v`, and the step of a C-style `for`. The object on the left is evaluated once
(`f().x += 1` calls `f()` once). There are no dynamic fields: writing an undeclared name is
`Point has no field 'z'` with the help `fields are declared in the class`.

`p.sum` (no parentheses) is a function bound to `p` (printed `<method Point.sum>`). `Point.sum` is
a plain function that takes the object first, so `points.map(Point.sum)` works.

## Text, equality, copies

* The built-in form is how you build the object: `Point(x: 1, y: 2)` (strings quoted, nested
  objects recursively, private fields included).
* A class may define `pub fn to_string(self)` and/or `pub fn to_debug(self)`. `println`, `string`,
  `{x}`, `{x:spec}` and `.to_string()` use `to_string`; elements of arrays and dicts and `{x=}` use
  `to_debug`. They must return a string (`TypeError: to_string() of Point must return a string, got int`).
  A `to_string` that calls itself runs into the callback depth limit (a sandbox error).
* `{x:s}` and an empty spec use `to_string`; any other spec is an error.
* `a == b` is true when both have the same class and every field (private ones too) is `==`.
  `in`, `switch`, `!=`, `not equal` follow. Objects cannot be dict keys.
* `identical(a, b)`: true for the same array, dict, object or function; for numbers, `nil`,
  booleans and strings it is `==`.
* `x.copy()` copies the fields without running `init`. `x.with(name: value, ...)` copies, replaces
  the named `pub` fields, then runs `init`. A class may define its own `copy` or `with`.

Member lookup on an object: its class (with visibility) → the built-in methods (`to_string`,
`to_debug`, `format`, `to_bool`, ...) → a function of that name taking the object first.

## Class names as types

A class name works wherever a type does: `fn f(p: Point)`, `let q: Point`, `pub next: Point`,
`Point[]`. It matches instances of that class. Class names are known from their declaration on
(a method may name its own class).

## Error kinds

Every error above is catchable. `ArityError`: constructor and `with` arguments. `TypeError`:
types. `RuntimeError`: privacy, read-only, unknown members, properties, manual `init`, fields
without a value. The sandbox limits (fuel, heap, depth) are not catchable and pass through
`finally`, including inside constructors, `init` and `to_string`.

## Decisions

Choices made where the specification was open:

* **Messages.** Field type errors read `Point.x expects int, got string`. Unbound methods check
  `self`: `Point.sum() needs a Point as its first argument, got string`. Unknown members are
  `RuntimeError`. A cycle is refused as `cannot store: this would create a reference cycle
  (through field 'next' of Node)`. Objects as dict keys: `a Point cannot be a dict key` (the existing wording).
* **Array types** use the existing syntax `int[]`, not `[int]`.
* **Printing.** A function prints `<fn Point.origin>` and a bound method `<method Point.sum>`.
* **Class names in types** are matched by name. Two classes with the same name are one type.
* **Unknown names.** In a file that declares a class, `x.name()` first asks the object. Calls
  whose receiver is an object are typed `any`, and so are field reads (`e.message` too), because
  the static types of builtin methods and error fields cannot be trusted there. Files without a class compile
  exactly as before.
* **Nested calls.** The constructor, `init`, field defaults, getters and `to_string` run through a
  nested call of the interpreter, so they share the limit of 128 nested callbacks. Ordinary
  method calls use normal frames and are not limited this way.
* **Where "inside the class" comes from.** Each function carries the name of the class it was
  compiled in (`LzFunction.owner`); closures inside inherit it. `lz_class_sees` is the one place
  to widen for subclasses.
* **Fields** are limited to 64 per class (a bitmask tracks the ones without a value).
* **Constants** are dropped when the VM releases its roots, because a constant may hold an
  object of its own class (a cycle that would otherwise be reported as a leak).

## Memory and cycles

Objects are counted like arrays: an object is a container node in every cycle mechanism.

* A cycle that goes only through objects, arrays and dicts is **refused when stored**, with the
  field named: `a.next = b` after `b.next = a`. `a.items.push(a)` keeps the existing array
  message.
* A cycle through a closure (a method that stores a lambda capturing `self` in a field, or a
  stored bound method `self.f = self.method`) is collected by the skink collector, which
  treats objects and bound methods as nodes.
* Failed constructions (wrong arguments, `init` throwing) and objects held by closures leave no
  live objects; the tests check `live_objects == 0` and run under ASan/UBSan/LSan.

## Engines

`--engine=ref`, `--jit=off` and `--jit=eager` print the same output. The new opcodes
(`GET_FIELD_KEEP`, `SET_FIELD`, `GET_FIELD_W`, `METHOD_LOOKUP`, `CLASS_INIT`) are run by the
reference interpreter; GE runs them as delegated instructions. **Functions that use them are not
compiled by the JIT** (like functions with `try`); they run on GE. Calling a method or class
value from JIT-compiled code works through the generic call path. The JIT is not asked to emit
code for classes in stage 1.

## Planned (stages 2 and 3)

Parsed today and rejected with `... is not supported yet`: `class A : B`, `override`, `operator`,
`trait`, `extend`, `super`, `pub class`. The design leaves room: `LzClass.parent`, `lz_class_sees`
(visibility for subclasses), the type check in `lz_type_matches`/`lz_type_assignable` (subtypes),
the member table (members from `extend`), and members that can be exported by modules.

Stage 3 opens reference cycles through objects. Today's refusal is in `lz_instance_store`
(`lz_containers_would_cycle` with an object target); relaxing it also needs the skink to start
from object stores, as it does for closures.
