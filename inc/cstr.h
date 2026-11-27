#ifndef MOLKIE_CSTR_H
#define MOLKIE_CSTR_H

#include "attr.h"
#include "err.h"
#include "io.h"
#include <stddef.h>

/// returns 0 if s is null/empty or any character in `s` is not a digit
int cstr_isdigits(const char *s);

/// parses the decimal number in `src` to `dst`. `src` must be digits only
/// (no sign, no spaces, no trailing text). `dst` is untouched on error.
/// returns an error if `src` isn't a number or doesn't fit
NODISCARD Error cstr_to_sizet(const char *src, size_t *dst);

/// Parses all of [src] into `*dst`: decimal only, no whitespace, no trailing
/// text. `dst` is untouched on error. `EINVAL` if [src] (or [dst]) is null or
/// isn't a number, `ERANGE` if it doesn't fit.
/// - integers: optional `-`/`+` sign for the signed types, none for unsigned
/// - floats: `[+-]digits[.digits][e[+-]digits]`, also `inf`/`nan`; no hex.
///   Overflow is `ERANGE`, underflow just rounds toward zero
/// - bool: exactly `true` or `false`
///
/// `cstr_to_<name>(const char *, T *)` for each (T, name) in CSTR_TO_TYPES, or
/// let the destination pick: `cstr_to(src, &n)` (see below)
#define CSTR_TO_TYPES(X)                                                       \
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
  X(_Bool, bool)

#define CSTR_DECLARE_TO_(T, N) Error cstr_to_##N(const char *src, T *dst);
CSTR_TO_TYPES(CSTR_DECLARE_TO_)
#undef CSTR_DECLARE_TO_

#define CSTR_TO_ASSOC_(T, N) T *: cstr_to_##N,
/// `cstr_to(src, &dst)`: picks the parser from the type of `dst` at compile
/// time (C11 `_Generic`). `size_t *` works, it is one of the unsigned types.
/// Other destination types are a compile error
#define cstr_to(src, dst)                                                      \
  _Generic((dst), CSTR_TO_TYPES(CSTR_TO_ASSOC_) struct cstr_no_parse_ *:       \
               cstr_to_int)((src), (dst))

/// sets [out] to a malloc'd printf-style formatted string based on [fmt] and
/// [...]. [out] is NULL on error
NODISCARD Error cstr_format(char **out, const char *fmt, ...) PRINTF_LIKE(2, 3);

/// like `snprintf` into [s] (capacity [n]), but returns an error if the output
/// doesn't fit or can't be formatted. [s] is always NUL-terminated when
/// `n > 0`
NODISCARD Error cstr_sformat(char *s, size_t n, const char *fmt, ...) PRINTF_LIKE(3, 4);

/// A counted string: `len` bytes at `p`. Not owned, not required to be
/// NUL-terminated, and may hold NULs.
typedef struct {
  int len;
  char *p;
} zstr;

// `{}` formatting: pass a zstr by address, `io_fprint(w, "{:>8}", &z)`. Same
// specs as a `char *` (width, fill, align, `.N` to truncate), but exactly `len`
// bytes are the string: it doesn't stop at a NUL, and `p[len]` is never read.
// A NULL `p` or `len <= 0` prints as empty. Don't also list zstr in
// IO_FORMAT_TYPES. Any type char other than none or `s` is EINVAL.
#undef IO_FORMAT_EXT_CSTR
#define IO_FORMAT_EXT_CSTR(X) X(zstr)
IO_FORMAT_DEFINE(zstr)

#endif // MOLKIE_CSTR_H

#if defined(MOLKIE_CSTR_IMPL) && !defined(MOLKIE_CSTR_IMPL_DONE)
#define MOLKIE_CSTR_IMPL_DONE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cstr_isdigits(const char *s) {
  if (!s || !*s)
    return 0;
  for (; *s; s++) {
    if (!isdigit((unsigned char)*s))
      return 0;
  }
  return 1;
}

Error cstr_to_sizet(const char *src, size_t *dst) {
  if (!dst)
    return enew(EINVAL, "dst is null");
  if (!cstr_isdigits(src))
    return enew(EINVAL, "\"%s\" is not a number", src ? src : "(null)");

  errno = 0;
  char *end = NULL;
  unsigned long long n = strtoull(src, &end, 10);
  if (errno == ERANGE || n > SIZE_MAX)
    return enew(ERANGE, "\"%s\" is too large", src);

  *dst = (size_t)n;
  return eok();
}

// ---- typed parsers ----

// Checks the syntax of an integer and returns its magnitude. Signs only when
// [allowSign]; *neg says whether there was a '-'
static Error _cstrMagnitude(const char *src, bool allowSign, bool *neg,
                            unsigned long long *mag) {
  if (!src)
    return enew(EINVAL, "\"(null)\" is not a number");
  const char *p = src;
  *neg = false;
  if (allowSign && (*p == '-' || *p == '+'))
    *neg = *p++ == '-';
  if (!cstr_isdigits(p))
    return enew(EINVAL, "\"%s\" is not a number", src);
  errno = 0;
  *mag = strtoull(p, NULL, 10);
  if (errno == ERANGE)
    return enew(ERANGE, "\"%s\" is too large", src);
  return eok();
}

// signed: the magnitude must fit MAX (or MAX + 1 when negative)
#define CSTR_SIGNED_(T, N, MIN, MAX)                                           \
  Error cstr_to_##N(const char *src, T *dst) {                                 \
    if (!dst)                                                                  \
      return enew(EINVAL, "dst is null");                                      \
    bool neg;                                                                  \
    unsigned long long mag;                                                    \
    Error e = _cstrMagnitude(src, true, &neg, &mag);                           \
    if (!e.ok)                                                                 \
      return e;                                                                \
    unsigned long long limit = (unsigned long long)(MAX) + (neg ? 1 : 0);      \
    if (mag > limit)                                                           \
      return enew(ERANGE, "\"%s\" is out of range for " #T, src);              \
    *dst = neg ? (T)(-(long long)(mag - 1) - 1) : (T)mag;                      \
    return eok();                                                              \
  }
#define CSTR_UNSIGNED_(T, N, MAX)                                              \
  Error cstr_to_##N(const char *src, T *dst) {                                 \
    if (!dst)                                                                  \
      return enew(EINVAL, "dst is null");                                      \
    bool neg;                                                                  \
    unsigned long long mag;                                                    \
    Error e = _cstrMagnitude(src, false, &neg, &mag);                          \
    if (!e.ok)                                                                 \
      return e;                                                                \
    if (mag > (MAX))                                                           \
      return enew(ERANGE, "\"%s\" is out of range for " #T, src);              \
    *dst = (T)mag;                                                             \
    return eok();                                                              \
  }
CSTR_SIGNED_(short, short, SHRT_MIN, SHRT_MAX)
CSTR_UNSIGNED_(unsigned short, ushort, USHRT_MAX)
CSTR_SIGNED_(int, int, INT_MIN, INT_MAX)
CSTR_UNSIGNED_(unsigned, uint, UINT_MAX)
CSTR_SIGNED_(long, long, LONG_MIN, LONG_MAX)
CSTR_UNSIGNED_(unsigned long, ulong, ULONG_MAX)
CSTR_SIGNED_(long long, llong, LLONG_MIN, LLONG_MAX)
CSTR_UNSIGNED_(unsigned long long, ullong, ULLONG_MAX)
#undef CSTR_SIGNED_
#undef CSTR_UNSIGNED_

// float syntax: strtod also takes leading spaces and hex floats; we don't
static Error _cstrFloatSyntax(const char *src) {
  if (!src)
    return enew(EINVAL, "\"(null)\" is not a number");
  const char *p = src;
  if (*p == '-' || *p == '+')
    p++;
  if (!*p || isspace((unsigned char)*p) ||
      (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')))
    return enew(EINVAL, "\"%s\" is not a number", src);
  return eok();
}

Error cstr_to_double(const char *src, double *dst) {
  if (!dst)
    return enew(EINVAL, "dst is null");
  Error e = _cstrFloatSyntax(src);
  if (!e.ok)
    return e;
  errno = 0;
  char *end;
  double v = strtod(src, &end);
  if (end == src || *end)
    return enew(EINVAL, "\"%s\" is not a number", src);
  if (errno == ERANGE && isinf(v)) // underflow is fine, overflow isn't
    return enew(ERANGE, "\"%s\" is out of range for double", src);
  *dst = v;
  return eok();
}

Error cstr_to_float(const char *src, float *dst) {
  if (!dst)
    return enew(EINVAL, "dst is null");
  Error e = _cstrFloatSyntax(src);
  if (!e.ok)
    return e;
  errno = 0;
  char *end;
  float v = strtof(src, &end);
  if (end == src || *end)
    return enew(EINVAL, "\"%s\" is not a number", src);
  if (errno == ERANGE && isinf(v))
    return enew(ERANGE, "\"%s\" is out of range for float", src);
  *dst = v;
  return eok();
}

Error cstr_to_bool(const char *src, _Bool *dst) {
  if (!dst)
    return enew(EINVAL, "dst is null");
  if (src && !strcmp(src, "true"))
    *dst = 1;
  else if (src && !strcmp(src, "false"))
    *dst = 0;
  else
    return enew(EINVAL, "\"%s\" is not true or false", src ? src : "(null)");
  return eok();
}

PRINTF_LIKE(3, 0)
static Error _vsformat(char *s, size_t n, const char *fmt, va_list argp) {
  int len = vsnprintf(s, n, fmt, argp);
  if (len < 0)
    return enew(EINVAL, "unable to format \"%s\"", fmt);
  if ((size_t)len >= n)
    return enew(ENOSPC, "output needs %d bytes but only %zu fit", len, n);
  return eok();
}

Error cstr_format(char **out, const char *fmt, ...) {
  if (!out)
    return enew(EINVAL, "out is null");
  *out = NULL;
  va_list argp;
  va_start(argp, fmt);
  int n = vsnprintf(NULL, 0, fmt, argp);
  va_end(argp);
  if (n < 0)
    return enew(EINVAL, "unable to format \"%s\"", fmt);

  char *s = malloc((size_t)n + 1);
  if (!s)
    return enew(ENOMEM, "unable to allocate %d bytes for \"%s\"", n + 1, fmt);

  va_start(argp, fmt);
  Error e = _vsformat(s, (size_t)n + 1, fmt, argp);
  va_end(argp);
  if (!e.ok) {
    free(s);
    return e;
  }
  *out = s;
  return eok();
}

Error cstr_sformat(char *s, size_t n, const char *fmt, ...) {
  if (!s && n)
    return enew(EINVAL, "s is null");
  va_list argp;
  va_start(argp, fmt);
  Error e = _vsformat(s, n, fmt, argp);
  va_end(argp);
  return e;
}

Error zstr_format(Writer w, const zstr *v, FormatSpec spec) {
  if (spec.type != 0 && spec.type != 's')
    return enew(EINVAL, "'%c' is not a format for zstr", spec.type);
  const char *str = v && v->p && v->len > 0 ? v->p : "";
  size_t n = v && v->p && v->len > 0 ? (size_t)v->len : 0, chars;
  if (spec.hasPrecision) { // keep at most `precision` characters
    size_t i = 0;
    chars = 0;
    while (i < n && chars < (size_t)spec.precision) {
      i++;
      while (i < n && ((unsigned char)str[i] & 0xC0) == 0x80)
        i++;
      chars++;
    }
    n = i;
  } else {
    chars = io_utf8Chars(str, n);
  }
  return io_emit(w, spec, "", 0, str, n, chars, '<', false);
}

#endif // MOLKIE_CSTR_IMPL

// Tests: compiled where MTEST (all modules) or MOLKIE_CSTR_TEST (just this one)
// is defined. Needs MOLKIE_CSTR_IMPL and
// MOLKIE_ERR_IMPL in the same program.
#if (defined(MTEST) || defined(MOLKIE_CSTR_TEST)) && !defined(MOLKIE_CSTR_TESTS)
#define MOLKIE_CSTR_TESTS
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

static void cstr_to_ints(void) {
  int i = 7;
  assert(cstr_to_int("-42", &i).ok && i == -42);
  assert(cstr_to_int("+5", &i).ok && i == 5);
  assert(cstr_to_int("-2147483648", &i).ok && i == INT_MIN);
  assert(cstr_to_int("2147483647", &i).ok && i == INT_MAX);
  i = 7;
  Error e = cstr_to_int("2147483648", &i); // one past
  assert(!e.ok && e.code == ERANGE && i == 7);
  e = cstr_to_int("-2147483649", &i);
  assert(!e.ok && e.code == ERANGE && i == 7);
  const char *bad[] = {"", "-", "+", " 1", "1 ", "1x", "0x10", "1.5", "--1", "-+1", "1e3"};
  for (size_t k = 0; k < sizeof bad / sizeof *bad; k++) {
    e = cstr_to_int(bad[k], &i);
    assert(!e.ok && e.code == EINVAL && i == 7);
  }
  assert(cstr_to_int(NULL, &i).code == EINVAL);
  assert(cstr_to_int("1", NULL).code == EINVAL);

  unsigned u = 7;
  assert(cstr_to_uint("4294967295", &u).ok && u == UINT_MAX);
  assert(cstr_to_uint("4294967296", &u).code == ERANGE);
  assert(cstr_to_uint("-1", &u).code == EINVAL); // no sign on unsigned
  assert(cstr_to_uint("+1", &u).code == EINVAL);
  assert(u == UINT_MAX);

  short sh;
  assert(cstr_to_short("-32768", &sh).ok && sh == SHRT_MIN);
  assert(cstr_to_short("32768", &sh).code == ERANGE);
  unsigned short us;
  assert(cstr_to_ushort("65535", &us).ok && us == 65535);
  assert(cstr_to_ushort("65536", &us).code == ERANGE);

  long long ll;
  assert(cstr_to_llong("-9223372036854775808", &ll).ok && ll == LLONG_MIN);
  assert(cstr_to_llong("9223372036854775808", &ll).code == ERANGE);
  assert(cstr_to_llong("99999999999999999999999", &ll).code == ERANGE);
  unsigned long long ull;
  assert(cstr_to_ullong("18446744073709551615", &ull).ok && ull == ULLONG_MAX);
  assert(cstr_to_ullong("18446744073709551616", &ull).code == ERANGE);
  long l;
  assert(cstr_to_long("-1", &l).ok && l == -1);
  unsigned long ul;
  assert(cstr_to_ulong("12", &ul).ok && ul == 12);
}

static void cstr_to_floats(void) {
  double d = 7;
  assert(cstr_to_double("1.5", &d).ok && d == 1.5);
  assert(cstr_to_double("-2e3", &d).ok && d == -2000);
  assert(cstr_to_double("+.5", &d).ok && d == 0.5);
  assert(cstr_to_double("5.", &d).ok && d == 5);
  assert(cstr_to_double("0.1", &d).ok && d == 0.1);
  assert(cstr_to_double("1e-400", &d).ok && d == 0); // underflow rounds to 0
  assert(cstr_to_double("inf", &d).ok && isinf(d));
  assert(cstr_to_double("nan", &d).ok && isnan(d));
  d = 7;
  Error e = cstr_to_double("1e999", &d);
  assert(!e.ok && e.code == ERANGE && d == 7);
  const char *bad[] = {"", "-", " 1", "1 ", "1x", "0x1p3", "0X10", "1e", ".", "e5"};
  for (size_t k = 0; k < sizeof bad / sizeof *bad; k++) {
    e = cstr_to_double(bad[k], &d);
    assert(!e.ok && e.code == EINVAL && d == 7);
  }
  float f = 7;
  assert(cstr_to_float("0.25", &f).ok && f == 0.25f);
  assert(cstr_to_float("1e39", &f).code == ERANGE && f == 0.25f);
  assert(cstr_to_float("x", &f).code == EINVAL);
}

static void cstr_to_bools(void) {
  _Bool b = 0;
  assert(cstr_to_bool("true", &b).ok && b);
  assert(cstr_to_bool("false", &b).ok && !b);
  b = 1;
  const char *bad[] = {"", "True", "TRUE", "1", "0", " true", "true ", "yes"};
  for (size_t k = 0; k < sizeof bad / sizeof *bad; k++) {
    Error e = cstr_to_bool(bad[k], &b);
    assert(!e.ok && e.code == EINVAL && b == 1);
  }
  assert(cstr_to_bool(NULL, &b).code == EINVAL);
}

static void cstr_to_generic(void) {
  int i;
  unsigned long long ull;
  size_t n;
  double d;
  float f;
  _Bool b;
  short s;
  assert(cstr_to("-3", &i).ok && i == -3);
  assert(cstr_to("7", &ull).ok && ull == 7);
  assert(cstr_to("8", &n).ok && n == 8); // size_t is one of the unsigned types
  assert(cstr_to("2.5", &d).ok && d == 2.5);
  assert(cstr_to("2.5", &f).ok && f == 2.5f);
  assert(cstr_to("true", &b).ok && b);
  assert(cstr_to("40000", &s).code == ERANGE);
}

static void cstr_digits(void) {
  assert(cstr_isdigits("0123"));
  assert(!cstr_isdigits(""));
  assert(!cstr_isdigits(NULL));
  assert(!cstr_isdigits("12a"));
  assert(!cstr_isdigits("-1"));
}

static void cstr_sizet(void) {
  size_t n = 7;
  assert(cstr_to_sizet("1234", &n).ok);
  ASSERT_EQ(n, 1234);
  Error e = cstr_to_sizet("12x", &n);
  assert(!e.ok);
  ASSERT_EQ(e.code, EINVAL);
  ASSERT_EQ(n, 1234); // untouched on error
  e = cstr_to_sizet("99999999999999999999999999", &n);
  assert(!e.ok);
  ASSERT_EQ(e.code, ERANGE);
  ASSERT_EQ(n, 1234);
}

static void cstr_null_args(void) {
  size_t n;
  Error e = cstr_to_sizet("12", NULL);
  assert(!e.ok);
  ASSERT_EQ(e.code, EINVAL);
  e = cstr_to_sizet(NULL, &n);
  assert(!e.ok);
  e = cstr_format(NULL, "x");
  assert(!e.ok);
  ASSERT_EQ(e.code, EINVAL);
  e = cstr_sformat(NULL, 4, "x");
  assert(!e.ok);
  ASSERT_EQ(e.code, EINVAL);
}

static void cstr_formats(void) {
  char *s = NULL;
  assert(cstr_format(&s, "%s-%d", "ab", 5).ok);
  assert(s && !strcmp(s, "ab-5"));
  free(s);

  char buf[6];
  assert(cstr_sformat(buf, sizeof buf, "%d", 12345).ok);
  assert(!strcmp(buf, "12345"));
  Error e = cstr_sformat(buf, sizeof buf, "%d", 123456);
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOSPC);
  assert(buf[sizeof buf - 1] == 0); // still NUL-terminated
}

static Error zstr_capture(void *p, const char *b, size_t n) {
  struct {
    char buf[8];
    size_t n;
  } *c = p;
  memcpy(c->buf + c->n, b, n);
  c->n += n;
  return eok();
}

static void cstr_zstr_format(void) {
#define ZCHECK(want, ...)                                                      \
  do {                                                                         \
    char b_[64];                                                               \
    Scratch sc_ = Scratch_init(b_, sizeof b_);                                 \
    Error e_ = io_fprint(Scratch_writer(&sc_), __VA_ARGS__);                   \
    assert(e_.ok && !strcmp(b_, want));                                        \
  } while (0)
  char raw[] = {'a', 'b', 'c', 'd'}; // no NUL: len must bound the read
  zstr z = {3, raw};
  ZCHECK("abc", "{}", &z);
  ZCHECK("[  abc]", "[{:>5}]", &z);
  ZCHECK("[abc  ]", "[{:5}]", &z);
  ZCHECK("[*abc*]", "[{:*^5}]", &z);
  ZCHECK("ab", "{:.2}", &z);
  const zstr *cz = &z;
  ZCHECK("abc!", "{}!", cz);
  zstr u = {4, "h\xC3\xA9y"}; // é counts as one character
  ZCHECK("[ h\xC3\xA9y]", "[{:>4}]", &u);
  ZCHECK("h\xC3\xA9", "{:.2}", &u);
  zstr e0 = {0, raw}, neg = {-1, raw}, nul = {5, NULL};
  ZCHECK("[]", "[{}]", &e0);
  ZCHECK("[]", "[{}]", &neg);
  ZCHECK("[]", "[{}]", &nul);
  ZCHECK("[  ]", "[{:2}]", &nul);
  // stops nowhere near a NUL: the NUL is part of the string
  zstr w = {3, "a\0b"};
  struct {
    char buf[8];
    size_t n;
  } cap = {{0}, 0};
  Writer cw = {zstr_capture, &cap};
  assert(io_fprint(cw, "{}", &w).ok);
  assert(cap.n == 3 && !memcmp(cap.buf, "a\0b", 3));
  char b2[16];
  Scratch sc2 = Scratch_init(b2, sizeof b2);
  assert(io_fprint(Scratch_writer(&sc2), "{:d}", &z).code == EINVAL);
#undef ZCHECK
}

static int cstr_tests(void) {
  static const TestCase tests[] = {
      T(cstr_digits),
      T(cstr_sizet),
      T(cstr_to_ints),
      T(cstr_to_floats),
      T(cstr_to_bools),
      T(cstr_to_generic),
      T(cstr_formats),
      T(cstr_null_args),
      T(cstr_zstr_format),
  };
  size_t n = sizeof tests / sizeof *tests, passed = 0;
  for (size_t i = 0; i < n; i++)
    passed += run_in_child(&tests[i]);
  printf("cstr: %zu/%zu passed\n", passed, n);
  return passed == n;
}

#endif // MTEST
