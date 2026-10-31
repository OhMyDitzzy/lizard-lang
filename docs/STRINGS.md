# String literals

Lizard has one literal form that covers plain, multi-line, template and raw strings:

```
[$][r] fence body fence
```

| Written | Meaning |
| --- | --- |
| `"text"` | plain, with escapes |
| `r"text"` | **raw**: no escapes at all |
| `$"x = {x}"` | **template**: `{expr}` is replaced by a value |
| `$r"{x}\n"` | raw template: interpolation, but `\n` stays two characters |
| `"""` ... `"""` | **multi-line** (also `r"""`, `$"""`, `$r"""`) |

`$` and `r` sit directly against the quote, always in the order `$r`. Single quotes work the
same way (`'text'`, `'''...'''`).

## Fences

* One quote: a single-line string.
* Three or more quotes followed by a line break: a multi-line string. It ends at a line that
  holds only the same number of quotes (after indentation). `"""` in the middle of a line is
  plain text, so a longer fence is only needed when a *line* is exactly the fence.
* Three or more quotes followed by text on the same line: a single-line string that may hold
  quotes; it ends at a run of exactly as many quotes. `r"""C:\"dir" """`.
* A run of quotes longer than the fence inside the text is an error ("ambiguous quotes").

## Multi-line strings

```
let help = """
    Usage: lizard FILE
      --color   force colour
    """
```

* The first line break (after the opening fence) and the last one (before the closing fence)
  are not part of the text; add a blank line before the closing fence to end with a newline.
* The indentation of the closing fence is removed from every line. Blank lines need none. A
  non-blank line indented less, or with different tabs/spaces, is an error pointing at it.
* CRLF becomes LF. In a cooked string a `\` at the end of a line joins it with the next.

## Escapes

Cooked strings: `\n \t \r \0 \\ \" \' \xHH` (00–7F) and `\u{1F600}`. Raw strings have none.
There is no `\{`: write `{{` and `}}` for literal braces (in cooked and raw templates alike).
In a plain string, `{` and `}` are ordinary characters.

## Templates

```
let name = "Ana"
println($"Hello {name}!")            # Hello Ana!
println($"{{literal}} {1 + 2}")      # {literal} 3
println($"{names.join(", ")}")       # strings and calls inside
```

`$"a {x} b"` means `"a " + x.to_string() + " b"`: parts are evaluated left to right, each once,
and the result is always a `string`. A template with no `{}` is a constant. Inside `{}` you may
write any expression (method calls, lambdas, nested strings and templates); in a multi-line
template it may span lines. Comments are not allowed inside `{}`. A lone `{` or `}`, or `{}`, is
an error that reminds you of `{{`/`}}`. Templates use the builtin `to_string`, `format` and
`to_debug` directly, so a function of yours with one of those names cannot change them.
A template cannot be used as a type.

## Format specs: `{x:spec}`

```
spec = [[fill]align] [sign] [#] [0] [width] [group] [.precision] [type]
```

| Part | Values |
| --- | --- |
| fill, align | any one character, then `<` left, `>` right, `^` centre (extra space goes right). Default: left for text, right for numbers |
| sign | `+` always, `-` negatives only (default), space |
| `#` | `0x`, `0o`, `0b` for `x o b` (`X` gives `0X`) |
| `0` | zeros after the sign; not with an align |
| width, precision | a number, or `{expr}`: `{name:>{w}}`, `{pi:.{n}f}` |
| group | `,` every 3 digits (decimal only); `_` every 3 (4 for `x o b`) |

| Type | Accepts | Result |
| --- | --- | --- |
| `s` | anything | text; precision keeps that many characters |
| `d` | int | decimal |
| `x X o b` | int | hex (lower/upper), octal, binary |
| `c` | int | the character with that code point |
| `f` | int, float | fixed, precision 6 by default |
| `e E` | int, float | scientific: `1.50e+03` |
| `%` | int, float | times 100, with `%` |
| none | | text; an int is `d`; a float is its shortest form, or `f` when a precision is given |

```
$"{1234.5:,.2f}"   # 1,234.50        $"{255:#06x}"    # 0x00ff
$"{7:03d}"         # 007             $"{"ab":*^6}"    # **ab**
$"{0.256:.1%}"     # 25.6%           $"{65:c}"        # A
```

A spec written in the source is checked when the program is compiled (the caret points at
the wrong character, an unknown type letter in a different case is suggested). One built
from `{w}` parts is checked when it is used. Options that do not fit are errors: a sign on
text, `,` with `x`, `d` on a float (use `f`, or `.round()` first), a precision on an int.
Width counts characters (code points), not screen columns, so wide characters and emoji may
not line up. `nan` and `inf` are padded with spaces, never zeros.

`x.format(spec)` is the same as `{x:spec}`, for specs built at run time.

## Debug form: `{x=}`

`{expr=}` shows the source text of the expression, `=`, and the value; strings are quoted:

```
let name = "Ana"
let n = 5
println($"{name=} {n + 1=} {n=:>4}")    # name="Ana" n + 1=6 n=   5
```

Spaces around `=` are kept as written (`{n = }` prints `n = 5`). `==`, `!=`, `<=`, `>=` and `=>`
are not affected. `x.to_debug()` gives the quoted form of a value.

## Errors

Mistakes are reported with the usual colored report, with the caret on the exact character:
unterminated or ambiguous fences, text before the closing fence, indentation, a lone `{`/`}`,
`{}`, comments in `{}`, bad escapes (`\{` is explained), bad specs and options.
