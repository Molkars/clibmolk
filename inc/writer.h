#ifndef MOLKIE_WRITER_H
#define MOLKIE_WRITER_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "attr.h"
#include "err.h"

/// must be called if you want to use stdoutW & stderrW
void Writer_init(void);

/// Callbacks return `eok()`, or an error. They do not report how much was
/// written.
typedef Error WriterFormatFn(void *this, const char *fmt, va_list argp)
    PRINTF_LIKE(2, 0);

typedef struct Writer Writer;
struct Writer {
  WriterFormatFn *format;
  Error (*putc)(void *this, int c);
  Error (*puts)(void *this, const char *s);
  void *this;
};

/// All of these return `eok()`, or the error:
/// ```
/// Error e = Wprintf(w, "pid=%zu", pid);
/// if (!e.ok)
///   return e;
/// ```
Error Wprintf(Writer *w, const char *fmt, ...) PRINTF_LIKE(2, 3);
Error Wvprintf(Writer *w, const char *fmt, va_list argp) PRINTF_LIKE(2, 0);
Error Wputc(Writer *w, int c);
Error Wputs(Writer *w, const char *s);

/// A fixed buffer you can write into. Invariant: `buf` is always
/// NUL-terminated and `written < cap`, so it is always a valid C string. A
/// write that doesn't fully fit is truncated and fails with `ENOSPC`.
typedef struct {
  char *buf;
  size_t cap;
  size_t written;
} Scratch;

Scratch Scratch_init(char *buf, size_t cap);
void Scratch_reset(Scratch *s);
Writer Scratch_writer(Scratch *s);

/// writes to [f]. failures carry the `errno` text
Writer FILE_writer(FILE *f);

extern Writer stdoutW;
extern Writer stderrW;

/// writes `error: <ctx>: ...: <ctx>: <msg>\n` (newest context first, then the
/// original message) to [w]; writes nothing if [e] is ok. Returns the writer's
/// own error, if it has one. This is the one place err and writer meet: err.h
/// has no Writer, writer.h owns `eprint`
Error eprint(Error *e, Writer *w);

#endif // MOLKIE_WRITER_H

#if defined(MOLKIE_WRITER_IMPL) && !defined(MOLKIE_WRITER_IMPL_DONE)
#define MOLKIE_WRITER_IMPL_DONE
#include <assert.h>
#include <errno.h>
#include <string.h>

Writer stdoutW;
Writer stderrW;

Error Wprintf(Writer *w, const char *fmt, ...) {
  assert(w && w->format && "writer is not initialised (Writer_init?)");
  va_list argp;
  va_start(argp, fmt);
  Error e = w->format(w->this, fmt, argp);
  va_end(argp);
  return e;
}

Error Wvprintf(Writer *w, const char *fmt, va_list argp) {
  assert(w && w->format && "writer is not initialised (Writer_init?)");
  return w->format(w->this, fmt, argp);
}

Error Wputc(Writer *w, int c) {
  assert(w && w->putc && "writer is not initialised (Writer_init?)");
  return w->putc(w->this, c);
}

Error Wputs(Writer *w, const char *s) {
  assert(w && w->puts && "writer is not initialised (Writer_init?)");
  return w->puts(w->this, s);
}

PRINTF_LIKE(2, 0)
static Error _fileFormat(void *p, const char *fmt, va_list argp) {
  if (vfprintf((FILE *)p, fmt, argp) < 0)
    return eerrno("unable to write to file");
  return eok();
}

static Error _filePutc(void *p, int c) {
  if (fputc(c, (FILE *)p) == EOF)
    return eerrno("unable to write to file");
  return eok();
}

static Error _filePuts(void *p, const char *s) {
  if (fputs(s, (FILE *)p) == EOF)
    return eerrno("unable to write to file");
  return eok();
}

Writer FILE_writer(FILE *f) {
  return (Writer){
      .format = _fileFormat, .putc = _filePutc, .puts = _filePuts, .this = f};
}

void Writer_init(void) {
  stdoutW = FILE_writer(stdout);
  stderrW = FILE_writer(stderr);
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

static Error _scratchFull(Scratch *s) {
  return enew(ENOSPC, "scratch buffer full (%zu bytes)", s->cap - 1);
}

PRINTF_LIKE(2, 0)
static Error _scratchFormat(void *p, const char *fmt, va_list argp) {
  Scratch *s = p;
  size_t avail = s->cap - s->written; // >= 1 by invariant
  int n = vsnprintf(s->buf + s->written, avail, fmt, argp);
  if (n < 0) {
    s->buf[s->written] = 0; // drop whatever vsnprintf left behind
    return enew(EINVAL, "unable to format \"%s\"", fmt);
  }
  if ((size_t)n >= avail) { // truncated
    s->written = s->cap - 1;
    return _scratchFull(s);
  }
  s->written += (size_t)n;
  return eok();
}

static Error _scratchPutc(void *p, int c) {
  Scratch *s = p;
  if (c == 0) // would end the C string early; written must equal strlen
    return enew(EINVAL, "cannot write a NUL byte to a scratch buffer");
  if (s->written + 1 >= s->cap) // no room for char + NUL
    return _scratchFull(s);
  s->buf[s->written++] = (char)c;
  s->buf[s->written] = 0;
  return eok();
}

static Error _scratchPuts(void *p, const char *str) {
  Scratch *s = p;
  size_t avail = s->cap - s->written - 1; // reserve NUL
  size_t len = strlen(str);
  int ok = len <= avail;
  size_t n = ok ? len : avail;
  memcpy(s->buf + s->written, str, n);
  s->written += n;
  s->buf[s->written] = 0;
  return ok ? eok() : _scratchFull(s);
}

Writer Scratch_writer(Scratch *s) {
  return (Writer){
      .format = _scratchFormat,
      .putc = _scratchPutc,
      .puts = _scratchPuts,
      .this = s,
  };
}

Error eprint(Error *e, Writer *w) {
  if (e->ok)
    return eok();
  Error r = Wprintf(w, "error: ");
  for (ErrCtx *c = e->ctx; r.ok && c; c = c->next)
    r = Wprintf(w, "%s: ", c->msg);
  if (r.ok)
    r = Wprintf(w, "%s\n", e->msg);
  return r;
}
#endif // MOLKIE_WRITER_IMPL

// Tests: compiled where MTEST (all modules) or MOLKIE_WRITER_TEST (just this one)
// is defined. Needs MOLKIE_WRITER_IMPL and
// MOLKIE_ERR_IMPL in the same program.
#if (defined(MTEST) || defined(MOLKIE_WRITER_TEST)) && !defined(MOLKIE_WRITER_TESTS)
#define MOLKIE_WRITER_TESTS
#include <errno.h>
#include <string.h>

#include "test.h"

static void writer_scratch(void) {
  char buf[16];
  Scratch s = Scratch_init(buf, sizeof buf);
  Writer w = Scratch_writer(&s);

  assert(Wprintf(&w, "a=%d", 1).ok);
  assert(Wputc(&w, ',').ok);
  assert(Wputs(&w, "b").ok);
  assert(Wprintf(&w, "%s", "").ok); // writing nothing is success
  assert(!strcmp(buf, "a=1,b"));

  Scratch_reset(&s);
  assert(!strcmp(buf, ""));
}

static void writer_scratch_full(void) {
  char buf[8];
  Scratch s = Scratch_init(buf, sizeof buf);
  Writer w = Scratch_writer(&s);

  Error e = Wprintf(&w, "%s", "0123456789"); // truncated
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOSPC);
  assert(!strcmp(buf, "0123456")); // still a valid C string

  e = Wputc(&w, 'x');
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOSPC);

  e = Wputs(&w, "xy");
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOSPC);
  assert(strstr(e.msg, "scratch buffer full"));
  assert(!strcmp(buf, "0123456"));
}

static void writer_scratch_nul(void) {
  char buf[16];
  Scratch s = Scratch_init(buf, sizeof buf);
  Writer w = Scratch_writer(&s);
  assert(Wputc(&w, 'a').ok);
  Error e = Wputc(&w, 0);
  assert(!e.ok);
  ASSERT_EQ(e.code, EINVAL);
  assert(Wputs(&w, "bc").ok);
  assert(!strcmp(buf, "abc"));
  ASSERT_EQ(s.written, strlen(buf));
}

static void writer_file(void) {
  FILE *f = tmpfile();
  assert(f);
  Writer w = FILE_writer(f);
  assert(Wprintf(&w, "n=%d", 5).ok);
  assert(Wputc(&w, '!').ok);
  assert(Wputs(&w, "?").ok);
  rewind(f);
  char line[32] = {0};
  assert(fgets(line, sizeof line, f));
  assert(!strcmp(line, "n=5!?"));
  fclose(f);
}

static void writer_file_error(void) {
  FILE *f = fopen("/dev/full", "w"); // every write fails with ENOSPC
  assert(f);
  setvbuf(f, NULL, _IONBF, 0);
  Writer w = FILE_writer(f);

  Error e = Wprintf(&w, "x");
  assert(!e.ok);
  ASSERT_EQ(e.code, ENOSPC);
  assert(strstr(e.msg, strerror(ENOSPC)));
  assert(!Wputc(&w, 'x').ok);
  assert(!Wputs(&w, "x").ok);
  fclose(f);
}

static void writer_eprint(void) {
  char buf[64];
  Scratch s = Scratch_init(buf, sizeof buf);
  Writer w = Scratch_writer(&s);

  Error ok = eok();
  assert(eprint(&ok, &w).ok);
  assert(!strcmp(buf, "")); // nothing for success

  Error e = enew(1, "boom");
  assert(eprint(&e, &w).ok);
  assert(!strcmp(buf, "error: boom\n"));
}

static void writer_eprint_ctx(void) {
  char buf[128];
  Scratch s = Scratch_init(buf, sizeof buf);
  Writer w = Scratch_writer(&s);

  Error e = enew(2, "boom");
  ectx(&e, "reading %s", "a.txt");
  ectx(&e, "step %d", 2);
  assert(eprint(&e, &w).ok);
  assert(!strcmp(buf, "error: step 2: reading a.txt: boom\n"));
  efree(&e);
}

static int writer_tests(void) {
  static const TestCase tests[] = {
      T(writer_scratch),
      T(writer_scratch_full),
      T(writer_scratch_nul),
      T(writer_file),
      T(writer_file_error),
      T(writer_eprint),
      T(writer_eprint_ctx),
  };
  size_t n = sizeof tests / sizeof *tests, passed = 0;
  for (size_t i = 0; i < n; i++)
    passed += run_in_child(&tests[i]);
  printf("writer: %zu/%zu passed\n", passed, n);
  return passed == n;
}

#endif // MTEST
