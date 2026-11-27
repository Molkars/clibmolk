# clibmolk guide

Practical usage. See `README.md` for fetching, deps and releasing.
Each lib stands alone: take only what you need plus its deps.

## Setup

Single-header. In exactly one `.c` file, define each module's IMPL before
including. Every other file just includes the header.

```c
#define MOLKIE_ERR_IMPL
#define MOLKIE_ARRAY_IMPL
#include "array.h"
```

Rule: if you use module X, the IMPL of X **and all its deps** must be defined
somewhere in the program (deps: see `README.md` table). Missing one = link error.

| you use | define IMPLs |
|---------|--------------|
| err     | ERR |
| io      | ERR, IO |
| array   | ERR, ARRAY |
| btree   | ERR, BTREE |
| cstr    | ERR, CSTR |
| str     | ERR, ARRAY, CSTR, IO, STR |
| parser  | ERR, ARRAY, CSTR, IO, STR, PARSER |

Build flags: `-std=c11 -D_POSIX_C_SOURCE=200809L -Iinc`. `array.h` and
`btree.h` need GNU `__typeof__` (gcc/clang); `io.h` is plain C11.

## attr.h

`PRINTF_LIKE(fmt_idx, args_idx)`: compile-time printf format checks on your own
wrappers. 1-based param positions; `args_idx = 0` for `va_list` variants.

```c
Error log_to(Writer w, const char *fmt, ...) PRINTF_LIKE(2, 3);
```

## err.h — errors as values

Anything that can fail returns `Error`. Check `.ok`.

```c
Error e = enew(EINVAL, "bad id %d", 3);   // code + message
Error e = eerrno("open %s", path);        // code=errno, msg "open x: strerror"
return eok();
```

- `msg` is 160 chars, truncated. `code` is errno or your own.
- `etry(expr)`: if `expr` failed, store it in `eret` (adds `file:line` ctx) and
  `goto end`. See "One way out" below.
- `etry2(expr, "while %s", what)`: same, plus your message.
- `ejoin(&e, later)`: keep the first error, drop `later` (frees its ctx). For
  cleanup that can fail without hiding the real error.
- `ectx(&e, fmt, ...)`: add context by hand.
- `efree(&e)`: free ctx chain. Needed only if you **swallow** a failure.
  If you return it, the caller owns it. Safe on ok, safe twice.
- An `Error` with ctx owns heap memory: move it, don't copy it around.
- Most `Error`-returning functions are `NODISCARD`: ignoring the result warns
  (not the print helpers `efprint`, `eprint`, `W*`, `io_fprint`). gcc warns even
  on a `(void)` cast, so to drop one on purpose keep it and `efree` it.

### One way out

`just lint` (`tools/lint.py`) checks all of this, and gcc's `-Wjump-misses-init`
(added to the build flags when available) catches a skipped initialiser.

A function that uses `etry` declares `Error eret`, has an `end:` label, and
returns from exactly one place. Cleanup goes after `end:`. (Snippets elsewhere
in this guide show bare `etry(...)` lines: they sit inside a function shaped
like this.)

```c
Error load(const char *path, Strings *out) {
  Error eret = eok();
  FILE *f = NULL;                      // declare everything BEFORE the first etry
  etry(io_open(&f, path, "r"));        // failed: eret = it, goto end
  etry(FILE_readLines(f, out));
end:
  if (f) ejoin(&eret, io_close(f));    // a failing close never hides eret
  return eret;
}
```

- No `end:` label = compile error. Don't `return` from the middle of such a
  function (it skips the cleanup); checks *before* you acquire anything may.
- A `goto` skips initialisers, so put every variable that `end:` uses above
  the first `etry`.

Defer: register cleanup right after acquiring, run it all at `end:`, newest
first, only for what was acquired. Pure C11 + a small stack array:

```c
Error load(const char *path) {
  Error eret = eok();
  EDEFERS(3);                          // capacity; before the first etry
  FILE *f;
  etry(io_open(&f, path, "r"));
  edefer(io_close, f);                 // returns Error: joined into eret
  char *buf = malloc(4096);
  edefer_void(free, buf);              // returns void
  edefer_void(array_free0_raw, &words.raw, String_free2);  // any arguments
  etry(...);
end:
  erundefers();
  return eret;
}
```

`edefer(fn, args...)` stores `fn` and up to 6 arguments **now** and calls
`fn(args...)` at `erundefers()`. The call is type-checked where you write it:
wrong arity or argument types are compile errors, and `edefer` (not
`edefer_void`) requires `fn` to return `Error`. Arguments must be integers,
`bool`, pointers or function pointers (floats, doubles and structs are compile
errors). `fn` must be a real function, not a macro: for a typed macro API defer
its `_raw` function or a small wrapper. More defers than `EDEFERS(n)` asserts.

Portability: the stored call goes through a cast function pointer with
word-sized arguments. The C standard doesn't define that; it works on 64-bit
x86-64 and aarch64 for the argument kinds above (a static assert rejects
32-bit targets), and clang's `-fsanitize=function` reports it.

Print one:

```c
if (!e.ok) { efprint(&e, stderr); efree(&e); }     // any FILE*, err.h only
if (!e.ok) { eprint(&e, stderrW); efree(&e); }     // any Writer, from io.h
// error: <newest ctx>: ...: <msg>
```

Both return an `Error` for the write itself; neither frees `e`.

## io.h — FILE* ops, Writer, Reader, formatting

Thin `Error`-returning wrappers over stdio. Retry on `EINTR`, report deferred
write errors (`ENOSPC`).

```c
FILE *f;
etry(io_open(&f, "out.txt", "w"));
etry(io_write(f, buf, n, NULL));     // writes all n, or errors
etry(io_close(f));                   // flushes; f is gone even on error
```

- `io_read(f, buf, n, &got)`: short count + ok = EOF.
- `io_write`/`io_read` take optional count out-param; on failure it says how
  much went through.
- `io_forward(dst, src)`: copy to EOF (doesn't flush `dst`).
- `io_flush(f)`: null is `EINVAL`, not "flush all".
- Always `io_close` write streams and check it: that's where disk-full shows.
- Every `io_*` failure is an `Error`: print it to any `FILE *` with
  `efprint(&e, stderr)` (from `err.h`, no `Writer` needed), then `efree(&e)`.

```c
Error e = io_forward(dst, src);
if (!e.ok) { efprint(&e, stderr); efree(&e); }
```

### Writer and Reader

A `Writer` is one callback plus a context pointer. It is never modified, so
**pass it by value**. Functions that print take a `Writer`, so they work on
stdout, a file, or a buffer.

```c
Wputs(stdoutW, "hi\n");            // stdoutW / stderrW need no setup
Wprintf(w, "pid=%zu\n", pid);      // printf style; also Wputc, Wwrite, Wvprintf

Writer w = FILE_writer(f);         // any FILE*; header-only, inlines

char buf[64];
Scratch s = Scratch_init(buf, sizeof buf);
Writer w = Scratch_writer(&s);     // fixed buffer; always a valid C string
Scratch_reset(&s);
```

All return `Error`. Scratch overflow truncates and returns `ENOSPC`. Custom
sink: fill a `Writer { write, ctx }` with a `write(ctx, buf, n)` callback.

A `Reader` is the same idea, with `read(ctx, buf, n, &nRead)` and `io_read`'s
rules (short count + ok = end of input):

```c
Reader r = FILE_reader(f);
Cursor c = Cursor_init(data, len);  // read from memory
Reader r = Cursor_reader(&c);
size_t n;
etry(Rread(r, buf, sizeof buf, &n));
int ch;
etry(Rgetc(r, &ch));               // ch == EOF at the end
etry(io_copy(w, r));               // reader -> writer until the end
```

### `{}` formatting

`io_fprint(w, fmt, ...)` writes `fmt` with `{}` placeholders. Argument types
are found at compile time with `_Generic` (C11, no `typeof`), so no `%d`/`%s`
to keep in sync:

```c
etry(io_fprint(w, "{} has {} items, {:.1}% full\n", name, count, pct));
io_print("{:>8} | {:<#6x} | {:^7}\n", name, flags, ok);   // stdout
io_println("done in {} ms", ms);                          // + newline
io_fprintln(w, "{}: {}", key, val);                       // any Writer
```

- Spec: `{:[[fill]align][+][#][0][width][.precision][type]}`. `align` is `<`
  `^` `>`; `+` forces a sign; `#` adds `0x`/`0b`/`0o`; `0` zero-pads numbers;
  `.N` is max chars for text, digits after the point for floats. `type`:
  `x X o b` for ints, `c` for an int or char as a character (`{:c}` with `'x'`), `e E f g G` for floats. Width counts
  UTF-8 characters. `{{` and `}}` are literal braces.
- Text defaults to left, numbers to right. A plain `{}` on a float prints the
  shortest text that reads back the same (`0.1`, not `0.10000000000000001`).
- On a real `_Bool`, `{:b}` is the same as `{}` (`true`/`false`); on other integers it is binary.
- Builtin args (by value): `bool`/`_Bool`, all `char`/`short`/`int`/`long`/
  `long long` (signed and unsigned), `float`, `double`, `long double`,
  `char *`, `void *` (prints `0x...`). `'x'` in C is an `int`: use `{:c}`. `true`/`false` are ints too: cast to `_Bool`.
- Anything else is a compile error. A wrong placeholder count or bad spec is
  `EINVAL` at runtime, and nothing is written. Up to 16 args.
- Returns an `Error` (the writer's, if it failed). Args are evaluated once.

Your own types are passed **by address** and found through `T_format`:

```c
typedef struct { int x, y; } Vec2;
#define IO_FORMAT_TYPES(X) X(Vec2) X(Color)   // BEFORE including io.h
#include "io.h"
IO_FORMAT_DEFINE(Vec2)       // right after the typedef: declares Vec2_format
IO_FORMAT_DEFINE(Color)

Error Vec2_format(Writer w, const Vec2 *v, FormatSpec spec) {
  return io_fprint(w, "({}, {})", v->x, v->y);   // spec: width, fill, ...
}
io_fprint(w, "at {}", &pos);
```

Formatters may be used directly: `int_format(w, &v, (FormatSpec){.width = 5})`
(a zeroed `FormatSpec` is plain `{}`). `io_format_args(fmt, ...)` is the macro
under `io_fprint`: it expands to `fmt, args, count`, the last three arguments of
`io_format(w, ...)`.

Including `str.h` makes `String` formattable too, by address (`io_fprint(w, "{:>8}", &s)`); don't list it in `IO_FORMAT_TYPES` yourself.

## array.h — typed growable array

```c
typedef array_t(int) Ints;           // typedef it; use the typedef in params
Ints a = array_empty(int);           // or array_new(cap, int)

int v = 5;
etry(array_push(&a, &v));            // pass a POINTER to the element
int *p = array_at(&a, 0);            // NULL if out of range
for (size_t i = 0; i < a.len; i++) use(a.elements[i]);

array_free(&a);
```

- Fields: `elements`, `len`, `cap`, `esz`. Read freely.
- `array_extend(&a, &b)`: append another array of same `T`.
  `array_extend2(&a, ptr, n)`: append `n` from a C array.
  `array_grow(&a, n)`: reserve room.
- Elements that own memory: `array_free0(&a, free_fn)` calls
  `void (*)(void *elem_ptr)` per element. Example: `array_free0(&words, String_free2)`.
- Push failure (`ENOMEM`) leaves the array unchanged.
- Wrong element type = compile error (that's the point).
- **`elements` can move on push.** Don't hold `array_at` pointers across one.

## btree.h — typed ordered map

Key `K` -> value `V`. Keys/values are copied in (shallow, by size). Iterates in
key order.

```c
typedef btree_str_t(int) Ages;       // string keys (const char *), int values
Ages t = btree_str_empty(int);

const char *k = "ann";  int v = 30;
etry(btree_insert(&t, &k, &v));      // pointers to key and value; overwrites
int *got = btree_get(&t, &k);        // NULL if absent
btree_contains(&t, &k);              // 1/0
btree_remove(&t, &k);                // 1 if removed
btree_size(&t);

btree_iter_t(const char *, int) it;
btree_iter(&it, &t);
while (btree_next(&it)) printf("%s=%d\n", *it.item, *it.value);

btree_free(&t);
```

- Other key types: `typedef btree_t(int, double) M; M m = btree_empty(int, double, cmp);`
  where `int cmp(const void *a, const void *b)` gets pointers to keys.
- `_ex` variants (`btree_insert_ex`, `btree_remove_ex`) hand back the old value
  (`V *`, or `NULL` to ignore).
- **Key ownership.** `btree_str_t` trees own their strings: insert copies
  (`strdup`), so free or reuse yours right after. Remove and `btree_free` free
  the tree's copy. `*it.item` points at the tree's copy; valid until that key is
  removed.
- Other `K` are plain byte copies, so a pointer key is only borrowed and must
  outlive the tree. To own it, give clone/free hooks:
  `btree_empty_own(K, V, cmp, clone, free)`.
  `Error clone(void *dst, const void *src)` deep-copies the key slot (leave
  nothing allocated on error); `void free(void *slot)` releases what the key
  owns. Set both or neither.
- Values are always plain byte copies; the tree never frees what they point to.
- Don't modify the tree while iterating. `btree_extend(&a, &b)` merges (clones
  keys too).
- `btree_free` frees the tree and the keys it owns.

## cstr.h — C-string helpers

```c
cstr_isdigits("123");                // 1; null/empty/non-digit -> 0

size_t n;
etry(cstr_to_sizet("42", &n));       // digits only; EINVAL / ERANGE otherwise

int i; double d; bool ok;
etry(cstr_to("-7", &i));             // destination type picks the parser
etry(cstr_to("2.5e3", &d));          // (C11 _Generic; or cstr_to_int, _double, ...)
etry(cstr_to("true", &ok));
```

Types: `short ushort int uint long ulong llong ullong float double bool`
(`size_t` works, it is one of the unsigned ones). Strict: decimal only, no
whitespace, no trailing text. Signed ints take an optional `-`/`+`, unsigned
none. Floats are `[+-]digits[.digits][e[+-]digits]` (also `inf`/`nan`, no hex);
overflow is `ERANGE`, underflow rounds to 0. Bool is exactly `true`/`false`.
`EINVAL` if it isn't a number, `ERANGE` if it doesn't fit; `dst` is untouched
on error.

```c
char *s;
etry(cstr_format(&s, "%s-%d", a, 3)); // malloc'd, you free(s)

char small[16];
etry(cstr_sformat(small, sizeof small, "%d", x)); // error if it doesn't fit
```

## str.h — growable String

```c
String s = String_empty();           // allocates nothing, valid C string
String t = String_fromc("hello");    // copy; buf NULL on alloc failure
String_puts(&s, "a");                // appends; all return 0 on failure
String_putc(&s, 'b');
String_printf(&s, "%d", 7);
String_cstr(&s);                     // never NULL
String_eq(&s, "ab7");
String_trim(&s);                     // in place
String_reset(&s);                    // empty, keep memory
String_free(&s);
```

Parse/split:

```c
Strings words = array_empty(String);   // Strings = array_t(String)
etry(String_split(&s, &words));        // on whitespace, new copies
etry(String_splitOn(&s, ",", &words)); // on a delimiter string, keeps empty fields
array_free0(&words, String_free2);     // frees each String, then the array

// known shape: exactly N parts into a C array (EINVAL if the count differs)
String kv[2];
etry(String_split2(&line, kv, 2));           // whitespace
etry(String_splitOn2(&line, "=", kv, 2));    // any non-empty delimiter string
String_free(&kv[0]); String_free(&kv[1]);
```

`String_splitOn2` keeps empty fields (`"a,,b"` -> 3). On failure every slot is
`String_empty()`, so nothing to free; the slots' old contents are overwritten,
not freed.

```c
size_t n;
etry(String_to_sizet(&word, &n));      // String_isdigits() to pre-check
etry(String_to(&word, &i));            // same parsers as cstr_to: int, double, bool, ...
```

Read a whole file (replaces `str`; bigger than `memmax` bytes is `EFBIG`,
and `str` is left empty on any failure):

```c
String str = String_empty();
etry(String_readFile("in.txt", 1 << 20, &str));
```

Read a file by lines (no trailing `\n`/`\r\n`):

```c
Strings lines = array_empty(String);
etry(FILE_readLines(f, &lines));       // on failure lines is left empty
...
array_free0(&lines, String_free2);
```

`String` fields (`buf`, `len`, `cap`) are readable; `len` excludes the NUL.

## parser.h — recursive-descent parsing

A `Parser` reads from a `Reader` (lazily, kept in memory) or from bytes you
already have (borrowed, not copied). You look with `peek*`, eat with `take*`,
`expect*` and `p_*`. Failures are `Error`s that say `at line:col`.

```c
Parser p;
Parser_newc(&p, "12 (hello world) 1 2 3");     // or Parser_newBytes / Parser_newString
Parser_new(&p, FILE_reader(f));                // or any Reader
...
Parser_free(&p);
```

A piece of a String: `Parser_newBytes(&p, s.buf + from, len)`.

| call | does |
|------|------|
| `Parser_peek1` / `Parser_take1` | next byte or `0` at the end (`Parser_eof` tells a real NUL apart) |
| `Parser_peekn(p, i)` | the byte `i` ahead, nothing consumed |
| `Parser_peekc(p, "+-")` / `Parser_takec` | the next byte if it is in the set, else `0` |
| `Parser_peeks(p, "<=", "<")` / `Parser_takes` | the first listed string the input starts with, else `NULL` (longest first) |
| `Parser_expectc` / `Parser_expects` | consume it or `EINVAL` |
| `Parser_skipws`, `Parser_done`, `Parser_end` | skip whitespace; `done` also says if nothing is left; `end` fails if anything is |
| `Parser_fail(p, code, fmt, ...)` | your own `Error`, with `at line:col` added |
| `Parser_skipblank`, `Parser_atLineEnd`, `Parser_expectNewline`, `Parser_p_line` | line-based formats: spaces/tabs only, `\n` or `\r\n`, the end of input ends a last line |
| `Parser_p_list(p, ",", item, ctx)` | items separated by a string (NULL: whitespace) up to the end; `item` is `Error fn(Parser *, void *ctx)` |
| `Parser_p_sizet`, `Parser_p(p, &x)` | a number; `Parser_p` picks the type from `&x` like `cstr_to` (int, double, bool, ...) |
| `Parser_p_until(p, ",;", &s)` / `Parser_p_while(p, isalpha, &s)` | text into a `String`, stop byte not consumed |
| `Parser_mark` / `Parser_reset` | backtrack (free: the bytes stay in memory) |

`Parser_skipws` skips newlines too. For one record per line use `Parser_skipblank`
and `Parser_expectNewline` instead.

A number takes the longest run that looks like one, so `"12abc"` gives 12 and
leaves `abc`. A failed number consumes nothing.

Groups parse a delimited piece on its own, and can't read past its end. They
nest when the delimiters differ. There are no escapes:

```c
Error Rec_parse(Parser *p, Rec *out) {          // "<n> (<chars>) <n> <n> ... <n>"
  Error eret = eok();
  Parser g;
  etry(Parser_p_sizet(p, &out->id));
  Parser_skipws(p);
  etry(Parser_group(p, &g, "(", ")"));
  etry(Parser_p_until(&g, NULL, &out->name));   // to the end of the group
  etry(Parser_groupEnd(&g));                    // unconsumed bytes are an error; p moves past ')'
  while (!Parser_done(p)) {                      // skips whitespace, then checks for the end
    size_t n;
    etry(Parser_p_sizet(p, &n));
    etry(array_push(&out->nums, &n));
  }
end:
  return eret;
}
```

- The parent doesn't move until `Parser_groupEnd`. Don't use it meanwhile.
- A read error ends the input early and is remembered. `Parser_end`,
  `Parser_groupEnd`, `Parser_ioerr` and any failing `expect*`/`p_*` report it;
  a parser that never does can mistake it for the end.
- A Reader's bytes are all kept until `Parser_free`.

## test.h — tiny test runner

Each test runs in a forked child; a failed `assert` or a crash fails only that
test and replays its output.

```c
#include "test.h"
static void adds(void) { ASSERT_EQ(1 + 1, 2); }

int main(void) {
  static const TestCase tests[] = { T(adds) };
  size_t pass = 0, n = sizeof tests / sizeof *tests;
  for (size_t i = 0; i < n; i++) pass += run_in_child(&tests[i]);
  return pass == n ? 0 : 1;
}
```

`ASSERT_EQ` compares as `size_t` (not for signed/negative or floats).

The libs' own tests live in the headers behind `MTEST` (all) or
`MOLKIE_<NAME>_TEST` (one); see `tests/all.c`.

## Common mistakes

- Forgot an IMPL (own or dep): undefined reference at link.
- `eprint` takes a `Writer` by value (`eprint(&e, stderrW)`). For a `FILE *`
  use `efprint` (`err.h`).
- `io_fprint(&w, ...)`: writers are passed by value, not by pointer.
- A type with `T_format` but not listed in `IO_FORMAT_TYPES` (or listed
  without `IO_FORMAT_DEFINE`): compile error. Pass it as `&value`.
- Swallowed an `Error` without `efree`: leaks its ctx chain.
- Non-string `btree` pointer keys without `btree_empty_own`: borrowed, so
  they dangle if freed before the tree.
- Used an `array_at` pointer after a push: it may have moved.
- Passed a value instead of a pointer to `array_push` / `btree_insert`.
