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
- `etry(expr)`: if `expr` failed, return it up (adds `file:line` ctx).
- `etry2(expr, "while %s", what)`: same, plus your message.
- `ectx(&e, fmt, ...)`: add context by hand.
- `efree(&e)`: free ctx chain. Needed only if you **swallow** a failure.
  If you return it, the caller owns it. Safe on ok, safe twice.
- An `Error` with ctx owns heap memory: move it, don't copy it around.

```c
Error load(const char *path, Strings *out) {
  FILE *f;
  etry(io_open(&f, path, "r"));
  Error e = FILE_readLines(f, out);
  Error c = io_close(f);
  if (!e.ok) { efree(&c); return e; }
  return c;
}
```

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
array_free0(&words, String_free2);     // frees each String, then the array

size_t n;
etry(String_to_sizet(&word, &n));      // String_isdigits() to pre-check
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
