#ifndef MOLKIE_ERR_H
#define MOLKIE_ERR_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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
/// (for functions that call other failing ones, see `etry` below)
///
/// Error e = read_thing(p);
/// if (!e.ok)
///   efprint(&e, stderr); // or eprint(&e, stderrW), from io.h
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
NODISCARD Error eok(void);

/// a failure with [code] and a printf-style message
NODISCARD Error enew(int code, const char *fmt, ...) PRINTF_LIKE(2, 3);

/// a failure with `code = errno` and the message `fmt: strerror(errno)`
NODISCARD Error eerrno(const char *fmt, ...) PRINTF_LIKE(1, 2);

/// adds a printf-style context message to the failure [e], e.g.
/// `ectx(&e, "while loading %s", path)`. Does nothing if [e] is ok. If the
/// allocation fails the context is silently dropped
void ectx(Error *e, const char *fmt, ...) PRINTF_LIKE(2, 3);

/// frees the context chain of [e]. Safe on ok errors and to call twice. The
/// error stays a failure: `ok`, `code` and `msg` are untouched, only `ctx` is
/// cleared
void efree(Error *e);

/// Keeps the first error: if [e] is ok it becomes [later]; otherwise [later]
/// is dropped (its context freed). For cleanup that can fail but must not hide
/// the error that got you there:
/// ```
/// ejoin(&eret, io_close(f));
/// ```
void ejoin(Error *e, Error later);

// ---- etry: one way out ----------------------------------------------------
//
// A function that uses `etry` has a single exit. It declares `Error eret` (its
// result) and an `end:` label; a failed `etry` stores the error in `eret` and
// jumps to `end`, where cleanup runs and the function returns:
//
//   Error load(const char *path, String *out) {
//     Error eret = eok();
//     FILE *f = NULL;                  // declare everything BEFORE the first
//     etry(io_open(&f, path, "r"));    // etry: a goto skips initialisers
//     etry(String_readFile_(f, out));
//   end:
//     if (f)
//       ejoin(&eret, io_close(f));
//     return eret;
//   }
//
// A function with `etry` but no `end:` label doesn't compile. Don't `return`
// from the middle of one: that skips the cleanup.

/// if the `Error` [expr] failed, stores it in `eret` with `file:line` added as
/// context and jumps to `end`. [expr] is evaluated exactly once
#define _ECAT2(a, b) a##b
#define _ECAT(a, b) _ECAT2(a, b)
#define etry(expr) _etry_impl(_ECAT(_etry_e_, __LINE__), expr)
#define _etry_impl(v, expr)                                                    \
  do {                                                                         \
    Error v = (expr);                                                          \
    if (etry_check_(&v, __FILE__, __LINE__)) {                                 \
      eret = v;                                                                \
      goto end;                                                                \
    }                                                                          \
  } while (0)

/// like [etry], also adding a printf-style message: `etry2(f(), "while %s", s)`
#define etry2(...) _etry2_impl(_ECAT(_etry_e_, __LINE__), __VA_ARGS__)
#define _etry2_impl(v, expr, ...)                                              \
  do {                                                                         \
    Error v = (expr);                                                          \
    if (etry_check_(&v, __FILE__, __LINE__)) {                                 \
      ectx(&v, __VA_ARGS__);                                                   \
      eret = v;                                                                \
      goto end;                                                                \
    }                                                                          \
  } while (0)

// ---- defer ----------------------------------------------------------------
//
// Register cleanup right after acquiring something; it runs at `end`, last in
// first out, and only for what was actually acquired:
//
//   Error eret = eok();
//   EDEFERS(4);                        // room for 4, declared before any etry
//   FILE *f;
//   etry(io_open(&f, path, "r"));
//   edefer(io_close, f);               // returns Error: joined into eret
//   char *buf = malloc(n);
//   edefer_void(free, buf);            // returns void
//   edefer_void(array_free0_raw, &a.raw, String_free2);   // any args
//   ...
// end:
//   erundefers();
//   return eret;
//
// `edefer(fn, args...)` stores `fn` and its arguments NOW (up to 6) and calls
// `fn(args...)` at erundefers(). The call is type-checked where you write it
// (wrong arity or argument types are compile errors), and an `edefer` function
// must return `Error`. Arguments must be integers, `bool`s, pointers or
// function pointers: floats, doubles and structs are compile errors.
//
// `fn` must be a real function, not a macro: for a typed macro API defer its
// `_raw` function or a small wrapper.
//
// Portability: the stored call goes through a cast function pointer taking
// word-sized arguments. The C standard doesn't define that, but it works on the
// 64-bit ABIs (x86-64, aarch64) for the argument kinds allowed above. It needs
// a 64-bit target (a static assert says so). clang's -fsanitize=function
// reports it.

#define EDEFER_MAXARGS 6

typedef struct EDefer {
  void (*fn)(void); // really the user's function; cast back in edefers_run_
  uintptr_t args[EDEFER_MAXARGS];
  unsigned char nargs;
  bool isError; // returns Error (joined into eret) rather than void
} EDefer;

/// room for [cap] deferred calls in this function
#define EDEFERS(cap)                                                           \
  EDefer edefers_[cap];                                                        \
  size_t edeferN_ = 0

/// `edefer(fn, args...)`: fn returns `Error`, which is `ejoin`ed into `eret`
#define edefer(...) EDEFER_DISPATCH_(1, ECHK_ERR_, __VA_ARGS__)
/// `edefer_void(fn, args...)`: fn returns `void`, e.g. `free`
#define edefer_void(...) EDEFER_DISPATCH_(0, ECHK_NONE_, __VA_ARGS__)
/// runs the deferred calls, newest first, and empties the stack. Put it after
/// `end:`
#define erundefers() edefers_run_(edefers_, &edeferN_, &eret)

void edefer_push_(EDefer *stack, size_t cap, size_t *n, void (*fn)(void),
                  bool isError, unsigned nargs, const uintptr_t *args);
void edefers_run_(EDefer *stack, size_t *n, Error *eret);

// ---- the machinery ----
void edefer_arg_not_allowed_(void); // never defined: selecting it is an error
void edefer_must_return_error_(void);
#define EWORD_(x)                                                              \
  ((uintptr_t)(x) + _Generic((x), float: edefer_arg_not_allowed_,              \
                             double: edefer_arg_not_allowed_,                  \
                             long double: edefer_arg_not_allowed_,             \
                             default: 0))
// `edefer`: the call must return Error. (`edefer_void` can't check its return
// type portably)
#define ECHK_ERR_(call) (void)_Generic((call), Error: 0)
#define ECHK_NONE_(call) (void)0

#define EDEFER_0(ISERR, CHK, f) \
  do { \
    _Static_assert(sizeof(long long) <= sizeof(uintptr_t), \
                   "edefer needs a 64-bit target"); \
    (void)sizeof((f(), 0)); \
    CHK(f()); \
    const uintptr_t w_[] = {0}; \
    edefer_push_(edefers_, sizeof edefers_ / sizeof *edefers_, &edeferN_, \
                 (void (*)(void))(f), ISERR, 0, w_); \
  } while (0)
#define EDEFER_1(ISERR, CHK, f, a1) \
  do { \
    _Static_assert(sizeof(long long) <= sizeof(uintptr_t), \
                   "edefer needs a 64-bit target"); \
    (void)sizeof((f(a1), 0)); \
    CHK(f(a1)); \
    const uintptr_t w_[] = {EWORD_(a1)}; \
    edefer_push_(edefers_, sizeof edefers_ / sizeof *edefers_, &edeferN_, \
                 (void (*)(void))(f), ISERR, 1, w_); \
  } while (0)
#define EDEFER_2(ISERR, CHK, f, a1, a2) \
  do { \
    _Static_assert(sizeof(long long) <= sizeof(uintptr_t), \
                   "edefer needs a 64-bit target"); \
    (void)sizeof((f(a1, a2), 0)); \
    CHK(f(a1, a2)); \
    const uintptr_t w_[] = {EWORD_(a1), EWORD_(a2)}; \
    edefer_push_(edefers_, sizeof edefers_ / sizeof *edefers_, &edeferN_, \
                 (void (*)(void))(f), ISERR, 2, w_); \
  } while (0)
#define EDEFER_3(ISERR, CHK, f, a1, a2, a3) \
  do { \
    _Static_assert(sizeof(long long) <= sizeof(uintptr_t), \
                   "edefer needs a 64-bit target"); \
    (void)sizeof((f(a1, a2, a3), 0)); \
    CHK(f(a1, a2, a3)); \
    const uintptr_t w_[] = {EWORD_(a1), EWORD_(a2), EWORD_(a3)}; \
    edefer_push_(edefers_, sizeof edefers_ / sizeof *edefers_, &edeferN_, \
                 (void (*)(void))(f), ISERR, 3, w_); \
  } while (0)
#define EDEFER_4(ISERR, CHK, f, a1, a2, a3, a4) \
  do { \
    _Static_assert(sizeof(long long) <= sizeof(uintptr_t), \
                   "edefer needs a 64-bit target"); \
    (void)sizeof((f(a1, a2, a3, a4), 0)); \
    CHK(f(a1, a2, a3, a4)); \
    const uintptr_t w_[] = {EWORD_(a1), EWORD_(a2), EWORD_(a3), EWORD_(a4)}; \
    edefer_push_(edefers_, sizeof edefers_ / sizeof *edefers_, &edeferN_, \
                 (void (*)(void))(f), ISERR, 4, w_); \
  } while (0)
#define EDEFER_5(ISERR, CHK, f, a1, a2, a3, a4, a5) \
  do { \
    _Static_assert(sizeof(long long) <= sizeof(uintptr_t), \
                   "edefer needs a 64-bit target"); \
    (void)sizeof((f(a1, a2, a3, a4, a5), 0)); \
    CHK(f(a1, a2, a3, a4, a5)); \
    const uintptr_t w_[] = {EWORD_(a1), EWORD_(a2), EWORD_(a3), EWORD_(a4), EWORD_(a5)}; \
    edefer_push_(edefers_, sizeof edefers_ / sizeof *edefers_, &edeferN_, \
                 (void (*)(void))(f), ISERR, 5, w_); \
  } while (0)
#define EDEFER_6(ISERR, CHK, f, a1, a2, a3, a4, a5, a6) \
  do { \
    _Static_assert(sizeof(long long) <= sizeof(uintptr_t), \
                   "edefer needs a 64-bit target"); \
    (void)sizeof((f(a1, a2, a3, a4, a5, a6), 0)); \
    CHK(f(a1, a2, a3, a4, a5, a6)); \
    const uintptr_t w_[] = {EWORD_(a1), EWORD_(a2), EWORD_(a3), EWORD_(a4), EWORD_(a5), EWORD_(a6)}; \
    edefer_push_(edefers_, sizeof edefers_ / sizeof *edefers_, &edeferN_, \
                 (void (*)(void))(f), ISERR, 6, w_); \
  } while (0)

#define EDEFER_PICK_(_1, _2, _3, _4, _5, _6, _7, _8, NAME, ...) NAME
#define EDEFER_TOO_MANY_(iserr, chk, ...)                                      \
  _Static_assert(0, "edefer takes a function and at most 6 arguments")
#define EDEFER_DISPATCH_(iserr, chk, ...)                                      \
  EDEFER_PICK_(__VA_ARGS__, EDEFER_TOO_MANY_, EDEFER_6, EDEFER_5, EDEFER_4,    \
               EDEFER_3, EDEFER_2, EDEFER_1, EDEFER_0, 0)(iserr, chk,          \
                                                          __VA_ARGS__)

/// used by [etry]: true if [e] failed, after adding `file:line` as context.
/// Note [etry] allocates, which may clobber `errno`: read `e.code` instead
bool etry_check_(Error *e, const char *file, int line);

/// writes `error: <ctx>: ...: <ctx>: <msg>\n` (newest context first, then the
/// original message) to the stdio stream [f]; writes nothing if [e] is ok.
/// Returns the write's own error, if any. Does not free [e]. For a `Writer`
/// (buffers, custom sinks) use `eprint` from io.h
Error efprint(const Error *e, FILE *f);

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

void ejoin(Error *e, Error later) {
  if (e->ok)
    *e = later;
  else
    efree(&later);
}

void edefer_push_(EDefer *stack, size_t cap, size_t *n, void (*fn)(void),
                  bool isError, unsigned nargs, const uintptr_t *args) {
  assert(*n < cap && "too many edefer");
  assert(nargs <= EDEFER_MAXARGS);
  EDefer *d = &stack[(*n)++];
  d->fn = fn;
  d->nargs = (unsigned char)nargs;
  d->isError = isError;
  for (unsigned i = 0; i < nargs; i++)
    d->args[i] = args[i];
}

typedef uintptr_t W_;
#define A_ d.args
#define CALL_(R, ...) ((R (*)(__VA_ARGS__))d.fn)

void edefers_run_(EDefer *stack, size_t *n, Error *eret) {
  while (*n) {
    EDefer d = stack[--*n];
    if (d.isError) {
      Error r;
      switch (d.nargs) {
      case 0: r = CALL_(Error, void)(); break;
      case 1: r = CALL_(Error, W_)(A_[0]); break;
      case 2: r = CALL_(Error, W_, W_)(A_[0], A_[1]); break;
      case 3: r = CALL_(Error, W_, W_, W_)(A_[0], A_[1], A_[2]); break;
      case 4: r = CALL_(Error, W_, W_, W_, W_)(A_[0], A_[1], A_[2], A_[3]); break;
      case 5:
        r = CALL_(Error, W_, W_, W_, W_, W_)(A_[0], A_[1], A_[2], A_[3], A_[4]);
        break;
      default:
        r = CALL_(Error, W_, W_, W_, W_, W_, W_)(A_[0], A_[1], A_[2], A_[3],
                                                 A_[4], A_[5]);
      }
      ejoin(eret, r);
    } else {
      switch (d.nargs) {
      case 0: CALL_(void, void)(); break;
      case 1: CALL_(void, W_)(A_[0]); break;
      case 2: CALL_(void, W_, W_)(A_[0], A_[1]); break;
      case 3: CALL_(void, W_, W_, W_)(A_[0], A_[1], A_[2]); break;
      case 4: CALL_(void, W_, W_, W_, W_)(A_[0], A_[1], A_[2], A_[3]); break;
      case 5:
        CALL_(void, W_, W_, W_, W_, W_)(A_[0], A_[1], A_[2], A_[3], A_[4]);
        break;
      default:
        CALL_(void, W_, W_, W_, W_, W_, W_)(A_[0], A_[1], A_[2], A_[3], A_[4],
                                            A_[5]);
      }
    }
  }
}
#undef CALL_
#undef A_

bool etry_check_(Error *e, const char *file, int line) {
  if (e->ok)
    return false;
  ectx(e, "%s:%d", file, line);
  return true;
}

Error efprint(const Error *e, FILE *f) {
  if (e->ok)
    return eok();
  if (!f)
    return enew(EINVAL, "file is null");
  if (fputs("error: ", f) == EOF)
    return eerrno("unable to write error");
  for (const ErrCtx *c = e->ctx; c; c = c->next)
    if (fprintf(f, "%s: ", c->msg) < 0)
      return eerrno("unable to write error");
  if (fprintf(f, "%s\n", e->msg) < 0)
    return eerrno("unable to write error");
  return eok();
}
#endif // MOLKIE_ERR_IMPL

// Tests: compiled where MTEST (all modules) or MOLKIE_ERR_TEST (just this one)
// is defined. Needs MOLKIE_ERR_IMPL in the
// same program. Printing the error chain with `eprint` is tested in io.h.
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
  Error eret = eok();
  etry(err_fails());
end:
  return eret;
}
static Error err_try2_user(void) {
  Error eret = eok();
  etry2(err_fails(), "while %s", "testing");
end:
  return eret;
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

static int err_log[16];
static size_t err_nlog;
static void err_note(int v) { err_log[err_nlog++] = v; }
static Error err_closeA(int *a) {
  err_note(*a);
  return eok();
}
static Error err_closeFail(int *a) {
  err_note(*a);
  return enew(9, "close failed");
}
static void err_voidNote(int *a) { err_note(*a); }

static void err_ejoin(void) {
  Error e = eok();
  ejoin(&e, eok());
  assert(e.ok);
  ejoin(&e, enew(1, "first")); // an ok error adopts the later one
  assert(!e.ok && e.code == 1);
  Error later = enew(2, "second");
  ectx(&later, "ctx");
  ejoin(&e, later); // the first error wins; later one's ctx is freed
  assert(e.code == 1 && !strcmp(e.msg, "first"));
  efree(&e);
}

// opens "1" then "2" then fails: both are closed, 2 first
static Error err_deferUser(int failAt, Error (*closer)(int *)) {
  Error eret = eok();
  EDEFERS(3);
  int a = 1, b = 2, c = 3;
  err_note(10);
  edefer(closer, &a);
  if (failAt == 1)
    etry(err_fails());
  edefer(closer, &b);
  edefer_void(err_voidNote, &c);
  if (failAt == 2)
    etry(err_fails());
  err_note(11);
end:
  erundefers();
  return eret;
}

// every callee logs one value, so the order and the arguments can be checked
static void err_add3(int a, long b, const char *c) {
  err_note(a + (int)b + (int)strlen(c));
}
static void err_add6(int a, int b, int c, int d, int e, int f) {
  err_note(a + b + c + d + e + f);
}
static void err_nothing(void) { err_note(100); }
static void err_negs(signed char a, short b, _Bool c, unsigned char d) {
  err_note(a + b + c + d);
}
static void err_fnarg(void (*cb)(int *), int *p) { cb(p); }
static void err_setOne(int *p) { err_note(*p + 7); }
static Error err_ret3(int a, void *p, long b) {
  err_note(a + (p != NULL) + (int)b);
  return eok();
}

static Error err_deferArgs(void) {
  Error eret = eok();
  EDEFERS(8);
  int slot = 0;
  edefer_void(err_nothing);                   // no args
  edefer_void(err_add3, 1, 2L, "abc");        // mixed kinds: 1 + 2 + 3
  edefer_void(err_add6, 1, 2, 3, 4, 5, 6);    // the maximum: 21
  edefer_void(err_negs, (signed char)-3, (short)-4, 1, 200); // 194
  edefer_void(err_fnarg, err_setOne, &slot);  // a function pointer argument
  edefer(err_ret3, 5, &slot, -2L);            // Error-returning: 5 + 1 - 2
  slot = 10;                                  // pointer arg: sees 10
  etry(eok());
end:
  erundefers();
  return eret;
}

static void err_defer_args(void) {
  err_nlog = 0;
  Error e = err_deferArgs();
  assert(e.ok);
  ASSERT_EQ(err_nlog, 6);
  // newest first; slot is a pointer, so err_setOne sees the final 10
  int want[] = {4, 17, 194, 21, 6, 100};
  for (size_t i = 0; i < 6; i++)
    ASSERT_EQ(err_log[i], want[i]);
}

static void err_defer(void) {
  err_nlog = 0;
  Error e = err_deferUser(0, err_closeA); // runs at the end, newest first
  assert(e.ok);
  ASSERT_EQ(err_nlog, 5);
  int want[] = {10, 11, 3, 2, 1};
  for (size_t i = 0; i < 5; i++)
    ASSERT_EQ(err_log[i], want[i]);

  err_nlog = 0;
  e = err_deferUser(1, err_closeA); // fails after one defer: only that runs
  assert(!e.ok && e.code == 3);
  ASSERT_EQ(err_nlog, 2);
  ASSERT_EQ(err_log[1], 1);
  efree(&e);

  err_nlog = 0;
  e = err_deferUser(2, err_closeA); // both ran, even though it failed
  assert(!e.ok && e.code == 3);
  ASSERT_EQ(err_nlog, 4);
  efree(&e);
}

static void err_defer_errors(void) {
  err_nlog = 0;
  Error e = err_deferUser(0, err_closeFail); // a failing close surfaces...
  assert(!e.ok && e.code == 9);
  ASSERT_EQ(err_nlog, 5); // ...but every deferred call still ran
  efree(&e);

  err_nlog = 0;
  e = err_deferUser(2, err_closeFail); // ...and never hides the first error
  assert(!e.ok && e.code == 3);
  efree(&e);
}

static void err_fprint(void) {
  FILE *f = tmpfile();
  assert(f);
  Error ok = eok();
  assert(efprint(&ok, f).ok);
  Error e = enew(2, "boom");
  ectx(&e, "reading %s", "a.txt");
  ectx(&e, "step %d", 2);
  assert(efprint(&e, f).ok);
  efree(&e);
  rewind(f);
  char line[64] = {0};
  assert(fgets(line, sizeof line, f));
  assert(!strcmp(line, "error: step 2: reading a.txt: boom\n"));
  assert(!fgets(line, sizeof line, f)); // nothing for the ok error
  fclose(f);

  Error bad = enew(1, "x");
  assert(efprint(&bad, NULL).code == EINVAL);
}

static int err_tests(void) {
  static const TestCase tests[] = {
      T(err_ok),  T(err_new), T(err_errno),
      T(err_truncate), T(err_errno_truncate), T(err_ctx), T(err_try), T(err_ejoin), T(err_defer), T(err_defer_args), T(err_defer_errors), T(err_fprint),
  };
  size_t n = sizeof tests / sizeof *tests, passed = 0;
  for (size_t i = 0; i < n; i++)
    passed += run_in_child(&tests[i]);
  printf("err: %zu/%zu passed\n", passed, n);
  return passed == n;
}

#endif // MTEST
