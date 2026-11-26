#ifndef MOLKIE_ERR_H
#define MOLKIE_ERR_H

#include <stdbool.h>

#include "attr.h"

/// One context message attached by [ectx]. The chain hangs off `Error.ctx`,
/// newest first. Read-only for callers; walk it to print or inspect an error
typedef struct ErrCtx ErrCtx;
struct ErrCtx {
  ErrCtx *next; // older context
  char msg[];
};

/// A small error value. A function that can fail returns one. [ectx] may
/// attach heap-allocated context, so an Error that has gone through [ectx] owns
/// that chain: pass it on by value (moving it), don't keep two copies, and
/// call [efree] when you are done with a failed one you don't return:
/// ```
/// Error read_thing(const char *path) {
///   FILE *f = fopen(path, "r");
///   if (!f)
///     return eerrno("unable to open %s", path);
///   ...
///   return eok();
/// }
///
/// Error e = read_thing(p);
/// if (!e.ok)
///   eprint(&e, &stderrW); // eprint comes with writer.h
/// ```
typedef struct {
  bool ok;
  /// an `errno` value or whatever the caller chose; 0 when `ok`
  int code;
  /// NUL-terminated, truncated if it didn't fit; "" when `ok`
  char msg[160];
  /// context added by [ectx], newest first; null if none. Owned by the Error
  ErrCtx *ctx;
} Error;

/// success
Error eok(void);

/// a failure with [code] and a printf-style message
Error enew(int code, const char *fmt, ...) PRINTF_LIKE(2, 3);

/// a failure with `code = errno` and the message `fmt: strerror(errno)`
Error eerrno(const char *fmt, ...) PRINTF_LIKE(1, 2);

/// adds a printf-style context message to the failure [e], e.g.
/// `ectx(&e, "while loading %s", path)`. Does nothing if [e] is ok. If the
/// allocation fails the context is silently dropped
void ectx(Error *e, const char *fmt, ...) PRINTF_LIKE(2, 3);

/// frees the context chain of [e]. Safe on ok errors and to call twice. The
/// error stays a failure: `ok`, `code` and `msg` are untouched, only `ctx` is
/// cleared
void efree(Error *e);

/// if the `Error` [expr] failed, returns it from the calling function with
/// `file:line` added as context. [expr] is evaluated exactly once
#define _ECAT2(a, b) a##b
#define _ECAT(a, b) _ECAT2(a, b)
#define etry(expr) _etry_impl(_ECAT(_etry_e_, __LINE__), expr)
#define _etry_impl(v, expr)                                                    \
  do {                                                                         \
    Error v = (expr);                                                          \
    if (etry_check_(&v, __FILE__, __LINE__))                                   \
      return v;                                                                \
  } while (0)

/// like [etry], also adding a printf-style message: `etry2(f(), "while %s", s)`
#define etry2(...) _etry2_impl(_ECAT(_etry_e_, __LINE__), __VA_ARGS__)
#define _etry2_impl(v, expr, ...)                                              \
  do {                                                                         \
    Error v = (expr);                                                          \
    if (etry_check_(&v, __FILE__, __LINE__)) {                                 \
      ectx(&v, __VA_ARGS__);                                                   \
      return v;                                                                \
    }                                                                          \
  } while (0)

/// used by [etry]: true if [e] failed, after adding `file:line` as context.
/// Note [etry] allocates, which may clobber `errno`: read `e.code` instead
bool etry_check_(Error *e, const char *file, int line);

#endif // MOLKIE_ERR_H

#if defined(MOLKIE_ERR_IMPL) && !defined(MOLKIE_ERR_IMPL_DONE)
#define MOLKIE_ERR_IMPL_DONE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Error eok(void) { return (Error){.ok = true}; }

PRINTF_LIKE(3, 0)
static Error _fail(int code, bool withErrno, const char *fmt, va_list argp) {
  Error e = {.ok = false, .code = code};
  int n = vsnprintf(e.msg, sizeof e.msg, fmt, argp);
  if (n < 0)
    e.msg[0] = 0;
  if (withErrno) {
    size_t len = strlen(e.msg);
    snprintf(e.msg + len, sizeof e.msg - len, "%s%s", len ? ": " : "",
             strerror(code));
  }
  return e;
}

Error enew(int code, const char *fmt, ...) {
  va_list argp;
  va_start(argp, fmt);
  Error e = _fail(code, false, fmt, argp);
  va_end(argp);
  return e;
}

Error eerrno(const char *fmt, ...) {
  int code = errno; // before anything below can change it
  va_list argp;
  va_start(argp, fmt);
  Error e = _fail(code, true, fmt, argp);
  va_end(argp);
  return e;
}

void ectx(Error *e, const char *fmt, ...) {
  if (e->ok)
    return;
  va_list argp;
  va_start(argp, fmt);
  va_list copy;
  va_copy(copy, argp);
  int n = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  ErrCtx *c = n < 0 ? NULL : malloc(sizeof *c + (size_t)n + 1);
  if (c) {
    vsnprintf(c->msg, (size_t)n + 1, fmt, argp);
    c->next = e->ctx;
    e->ctx = c;
  }
  va_end(argp);
}

void efree(Error *e) {
  ErrCtx *c = e->ctx;
  while (c) {
    ErrCtx *next = c->next;
    free(c);
    c = next;
  }
  e->ctx = NULL;
}

bool etry_check_(Error *e, const char *file, int line) {
  if (e->ok)
    return false;
  ectx(e, "%s:%d", file, line);
  return true;
}
#endif // MOLKIE_ERR_IMPL

// Tests: compiled where MTEST (all modules) or MOLKIE_ERR_TEST (just this one)
// is defined. Needs MOLKIE_ERR_IMPL in the
// same program. Printing the error chain is tested in writer.h.
#if (defined(MTEST) || defined(MOLKIE_ERR_TEST)) && !defined(MOLKIE_ERR_TESTS)
#define MOLKIE_ERR_TESTS
#include <errno.h>
#include <string.h>

#include "test.h"

static void err_ok(void) {
  Error e = eok();
  assert(e.ok);
  ASSERT_EQ(e.code, 0);
  assert(!strcmp(e.msg, ""));
  assert(!e.ctx);
}

static void err_new(void) {
  Error e = enew(7, "bad %s #%d", "thing", 3);
  assert(!e.ok);
  ASSERT_EQ(e.code, 7);
  assert(!strcmp(e.msg, "bad thing #3"));
}

static void err_errno(void) {
  errno = ENOENT;
  Error e = eerrno("unable to open %s", "/nope");
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOENT);
  assert(!strcmp(e.msg, "unable to open /nope: No such file or directory"));
}

static void err_truncate(void) {
  char big[400];
  memset(big, 'x', sizeof big - 1);
  big[sizeof big - 1] = 0;
  Error e = enew(1, "%s", big);
  assert(!e.ok);
  ASSERT_EQ(strlen(e.msg), sizeof e.msg - 1);
}

static void err_ctx(void) {
  Error ok = eok();
  ectx(&ok, "ignored"); // no-op on success
  assert(ok.ok && !ok.ctx);

  Error e = enew(2, "boom");
  ectx(&e, "reading %s", "a.txt");
  ectx(&e, "step %d", 2);
  assert(!e.ok);
  ASSERT_EQ(e.code, 2);
  assert(!strcmp(e.msg, "boom"));
  // newest first
  assert(e.ctx && !strcmp(e.ctx->msg, "step 2"));
  assert(e.ctx->next && !strcmp(e.ctx->next->msg, "reading a.txt"));
  assert(!e.ctx->next->next);
  Error moved = e; // moving by value keeps the chain
  efree(&moved);
  assert(!moved.ctx);
  assert(!moved.ok); // still a failure, code and msg stay readable
  ASSERT_EQ(moved.code, 2);
  assert(!strcmp(moved.msg, "boom"));
  efree(&moved); // twice is fine
}

static void err_errno_truncate(void) {
  char big[400];
  memset(big, 'x', sizeof big - 1);
  big[sizeof big - 1] = 0;
  errno = ENOENT;
  Error e = eerrno("%s", big); // no room left for ": strerror"
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOENT);
  ASSERT_EQ(strlen(e.msg), sizeof e.msg - 1);
}

static Error err_fails(void) { return enew(3, "inner"); }
static Error err_try_user(void) {
  etry(err_fails());
  return eok();
}
static Error err_try2_user(void) {
  etry2(err_fails(), "while %s", "testing");
  return eok();
}

static void err_try(void) {
  Error e = err_try_user();
  assert(!e.ok);
  ASSERT_EQ(e.code, 3);
  assert(e.ctx && !e.ctx->next); // just file:line
  efree(&e);

  e = err_try2_user();
  assert(!e.ok);
  assert(e.ctx && !strcmp(e.ctx->msg, "while testing")); // newest
  assert(e.ctx->next && !e.ctx->next->next);             // + file:line
  efree(&e);
}

static int err_tests(void) {
  static const TestCase tests[] = {
      T(err_ok),  T(err_new), T(err_errno),
      T(err_truncate), T(err_errno_truncate), T(err_ctx), T(err_try),
  };
  size_t n = sizeof tests / sizeof *tests, passed = 0;
  for (size_t i = 0; i < n; i++)
    passed += run_in_child(&tests[i]);
  printf("err: %zu/%zu passed\n", passed, n);
  return passed == n;
}

#endif // MTEST
