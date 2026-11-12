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

## `pub` on a class

A class (or trait, or error) may be written `pub class Point { ... }` at the top level, like any
other declaration (see Syntax). Without modules nothing changes: the class works exactly as
without `pub`. The compiler records the interface of a `pub` class: its `pub` members with their
signatures, its parent and traits, and one `init` that says how to create it (the constructor
arguments below). Private fields and methods are never recorded, though the class stays usable
from outside. A `pub` member may not mention a class, trait or alias that is not `pub` itself.

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
(a method may name its own class). A class made by an earlier source run in the same VM can be
named too, by the binding of the name when the annotation is compiled; a name bound to something
that is not a class is `'Foo' is not a type`.

A class of another module is named through the import: `util.Point` after `import util`, or `Point`
after `from util import Point`. The same goes for traits and error types, in annotations, `is`,
class heads and `catch`. See [MODULES.md](MODULES.md).

A type names a class by its **identity** (`LzClass.id`, a number that is never reused), not by its
name. If a class is defined again in a later source, objects of the old class do not match the new
`A` and the message says so: `expected A, got A (made by an earlier definition of that class)`.

## Inheritance

`class Dog : Animal { ... }` makes `Dog` a class that builds on `Animal`. A class has at most one
parent class; it is the first name after the colon, and any other names are traits (see below).

```lizard
class Animal {
    pub name: string
    pub fn speak(self) => "..."
    pub fn intro(self) => $"I am {self.name} and I say {self.speak()}"
}
class Dog : Animal {
    pub breed: string = "mixed"
    pub override fn speak(self) => "Woof"
}
println(Dog("Rex", breed: "Husky").intro())    # I am Rex and I say Woof
```

* The parent must exist when the `class` statement runs. A name that is not a class or a trait is
  `'x' is not a class or a trait`; a builtin type is `cannot inherit from a builtin type`; an
  error kind is `use 'error X : Y' to declare an error`.
* **Fields:** the parent's first, then the child's. The constructor takes the `pub` fields of the
  parent (from the root down) and then the child's: `Dog("Rex", breed: "Husky")`. A child cannot
  declare a field its parent has (`Dog redeclares field 'name' of Animal`), and a name cannot
  change kind (`'x' is a field of Animal and cannot be redeclared as a function`).
* **Creating an object:** the fields are set from the arguments, then every default (the parent's
  first), then each `init` runs from the root down to the class of the object (never write
  `super.init()`), then a check that no field is missing. Every default is set before any `init`.
* **Visibility:** code in a class sees the private members of the class and of its ancestors, never
  of its descendants. A child may write the parent's `pub read` fields. The message names the class
  that declared the member.
* **Lookup** goes from the class of the object up through its parents, then to the default
  functions of traits, then to the builtin methods and global functions. Static functions and
  constants are inherited (`Dog.NAME`). `Animal.speak(dog)` runs the `Animal` version on a `Dog`.
* `==` between objects still needs the very same class: a `Dog` and an `Animal` are never equal
  (unless `operator ==` says so). `copy` keeps the class, and `with` makes an object of the same
  class and runs the `init` chain again.
* A type `a: Animal` accepts a `Dog` and a `Puppy`, in parameters, results, `let` and fields.

## `override` and `super`

`override` is written after `pub`: `pub override fn speak(self)`, `override get area(self)`. It is
for functions, static functions and properties.

* It is **required** when the parent (or an ancestor, or a ready-made function of a trait) already
  has the name: `Dog.speak() hides Animal.speak(); add 'override'`. It is allowed, not required, for
  a function that a trait only requires.
* With nothing to replace: `Dog.speek() overrides nothing (did you mean 'speak'?)`.
* The same number of parameters (`must take the same parameters as Animal.speak() (expected 0, got
  1)`), the same kind (`Dog.area is a property but Animal.area is a function`) and a visibility that
  is not smaller (`Dog.speak() cannot be private because Animal.speak() is public`). A private
  member of an ancestor may be overridden.
* Not for fields, constants or `init` (every `init` runs).

`super.name(args)` runs the parent's version, with the same `self`. The search starts at the parent
of the class **where the code is written**, not at the class of the object, so with
`Puppy : Dog : Animal`, `super.speak()` inside `Dog` is `Animal.speak` even for a `Puppy`.
`super.name` without parentheses reads a property. It works in methods, properties and lambdas
inside them. Elsewhere: `'super' can only be used inside a method`, `'super' cannot be used in a
static function`, `Animal has no parent class, so 'super' has nothing to refer to`, `the parent of
Dog has no method 'x'`, `'super' must be followed by .name`, `init() runs automatically;
super.init() is not needed`, `'super' cannot be used in a trait`.

## `is` and `is not`

```lizard
println(d is Animal)        # true for a Dog, a Puppy, an Animal
println(d is not Puppy)
println(5 is int, nil is Dog, [1] is int[])
if shape is Circle and shape.r > 0 { ... }
```

The right side is a type, read the same way as an annotation, and `x is T` is true exactly when
`x` would be accepted by `: T`. A value that is not an object is simply not a class or a trait
(`false`). It sits at the level of `==` and does not chain with it (`a is B == c` needs
parentheses). `is` is a word only after an expression on its line: a variable called `is` still
works. A name that is not a type: `'foo' is not a type`. Patterns in `switch` are not included.

## Traits

A trait is a promise: what a class must provide, plus ready-made functions that use it.

```lizard
trait Shape {
    fn area(self)                      # no body: a class must write it
    get label(self)                    # a property it must have
    fn describe(self) => $"{self.label}: {self.area()}"     # a default
}
class Square : Shape {
    pub side: int
    pub fn area(self) => self.side * self.side
    pub get label(self) => "square"
}
```

* A trait may extend traits (`trait Greeter : Named`). A trait cannot have fields, constants,
  static functions, `init` or operators (`a trait cannot have fields`, ...). Everything in a trait
  is public. A trait is not created: `Shape is a trait and cannot be created; use a class that
  implements it`. `println(Shape)` is `<trait Shape>`.
* A function with no body in a class: `this function needs a body (only a trait can leave it out)`.
* **The contract**, checked when the `class` statement runs: `Circle does not implement 'area'
  required by Shape`; the same parameters; public (`Circle.area() must be public because Shape
  requires it`); a required `get` is met by a `get`, not by a field. A parent class can supply what
  a trait requires.
* A **default** is part of the class that uses the trait and runs on the real object, so
  `self.area()` finds the class's own `area`. Its code is not inside any class: it sees only public
  members. A function of the class chain wins over a default; replacing a default needs
  `override`.
* **Two traits, one name:** `Circle gets 'describe' from both Shape and Describable; define
  describe() in Circle to choose`. The same trait reached by two paths is no conflict, and a trait
  that extends another wins over it.
* A trait is a type: `fn show(s: Shape)`, `x is Shape`.

## Operators

```lizard
class Vec {
    pub x: int
    pub y: int
    pub operator +(self, other) => Vec(self.x + other.x, self.y + other.y)
    pub operator -(self) => Vec(-self.x, -self.y)          # unary: only self
    pub operator *(self, k) => Vec(self.x * k, self.y * k)
    pub operator <(self, other) => self.x < other.x
}
```

Definable: `+ - * / % == <` (and the unary `-`); `operator equal` is `operator ==`. Others are
refused: `operator '>' cannot be defined; define '<' and '>', '<=' and '>=' follow from it`,
`'!=' follows from '=='`, `operator '??' cannot be defined`. A wrong number of parameters:
`operator + takes 'self' and one other value`.

* `a + b` runs `operator +` of **a**'s class (or an ancestor's) with `(a, b)`. There is no reversed
  form: `2 * vec` is an error that says `Vec defines '*' only when it is on the left; write vec *
  2`. Without the operator: `operator '+' is not defined for Vec`.
* `==` runs `operator ==` of the left operand and **replaces** the content comparison for the class
  and its descendants, everywhere equality is used (`in`, `switch`, nested arrays and dicts). It
  must return a bool. `!=` is its opposite. This is deliberately lopsided: `5 == vec` is `false`
  and does not call anything.
* `a < b` runs `a`'s `operator <`; `a > b` is `b < a` (so it runs `b`'s), `a <= b` is `a < b or
  a == b`, `a >= b` is `b < a or a == b`. The result must be a bool. `sort`, `min` and `max` of
  an array of objects use `<`; `sort` leaves the array untouched if the operator fails.
* `+=`, `-=`, `*=` ... use the same operators, also on `obj.field` and `arr[i]`.
* Operators are inherited; redefining one needs `override`. A private operator works only inside
  the class. An error inside an operator has a trace line like `Vec.+`. They run as ordinary calls:
  they use fuel and the depth limit.
* Numbers and strings do not go through any of this: the hook is only in the slow path, reached
  when an operand is an object.

## Error kinds

Every error above is catchable. `ArityError`: constructor and `with` arguments. `TypeError`:
types. `RuntimeError`: privacy, read-only, unknown members, properties, manual `init`, fields
without a value. The sandbox limits (fuel, heap, depth) are not catchable and pass through
`finally`, including inside constructors, `init` and `to_string`.

## `extend`

```lizard
extend string { pub fn shout(self) => self.to_upper() + "!" }
extend int { pub get is_even(self) => self % 2 equal 0 }
extend Point { pub fn dist2(self, other) => ... }
```

`extend Target { members }` adds methods and `get` properties to a type that exists, at the top
level of a file. Target: `string`, `int`, `float`, `bool`, `array`, `dict`, `nil`, `function`
(every value of that kind), or any class (the class and its descendants). `int[]` is refused
(`extend works on a whole kind such as 'array', not on 'int[]'`), a trait too (`extending a trait
is not supported yet`).

* **Only new names.** Fields, constants, static functions, `init`, `operator`, `override` and
  `super` are errors. A name the target already has (builtin method, class member, ancestor,
  trait default, earlier extension) is `'x' is already defined for T`, with a help line.
  A function of your own with the same name is no obstacle: it is not a method, so `extend` may
  add any name the type does not have yet.
* **From the statement on.** The block is a hidden class (`LzClass.is_ext`) registered in
  `vm->exts` when its statement runs; it is not hoisted. Two blocks cannot add the same name.
* **Modules.** A block is private to the module that writes it and is seen only by code written
  in that module, whoever calls that code. `pub extend` is an error
  (`'pub' cannot be used on 'extend'`). Two modules may add the same name to one type. The target
  may be a class of another module, `extend util.Point { ... }`: the new methods exist only in the
  file that wrote the block, and `util` itself never sees them. See [MODULES.md](MODULES.md).
* **Privacy.** `pub` members are visible everywhere; others only inside the same block. The code in
  a block is not "inside the class": it sees only the public members of `self`
  (`'x' is private to Point`, help: `an extension can only use the public members of Point`).
* **Resolution** for an instance: class chain (leaf to root), then extensions (leaf to root), then
  trait defaults, then builtin methods, then global functions. For a builtin value: builtin
  methods, extensions of its kind, global functions. A subclass's own member wins silently; a
  subclass written later with the name of an extension needs `override`, and `super.name()`
  reaches the extension.
* `self` of an extension of a builtin kind is checked against that kind; of a class, not at all.
  The result of an extension call has type `any`. Traces say `string.shout`, `Point.dist2`.
* Not supported: `string.shout` (unbound, with a builtin type name as prefix); the bound forms
  `"hi".shout` and `Point.dist2` work. Extending an error class or `Error` is not tested.

## `error` as a class

```lizard
error InsufficientFunds : ValueError { pub needed: int }
error Overdraft : InsufficientFunds {
    pub account: string = "main"
    pub get summary(self) => $"{self.account} short by {self.needed}"
}
throw Overdraft("not enough", needed: 15)      # or Overdraft("not enough", 15)
```

An `error` with a body has fields, methods, `get`, constants and `init` with the rules of a class.
The constructor takes the message, then the `pub` fields (root to leaf); `cause:` and `data:` are
only given by name. `e.message`, `e.kind`, `e.cause`, `e.data` and `e.trace` are unchanged, and
so are the printed text (`Kind: message`) and `==` (identity). A field cannot be called like one
of them (`E redeclares field 'message' of Error`).

* Kind names are types: `e is ValueError`, `fn f(e: Error)`. Errors made by the VM (`TypeError`,
  `RangeError`, ...) are of the builtin kinds, so `e is TypeError` is right for them.
  `catch Kind as e` works through inheritance.
* `class X : Error` is `use 'error X : Error' to declare an error`; `error X : SomeClass` is
  `'SomeClass' is not an error kind`.
* `to_string` of an error class is used by `println` and `{e}` (not by the report of an uncaught
  error); the builtin `to_debug` is `Overdraft("not enough", needed: 15, account: "main")`.

**Design (differs from a plain class):** an error stays one `LzError` object. One with fields
carries a *payload*, an instance of its class that holds the fields (`LzError.payload`); `self` in
its methods is the error itself. The class is bound to its kind (`LzErrorType.cls`) when the
statement runs, so it is not hoisted, while a declaration without a body is, as before. An error
class is not a global value: `Name(...)` is the kind's constructor. `is_kind` (a bitset of the
ancestor kinds in each error) lets type checks run without the VM.

* **Traits:** `error E : RuntimeError, Describe` takes the first name as the parent error kind and
  the rest as traits. The contract is checked when the statement runs (`E does not implement 'x'
  required by T`), defaults are methods of the error, `e is Describe` is true, and two traits
  giving the same name conflict as in a `class`.
* **`copy()` and `with()`:** `e.copy()` is a new error of the same kind with the message, the
  fields, `cause` and `data` copied; it has no `trace` and `init` does not run. `e.with(needed: 5)`
  copies, replaces the named `pub` fields (and `message`, `cause:`, `data:`), then runs `init`;
  it has no `trace` either. Positional arguments are an `ArityError`. Both work on errors with
  fields, plain errors and builtin kinds (`ValueError("x").with(message: "y")`).
* **Constructor arity** counts the message: `E takes at most 2 arguments (message, needed), got 3`.
* **Limit:** a script can have 256 error kinds, the 8 builtin ones included; one more is
  `too many error kinds (limit is 256)`.
* **Cycles:** an error counts as a holder of its payload, and the cycle check opens the payload,
  `data` and `cause`. `self.items = [self]` in a method of an error class is refused with the usual
  `cannot store: this would create a reference cycle` message. `e.data` and `e.cause` cannot be
  assigned, so they cannot close a cycle later.

Not done: `==` on the fields.

## Decisions

Choices made where the specification was open:

* **Messages.** Field type errors read `Point.x expects int, got string`. Unbound methods check
  `self`: `Point.sum() needs a Point as its first argument, got string`. Unknown members are
  `RuntimeError`. A cycle is refused as `cannot store: this would create a reference cycle`, with
  the field named in the label under the carets (see "Memory and cycles"). Objects as dict keys: `a Point cannot be a dict key` (the existing wording).
* **Array types** use the existing syntax `int[]`, not `[int]`.
* **Printing.** A function prints `<fn Point.origin>` and a bound method `<method Point.sum>`.
* **Class names in types** are matched by class identity (see above), never by name.
* **Unknown names.** `x.name()`, `x.field` and `Name(x: 3)` compile in any file: the callee
  decides what a name means when it runs. Where the receiver is an object, or its type is not
  known (a parameter, a result from another file), the compiler assumes the object may be an
  instance and types the result `any`; a receiver whose type is known not to be an object keeps the
  older handling. A VM that has already compiled a class treats every receiver this way. A global
  made by `error E` has no known type at compile time, so `a.cause = a` on it is a run-time error
  (`cannot assign to field 'cause' of error`). A callee that takes no named arguments says so at run
  time (`len(): builtin functions do not take named arguments`).
* **Nested calls.** The constructor, `init`, field defaults, getters, `to_string` and operator
  hooks run through a nested call of the interpreter (`lz_vm_call_value`). The count limit is the
  frame limit (2048), so runaway recursion ends with the ordinary depth error. Because each nesting
  also uses the C stack (about 1-2 KiB, about 3 KiB under AddressSanitizer), a second guard
  compares the real stack pointer with the end of the thread's stack (192 KiB kept back) and
  reports `stack overflow: callbacks nested too deeply for the C stack`. A VM needs more than
  about 640 KiB of stack to run at all. Moving these calls into plain VM frames was not done: it
  needs a state machine for the construction phases in both engines.
* **Printing depth.** A `to_string` that prints itself ends with `stack overflow: value is nested
  too deeply to print`, which is a limit error like the others.
* **Where "inside the class" comes from.** Each function carries the id of the class it was
  compiled in (`LzFunction.owner_id`, and the name in `owner` for messages); closures inside
  inherit it. `lz_class_sees` resolves the id through a registry of live classes (sorted by id), so
  it can tell whether that class derives from the one that declares a member.
* **Class layout.** A class object is built at compile time and linked at run time, when its
  `class` statement runs (`OP_CLASS_LINK`, one per name in the head, then `OP_CLASS_INIT`, which
  checks overrides, traits and constants). The field slots of a child start after the parent's;
  `field_refs` maps a slot to the class and member that declare it, and the constructor's
  parameters are slots.
* **Member lookup** walks the chain each time. A table cached per class, with a version number to
  invalidate it, is planned together with `extend`, which is what needs the invalidation.
* **Type checks of classes** are made when the code runs: the compiler cannot know a hierarchy that
  is built at run time, so `let d: Dog = Cat()` is a run-time type error, not a compile error.
* **Operators** are methods named by their symbol (`"+"`, `"-@"` for the unary minus ...), so
  inheritance, `override` and visibility need no special case. Equality inside nested comparisons
  has no VM argument, so the VM that is comparing is kept in a static for the length of the
  comparison (`lz_class_equal_scope_begin/end`): not safe for two VMs on two threads at once.
* **Trait members** live in the trait's class object (`is_trait`); a requirement is a member with
  `is_required`; defaults are found by `lz_class_lookup` after the class chain.
* **Errors at declaration** are run-time errors of the `class` statement, placed on the offending
  declaration (one source position, not two labels).
* **`is`** compiles to `OP_IS` with the type table of the chunk, so functions that use it run in
  the reference interpreter's slow path under `ge` (the same as for the other class opcodes).
* **Fields** are limited to 64 per class (a bitmask tracks the ones without a value).
* **Constants** are dropped when the VM releases its roots, because a constant may hold an
  object of its own class (a cycle that would otherwise be reported as a leak).

## Limits

* **`pub read` is shallow.** It forbids assigning the field from outside the class, nothing more:
  the array or dict a field holds can still be changed through its methods.

  ```lizard
  class Bag {
      pub read items: int[] = []
  }
  let b = Bag()
  b.items = []        # error: 'items' is read-only outside Bag
  b.items.push(1)     # allowed: the array itself is not protected
  ```

  If outsiders must not change a collection, keep it private and offer methods
  (`pub fn add(self, x: int)`, `pub fn count(self) => len(self.items)`) instead of exposing it.
* At most 64 fields per class and 64 classes per file.
* `link` is not for strings or scalars (they are not shared objects): `link(5)` and `link("s")`
  are a `TypeError`. `Table` and `region` are planned, not available yet.

## Memory and cycles

Objects are counted like arrays: an object is a container node in every cycle mechanism.

* A cycle that goes only through objects, arrays and dicts is **refused when stored**, with the
  field named: `a.next = b` after `b.next = a`. `a.items.push(a)` keeps the existing array
  message. The report shows the fix (§ below).

  Two objects that point at each other would keep each other alive forever, so Lizard stops the
  second store and shows how to fix it. Think of a `link` as a bookmark: it points at an object
  without owning it, so the object can still go away, and the link then reads as `nil`:

  ```
  error: cannot store: this would create a reference cycle
   --> class.lz:8:6
    |
  8 | anak.relation = ibu
    |      ^^^^^^^^ `Person.relation` would own an object that already owns this one
    |
    = note: an object owns what its fields hold, so two objects cannot own each other
  help: make the field a link: it points at an object without owning it
   --> class.lz:3:9
    |
  3 |     pub link relation = nil
    |         +++++
  ```
* A cycle through a closure (a method that stores a lambda capturing `self` in a field, or a
  stored bound method `self.f = self.method`) is collected by the skink collector, which
  treats objects and bound methods as nodes.
* A `pub link parent: Node = nil` field holds a **non-owning** reference: it neither raises the
  target's count nor closes a cycle. Use it for a pointer back to the owner, an observer or a
  cache entry. Reading the field gives the target, or `nil` once the target was freed; `link(x)`
  builds a standalone link with `.target` and `.alive`. A link can point at an instance, an
  array, a dict, a closure or a function, never at a string or a number.

  ```lizard
  class Node {
      pub name: string
      pub link parent: Node = nil
      pub children: Node[] = []
  }
  let root = Node(name: "root")
  let kid = Node(name: "kid", parent: root)   # no cycle: the link does not hold root
  root.children.push(kid)
  ```

  See `docs/MEMORY.md` for ownership, the collector and how to choose.
* Failed constructions (wrong arguments, `init` throwing) and objects held by closures leave no
  live objects; the tests check `live_objects == 0` and run under ASan/UBSan/LSan.

## Engines

`--engine=ref`, `--jit=off` and `--jit=eager` print the same output. The new opcodes
(`GET_FIELD_KEEP`, `SET_FIELD`, `GET_FIELD_W`, `METHOD_LOOKUP`, `CLASS_INIT`) are run by the
reference interpreter; GE runs them as delegated instructions. **Functions that use them are not
compiled by the JIT** (like functions with `try`); they run on GE. Calling a method or class
value from JIT-compiled code works through the generic call path. The JIT is not asked to emit
code for classes in stage 1.

## Messages of stage 2

| Situation | Message |
|---|---|
| more than one parent class, or not first | `a class can have only one parent class, and it must come first` |
| parent is not a class or trait | `'x' is not a class or a trait` |
| parent is a builtin type | `cannot inherit from a builtin type` |
| parent is an error kind | `use 'error X : Y' to declare an error` |
| field declared again | `Dog redeclares field 'name' of Animal` |
| forgot `override` | `Dog.speak() hides Animal.speak(); add 'override'` |
| `override` with nothing to replace | `Dog.speek() overrides nothing (did you mean 'speak'?)` |
| different parameters | `Dog.speak() must take the same parameters as Animal.speak() (expected 0, got 1)` |
| smaller visibility | `Dog.speak() cannot be private because Animal.speak() is public` |
| `super` outside a method | `'super' can only be used inside a method` |
| `super` without a parent | `Animal has no parent class, so 'super' has nothing to refer to` |
| trait contract missing | `Circle does not implement 'area' required by Shape` |
| two traits, one name | `Circle gets 'describe' from both Shape and Describable; define describe() in Circle to choose` |
| creating a trait | `Shape is a trait and cannot be created; use a class that implements it` |
| no operator | `operator '+' is not defined for Vec` |
| operator on the right | `Vec defines '*' only when it is on the left; write vec * 2` |
| forbidden operator | `operator '>' cannot be defined; define '<' and '>', '<=' and '>=' follow from it` |

## Planned (stage 3)

Still parsed and refused with `... is not supported yet`: `pub class`, and `trait` or `operator`
written inside a trait's body (`operators in traits are not supported yet`). Stage 3a added
`extend` and `error` as a class (above). Left for later: a full cycle collector (the refusal of cycles through arrays, dicts and owning
fields stays; `link` covers the back-pointer case, see `docs/MEMORY.md`), `Table` and `region`, the
constructor, `init`, getters and operators in plain VM frames (see "Nested calls"), JIT for
methods, patterns of `is` in `switch`, modules, `extend` of a trait, and a per-class method cache
(member lookup is one function, `lz_class_lookup`; `vm->ext_version` changes with every `extend`).
