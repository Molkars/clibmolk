#ifndef MOLKIE_CSTR_H
#define MOLKIE_CSTR_H

#include "attr.h"
#include "err.h"
#include <stddef.h>

/// returns 0 if s is null/empty or any character in `s` is not a digit
int cstr_isdigits(const char *s);

/// parses the decimal number in `src` to `dst`. `src` must be digits only
/// (no sign, no spaces, no trailing text). `dst` is untouched on error.
/// returns an error if `src` isn't a number or doesn't fit
Error cstr_to_sizet(const char *src, size_t *dst);

/// sets [out] to a malloc'd printf-style formatted string based on [fmt] and
/// [...]. [out] is NULL on error
Error cstr_format(char **out, const char *fmt, ...) PRINTF_LIKE(2, 3);

/// like `snprintf` into [s] (capacity [n]), but returns an error if the output
/// doesn't fit or can't be formatted. [s] is always NUL-terminated when
/// `n > 0`
Error cstr_sformat(char *s, size_t n, const char *fmt, ...) PRINTF_LIKE(3, 4);

#endif // MOLKIE_CSTR_H

#if defined(MOLKIE_CSTR_IMPL) && !defined(MOLKIE_CSTR_IMPL_DONE)
#define MOLKIE_CSTR_IMPL_DONE
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
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

#endif // MOLKIE_CSTR_IMPL

// Tests: compiled where MTEST (all modules) or MOLKIE_CSTR_TEST (just this one)
// is defined. Needs MOLKIE_CSTR_IMPL and
// MOLKIE_ERR_IMPL in the same program.
#if (defined(MTEST) || defined(MOLKIE_CSTR_TEST)) && !defined(MOLKIE_CSTR_TESTS)
#define MOLKIE_CSTR_TESTS
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "test.h"

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

static int cstr_tests(void) {
  static const TestCase tests[] = {
      T(cstr_digits),
      T(cstr_sizet),
      T(cstr_formats),
      T(cstr_null_args),
  };
  size_t n = sizeof tests / sizeof *tests, passed = 0;
  for (size_t i = 0; i < n; i++)
    passed += run_in_child(&tests[i]);
  printf("cstr: %zu/%zu passed\n", passed, n);
  return passed == n;
}

#endif // MTEST
