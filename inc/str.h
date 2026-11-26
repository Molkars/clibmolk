#ifndef MOLKIE_STR_H
#define MOLKIE_STR_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "array.h"
#include "attr.h"
#include "err.h"
#include "io.h"

/// Growable heap string.
/// Invariant: if `buf` is non-null it is always NUL-terminated and
/// `len < cap`, so it is always a valid C string. A zeroed String is empty and
/// valid. `len` does not count the NUL.
typedef struct {
  char *buf;
  size_t len, cap;
} String;

/// an empty string. allocates nothing
String String_empty(void);

/// copies the `n` bytes at `src` into a new string. `buf` is null on alloc
/// failure
String String_from(const char *src, size_t n);

/// copies the C string `src` into a new string. `buf` is null on alloc failure
String String_fromc(const char *src);

/// the contents as a C string. never null, even for an empty String
const char *String_cstr(const String *s);

/// true if `s` has exactly the contents of the C string `other`
int String_eq(const String *s, const char *other);

/// ensure `extra` more bytes (plus the NUL) fit in `s`. returns 0 on error
int String_grow(String *s, size_t extra);

/// appends the char `c` to `s`. returns 0 on error
int String_putc(String *s, char c);

/// appends the `n` bytes at `src` to `s`. returns 0 on error
int String_extend(String *s, const char *src, size_t n);

/// appends the C string `src` to `s`. returns 0 on error
int String_puts(String *s, const char *src);

/// appends printf-style formatted text to `s`. returns 0 on error
int String_printf(String *s, const char *fmt, ...) PRINTF_LIKE(2, 3);

/// like [String_printf], taking a `va_list`
int String_vprintf(String *s, const char *fmt, va_list argp) PRINTF_LIKE(2, 0);

/// empties `s`, keeping its allocation
void String_reset(String *s);

/// frees `s` and leaves it empty
void String_free(String *s);

/// `free` callback for `array_free0` on an array of String
void String_free2(void *s);

/// an array of Strings
typedef array_t(String) Strings;

/// reads every line of `f` into `lines`, without the
/// trailing `\n` or `\r\n`. A final line with no newline is still included.
/// returns an error on failure, in which case `lines` is left empty
Error FILE_readLines(FILE *f, Strings *lines);

/// reads the whole file at `path` into `out`, replacing its contents. At most
/// `memmax` bytes are read: a bigger file fails with `EFBIG` (a `memmax` of 0
/// only accepts an empty file). Works on non-seekable files too. On failure
/// `out` is left empty. `out` must be a valid String (a zeroed one is fine)
Error String_readFile(const char *path, size_t memmax, String *out);

/// removes leading and trailing whitespace from `s` in place
void String_trim(String *s);

/// splits `s` on runs of whitespace and appends each word (a new copy) to
/// `out`; leading, trailing and repeated whitespace yield no empty words. On
/// failure `out` is freed and left empty
Error String_split(const String *s, Strings *out);

/// true if `s` is non-empty and consists only of ASCII digits `0`-`9`
int String_isdigits(const String *s);

/// parses the decimal number in `s` to `dst`. `s` must be digits only (no
/// sign, no spaces, no trailing text, not empty). `dst` is untouched on error.
/// Fails with `EINVAL` if `s` isn't a number (or `dst` is null) and `ERANGE`
/// if it doesn't fit in a `size_t`
Error String_to_sizet(const String *s, size_t *dst);

#endif // MOLKIE_STR_H

#if defined(MOLKIE_STR_IMPL) && !defined(MOLKIE_STR_IMPL_DONE)
#define MOLKIE_STR_IMPL_DONE
#include "cstr.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

String String_empty(void) { return (String){NULL, 0, 0}; }

String String_from(const char *src, size_t n) {
  String s = String_empty();
  if (!String_extend(&s, src, n))
    String_free(&s);
  return s;
}

String String_fromc(const char *src) {
  assert(src && "src is null");
  return String_from(src, strlen(src));
}

const char *String_cstr(const String *s) {
  assert(s && "string is null");
  return s->buf ? s->buf : "";
}

int String_eq(const String *s, const char *other) {
  return strcmp(String_cstr(s), other) == 0;
}

int String_grow(String *s, size_t extra) {
  assert(s && "string is null");
  if (extra > SIZE_MAX - s->len - 1) { // len + extra + NUL would wrap
    errno = ENOMEM;
    return 0;
  }
  size_t need = s->len + extra + 1; // + NUL
  if (need <= s->cap)
    return 1;

  size_t cap = s->cap ? s->cap : 16;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) { // doubling would wrap: take exactly what's needed
      cap = need;
      break;
    }
    cap *= 2;
  }

  char *p = realloc(s->buf, cap);
  if (!p) {
    errno = ENOMEM;
    return 0;
  }
  s->buf = p;
  s->cap = cap;
  s->buf[s->len] = 0;
  return 1;
}

int String_putc(String *s, char c) { return String_extend(s, &c, 1); }

int String_extend(String *s, const char *src, size_t n) {
  assert(s && "string is null");
  // [src] may point into [s] itself; remember where, since grow can realloc
  size_t off = SIZE_MAX;
  if (s->buf && src >= s->buf && src < s->buf + s->cap)
    off = (size_t)(src - s->buf);
  if (!String_grow(s, n)) // always, so `buf` is non-null even when n == 0
    return 0;
  if (off != SIZE_MAX)
    src = s->buf + off;
  if (n)
    memmove(s->buf + s->len, src, n);
  s->len += n;
  s->buf[s->len] = 0;
  return 1;
}

int String_puts(String *s, const char *src) {
  assert(src && "src is null");
  return String_extend(s, src, strlen(src));
}

int String_vprintf(String *s, const char *fmt, va_list argp) {
  assert(s && "string is null");
  va_list argp2;
  va_copy(argp2, argp);
  int n = vsnprintf(NULL, 0, fmt, argp2);
  va_end(argp2);
  if (n < 0)
    return 0;

  if (!String_grow(s, (size_t)n))
    return 0;

  int n1 = vsnprintf(s->buf + s->len, (size_t)n + 1, fmt, argp);
  if (n1 < 0) {
    s->buf[s->len] = 0; // drop whatever vsnprintf left behind
    return 0;
  }

  s->len += (size_t)n1;
  return 1;
}

int String_printf(String *s, const char *fmt, ...) {
  va_list argp;
  va_start(argp, fmt);
  int ok = String_vprintf(s, fmt, argp);
  va_end(argp);
  return ok;
}

void String_reset(String *s) {
  assert(s && "string is null");
  s->len = 0;
  if (s->buf)
    s->buf[0] = 0;
}

void String_free(String *s) {
  if (!s)
    return;
  free(s->buf);
  s->buf = NULL;
  s->len = 0;
  s->cap = 0;
}

void String_free2(void *s) { String_free(s); }

static int _isws(char c) { return isspace((unsigned char)c); }

void String_trim(String *s) {
  assert(s && "string is null");
  if (!s->buf)
    return;
  size_t start = 0, end = s->len;
  while (end > start && _isws(s->buf[end - 1]))
    end--;
  while (start < end && _isws(s->buf[start]))
    start++;
  s->len = end - start;
  memmove(s->buf, s->buf + start, s->len);
  s->buf[s->len] = 0;
}

int String_isdigits(const String *s) {
  assert(s && "string is null");
  if (s->len == 0)
    return 0;
  for (size_t i = 0; i < s->len; i++)
    if (!isdigit((unsigned char)s->buf[i]))
      return 0;
  return 1;
}

Error String_to_sizet(const String *s, size_t *dst) {
  assert(s && "string is null");
  if (!dst)
    return enew(EINVAL, "dst is null");
  // String_isdigits is length-based, so an embedded NUL is rejected here
  // rather than silently ending the number
  if (!String_isdigits(s))
    return enew(EINVAL, "\"%.*s\" is not a number", (int)s->len,
                String_cstr(s));
  return cstr_to_sizet(String_cstr(s), dst);
}

Error String_split(const String *s, Strings *out) {
  assert(s && "string is null");
  assert(out && "out is null");

  const char *p = String_cstr(s), *end = p + s->len;
  while (p < end) {
    while (p < end && _isws(*p))
      p++;
    const char *w = p;
    while (p < end && !_isws(*p))
      p++;
    if (p == w)
      break;

    String word = String_from(w, (size_t)(p - w));
    if (!word.buf) {
      array_free0(out, String_free2);
      return enew(ENOMEM, "out of memory splitting string");
    }
    Error e = array_push(out, &word);
    if (!e.ok) {
      String_free(&word);
      array_free0(out, String_free2);
      return e;
    }
  }
  return eok();
}

// moves `*line` into `lines`. on success `*line` is reset to empty
static Error _pushLine(Strings *lines, String *line) {
  Error e = array_push(lines, line);
  if (!e.ok)
    return e;
  *line = String_empty();
  return eok();
}

Error FILE_readLines(FILE *f, Strings *lines) {
  assert(f && "file is null");
  assert(lines && "lines is null");

  String line = String_empty();
  Error e = eok();
  int have = 0; // a line is in progress (so a last line w/o '\n' still counts)

  // byte by byte, so embedded NULs and a "\r\n" split anywhere are handled
  int c;
  while ((c = getc(f)) != EOF) {
    have = 1;
    if (c == '\n') {
      if (line.len && line.buf[line.len - 1] == '\r') {
        line.len--;
        line.buf[line.len] = 0;
      }
      if (!String_grow(&line, 0)) { // non-null buf even for an empty line
        e = enew(ENOMEM, "out of memory reading line %zu", lines->len + 1);
        goto fail;
      }
      e = _pushLine(lines, &line);
      if (!e.ok)
        goto fail;
      have = 0;
    } else if (!String_putc(&line, (char)c)) {
      e = enew(ENOMEM, "out of memory reading line %zu", lines->len + 1);
      goto fail;
    }
  }
  if (ferror(f)) {
    e = eerrno("unable to read after line %zu", lines->len);
    goto fail;
  }

  if (have) {
    e = _pushLine(lines, &line);
    if (!e.ok)
      goto fail;
  }
  return eok();

fail:
  String_free(&line);
  array_free0(lines, String_free2);
  return e;
}

Error String_readFile(const char *path, size_t memmax, String *out) {
  if (!out)
    return enew(EINVAL, "out is null");
  String_reset(out);
  if (!path)
    return enew(EINVAL, "path is null");

  FILE *f;
  etry(io_open(&f, path, "rb"));
  Error err = eok();
  char chunk[BUFSIZ];
  size_t nRead, want;
  do {
    // ask for one byte past the limit so an oversized file is noticed
    size_t room = memmax - out->len;
    want = room < sizeof chunk ? room + 1 : sizeof chunk;
    err = io_read(f, chunk, want, &nRead);
    // keep what arrived even if the read then failed
    if (!String_extend(out, chunk, nRead) && err.ok)
      err = enew(ENOMEM, "out of memory reading %s", path);
    if (err.ok && out->len > memmax)
      err = enew(EFBIG, "%s is larger than %zu bytes", path, memmax);
  } while (err.ok && nRead == want);

  Error cerr = io_close(f);
  if (!err.ok) {
    efree(&cerr);
    String_reset(out);
    etry(err);
  }
  if (!cerr.ok)
    String_reset(out);
  etry(cerr);
  return eok();
}

#endif // MOLKIE_STR_IMPL

// Tests: compiled where MTEST (all modules) or MOLKIE_STR_TEST (just this one)
// is defined. Needs MOLKIE_STR_IMPL,
// MOLKIE_CSTR_IMPL, MOLKIE_ARRAY_IMPL and MOLKIE_ERR_IMPL in the same program.
#if (defined(MTEST) || defined(MOLKIE_STR_TEST)) && !defined(MOLKIE_STR_TESTS)
#define MOLKIE_STR_TESTS
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "test.h"

static void str_eq(void) {
  String s = String_fromc("abc");
  assert(String_eq(&s, "abc"));
  assert(!String_eq(&s, "abd"));
  assert(!String_eq(&s, "ab"));
  String e = String_empty();
  assert(String_eq(&e, ""));
  String_free(&s);
}

static void str_trim(void) {
  String s = String_fromc(" \t a b \r\n");
  String_trim(&s);
  assert(String_eq(&s, "a b"));
  String_free(&s);
  String blank = String_fromc("  \n");
  String_trim(&blank);
  assert(String_eq(&blank, "") && blank.len == 0);
  String_free(&blank);
}

static void str_split(void) {
  String s = String_fromc("  one two\t\tthree \n");
  Strings w = array_empty(String);
  assert(String_split(&s, &w).ok);
  ASSERT_EQ(w.len, 3);
  assert(String_eq(&w.elements[0], "one"));
  assert(String_eq(&w.elements[1], "two"));
  assert(String_eq(&w.elements[2], "three"));
  array_free0(&w, String_free2);

  String none = String_fromc(" \t ");
  assert(String_split(&none, &w).ok);
  ASSERT_EQ(w.len, 0);
  array_free0(&w, String_free2);
  String_free(&s);
  String_free(&none);
}

static void str_isdigits(void) {
  String d = String_fromc("0123456789");
  assert(String_isdigits(&d));
  String m = String_fromc("12a4");
  assert(!String_isdigits(&m));
  String e = String_empty();
  assert(!String_isdigits(&e));
  String neg = String_fromc("-1");
  assert(!String_isdigits(&neg));
  String_free(&d);
  String_free(&m);
  String_free(&neg);
}

static void str_to_sizet(void) {
  size_t n = 7;
  String s = String_fromc("12345");
  assert(String_to_sizet(&s, &n).ok);
  ASSERT_EQ(n, 12345);
  String_reset(&s);
  String_puts(&s, "007");
  assert(String_to_sizet(&s, &n).ok);
  ASSERT_EQ(n, 7);
  String_reset(&s);
  String_puts(&s, "18446744073709551615"); // SIZE_MAX on 64-bit
  assert(String_to_sizet(&s, &n).ok);
  ASSERT_EQ(n, SIZE_MAX);
  String_free(&s);
}

static void str_to_sizet_errors(void) {
  size_t n = 7;
  const char *bad[] = {"", "-1", "+1", " 1", "1 ", "12a", "1.5"};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    String s = String_fromc(bad[i]);
    Error e = String_to_sizet(&s, &n);
    assert(!e.ok);
    ASSERT_EQ(e.code, EINVAL);
    ASSERT_EQ(n, 7); // untouched
    String_free(&s);
  }
  String empty = String_empty(); // null buf
  assert(String_to_sizet(&empty, &n).code == EINVAL);

  // an embedded NUL must not be read as the end of the number
  String nul = String_from("12\0" "34", 5);
  assert(String_to_sizet(&nul, &n).code == EINVAL);
  ASSERT_EQ(n, 7);
  String_free(&nul);

  String big = String_fromc("99999999999999999999999");
  Error e = String_to_sizet(&big, &n);
  assert(!e.ok);
  ASSERT_EQ(e.code, ERANGE);
  ASSERT_EQ(n, 7);
  String_free(&big);

  String ok = String_fromc("1");
  assert(String_to_sizet(&ok, NULL).code == EINVAL);
  String_free(&ok);
}

static Strings str_read(const char *data, size_t n, Error *e) {
  Strings lines = array_empty(String);
  FILE *f = fmemopen((void *)data, n, "r");
  assert(f);
  *e = FILE_readLines(f, &lines);
  fclose(f);
  return lines;
}

static void str_readlines(void) {
  Error e;
  Strings l = str_read("a\r\nb\n\nlast", 12, &e);
  assert(e.ok);
  ASSERT_EQ(l.len, 4);
  assert(String_eq(&l.elements[0], "a"));
  assert(String_eq(&l.elements[1], "b"));
  assert(String_eq(&l.elements[2], "") && l.elements[2].buf);
  assert(String_eq(&l.elements[3], "last"));
  array_free0(&l, String_free2);
}

static void str_readlines_nul(void) {
  Error e;
  Strings l = str_read("ab\0cd\nef\n", 9, &e);
  assert(e.ok);
  ASSERT_EQ(l.len, 2);
  ASSERT_EQ(l.elements[0].len, 5);
  assert(!memcmp(l.elements[0].buf, "ab\0cd", 5));
  assert(String_eq(&l.elements[1], "ef"));
  array_free0(&l, String_free2);
}

static void str_readlines_crlf_boundary(void) {
  enum { N = 4094 }; // "\r\n" would straddle a 4096-byte chunk
  static char data[N + 4];
  memset(data, 'x', N);
  memcpy(data + N, "\r\ny\n", 5 - 1);
  Error e;
  Strings l = str_read(data, N + 4, &e);
  assert(e.ok);
  ASSERT_EQ(l.len, 2);
  ASSERT_EQ(l.elements[0].len, N);
  assert(l.elements[0].buf[N - 1] == 'x');
  assert(String_eq(&l.elements[1], "y"));
  array_free0(&l, String_free2);
}

static void str_self_append(void) {
  String s = String_fromc("0123456789abcde"); // len 15, cap 16: next append grows
  assert(String_puts(&s, s.buf));
  assert(String_eq(&s, "0123456789abcde0123456789abcde"));
  assert(String_extend(&s, s.buf + 10, 5));
  assert(String_eq(&s, "0123456789abcde0123456789abcdeabcde"));
  String_free(&s);
}

static void str_grow_overflow(void) {
  String s = String_fromc("x");
  assert(!String_grow(&s, SIZE_MAX));
  assert(!String_extend(&s, "y", SIZE_MAX));
  assert(String_eq(&s, "x")); // unchanged
  String_free(&s);
}

// writes [n] bytes of a repeating pattern to a fresh temp file
static void str_mkfile(char *path, size_t n) {
  int fd = mkstemp(path);
  assert(fd >= 0);
  FILE *f = fdopen(fd, "w");
  assert(f);
  for (size_t i = 0; i < n; i++)
    fputc('a' + (int)(i % 26), f);
  assert(fclose(f) == 0);
}

static void str_readfile(void) {
  size_t sizes[] = {0, 1, 10, BUFSIZ - 1, BUFSIZ, BUFSIZ + 1, BUFSIZ * 3 + 17};
  for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
    size_t n = sizes[k];
    char path[] = "/tmp/str_rf_XXXXXX";
    str_mkfile(path, n);
    String s = String_fromc("stale"); // contents are replaced
    Error e = String_readFile(path, n, &s); // exactly at the limit
    remove(path);
    assert(e.ok);
    ASSERT_EQ(s.len, n);
    for (size_t i = 0; i < n; i++)
      assert(s.buf[i] == 'a' + (int)(i % 26));
    assert(n == 0 || s.buf[n] == 0);
    String_free(&s);
  }
}

static void str_readfile_too_big(void) {
  size_t sizes[] = {1, BUFSIZ, BUFSIZ * 2 + 5};
  for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
    char path[] = "/tmp/str_rf_XXXXXX";
    str_mkfile(path, sizes[k]);
    String s = String_fromc("stale");
    Error e = String_readFile(path, sizes[k] - 1, &s);
    remove(path);
    assert(!e.ok);
    ASSERT_EQ(e.code, EFBIG);
    efree(&e);
    assert(s.len == 0 && String_eq(&s, ""));
    String_free(&s);
  }
}

static void str_readfile_errors(void) {
  String s = String_empty();
  Error e = String_readFile("/nonexistent/x", 10, &s);
  ASSERT_EQ(e.code, ENOENT);
  efree(&e);
  e = String_readFile(NULL, 10, &s);
  ASSERT_EQ(e.code, EINVAL);
  e = String_readFile("/dev/null", 10, NULL);
  ASSERT_EQ(e.code, EINVAL);
  e = String_readFile("/tmp", 10, &s);
  ASSERT_EQ(e.code, EISDIR);
  efree(&e);
  // a stream with no end hits the limit instead of eating memory
  e = String_readFile("/dev/zero", 100000, &s);
  ASSERT_EQ(e.code, EFBIG);
  efree(&e);
  ASSERT_EQ(s.len, 0);
  String_free(&s);
}

static int str_tests(void) {
  static const TestCase tests[] = {
      T(str_readfile),
      T(str_readfile_too_big),
      T(str_readfile_errors),
      T(str_readlines),
      T(str_readlines_nul),
      T(str_readlines_crlf_boundary),
      T(str_self_append),
      T(str_grow_overflow),
      T(str_eq),        T(str_trim),
      T(str_split),     T(str_isdigits),
      T(str_to_sizet),  T(str_to_sizet_errors),
  };
  size_t n = sizeof tests / sizeof *tests, passed = 0;
  for (size_t i = 0; i < n; i++)
    passed += run_in_child(&tests[i]);
  printf("str: %zu/%zu passed\n", passed, n);
  return passed == n;
}

#endif // MTEST
