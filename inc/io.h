#ifndef MOLKIE_IO_H
#define MOLKIE_IO_H

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "attr.h"
#include "err.h"

/// opens [path] with fopen [mode] and stores the stream in [*out]. [*out] is
/// null on failure. Retries on EINTR; a bad [mode] is EINVAL
NODISCARD Error io_open(FILE **out, const char *path, const char *mode);
/// flushes and closes [f], reporting any deferred write error (e.g. ENOSPC).
/// [f] is closed even when this fails, so never close it again. A null [f] is
/// EINVAL
NODISCARD Error io_close(FILE *f);
/// writes all [bufsz] bytes of [buf] to [f], retrying on EINTR. [nWritten] (if
/// non-null) is zeroed first and kept up to date as bytes go out, so on
/// failure it says how much of [buf] was written. A [bufsz] of 0 is a no-op
/// that does not look at the stream
NODISCARD Error io_write(FILE *f, const char *buf, size_t bufsz, size_t *nWrote);
/// reads up to [bufsz] bytes into [buf] and stores the count in [nRead]. A
/// short count (including 0) with an ok result means end of file; failures
/// return an errno code. [nRead] (if non-null) is zeroed first and kept up to
/// date as bytes come in, so on failure it says how much of [buf] is valid. A
/// [bufsz] of 0 is a no-op that does not look at the stream
NODISCARD Error io_read(FILE *f, char *buf, size_t bufsz, size_t *nRead);
/// copies [src] to [dst] until EOF. Does not flush [dst]. If reading fails
/// midway, the bytes read before the failure are still written to [dst]
NODISCARD Error io_forward(FILE *dst, FILE *src);
/// flushes [f]; a null [f] is EINVAL (not "flush everything")
NODISCARD Error io_flush(FILE *f);

// ---------------------------------------------------------------------------
// Writer / Reader: byte sinks and sources. Both are tiny values (a callback and
// a context pointer): pass them by value. The callback gets the context, the
// `Writer` itself is never modified, so `FILE_writer(f)` is cheap and the
// compiler can inline it.
// ---------------------------------------------------------------------------

/// Writes all [n] bytes of [buf] or fails. Does not report partial progress
typedef Error WriterWriteFn(void *ctx, const char *buf, size_t n);

typedef struct Writer {
  WriterWriteFn *write;
  void *ctx;
} Writer;

/// All of these return `eok()`, or the error:
/// ```
/// Error e = Wprintf(w, "pid=%zu", pid);
/// if (!e.ok)
///   return e;
/// ```
static inline Error Wwrite(Writer w, const char *buf, size_t n) {
  assert(w.write && "writer is not initialised");
  return w.write(w.ctx, buf, n);
}
static inline Error Wputs(Writer w, const char *s) {
  return Wwrite(w, s, strlen(s));
}
static inline Error Wputc(Writer w, int c) {
  char ch = (char)c;
  return Wwrite(w, &ch, 1);
}
/// printf-style (`%` specifiers). For `{}` specifiers see `io_fprint`
Error Wprintf(Writer w, const char *fmt, ...) PRINTF_LIKE(2, 3);
Error Wvprintf(Writer w, const char *fmt, va_list argp) PRINTF_LIKE(2, 0);

static inline NODISCARD Error FILE_write(void *f, const char *buf, size_t n) {
  return io_write((FILE *)f, buf, n, NULL);
}
/// writes to [f]. Failures carry the `errno` text. Header-only on purpose, so
/// a call with a known [f] inlines down to `io_write`
static inline Writer FILE_writer(FILE *f) { return (Writer){FILE_write, f}; }

/// `stdout` / `stderr` as writers. No setup needed
#define stdoutW FILE_writer(stdout)
#define stderrW FILE_writer(stderr)

/// A fixed buffer you can write into. Invariant: `buf` is always
/// NUL-terminated and `written < cap`, so it is always a valid C string. A
/// write that doesn't fully fit is truncated and fails with `ENOSPC`. A write
/// containing a NUL byte is `EINVAL` and writes nothing
typedef struct {
  char *buf;
  size_t cap;
  size_t written;
} Scratch;

Scratch Scratch_init(char *buf, size_t cap);
void Scratch_reset(Scratch *s);
/// the writer borrows [s]: keep it alive and in place while the writer is used
Writer Scratch_writer(Scratch *s);

/// Reads up to [n] bytes into [buf] and stores the count in [nRead]. Same
/// contract as `io_read`: a short count (including 0) with an ok result means
/// end of input. Callbacks always get a non-null [nRead], already zeroed
typedef Error ReaderReadFn(void *ctx, char *buf, size_t n, size_t *nRead);

typedef struct Reader {
  ReaderReadFn *read;
  void *ctx;
} Reader;

/// [nRead] is optional
static inline NODISCARD Error Rread(Reader r, char *buf, size_t n, size_t *nRead) {
  assert(r.read && "reader is not initialised");
  size_t ignored;
  if (!nRead)
    nRead = &ignored;
  *nRead = 0;
  return r.read(r.ctx, buf, n, nRead);
}
/// reads one byte into [*c], or `EOF` at the end of input
static inline NODISCARD Error Rgetc(Reader r, int *c) {
  unsigned char b;
  size_t n;
  Error e = Rread(r, (char *)&b, 1, &n);
  *c = e.ok && n ? b : EOF;
  return e;
}

static inline NODISCARD Error FILE_read(void *f, char *buf, size_t n, size_t *nRead) {
  return io_read((FILE *)f, buf, n, nRead);
}
/// reads from [f], see `io_read`
static inline Reader FILE_reader(FILE *f) { return (Reader){FILE_read, f}; }

/// Reads from memory. The reader borrows the cursor and the bytes
typedef struct {
  const char *buf;
  size_t len;
  size_t pos;
} Cursor;

Cursor Cursor_init(const char *buf, size_t len);
Reader Cursor_reader(Cursor *c);

/// copies [src] to [dst] until the end of input
NODISCARD Error io_copy(Writer dst, Reader src);

/// writes `error: <ctx>: ...: <ctx>: <msg>\n` (newest context first, then the
/// original message) to [w]; writes nothing if [e] is ok. Returns the writer's
/// own error, if it has one. Does not free [e]. For a plain `FILE *` there is
/// also `efprint` in err.h
Error eprint(const Error *e, Writer w);

// ---------------------------------------------------------------------------
// Formatting: `{}` placeholders, type picked with C11 `_Generic`.
//
//   io_fprint(w, "{} has {} items, {:.2}% full\n", name, count, pct);
//   io_print("{:>8} | {:<#6x} | {:^7}", name, flags, true);
//   io_println("done in {} ms", ms);   // same, plus a newline
//
// Placeholders: `{}` or `{:spec}`, with `{{` and `}}` for literal braces.
//   spec = [[fill]align][+][#][0][width][.precision][type]
//   fill   any one byte (not `}`); align `<` `^` `>`
//   +      always show a sign on numbers
//   #      alternate form: 0x / 0o / 0b prefixes, always a decimal point
//   0      pad numbers with zeros after the sign (overrides fill and align)
//   width  minimum width in characters (UTF-8 aware)
//   .prec  strings: max characters; floats: digits after the point
//   type   ints: `x` `X` `o` `b` (`d` is the default); `c` prints the integer
//          as a character (UTF-8), which is how to print a `'x'` literal;
//          floats: `e` `E` `f` `g` `G`; none = shortest text that round-trips;
//          `?` debug form, only for `String` (see str.h): quoted and escaped,
//          so NULs and control bytes show. Not valid for `char *`
//
// Arguments are plain values of the types in IO_BUILTIN_TYPES below (integers,
// floats, bool, char, `char *`, `void *`). Other types are passed BY ADDRESS and
// found through `T_format`, see IO_FORMAT_DEFINE. Anything else is a compile
// error. Placeholder/argument mismatches and bad specs are `EINVAL` at runtime
// and nothing is written. At most 16 arguments.
// ---------------------------------------------------------------------------

typedef struct FormatSpec {
  char fill;         // 0 is a space
  char align;        // '<', '^', '>' or 0 for the type's default
  char sign;         // '+' or 0
  bool alt;          // '#'
  bool zero;         // '0'
  bool hasPrecision; // `.N` given
  int width;         // 0 for none
  int precision;     // only meaningful if hasPrecision
  char type;         // 'x', 'e', ... or 0
} FormatSpec;

/// A formatter writes the value [v] points to, as asked by [spec]. For
/// `T_format(Writer, const T *, FormatSpec)` the adapter made by
/// IO_FORMAT_DEFINE is what ends up here. Return `EINVAL` for a [spec] the type
/// doesn't support
typedef Error FormatFn(Writer w, const void *v, FormatSpec spec);

typedef const char *IoCharPtr;
typedef const void *IoVoidPtr;

/// (type, name): the formatter for `type` is `name##_format`. (`long double`,
/// formatter `ldouble_format`, is handled by hand below)
#define IO_BUILTIN_TYPES(X)                                                    \
  X(_Bool, Bool)                                                               \
  X(char, char)                                                                \
  X(signed char, schar)                                                        \
  X(unsigned char, uchar)                                                      \
  X(short, short)                                                              \
  X(unsigned short, ushort)                                                    \
  X(int, int)                                                                  \
  X(unsigned, uint)                                                            \
  X(long, long)                                                                \
  X(unsigned long, ulong)                                                      \
  X(long long, llong)                                                          \
  X(unsigned long long, ullong)                                                \
  X(float, float)                                                              \
  X(double, double)                                                            \
  X(IoCharPtr, charptr)                                                        \
  X(IoVoidPtr, voidptr)

#define IO_DECLARE_FORMAT_(T, N)                                               \
  Error N##_format(Writer w, const T *v, FormatSpec spec);
IO_BUILTIN_TYPES(IO_DECLARE_FORMAT_)
#undef IO_DECLARE_FORMAT_

/// The value of one argument, or a pointer to it. Built by the macros below.
/// (`long double` sits outside the union: unions holding one trip a GCC ABI
/// note on every function that returns them)
typedef struct FormatArg {
  FormatFn *fn;
  bool byptr;
  bool isLongDouble;
  const void *ptr; // byptr: the object to format
  long double ld;  // isLongDouble: the value
  union {
#define IO_VAL_MEMBER_(T, N) T v_##N;
    IO_BUILTIN_TYPES(IO_VAL_MEMBER_)
#undef IO_VAL_MEMBER_
  } val; // otherwise the formatter gets `&val`
} FormatArg;

#define IO_BUILTIN_ARG_(T, N)                                                  \
  static inline Error io_adapt_##N(Writer w, const void *p, FormatSpec s) {    \
    return N##_format(w, p, s);                                                \
  }                                                                            \
  static inline FormatArg io_arg_##N(T v) {                                    \
    FormatArg a = {.fn = io_adapt_##N};                                        \
    a.val.v_##N = v;                                                           \
    return a;                                                                  \
  }
IO_BUILTIN_TYPES(IO_BUILTIN_ARG_)
#undef IO_BUILTIN_ARG_
NODISCARD Error ldouble_format(Writer w, const long double *v, FormatSpec spec);
static inline NODISCARD Error io_adapt_ldouble(Writer w, const void *p, FormatSpec s) {
  return ldouble_format(w, p, s);
}
static inline FormatArg io_arg_ldouble(long double v) {
  return (FormatArg){.fn = io_adapt_ldouble, .isLongDouble = true, .ld = v};
}

/// Makes your type `T` formattable, passed by address. Every type listed in
/// IO_FORMAT_TYPES needs this, at file scope, before the first `io_fprint` in
/// the program (so right after the typedef). It declares
/// `Error T_format(Writer w, const T *v, FormatSpec spec)`, which you then
/// write. `T` must be a single identifier (typedef it):
/// ```
/// typedef struct { int x, y; } Vec2;
/// #define IO_FORMAT_TYPES(X) X(Vec2) X(Color)   // before including io.h
/// #include "io.h"
/// IO_FORMAT_DEFINE(Vec2)
/// ...
/// Error Vec2_format(Writer w, const Vec2 *v, FormatSpec spec) {
///   return io_fprint(w, "({}, {})", v->x, v->y);
/// }
/// ...
/// io_fprint(w, "at {}", &pos);
/// ```
#define IO_FORMAT_DEFINE(T)                                                    \
  Error T##_format(Writer w, const T *v, FormatSpec spec);                     \
  static inline Error io_adapt_##T(Writer w, const void *p, FormatSpec s) {    \
    return T##_format(w, p, s);                                                \
  }                                                                            \
  static inline FormatArg io_arg_ptr_##T(const T *p) {                         \
    return (FormatArg){.fn = io_adapt_##T, .byptr = true, .ptr = p};           \
  }

/// Hooks for other modules, so including them is enough to make their types
/// formattable (str.h redefines IO_FORMAT_EXT_STR to `X(String)`). Not for
/// users: list your own types in IO_FORMAT_TYPES
#define IO_FORMAT_EXT_STR(X)

/// The user's types: `#define IO_FORMAT_TYPES(X) X(A) X(B)`. Must be defined
/// before this header is included
#ifndef IO_FORMAT_TYPES
#define IO_FORMAT_TYPES(X)
#endif

struct io_no_format_; // never matches; ends the _Generic list
#define IO_BUILTIN_ASSOC_(T, N) T : io_arg_##N,
#define IO_USER_ASSOC_(T)                                                      \
  const T *: io_arg_ptr_##T, T *: io_arg_ptr_##T,
#define IO_ARG_(x)                                                             \
  _Generic((x),                                                                \
      IO_BUILTIN_TYPES(IO_BUILTIN_ASSOC_)                                      \
      long double: io_arg_ldouble,                                             \
      char *: io_arg_charptr,                                                  \
      void *: io_arg_voidptr,                                                  \
      IO_FORMAT_TYPES(IO_USER_ASSOC_)                                          \
      IO_FORMAT_EXT_STR(IO_USER_ASSOC_)                                        \
      struct io_no_format_ *: io_arg_Bool)(x)

#define IO_MAP_1(m, a) m(a)
#define IO_MAP_2(m, a, ...) m(a), IO_MAP_1(m, __VA_ARGS__)
#define IO_MAP_3(m, a, ...) m(a), IO_MAP_2(m, __VA_ARGS__)
#define IO_MAP_4(m, a, ...) m(a), IO_MAP_3(m, __VA_ARGS__)
#define IO_MAP_5(m, a, ...) m(a), IO_MAP_4(m, __VA_ARGS__)
#define IO_MAP_6(m, a, ...) m(a), IO_MAP_5(m, __VA_ARGS__)
#define IO_MAP_7(m, a, ...) m(a), IO_MAP_6(m, __VA_ARGS__)
#define IO_MAP_8(m, a, ...) m(a), IO_MAP_7(m, __VA_ARGS__)
#define IO_MAP_9(m, a, ...) m(a), IO_MAP_8(m, __VA_ARGS__)
#define IO_MAP_10(m, a, ...) m(a), IO_MAP_9(m, __VA_ARGS__)
#define IO_MAP_11(m, a, ...) m(a), IO_MAP_10(m, __VA_ARGS__)
#define IO_MAP_12(m, a, ...) m(a), IO_MAP_11(m, __VA_ARGS__)
#define IO_MAP_13(m, a, ...) m(a), IO_MAP_12(m, __VA_ARGS__)
#define IO_MAP_14(m, a, ...) m(a), IO_MAP_13(m, __VA_ARGS__)
#define IO_MAP_15(m, a, ...) m(a), IO_MAP_14(m, __VA_ARGS__)
#define IO_MAP_16(m, a, ...) m(a), IO_MAP_15(m, __VA_ARGS__)

// IO_FA_<k>(fmt, args...): `fmt, array-of-FormatArg, count` for k-1 args
#define IO_FA_1(f) f, NULL, 0
#define IO_FA_2(f, ...) f, (FormatArg[]){IO_MAP_1(IO_ARG_, __VA_ARGS__)}, 1
#define IO_FA_3(f, ...) f, (FormatArg[]){IO_MAP_2(IO_ARG_, __VA_ARGS__)}, 2
#define IO_FA_4(f, ...) f, (FormatArg[]){IO_MAP_3(IO_ARG_, __VA_ARGS__)}, 3
#define IO_FA_5(f, ...) f, (FormatArg[]){IO_MAP_4(IO_ARG_, __VA_ARGS__)}, 4
#define IO_FA_6(f, ...) f, (FormatArg[]){IO_MAP_5(IO_ARG_, __VA_ARGS__)}, 5
#define IO_FA_7(f, ...) f, (FormatArg[]){IO_MAP_6(IO_ARG_, __VA_ARGS__)}, 6
#define IO_FA_8(f, ...) f, (FormatArg[]){IO_MAP_7(IO_ARG_, __VA_ARGS__)}, 7
#define IO_FA_9(f, ...) f, (FormatArg[]){IO_MAP_8(IO_ARG_, __VA_ARGS__)}, 8
#define IO_FA_10(f, ...) f, (FormatArg[]){IO_MAP_9(IO_ARG_, __VA_ARGS__)}, 9
#define IO_FA_11(f, ...) f, (FormatArg[]){IO_MAP_10(IO_ARG_, __VA_ARGS__)}, 10
#define IO_FA_12(f, ...) f, (FormatArg[]){IO_MAP_11(IO_ARG_, __VA_ARGS__)}, 11
#define IO_FA_13(f, ...) f, (FormatArg[]){IO_MAP_12(IO_ARG_, __VA_ARGS__)}, 12
#define IO_FA_14(f, ...) f, (FormatArg[]){IO_MAP_13(IO_ARG_, __VA_ARGS__)}, 13
#define IO_FA_15(f, ...) f, (FormatArg[]){IO_MAP_14(IO_ARG_, __VA_ARGS__)}, 14
#define IO_FA_16(f, ...) f, (FormatArg[]){IO_MAP_15(IO_ARG_, __VA_ARGS__)}, 15
#define IO_FA_17(f, ...) f, (FormatArg[]){IO_MAP_16(IO_ARG_, __VA_ARGS__)}, 16

#define IO_FA_PICK_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13,    \
                    _14, _15, _16, _17, NAME, ...)                             \
  NAME
/// `io_format_args(fmt, args...)` expands to the last three parameters of
/// `io_format`: the format string, a temporary `FormatArg` array, its length.
/// Only valid directly inside a call to `io_format`
#define io_format_args(...)                                                    \
  IO_FA_PICK_(__VA_ARGS__, IO_FA_17, IO_FA_16, IO_FA_15, IO_FA_14, IO_FA_13,   \
              IO_FA_12, IO_FA_11, IO_FA_10, IO_FA_9, IO_FA_8, IO_FA_7,         \
              IO_FA_6, IO_FA_5, IO_FA_4, IO_FA_3, IO_FA_2, IO_FA_1, 0)         \
  (__VA_ARGS__)

/// Writes [fmt] to [w], substituting the [nargs] arguments. Normally reached
/// through `io_fprint`
Error io_format(Writer w, const char *fmt, const FormatArg *args,
                size_t nargs);

// IO_FW_<k>(w, fmt, args...): `w, fmt, array, count`
#define IO_FW_2(w, f) w, IO_FA_1(f)
#define IO_FW_3(w, ...) w, IO_FA_2(__VA_ARGS__)
#define IO_FW_4(w, ...) w, IO_FA_3(__VA_ARGS__)
#define IO_FW_5(w, ...) w, IO_FA_4(__VA_ARGS__)
#define IO_FW_6(w, ...) w, IO_FA_5(__VA_ARGS__)
#define IO_FW_7(w, ...) w, IO_FA_6(__VA_ARGS__)
#define IO_FW_8(w, ...) w, IO_FA_7(__VA_ARGS__)
#define IO_FW_9(w, ...) w, IO_FA_8(__VA_ARGS__)
#define IO_FW_10(w, ...) w, IO_FA_9(__VA_ARGS__)
#define IO_FW_11(w, ...) w, IO_FA_10(__VA_ARGS__)
#define IO_FW_12(w, ...) w, IO_FA_11(__VA_ARGS__)
#define IO_FW_13(w, ...) w, IO_FA_12(__VA_ARGS__)
#define IO_FW_14(w, ...) w, IO_FA_13(__VA_ARGS__)
#define IO_FW_15(w, ...) w, IO_FA_14(__VA_ARGS__)
#define IO_FW_16(w, ...) w, IO_FA_15(__VA_ARGS__)
#define IO_FW_17(w, ...) w, IO_FA_16(__VA_ARGS__)
#define IO_FW_18(w, ...) w, IO_FA_17(__VA_ARGS__)
#define IO_FW_PICK_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13,    \
                    _14, _15, _16, _17, _18, NAME, ...)                        \
  NAME
#define IO_FW_ARGS_(...)                                                       \
  IO_FW_PICK_(__VA_ARGS__, IO_FW_18, IO_FW_17, IO_FW_16, IO_FW_15, IO_FW_14,   \
              IO_FW_13, IO_FW_12, IO_FW_11, IO_FW_10, IO_FW_9, IO_FW_8,        \
              IO_FW_7, IO_FW_6, IO_FW_5, IO_FW_4, IO_FW_3, IO_FW_2,            \
              0)(__VA_ARGS__)
/// `io_fprint(w, "x={} y={:>4}", x, y)`: returns an `Error`. [w] is a `Writer`
#define io_fprint(...) io_format(IO_FW_ARGS_(__VA_ARGS__))
/// `io_fprint` to stdout
#define io_print(...) io_fprint(stdoutW, __VA_ARGS__)

/// like `io_format`, then writes a `\n`. If the format fails, nothing at all
/// is written
Error io_formatln(Writer w, const char *fmt, const FormatArg *args,
                  size_t nargs);
/// `io_fprint` followed by a newline: `io_fprintln(w, "x={}", x)`
#define io_fprintln(...) io_formatln(IO_FW_ARGS_(__VA_ARGS__))
/// `io_println("x={}", x)`: `io_fprintln` to stdout
#define io_println(...) io_fprintln(stdoutW, __VA_ARGS__)

#endif // MOLKIE_IO_H

#if defined(MOLKIE_IO_IMPL) && !defined(MOLKIE_IO_IMPL_DONE)
#define MOLKIE_IO_IMPL_DONE

Error io_open(FILE **out, const char *path, const char *mode) {
  if (!out)
    return enew(EINVAL, "out is null");
  *out = NULL;
  if (!path)
    return enew(EINVAL, "path is null");
  if (!mode)
    return enew(EINVAL, "mode is null");

  FILE *f;
  do {
    errno = 0;
    f = fopen(path, mode);
  } while (!f && errno == EINTR);
  if (!f) {
    if (!errno)
      errno = EIO;
    return eerrno("unable to open %s", path);
  }
  *out = f;
  return eok();
}

Error io_close(FILE *f) {
  if (!f)
    return enew(EINVAL, "file is null");
  // not retried on EINTR: the stream is gone after fclose, whatever it returns
  errno = 0;
  if (fclose(f) == 0)
    return eok();
  if (!errno)
    errno = EIO;
  return eerrno("unable to close file");
}

Error io_write(FILE *f, const char *buf, size_t bufsz, size_t *nWritten) {
  size_t ignored;
  if (!nWritten)
    nWritten = &ignored;
  *nWritten = 0;
  if (!f)
    return enew(EINVAL, "file is null");
  if (!buf && bufsz)
    return enew(EINVAL, "buf is null");

  while (*nWritten < bufsz) {
    errno = 0;
    *nWritten += fwrite(buf + *nWritten, 1, bufsz - *nWritten, f);
    if (*nWritten == bufsz)
      break;
    if (errno == EINTR && ferror(f)) {
      clearerr(f); // interrupted: retry the remainder
      continue;
    }
    if (!errno)
      errno = EIO;
    return eerrno("unable to write file");
  }
  return eok();
}

Error io_read(FILE *f, char *buf, size_t bufsz, size_t *nRead) {
  size_t ignored;
  if (!nRead)
    nRead = &ignored;
  *nRead = 0;
  if (!f)
    return enew(EINVAL, "file is null");
  if (!buf && bufsz)
    return enew(EINVAL, "buf is null");

  while (*nRead < bufsz) {
    errno = 0;
    *nRead += fread(buf + *nRead, 1, bufsz - *nRead, f);
    if (*nRead == bufsz || feof(f))
      break;
    if (ferror(f)) {
      if (errno == EINTR) {
        clearerr(f); // interrupted: retry the remainder
        continue;
      }
      if (!errno)
        errno = EIO;
      return eerrno("unable to read file");
    }
    errno = EIO; // short read with neither flag set should be impossible
    return eerrno("short read without error or EOF");
  }
  return eok();
}

Error io_flush(FILE *f) {
  if (!f)
    return enew(EINVAL, "file is null");
  errno = 0;
  if (fflush(f) == 0)
    return eok();
  if (!errno)
    errno = EIO;
  return eerrno("unable to flush file");
}

Error io_forward(FILE *dst, FILE *src) {
  Error eret = eok();
  char buf[BUFSIZ];
  size_t nRead, nWrote;
  do {
    // io_read only comes back short at EOF
    Error rerr = io_read(src, buf, sizeof buf, &nRead);
    // write what was read even if the read then failed, so no data is lost
    Error werr = io_write(dst, buf, nRead, &nWrote);
    if (!werr.ok) {
      efree(&rerr);
      etry(werr);
    }
    etry(rerr);
  } while (nRead == sizeof buf);
end:
  return eret;
}

// ---- Writer / Reader ------------------------------------------------------

#include <stdlib.h>

Error Wvprintf(Writer w, const char *fmt, va_list argp) {
  char stack[256];
  va_list copy;
  va_copy(copy, argp);
  int n = vsnprintf(stack, sizeof stack, fmt, copy);
  va_end(copy);
  if (n < 0)
    return enew(EINVAL, "unable to format \"%s\"", fmt);
  if ((size_t)n < sizeof stack)
    return Wwrite(w, stack, (size_t)n);
  char *heap = malloc((size_t)n + 1);
  if (!heap)
    return enew(ENOMEM, "out of memory formatting %d bytes", n);
  vsnprintf(heap, (size_t)n + 1, fmt, argp);
  Error e = Wwrite(w, heap, (size_t)n);
  free(heap);
  return e;
}

Error Wprintf(Writer w, const char *fmt, ...) {
  va_list argp;
  va_start(argp, fmt);
  Error e = Wvprintf(w, fmt, argp);
  va_end(argp);
  return e;
}

// Invariant: buf is always NUL-terminated and written < cap, so the contents
// are always a valid C string. Writes that don't fully fit are truncated and
// fail with ENOSPC.

Scratch Scratch_init(char *buf, size_t cap) {
  assert(buf && cap);
  buf[0] = 0;
  return (Scratch){buf, cap, 0};
}

void Scratch_reset(Scratch *s) {
  assert(s->buf && s->cap);
  s->written = 0;
  s->buf[0] = 0;
}

static Error _scratchWrite(void *p, const char *buf, size_t n) {
  Scratch *s = p;
  if (n && memchr(buf, 0, n)) // would end the C string early
    return enew(EINVAL, "cannot write a NUL byte to a scratch buffer");
  size_t avail = s->cap - s->written - 1; // reserve NUL
  size_t take = n <= avail ? n : avail;
  if (take)
    memcpy(s->buf + s->written, buf, take);
  s->written += take;
  s->buf[s->written] = 0;
  if (take < n)
    return enew(ENOSPC, "scratch buffer full (%zu bytes)", s->cap - 1);
  return eok();
}

Writer Scratch_writer(Scratch *s) { return (Writer){_scratchWrite, s}; }

Cursor Cursor_init(const char *buf, size_t len) {
  return (Cursor){buf, len, 0};
}

static Error _cursorRead(void *p, char *buf, size_t n, size_t *nRead) {
  Cursor *c = p;
  size_t left = c->len - c->pos;
  size_t take = n < left ? n : left;
  if (take)
    memcpy(buf, c->buf + c->pos, take);
  c->pos += take;
  *nRead = take;
  return eok();
}

Reader Cursor_reader(Cursor *c) { return (Reader){_cursorRead, c}; }

Error io_copy(Writer dst, Reader src) {
  Error eret = eok();
  char buf[BUFSIZ];
  size_t nRead;
  do {
    Error rerr = Rread(src, buf, sizeof buf, &nRead);
    // write what was read even if the read then failed, so no data is lost
    Error werr = Wwrite(dst, buf, nRead);
    if (!werr.ok) {
      efree(&rerr);
      etry(werr);
    }
    etry(rerr);
  } while (nRead == sizeof buf);
end:
  return eret;
}

Error eprint(const Error *e, Writer w) {
  if (e->ok)
    return eok();
  Error r = Wputs(w, "error: ");
  for (const ErrCtx *c = e->ctx; r.ok && c; c = c->next) {
    r = Wputs(w, c->msg);
    if (r.ok)
      r = Wputs(w, ": ");
  }
  if (r.ok)
    r = Wputs(w, e->msg);
  if (r.ok)
    r = Wputc(w, '\n');
  return r;
}

// ---- formatting -----------------------------------------------------------

#include <math.h>
#include <stdint.h>

#define IO_MAX_DIM_ 1000000 // widest width / precision accepted

// ---- padding ----

static size_t io_utf8Chars(const char *s, size_t n) {
  size_t chars = 0;
  for (size_t i = 0; i < n; i++)
    chars += ((unsigned char)s[i] & 0xC0) != 0x80;
  return chars;
}

static Error io_fill(Writer w, char c, size_t n) {
  char chunk[32];
  memset(chunk, c, sizeof chunk < n ? sizeof chunk : n);
  while (n) {
    size_t k = n < sizeof chunk ? n : sizeof chunk;
    Error e = Wwrite(w, chunk, k);
    if (!e.ok)
      return e;
    n -= k;
  }
  return eok();
}

// Writes `pfx`, `body`, padded to spec.width characters. With `zeroOk` and the
// `0` flag the padding is zeros between pfx and body instead. defAlign is the
// alignment when the spec has none: '<' for text, '>' for numbers
static Error io_emit(Writer w, FormatSpec s, const char *pfx, size_t pfxn,
                     const char *body, size_t bodyn, size_t bodyChars,
                     char defAlign, bool zeroOk) {
  size_t chars = pfxn + bodyChars;
  size_t pad = (size_t)s.width > chars ? (size_t)s.width - chars : 0;
  size_t before = 0, after = 0, zeros = 0;
  char fill = s.fill ? s.fill : ' ';
  if (zeroOk && s.zero) {
    zeros = pad;
  } else {
    char align = s.align ? s.align : defAlign;
    before = align == '>' ? pad : align == '^' ? pad / 2 : 0;
    after = pad - before;
  }
  Error e = io_fill(w, fill, before);
  if (e.ok && pfxn)
    e = Wwrite(w, pfx, pfxn);
  if (e.ok)
    e = io_fill(w, '0', zeros);
  if (e.ok && bodyn)
    e = Wwrite(w, body, bodyn);
  if (e.ok)
    e = io_fill(w, fill, after);
  return e;
}

static Error io_badType(const char *what, FormatSpec s) {
  return enew(EINVAL, "format type '%c' is not valid for %s", s.type, what);
}

// ---- integers ----

static Error io_fmtUint(Writer w, FormatSpec s, bool neg,
                        unsigned long long mag) {
  if (s.type == 'c') { // the integer is a code point, written as UTF-8
    if (neg || mag > 0x10FFFF)
      return enew(EINVAL, "{:c} needs a character code, got an integer out of range");
    char u[4];
    size_t k = 0;
    if (mag < 0x80) {
      u[k++] = (char)mag;
    } else if (mag < 0x800) {
      u[k++] = (char)(0xC0 | mag >> 6);
      u[k++] = (char)(0x80 | (mag & 0x3F));
    } else if (mag < 0x10000) {
      u[k++] = (char)(0xE0 | mag >> 12);
      u[k++] = (char)(0x80 | (mag >> 6 & 0x3F));
      u[k++] = (char)(0x80 | (mag & 0x3F));
    } else {
      u[k++] = (char)(0xF0 | mag >> 18);
      u[k++] = (char)(0x80 | (mag >> 12 & 0x3F));
      u[k++] = (char)(0x80 | (mag >> 6 & 0x3F));
      u[k++] = (char)(0x80 | (mag & 0x3F));
    }
    return io_emit(w, s, "", 0, u, k, 1, '<', false);
  }
  unsigned base;
  const char *digits = "0123456789abcdef";
  const char *prefix = "";
  switch (s.type) {
  case 0:
  case 'd': base = 10; break;
  case 'x': base = 16; prefix = "0x"; break;
  case 'X': base = 16; prefix = "0X"; digits = "0123456789ABCDEF"; break;
  case 'o': base = 8; prefix = "0o"; break;
  case 'b': base = 2; prefix = "0b"; break;
  default: return io_badType("integers", s);
  }
  char buf[66]; // 64 binary digits at most
  size_t i = sizeof buf;
  do {
    buf[--i] = digits[mag % base];
    mag /= base;
  } while (mag);

  char pfx[4];
  size_t pn = 0;
  if (neg)
    pfx[pn++] = '-';
  else if (s.sign == '+')
    pfx[pn++] = '+';
  if (s.alt)
    for (const char *p = prefix; *p; p++)
      pfx[pn++] = *p;
  return io_emit(w, s, pfx, pn, buf + i, sizeof buf - i, sizeof buf - i, '>',
                 true);
}

// signed decimal shows the sign; every other base shows the two's complement
// bits of the type, like unsigned. UT is the unsigned type of the same width
#define IO_SINT_FORMAT_(T, N, UT)                                              \
  Error N##_format(Writer w, const T *v, FormatSpec s) {                       \
    if (s.type == 0 || s.type == 'd') {                                        \
      bool neg = *v < 0;                                                       \
      unsigned long long mag = neg ? 0ULL - (unsigned long long)*v             \
                                   : (unsigned long long)*v;                   \
      return io_fmtUint(w, s, neg, mag);                                       \
    }                                                                          \
    return io_fmtUint(w, s, false, (unsigned long long)(UT) * v);              \
  }
#define IO_UINT_FORMAT_(T, N)                                                  \
  Error N##_format(Writer w, const T *v, FormatSpec s) {                       \
    return io_fmtUint(w, s, false, *v);                                        \
  }
IO_SINT_FORMAT_(signed char, schar, unsigned char)
IO_UINT_FORMAT_(unsigned char, uchar)
IO_SINT_FORMAT_(short, short, unsigned short)
IO_UINT_FORMAT_(unsigned short, ushort)
IO_SINT_FORMAT_(int, int, unsigned)
IO_UINT_FORMAT_(unsigned, uint)
IO_SINT_FORMAT_(long, long, unsigned long)
IO_UINT_FORMAT_(unsigned long, ulong)
IO_SINT_FORMAT_(long long, llong, unsigned long long)
IO_UINT_FORMAT_(unsigned long long, ullong)
#undef IO_SINT_FORMAT_
#undef IO_UINT_FORMAT_

Error char_format(Writer w, const char *v, FormatSpec s) {
  if (s.type == 0 || s.type == 'c')
    return io_emit(w, s, "", 0, v, 1, 1, '<', false);
  int i = (unsigned char)*v;
  return int_format(w, &i, s);
}

Error Bool_format(Writer w, const _Bool *v, FormatSpec s) {
  if (s.type != 0 && s.type != 's' && s.type != 'b')
    return io_badType("bool", s);
  const char *t = *v ? "true" : "false";
  return io_emit(w, s, "", 0, t, strlen(t), strlen(t), '<', false);
}

Error charptr_format(Writer w, const IoCharPtr *v, FormatSpec s) {
  if (s.type != 0 && s.type != 's')
    return io_badType("strings", s);
  const char *str = *v ? *v : "(null)";
  size_t n = strlen(str), chars;
  if (s.hasPrecision) { // keep at most `precision` characters
    size_t i = 0;
    chars = 0;
    while (i < n && chars < (size_t)s.precision) {
      i++;
      while (i < n && ((unsigned char)str[i] & 0xC0) == 0x80)
        i++;
      chars++;
    }
    n = i;
  } else {
    chars = io_utf8Chars(str, n);
  }
  return io_emit(w, s, "", 0, str, n, chars, '<', false);
}

Error voidptr_format(Writer w, const IoVoidPtr *v, FormatSpec s) {
  if (s.type == 0 || s.type == 'p') {
    s.type = 'x';
    s.alt = true;
  }
  return io_fmtUint(w, s, false, (unsigned long long)(uintptr_t)*v);
}

// ---- floats ----

// snprintf with a literal format for each conversion (keeps -Wformat happy)
static int io_snprintfFloat(char *buf, size_t n, char conv, bool alt, int prec,
                            long double v) {
  switch (conv) {
#define IO_FLT_CASE_(c, ch)                                                    \
  case ch:                                                                     \
    return alt ? snprintf(buf, n, "%#.*L" #c, prec, v)                         \
               : snprintf(buf, n, "%.*L" #c, prec, v);
    IO_FLT_CASE_(e, 'e') IO_FLT_CASE_(E, 'E') IO_FLT_CASE_(f, 'f')
    IO_FLT_CASE_(F, 'F') IO_FLT_CASE_(g, 'g') IO_FLT_CASE_(G, 'G')
#undef IO_FLT_CASE_
  }
  return -1;
}

// kind: 0 float, 1 double, 2 long double: how many digits round-trip
static Error io_fmtFloat(Writer w, FormatSpec s, long double v, int kind) {
  bool neg = signbit(v) != 0;
  if (neg)
    v = -v;
  char tmp[64], *text = tmp;
  char *heap = NULL;
  size_t n;
  bool finite = !isnan(v) && !isinf(v);

  if (!finite) {
    const char *t = isnan(v) ? "NaN" : "inf";
    n = strlen(t);
    memcpy(tmp, t, n);
    neg = neg && !isnan(v);
  } else {
    char conv = s.type;
    if (conv == 0 && s.hasPrecision)
      conv = 'f';
    if (conv != 0 && !strchr("eEfFgG", conv))
      return io_badType("floats", s);
    if (conv == 0) { // shortest digits that read back as the same value
      static const int maxDigits[] = {9, 17, 21};
      int p;
      for (p = 1; p < maxDigits[kind]; p++) {
        snprintf(tmp, sizeof tmp, "%.*Le", p - 1, v);
        if (kind == 0 ? strtof(tmp, NULL) == (float)v
            : kind == 1 ? strtod(tmp, NULL) == (double)v
                        : strtold(tmp, NULL) == v)
          break;
      }
      snprintf(tmp, sizeof tmp, "%.*Le", p - 1, v);
      int exp10 = atoi(strchr(tmp, 'e') + 1);
      // plain notation for ordinary magnitudes, `1.5e+20` for the extremes
      if (exp10 >= -5 && exp10 < 16)
        snprintf(tmp, sizeof tmp, "%.*Lf", p - 1 - exp10 > 0 ? p - 1 - exp10 : 0,
                 v);
      n = strlen(tmp);
    } else {
      int prec = s.hasPrecision ? s.precision : 6;
      int need = io_snprintfFloat(tmp, sizeof tmp, conv, s.alt, prec, v);
      if (need < 0)
        return enew(EINVAL, "unable to format a float");
      n = (size_t)need;
      if (n >= sizeof tmp) { // huge values with %f
        heap = malloc(n + 1);
        if (!heap)
          return enew(ENOMEM, "out of memory formatting a float");
        io_snprintfFloat(heap, n + 1, conv, s.alt, prec, v);
        text = heap;
      }
    }
  }
  char sign = neg ? '-' : s.sign == '+' ? '+' : 0;
  Error e = io_emit(w, s, &sign, sign ? 1 : 0, text, n, n, '>', finite);
  free(heap);
  return e;
}

Error float_format(Writer w, const float *v, FormatSpec s) {
  return io_fmtFloat(w, s, *v, 0);
}
Error double_format(Writer w, const double *v, FormatSpec s) {
  return io_fmtFloat(w, s, *v, 1);
}
Error ldouble_format(Writer w, const long double *v, FormatSpec s) {
  return io_fmtFloat(w, s, *v, 2);
}

// ---- format strings ----

typedef struct {
  const char *lit; // a run of literal text, or NULL
  size_t litn;
  bool hasArg; // a placeholder (spec is filled in)
  FormatSpec spec;
} IoTok;

static Error io_parseSpec(const char *fmt, const char *s, size_t n,
                          FormatSpec *out) {
  FormatSpec r = {.fill = ' '};
  size_t i = 0;
  if (n >= 2 && strchr("<^>", s[1])) {
    r.fill = s[0];
    r.align = s[1];
    i = 2;
  } else if (n >= 1 && strchr("<^>", s[0])) {
    r.align = s[0];
    i = 1;
  }
  if (i < n && s[i] == '+')
    r.sign = '+', i++;
  if (i < n && s[i] == '#')
    r.alt = true, i++;
  if (i < n && s[i] == '0')
    r.zero = true, i++;
  if (i < n && s[i] >= '0' && s[i] <= '9') {
    long v = 0;
    for (; i < n && s[i] >= '0' && s[i] <= '9' && v <= IO_MAX_DIM_; i++)
      v = v * 10 + (s[i] - '0');
    if (v > IO_MAX_DIM_)
      return enew(EINVAL, "format width too large in \"%.60s\"", fmt);
    r.width = (int)v;
  }
  if (i < n && s[i] == '.') {
    i++;
    if (i >= n || s[i] < '0' || s[i] > '9')
      return enew(EINVAL, "format precision has no digits in \"%.60s\"", fmt);
    long v = 0;
    for (; i < n && s[i] >= '0' && s[i] <= '9' && v <= IO_MAX_DIM_; i++)
      v = v * 10 + (s[i] - '0');
    if (v > IO_MAX_DIM_)
      return enew(EINVAL, "format precision too large in \"%.60s\"", fmt);
    r.hasPrecision = true;
    r.precision = (int)v;
  }
  if (i < n)
    r.type = s[i++];
  if (i < n || (r.type && !((r.type >= 'a' && r.type <= 'z') ||
                            (r.type >= 'A' && r.type <= 'Z') || r.type == '?')))
    return enew(EINVAL, "invalid format spec \"%.*s\" in \"%.60s\"", (int)n, s,
                fmt);
  *out = r;
  return eok();
}

// Reads the next literal run or placeholder at *pp and advances. A token with
// no lit and no arg is the end
static Error io_nextTok(const char *fmt, const char **pp, IoTok *t) {
  const char *p = *pp;
  *t = (IoTok){0};
  if (!*p)
    return eok();
  if (*p == '}') {
    if (p[1] != '}')
      return enew(EINVAL, "unmatched '}' in \"%.60s\"", fmt);
    t->lit = p, t->litn = 1;
    *pp = p + 2;
    return eok();
  }
  if (*p != '{') {
    t->lit = p;
    while (*p && *p != '{' && *p != '}')
      p++;
    t->litn = (size_t)(p - t->lit);
    *pp = p;
    return eok();
  }
  if (p[1] == '{') {
    t->lit = p, t->litn = 1;
    *pp = p + 2;
    return eok();
  }
  const char *close = strchr(p, '}');
  if (!close)
    return enew(EINVAL, "unmatched '{' in \"%.60s\"", fmt);
  const char *inner = p + 1;
  size_t n = (size_t)(close - inner);
  if (n && *inner != ':')
    return enew(EINVAL, "expected '}' or ':' after '{' in \"%.60s\"", fmt);
  t->hasArg = true;
  *pp = close + 1;
  if (!n)
    return io_parseSpec(fmt, "", 0, &t->spec);
  return io_parseSpec(fmt, inner + 1, n - 1, &t->spec);
}

Error io_format(Writer w, const char *fmt, const FormatArg *args,
                size_t nargs) {
  if (!fmt)
    return enew(EINVAL, "format string is null");
  IoTok t;
  const char *p = fmt;
  size_t count = 0;
  // check everything first, so a bad format writes nothing
  for (;;) {
    Error e = io_nextTok(fmt, &p, &t);
    if (!e.ok)
      return e;
    if (!t.lit && !t.hasArg)
      break;
    count += t.hasArg;
  }
  if (count != nargs)
    return enew(EINVAL, "\"%.60s\" has %zu placeholders but %zu arguments",
                fmt, count, nargs);

  p = fmt;
  for (size_t next = 0;;) {
    Error e = io_nextTok(fmt, &p, &t);
    if (!e.ok)
      return e;
    if (t.lit) {
      e = Wwrite(w, t.lit, t.litn);
      if (!e.ok)
        return e;
    } else if (t.hasArg) {
      const FormatArg *a = &args[next++];
      const void *v = a->byptr ? a->ptr
                      : a->isLongDouble ? (const void *)&a->ld
                                        : (const void *)&a->val;
      e = a->fn(w, v, t.spec);
      if (!e.ok)
        return e;
    } else {
      return eok();
    }
  }
}

Error io_formatln(Writer w, const char *fmt, const FormatArg *args,
                  size_t nargs) {
  Error e = io_format(w, fmt, args, nargs);
  if (!e.ok)
    return e;
  return Wputc(w, '\n');
}

#endif // MOLKIE_IO_IMPL

// Tests: compiled where MTEST (all modules) or MOLKIE_IO_TEST (just this one)
// is defined. Needs MOLKIE_IO_IMPL and
// MOLKIE_ERR_IMPL in the same program.
#if (defined(MTEST) || defined(MOLKIE_IO_TEST)) && !defined(MOLKIE_IO_TESTS)
#define MOLKIE_IO_TESTS
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"

static void io_test_report(Error *e) { efprint(e, stdout); }

// A test body returns an Error (so it can use etry, which needs `eret` and an
// `end:` label); the wrapper fails the test and prints the error chain if the
// body failed.
#define IO_TEST(name)                                                          \
  static Error name##_body(void);                                              \
  static void name(void) {                                                     \
    Error e = name##_body();                                                   \
    if (!e.ok) {                                                               \
      io_test_report(&e);                                                      \
      fflush(stdout);                                                          \
      assert(0);                                                               \
    }                                                                          \
  }                                                                            \
  static Error name##_body(void)

// ok only if [e] failed with [code]; frees [e]
static Error expect_fail(Error e, int code) {
  if (e.ok)
    return enew(EINVAL, "expected errno %d, got success", code);
  if (e.code != code) {
    Error r = enew(EINVAL, "expected errno %d, got %d (%s)", code, e.code,
                   e.msg);
    efree(&e);
    return r;
  }
  efree(&e);
  return eok();
}

// ok only if [cond]
#define expect(cond)                                                           \
  do {                                                                         \
    if (!(cond)) {                                                             \
      eret = enew(EINVAL, "expected: %s", #cond);                              \
      goto end;                                                                \
    }                                                                          \
  } while (0)

// forwards [n] bytes of a repeating pattern through a tmpfile and checks them
static Error io_roundtrip(size_t n) {
  Error eret = eok();
  char *in = malloc(n + 1), *out = malloc(n + 1);
  FILE *src = tmpfile(), *dst = tmpfile();
  expect(in && out && src && dst);
  for (size_t i = 0; i < n; i++)
    in[i] = (char)('a' + i % 26);
  expect(fwrite(in, 1, n, src) == n);
  rewind(src);

  etry(io_forward(dst, src));
  expect(feof(src));

  etry(io_flush(dst));
  rewind(dst);
  expect(fread(out, 1, n + 1, dst) == n); // exactly n bytes, no more
  expect(!memcmp(in, out, n));

  fclose(src);
  fclose(dst);
  free(in);
  free(out);
end:
  return eret;
}

IO_TEST(io_empty) { return io_roundtrip(0); }
IO_TEST(io_small) { return io_roundtrip(10); }
IO_TEST(io_exact_buf) { return io_roundtrip(BUFSIZ); }
IO_TEST(io_multi_chunk) { return io_roundtrip(BUFSIZ * 3 + 17); }

IO_TEST(io_forward_null_args) {
  Error eret = eok();
  FILE *f = tmpfile();
  expect(f);
  etry(expect_fail(io_forward(NULL, f), EINVAL));
  etry(expect_fail(io_forward(f, NULL), EINVAL));
  fclose(f);
end:
  return eret;
}

IO_TEST(io_forward_read_error) {
  Error eret = eok();
  // a write-only stream can't be read
  FILE *wo = fopen("/dev/null", "w"), *dst = tmpfile();
  expect(wo && dst);
  etry(expect_fail(io_forward(dst, wo), EBADF));
  fclose(wo);
  fclose(dst);
end:
  return eret;
}

IO_TEST(io_forward_write_error) {
  Error eret = eok();
  // a read-only dst can't be written
  FILE *src = tmpfile(), *dst = fopen("/dev/null", "r");
  expect(src && dst);
  fputs("hello", src);
  rewind(src);
  etry(expect_fail(io_forward(dst, src), EBADF));
  fclose(src);
  fclose(dst);
end:
  return eret;
}

IO_TEST(io_forward_error_has_location) {
  Error eret = eok();
  Error e;
  FILE *f = tmpfile();
  expect(f);
  // etry should have added file:line context to the failure
  e = io_forward(f, NULL);
  expect(!e.ok && e.ctx);
  efree(&e);
  fclose(f);
end:
  return eret;
}

IO_TEST(io_write_ok) {
  Error eret = eok();
  size_t n = 99;
  char b[7] = {0};
  FILE *f = tmpfile();
  expect(f);
  errno = EBADF; // a stale errno must not leak into a successful write
  etry(io_write(f, "abc", 3, &n));
  expect(n == 3);
  etry(io_write(f, NULL, 0, &n));
  expect(n == 0);
  etry(io_write(f, "def", 3, NULL)); // nWritten is optional
  etry(io_flush(f));
  rewind(f);
  expect(fread(b, 1, 6, f) == 6 && !strcmp(b, "abcdef"));
  fclose(f);
end:
  return eret;
}

IO_TEST(io_write_errors) {
  Error eret = eok();
  size_t n = 99;
  FILE *f = tmpfile();
  FILE *ro = fopen("/dev/null", "r");
  expect(f && ro);
  etry(expect_fail(io_write(NULL, "x", 1, &n), EINVAL));
  expect(n == 0);
  n = 99;
  etry(expect_fail(io_write(f, NULL, 1, &n), EINVAL));
  expect(n == 0);
  fclose(f);
  n = 99;
  etry(expect_fail(io_write(ro, "x", 1, &n), EBADF));
  expect(n == 0);
  fclose(ro);
end:
  return eret;
}

IO_TEST(io_write_full_device) {
  Error eret = eok();
  size_t n = 99;
  FILE *f = fopen("/dev/full", "w");
  expect(f);
  setvbuf(f, NULL, _IONBF, 0); // fail at write time, not at flush
  etry(expect_fail(io_write(f, "x", 1, &n), ENOSPC));
  expect(n == 0);
  fclose(f);
end:
  return eret;
}

IO_TEST(io_read_ok) {
  Error eret = eok();
  char b[16];
  size_t n = 99;
  FILE *f = tmpfile();
  expect(f);
  fputs("hello", f);
  rewind(f);
  errno = EBADF; // stale
  etry(io_read(f, b, 3, &n));
  expect(n == 3 && !memcmp(b, "hel", 3));
  // short read at EOF is ok, with the real count
  etry(io_read(f, b, sizeof b, &n));
  expect(n == 2 && !memcmp(b, "lo", 2));
  // already at EOF: ok, zero bytes
  etry(io_read(f, b, sizeof b, &n));
  expect(n == 0);
  etry(io_read(f, b, 0, NULL)); // nRead is optional
  fclose(f);
end:
  return eret;
}

IO_TEST(io_read_errors) {
  Error eret = eok();
  char b[4];
  size_t n = 99;
  FILE *f = tmpfile();
  FILE *wo = fopen("/dev/null", "w");
  expect(f && wo);
  etry(expect_fail(io_read(NULL, b, sizeof b, &n), EINVAL));
  expect(n == 0);
  n = 99;
  etry(expect_fail(io_read(f, NULL, 1, &n), EINVAL));
  expect(n == 0);
  fclose(f);
  n = 99;
  etry(expect_fail(io_read(wo, b, sizeof b, &n), EBADF));
  expect(n == 0);
  fclose(wo);
end:
  return eret;
}

IO_TEST(io_flush_cases) {
  Error eret = eok();
  FILE *f = tmpfile();
  FILE *full = fopen("/dev/full", "w");
  expect(f && full);
  etry(expect_fail(io_flush(NULL), EINVAL));
  errno = EBADF; // stale
  etry(io_flush(f));
  fclose(f);
  // a buffered write to a full device only fails at flush
  etry(io_write(full, "x", 1, NULL));
  etry(expect_fail(io_flush(full), ENOSPC));
  fclose(full);
end:
  return eret;
}

IO_TEST(io_open_close_roundtrip) {
  Error eret = eok();
  char path[] = "/tmp/io_test_XXXXXX";
  FILE *f = (FILE *)1;
  char b[8] = {0};
  size_t n;
  int fd = mkstemp(path);
  expect(fd >= 0);
  close(fd);

  etry(io_open(&f, path, "w"));
  expect(f);
  etry(io_write(f, "hello", 5, NULL));
  etry(io_close(f));

  etry(io_open(&f, path, "r"));
  etry(io_read(f, b, sizeof b, &n));
  etry(io_close(f));
  remove(path);
  expect(n == 5 && !strcmp(b, "hello"));
end:
  return eret;
}

IO_TEST(io_open_errors) {
  Error eret = eok();
  FILE *f = (FILE *)1;
  etry(expect_fail(io_open(&f, "/nonexistent/dir/file", "r"), ENOENT));
  expect(f == NULL);
  f = (FILE *)1;
  etry(expect_fail(io_open(&f, "/dev/null", "bogus"), EINVAL));
  expect(f == NULL);
  etry(expect_fail(io_open(&f, NULL, "r"), EINVAL));
  etry(expect_fail(io_open(&f, "/dev/null", NULL), EINVAL));
  etry(expect_fail(io_open(NULL, "/dev/null", "r"), EINVAL));
end:
  return eret;
}

IO_TEST(io_open_error_names_path) {
  Error eret = eok();
  FILE *f;
  Error e = io_open(&f, "/nonexistent/x", "r");
  expect(!e.ok && strstr(e.msg, "/nonexistent/x"));
  efree(&e);
end:
  return eret;
}

IO_TEST(io_close_cases) {
  Error eret = eok();
  etry(expect_fail(io_close(NULL), EINVAL));
  // buffered data that can't be written is reported at close
  FILE *full;
  etry(io_open(&full, "/dev/full", "w"));
  etry(io_write(full, "x", 1, NULL));
  etry(expect_fail(io_close(full), ENOSPC));
end:
  return eret;
}

// ---- Writer / Reader ----

static void io_writer_scratch(void) {
  char buf[16];
  Scratch s = Scratch_init(buf, sizeof buf);
  Writer w = Scratch_writer(&s);

  assert(Wprintf(w, "a=%d", 1).ok);
  assert(Wputc(w, ',').ok);
  assert(Wputs(w, "b").ok);
  assert(Wprintf(w, "%s", "").ok); // writing nothing is success
  assert(!strcmp(buf, "a=1,b"));

  Scratch_reset(&s);
  assert(!strcmp(buf, ""));
}

static void io_writer_scratch_full(void) {
  char buf[8];
  Scratch s = Scratch_init(buf, sizeof buf);
  Writer w = Scratch_writer(&s);

  Error e = Wprintf(w, "%s", "0123456789"); // truncated
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOSPC);
  assert(!strcmp(buf, "0123456")); // still a valid C string

  e = Wputc(w, 'x');
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOSPC);

  e = Wputs(w, "xy");
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOSPC);
  assert(strstr(e.msg, "scratch buffer full"));
  assert(!strcmp(buf, "0123456"));
}

static void io_writer_scratch_nul(void) {
  char buf[16];
  Scratch s = Scratch_init(buf, sizeof buf);
  Writer w = Scratch_writer(&s);
  assert(Wputc(w, 'a').ok);
  Error e = Wputc(w, 0);
  assert(!e.ok);
  ASSERT_EQ(e.code, EINVAL);
  assert(Wputs(w, "bc").ok);
  assert(!strcmp(buf, "abc"));
  ASSERT_EQ(s.written, strlen(buf));
}

static void io_writer_long_printf(void) {
  // longer than the stack buffer Wvprintf starts with
  char big[600];
  memset(big, 'z', sizeof big - 1);
  big[sizeof big - 1] = 0;
  char out[700];
  Scratch s = Scratch_init(out, sizeof out);
  assert(Wprintf(Scratch_writer(&s), "<%s>", big).ok);
  ASSERT_EQ(s.written, sizeof big + 1);
  assert(out[0] == '<' && out[sizeof big] == '>');
}

static void io_writer_file(void) {
  FILE *f = tmpfile();
  assert(f);
  Writer w = FILE_writer(f);
  assert(Wprintf(w, "n=%d", 5).ok);
  assert(Wputc(w, '!').ok);
  assert(Wputs(w, "?").ok);
  rewind(f);
  char line[32] = {0};
  assert(fgets(line, sizeof line, f));
  assert(!strcmp(line, "n=5!?"));
  fclose(f);
}

static void io_writer_file_error(void) {
  FILE *f = fopen("/dev/full", "w"); // every write fails with ENOSPC
  assert(f);
  setvbuf(f, NULL, _IONBF, 0);
  Writer w = FILE_writer(f);

  Error e = Wprintf(w, "x");
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOSPC);
  assert(strstr(e.msg, strerror(ENOSPC)));
  assert(!Wputc(w, 'x').ok);
  assert(!Wputs(w, "x").ok);
  fclose(f);
}

static void io_writer_std(void) {
  // stdoutW / stderrW need no setup; an empty write is a no-op
  assert(Wputs(stdoutW, "").ok);
  assert(Wputs(stderrW, "").ok);
}

static void io_writer_eprint(void) {
  char buf[64];
  Scratch s = Scratch_init(buf, sizeof buf);
  Writer w = Scratch_writer(&s);

  Error ok = eok();
  assert(eprint(&ok, w).ok);
  assert(!strcmp(buf, "")); // nothing for success

  Error e = enew(1, "boom");
  assert(eprint(&e, w).ok);
  assert(!strcmp(buf, "error: boom\n"));
}

static void io_writer_eprint_ctx(void) {
  char buf[128];
  Scratch s = Scratch_init(buf, sizeof buf);
  Writer w = Scratch_writer(&s);

  Error e = enew(2, "boom");
  ectx(&e, "reading %s", "a.txt");
  ectx(&e, "step %d", 2);
  assert(eprint(&e, w).ok);
  assert(!strcmp(buf, "error: step 2: reading a.txt: boom\n"));
  efree(&e);
}

static void io_reader_cursor(void) {
  Cursor c = Cursor_init("hello", 5);
  Reader r = Cursor_reader(&c);
  char b[8];
  size_t n = 99;
  assert(Rread(r, b, 3, &n).ok);
  ASSERT_EQ(n, 3);
  assert(!memcmp(b, "hel", 3));
  int ch;
  assert(Rgetc(r, &ch).ok);
  ASSERT_EQ(ch, 'l');
  assert(Rread(r, b, sizeof b, &n).ok); // short read: end of input
  ASSERT_EQ(n, 1);
  assert(Rgetc(r, &ch).ok);
  assert(ch == EOF);
  assert(Rread(r, b, 0, NULL).ok);
}

static void io_reader_file(void) {
  FILE *f = tmpfile();
  assert(f);
  fputs("xyz", f);
  rewind(f);
  Reader r = FILE_reader(f);
  char b[8];
  size_t n;
  assert(Rread(r, b, sizeof b, &n).ok);
  ASSERT_EQ(n, 3);
  assert(!memcmp(b, "xyz", 3));
  fclose(f);

  FILE *wo = fopen("/dev/null", "w");
  assert(wo);
  Error e = Rread(FILE_reader(wo), b, sizeof b, &n);
  assert(!e.ok);
  ASSERT_EQ(e.code, EBADF);
  fclose(wo);
}

static void io_copy_cases(void) {
  size_t sizes[] = {0, 5, BUFSIZ, BUFSIZ * 2 + 3};
  for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
    size_t len = sizes[k];
    char *in = malloc(len + 1), *out = malloc(len + 1);
    assert(in && out);
    for (size_t i = 0; i < len; i++)
      in[i] = (char)('a' + i % 26);
    Cursor c = Cursor_init(in, len);
    FILE *f = tmpfile();
    assert(f);
    assert(io_copy(FILE_writer(f), Cursor_reader(&c)).ok);
    rewind(f);
    ASSERT_EQ(fread(out, 1, len + 1, f), len);
    assert(!memcmp(in, out, len));
    fclose(f);
    free(in);
    free(out);
  }
  char tiny[4];
  Scratch s = Scratch_init(tiny, sizeof tiny);
  Cursor c = Cursor_init("abcdef", 6);
  Error e = io_copy(Scratch_writer(&s), Cursor_reader(&c));
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOSPC);
  efree(&e);
}

// ---- formatting ----

// formats into a scratch buffer and compares; every test case is one line
#define FMT_IS(want, ...)                                                      \
  do {                                                                         \
    char b_[256];                                                              \
    Scratch s_ = Scratch_init(b_, sizeof b_);                                  \
    Error e_ = io_fprint(Scratch_writer(&s_), __VA_ARGS__);                    \
    if (!e_.ok || strcmp(b_, want)) {                                          \
      fprintf(stderr, "format %s: want \"%s\" got \"%s\" (%s)\n", #__VA_ARGS__,\
              want, b_, e_.ok ? "ok" : e_.msg);                                \
      assert(0);                                                               \
    }                                                                          \
  } while (0)

// the format must fail with EINVAL and write nothing
#define FMT_BAD(...)                                                           \
  do {                                                                         \
    char b_[64];                                                               \
    Scratch s_ = Scratch_init(b_, sizeof b_);                                  \
    Error e_ = io_fprint(Scratch_writer(&s_), __VA_ARGS__);                    \
    assert(!e_.ok && e_.code == EINVAL && !b_[0]);                             \
  } while (0)

static void io_fmt_text(void) {
  FMT_IS("", "");
  FMT_IS("hello", "hello");
  FMT_IS("{}", "{{}}");
  FMT_IS("a{b}c", "a{{b}}c");
  FMT_IS("100%", "{}%", 100);
}

static void io_fmt_ints(void) {
  FMT_IS("42", "{}", 42);
  FMT_IS("-7", "{}", -7);
  FMT_IS("0", "{}", 0);
  FMT_IS("-2147483648", "{}", INT_MIN);
  FMT_IS("18446744073709551615", "{}", ULLONG_MAX);
  FMT_IS("-9223372036854775808", "{}", LLONG_MIN);
  FMT_IS("   42", "{:5}", 42);
  FMT_IS("42   ", "{:<5}", 42);
  FMT_IS(" 42  ", "{:^5}", 42);
  FMT_IS("**42**", "{:*^6}", 42);
  FMT_IS("00042", "{:05}", 42);
  FMT_IS("-0042", "{:05}", -42);
  FMT_IS("+42", "{:+}", 42);
  FMT_IS("ff", "{:x}", 255);
  FMT_IS("FF", "{:X}", 255);
  FMT_IS("0xff", "{:#x}", 255);
  FMT_IS("0x00ff", "{:#06x}", 255);
  FMT_IS("377", "{:o}", 255);
  FMT_IS("0b101", "{:#b}", 5);
  FMT_IS("ffffffff", "{:x}", -1); // two's complement at the type's width
  FMT_IS("ff", "{:x}", (signed char)-1);
  FMT_IS("65535", "{}", (unsigned short)65535);
  FMT_IS("-5", "{}", (short)-5);
  FMT_IS("12345678901", "{}", 12345678901L);
  FMT_IS("7", "{}", (size_t)7);
  FMT_IS("1 2 3", "{} {} {}", 1u, 2ul, 3ull);
  FMT_IS("1111111111111111111111111111111111111111111111111111111111111111",
         "{:b}", ULLONG_MAX);
}

static void io_fmt_text_args(void) {
  FMT_IS("hi", "{}", "hi");
  FMT_IS("  hi", "{:>4}", "hi");
  FMT_IS("hi  ", "{:4}", "hi"); // text is left-aligned by default
  FMT_IS("hi", "{:.2}", "hi!!");
  FMT_IS("--hi--", "{:-^6}", "hi");
  FMT_IS("(null)", "{}", (const char *)NULL);
  FMT_BAD("{:?}", "hi"); // `?` parses, but char * has no debug form
  FMT_BAD("{:>12?}", "hi");
  FMT_IS("héllo", "{}", "héllo");
  FMT_IS("  hé", "{:>4}", "hé"); // width counts characters, not bytes
  FMT_IS("hé", "{:.2}", "héllo");
  char buf[8] = "arr";
  FMT_IS("arr", "{}", buf);   // char arrays and
  char *mut = buf;
  FMT_IS("arr", "{}", mut);   // non-const pointers
  FMT_IS("true false", "{} {}", (_Bool)1, (_Bool)0);
  FMT_IS(" true", "{:>5}", (_Bool)1);
  FMT_IS("false", "{:b}", (_Bool)0);
  char c = 'x';
  FMT_IS("x", "{}", c);
  FMT_IS("120", "{:d}", c);
  FMT_IS("  x", "{:>3}", c);
  FMT_IS("x", "{:c}", 'x'); // a char literal is an int: {:c} prints it as text
  FMT_IS("é!", "{:c}{:c}", 0xE9, 33);
  FMT_BAD("{:c}", -1);
}

static void io_fmt_floats(void) {
  FMT_IS("1.5", "{}", 1.5);
  FMT_IS("0.1", "{}", 0.1);
  FMT_IS("100", "{}", 100.0);
  FMT_IS("0.30000000000000004", "{}", 0.1 + 0.2);
  FMT_IS("0.1", "{}", 0.1f);
  FMT_IS("-2.25", "{}", -2.25);
  FMT_IS("3.14", "{:.2}", 3.14159);
  FMT_IS("3", "{:.0}", 3.14159);
  FMT_IS("   3.142", "{:8.3}", 3.14159);
  FMT_IS("0003.142", "{:08.3}", 3.14159);
  FMT_IS("-003.142", "{:08.3}", -3.14159);
  FMT_IS("+1.5", "{:+}", 1.5);
  FMT_IS("1.500000e+00", "{:e}", 1.5);
  FMT_IS("1.50E+00", "{:.2E}", 1.5);
  FMT_IS("0.5", "{:g}", 0.5L);
  FMT_IS("inf", "{}", (double)INFINITY);
  FMT_IS("-inf", "{}", -(double)INFINITY);
  FMT_IS("NaN", "{}", (double)NAN);
  FMT_IS("  NaN", "{:05}", (double)NAN); // no zero padding for non-numbers
  FMT_IS("-0", "{}", -0.0);
}

static void io_fmt_pointers(void) {
  FMT_IS("0x0", "{}", (void *)0);
  FMT_IS("0x10", "{}", (const void *)(uintptr_t)16);
  int x;
  char want[32];
  snprintf(want, sizeof want, "%p", (void *)&x); // glibc: 0x7ffd...
  FMT_IS(want, "{}", (void *)&x);
}

static void io_fmt_many_args(void) {
  FMT_IS("1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16",
         "{} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {}", 1, 2, 3, 4, 5, 6, 7,
         8, 9, 10, 11, 12, 13, 14, 15, 16);
  FMT_IS("x=1, y=a, z=2.5", "x={}, y={}, z={}", 1, "a", 2.5);
}

static int io_fmt_calls;
static int io_fmt_once(void) { return ++io_fmt_calls; }

static void io_fmt_arg_evaluated_once(void) {
  FMT_IS("1", "{}", io_fmt_once());
  ASSERT_EQ(io_fmt_calls, 1);
}

static void io_fmt_errors(void) {
  FMT_BAD("{}");            // not enough arguments
  FMT_BAD("{} {}", 1);
  FMT_BAD("x", 1);          // too many
  FMT_BAD("{}", 1, 2);
  FMT_BAD("{");
  FMT_BAD("a {} {", 1);
  FMT_BAD("}");
  FMT_BAD("{}}", 1);
  FMT_BAD("{x}", 1);        // no named arguments
  FMT_BAD("{:q}", 1);       // bad type for an integer
  FMT_BAD("{:x}", 1.5);     // or a float
  FMT_BAD("{:x}", "s");     // or a string
  FMT_BAD("{:>}", 1, 2);    // (also a count mismatch)
  FMT_BAD("{:5.}", 1.0);    // precision with no digits
  FMT_BAD("{:99999999}", 1);
  FMT_BAD("{:5xx}", 1);
  FMT_BAD("{:?}", 1);

  char b[64];
  Scratch s = Scratch_init(b, sizeof b);
  Error e = io_format(Scratch_writer(&s), NULL, NULL, 0);
  assert(!e.ok && e.code == EINVAL);
}

static void io_fmt_writer_errors(void) {
  char tiny[4];
  Scratch s = Scratch_init(tiny, sizeof tiny);
  Error e = io_fprint(Scratch_writer(&s), "value: {}", 12345);
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOSPC);
  assert(!strcmp(tiny, "val"));

  FILE *f = fopen("/dev/full", "w");
  assert(f);
  setvbuf(f, NULL, _IONBF, 0);
  e = io_fprint(FILE_writer(f), "{}", 1);
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOSPC);
  fclose(f);
}

static void io_fmt_file(void) {
  FILE *f = tmpfile();
  assert(f);
  assert(io_fprint(FILE_writer(f), "{}-{:>3}", "a", 7).ok);
  rewind(f);
  char line[16] = {0};
  assert(fgets(line, sizeof line, f));
  assert(!strcmp(line, "a-  7"));
  fclose(f);
  assert(io_print("").ok);
}

static void io_fmt_println(void) {
  char b[64];
  Scratch s = Scratch_init(b, sizeof b);
  Writer w = Scratch_writer(&s);
  assert(io_fprintln(w, "x={} y={:>3}", 1, "a").ok);
  assert(io_fprintln(w, "plain").ok);
  assert(io_fprintln(w, "").ok);
  assert(!strcmp(b, "x=1 y=  a\nplain\n\n"));
  // a bad format writes nothing, not even the newline
  Scratch_reset(&s);
  Error e = io_fprintln(w, "{}");
  assert(!e.ok && e.code == EINVAL && !b[0]);
  // the writer failing on the newline is reported too
  char tiny[4];
  Scratch t = Scratch_init(tiny, sizeof tiny);
  e = io_fprintln(Scratch_writer(&t), "abc");
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOSPC);
  assert(io_println("{} {}", "to", "stdout").ok);
}

static void io_fmt_direct_spec(void) {
  // a zeroed FormatSpec is the plain `{}`
  char b[16];
  Scratch s = Scratch_init(b, sizeof b);
  int v = 12;
  assert(int_format(Scratch_writer(&s), &v, (FormatSpec){0}).ok);
  const char *str = "abc";
  assert(charptr_format(Scratch_writer(&s), &str, (FormatSpec){0}).ok);
  assert(!strcmp(b, "12abc"));
}

IO_TEST(io_defer_close) {
  Error eret = eok();
  EDEFERS(2);
  char path[] = "/tmp/io_defer_XXXXXX";
  FILE *f;
  char *buf;
  int fd = mkstemp(path);
  expect(fd >= 0);
  close(fd);
  etry(io_open(&f, path, "w"));
  edefer(io_close, f);
  etry(io_write(f, "hi", 2, NULL));
  buf = malloc(8);
  expect(buf);
  edefer_void(free, buf);
  etry(io_write(f, "!", 1, NULL));
end:
  erundefers(); // closes (flushes) f, frees buf
  if (eret.ok) {
    char b[8] = {0};
    FILE *r = fopen(path, "r");
    expect(r && fread(b, 1, 7, r) == 3 && !strcmp(b, "hi!"));
    fclose(r);
  }
  remove(path);
  return eret;
}

static int io_tests(void) {
  static const TestCase tests[] = {
      T(io_empty),
      T(io_small),
      T(io_exact_buf),
      T(io_multi_chunk),
      T(io_forward_null_args),
      T(io_forward_read_error),
      T(io_forward_write_error),
      T(io_forward_error_has_location),
      T(io_write_ok),
      T(io_write_errors),
      T(io_write_full_device),
      T(io_read_ok),
      T(io_read_errors),
      T(io_flush_cases),
      T(io_open_close_roundtrip),
      T(io_open_errors),
      T(io_open_error_names_path),
      T(io_close_cases),
      T(io_defer_close),
      T(io_writer_scratch),
      T(io_writer_scratch_full),
      T(io_writer_scratch_nul),
      T(io_writer_long_printf),
      T(io_writer_file),
      T(io_writer_file_error),
      T(io_writer_std),
      T(io_writer_eprint),
      T(io_writer_eprint_ctx),
      T(io_reader_cursor),
      T(io_reader_file),
      T(io_copy_cases),
      T(io_fmt_text),
      T(io_fmt_ints),
      T(io_fmt_text_args),
      T(io_fmt_floats),
      T(io_fmt_pointers),
      T(io_fmt_many_args),
      T(io_fmt_arg_evaluated_once),
      T(io_fmt_errors),
      T(io_fmt_writer_errors),
      T(io_fmt_file),
      T(io_fmt_println),
      T(io_fmt_direct_spec),
  };
  size_t n = sizeof tests / sizeof *tests, passed = 0;
  for (size_t i = 0; i < n; i++)
    passed += run_in_child(&tests[i]);
  printf("io: %zu/%zu passed\n", passed, n);
  return passed == n;
}

#endif // MTEST
