# Job recipe expression language

Job recipes (the step list posted to `POST /v1/jobs`) contain two kinds of
expressions, both evaluated server-side against the job's shared **context**
(the accumulating bag of step outputs and job `inputs`):

- **References / interpolation** in a step's `params` — resolved by
  `resolve_refs` (`lemon/jobs/job_expr.h`).
- **Boolean conditions** in a step's `when` and in each `branch[].when` —
  evaluated by `eval_condition`.

The language is intentionally tiny: references, literals, comparisons, boolean
logic, and basic arithmetic. It is **not** a general scripting language — there
are no variables, assignments, function calls, or loops, and nothing outside
the grammar below can execute.

## The context

The context is a JSON object. After each step runs, its raw result is stored
under `context[<step id>]`, and any `extract` mappings copy fields to top-level
keys. Job `inputs` are available under `context.inputs`. So a later step can
read `${run_v.timings.prompt_ms}` (the `run_v` step's output),
`${vulkan_tps}` (an extracted key), or `${inputs.model}`.

## References — `${path}`

A reference is `${` followed by a dotted **path** followed by `}`.

```
${model}
${run_v.timings.predicted_per_second}
${inputs.ctx}
${list.0}
```

- Each path segment selects an **object key**, or, when the current value is an
  array, a **numeric index** (`${list.0}` = first element).
- A path that does not resolve is an **error** (the job fails with a clear
  message). There is no "undefined" value.

### Use in `params`: type-preserving vs. interpolating

- **Whole-string reference** — when a params string is *exactly* one reference,
  the resolved value keeps its JSON type:

  ```jsonc
  { "ctx_size": "${inputs.ctx}" }      // -> a number, not the string "32768"
  { "messages": "${inputs.prompt}" }   // -> an array
  ```

- **Embedded reference(s)** — when a reference appears inside surrounding text
  (or more than one reference appears), each is stringified and interpolated,
  and the result is always a string:

  ```jsonc
  { "label": "vulkan @ ${inputs.ctx} tok" }   // -> "vulkan @ 32768 tok"
  ```

  Stringification: strings pass through; numbers/booleans use their literal
  form; `null` becomes empty; arrays/objects use compact JSON.

References inside `when`/`branch` conditions always resolve to the **typed**
value (they are expression operands, not text).

## Conditions — `when` and `branch[].when`

A condition is a boolean expression. An **empty** condition is `true` (an
omitted `when` never skips). The result is taken by **truthiness** (below).

### Literals

| Kind    | Examples |
|---------|----------|
| Number  | `42`, `3.14`, `.5` |
| String  | `'vulkan'`, `"q8_0"` (single or double quotes; `\` escapes the next char) |
| Boolean | `true`, `false` |
| Null    | `null` |

### Operators (highest-binding last)

| Precedence | Operators | Notes |
|------------|-----------|-------|
| 1 (lowest) | `\|\|` | logical or |
| 2 | `&&` | logical and |
| 3 | `!` | logical not (unary) |
| 4 | `==` `!=` `<` `<=` `>` `>=` | comparison; **does not chain** (`1 < 2 < 3` is an error) |
| 5 | `+` `-` | additive (numbers only) |
| 6 | `*` `/` | multiplicative (numbers only) |
| 7 | `-` | unary minus (numbers only) |
| 8 (highest)| `( … )`, literals, `${ref}` | grouping / primaries |

### Truthiness

A value used as a boolean (the whole condition, or an operand of `&&`/`||`/`!`)
is truthy when it is:

- boolean `true`,
- a non-zero number,
- a non-empty string,
- a non-empty array or object.

`false`, `0`, `""`, `null`, `[]`, and `{}` are falsy.

### Comparison semantics

- `==` / `!=` compare by **value and type** (deep JSON equality): `2 == 2.0`
  is true (numeric), `1 == '1'` is false (number vs string).
- `<` `<=` `>` `>=` compare **two numbers** numerically or **two strings**
  lexicographically. Ordering operands of mismatched types (e.g. a string vs a
  number) is an **error**.

### Arithmetic

`+ - * /` and unary `-` operate on **numbers only**; a non-numeric operand is an
error. Division by zero is an error. Division is floating-point
(`10 / 4 == 2.5`).

## Grammar (EBNF)

```
condition  = [ or ] ;                         (* empty = true *)
or         = and { "||" and } ;
and        = not { "&&" not } ;
not        = "!" not | comparison ;
comparison = additive [ ( "==" | "!=" | "<" | "<=" | ">" | ">=" ) additive ] ;
additive   = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative = unary { ( "*" | "/" ) unary } ;
unary      = "-" unary | primary ;
primary    = number | string | "true" | "false" | "null"
           | reference | "(" or ")" ;
reference  = "${" path "}" ;
path       = segment { "." segment } ;
```

## Errors

Evaluation raises a client-facing error (HTTP 400 at job creation for `when`
syntax; job failure at run time for bad references) on:

- an unknown reference path (`${no.such.key}`),
- ordering-comparison of mismatched types,
- arithmetic on a non-number, or division by zero,
- an unterminated `${…}` or string literal,
- an unknown identifier (only `true`/`false`/`null` are keywords),
- an unexpected character or trailing tokens.

At **job creation**, `when`/`branch` expressions are syntax-checked (tokenized)
and rejected early; reference *paths* are only checked at run time, since the
context they read is built as the job runs.

## Caveat: conditions are not short-circuit

`&&` and `||` evaluate **both** sides. A guard like
`${a} != null && ${a.b} > 0` will still evaluate `${a.b}` when `a` is null and
fail on the unknown reference. Structure recipes so a condition only references
context that is guaranteed to exist at that point (e.g. gate on an extracted
scalar you set earlier, not on a possibly-absent nested path).

## Examples

```jsonc
// Skip a step unless the vulkan run beat the rocm run.
{ "when": "${vulkan_tps} > ${rocm_tps}" }

// Batch-ladder gate: only adopt a larger batch if it improves prefill >5%.
{ "when": "${baseline.ttft_ms} / ${best.ttft_ms} > 1.05" }

// Branch to a deeper sweep on the winning backend.
{ "branch": [
    { "when": "${winner} == 'vulkan'", "goto": "deep_vulkan" },
    { "when": "${winner} == 'rocm'",   "goto": "deep_rocm" }
] }

// Interpolated params vs. type-preserving params.
{ "params": {
    "ctx_size": "${inputs.ctx}",                 // number
    "summary":  "won: ${winner} @ ${best.tps} tps"  // string
} }
```
