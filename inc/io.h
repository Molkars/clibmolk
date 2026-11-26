#ifndef MOLKIE_IO_H
#define MOLKIE_IO_H

#include "err.h"
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>

/// opens [path] with fopen [mode] and stores the stream in [*out]. [*out] is
/// null on failure. Retries on EINTR; a bad [mode] is EINVAL
Error io_open(FILE **out, const char *path, const char *mode);
/// flushes and closes [f], reporting any deferred write error (e.g. ENOSPC).
/// [f] is closed even when this fails, so never close it again. A null [f] is
/// EINVAL
Error io_close(FILE *f);
/// writes all [bufsz] bytes of [buf] to [f], retrying on EINTR. [nWritten] (if
/// non-null) is zeroed first and kept up to date as bytes go out, so on
/// failure it says how much of [buf] was written. A [bufsz] of 0 is a no-op
/// that does not look at the stream
Error io_write(FILE *f, const char *buf, size_t bufsz, size_t *nWrote);
/// reads up to [bufsz] bytes into [buf] and stores the count in [nRead]. A
/// short count (including 0) with an ok result means end of file; failures
/// return an errno code. [nRead] (if non-null) is zeroed first and kept up to
/// date as bytes come in, so on failure it says how much of [buf] is valid. A
/// [bufsz] of 0 is a no-op that does not look at the stream
Error io_read(FILE *f, char *buf, size_t bufsz, size_t *nRead);
/// copies [src] to [dst] until EOF. Does not flush [dst]. If reading fails
/// midway, the bytes read before the failure are still written to [dst]
Error io_forward(FILE *dst, FILE *src);
/// flushes [f]; a null [f] is EINVAL (not "flush everything")
Error io_flush(FILE *f);

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
  return eok();
}

#endif // MOLKIE_IO_IMPL

// Tests: compiled where MTEST (all modules) or MOLKIE_IO_TEST (just this one)
// is defined. Needs MOLKIE_IO_IMPL and
// MOLKIE_ERR_IMPL in the same program.
//
// Extension: if writer.h was included first, a failing test prints the full
// error chain through [eprint]; without it, just the message.
#if (defined(MTEST) || defined(MOLKIE_IO_TEST)) && !defined(MOLKIE_IO_TESTS)
#define MOLKIE_IO_TESTS
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"

static void io_test_report(Error *e) {
#ifdef MOLKIE_WRITER_H
  Writer w = FILE_writer(stdout);
  eprint(e, &w);
#else
  printf("error: %s\n", e->msg);
#endif
}

// A test body returns an Error (so it can use etry); the wrapper fails the
// test and prints the error chain if the body failed.
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
    if (!(cond))                                                               \
      return enew(EINVAL, "expected: %s", #cond);                              \
  } while (0)

// forwards [n] bytes of a repeating pattern through a tmpfile and checks them
static Error io_roundtrip(size_t n) {
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
  return eok();
}

IO_TEST(io_empty) { return io_roundtrip(0); }
IO_TEST(io_small) { return io_roundtrip(10); }
IO_TEST(io_exact_buf) { return io_roundtrip(BUFSIZ); }
IO_TEST(io_multi_chunk) { return io_roundtrip(BUFSIZ * 3 + 17); }

IO_TEST(io_forward_null_args) {
  FILE *f = tmpfile();
  expect(f);
  etry(expect_fail(io_forward(NULL, f), EINVAL));
  etry(expect_fail(io_forward(f, NULL), EINVAL));
  fclose(f);
  return eok();
}

IO_TEST(io_forward_read_error) {
  // a write-only stream can't be read
  FILE *wo = fopen("/dev/null", "w"), *dst = tmpfile();
  expect(wo && dst);
  etry(expect_fail(io_forward(dst, wo), EBADF));
  fclose(wo);
  fclose(dst);
  return eok();
}

IO_TEST(io_forward_write_error) {
  // a read-only dst can't be written
  FILE *src = tmpfile(), *dst = fopen("/dev/null", "r");
  expect(src && dst);
  fputs("hello", src);
  rewind(src);
  etry(expect_fail(io_forward(dst, src), EBADF));
  fclose(src);
  fclose(dst);
  return eok();
}

IO_TEST(io_forward_error_has_location) {
  // etry should have added file:line context to the failure
  FILE *f = tmpfile();
  expect(f);
  Error e = io_forward(f, NULL);
  expect(!e.ok && e.ctx);
  efree(&e);
  fclose(f);
  return eok();
}

IO_TEST(io_write_ok) {
  FILE *f = tmpfile();
  expect(f);
  size_t n = 99;
  errno = EBADF; // a stale errno must not leak into a successful write
  etry(io_write(f, "abc", 3, &n));
  expect(n == 3);
  etry(io_write(f, NULL, 0, &n));
  expect(n == 0);
  etry(io_write(f, "def", 3, NULL)); // nWritten is optional
  etry(io_flush(f));
  rewind(f);
  char b[7] = {0};
  expect(fread(b, 1, 6, f) == 6 && !strcmp(b, "abcdef"));
  fclose(f);
  return eok();
}

IO_TEST(io_write_errors) {
  size_t n = 99;
  etry(expect_fail(io_write(NULL, "x", 1, &n), EINVAL));
  expect(n == 0);
  FILE *f = tmpfile();
  expect(f);
  n = 99;
  etry(expect_fail(io_write(f, NULL, 1, &n), EINVAL));
  expect(n == 0);
  fclose(f);
  FILE *ro = fopen("/dev/null", "r");
  expect(ro);
  n = 99;
  etry(expect_fail(io_write(ro, "x", 1, &n), EBADF));
  expect(n == 0);
  fclose(ro);
  return eok();
}

IO_TEST(io_write_full_device) {
  FILE *f = fopen("/dev/full", "w");
  expect(f);
  setvbuf(f, NULL, _IONBF, 0); // fail at write time, not at flush
  size_t n = 99;
  etry(expect_fail(io_write(f, "x", 1, &n), ENOSPC));
  expect(n == 0);
  fclose(f);
  return eok();
}

IO_TEST(io_read_ok) {
  FILE *f = tmpfile();
  expect(f);
  fputs("hello", f);
  rewind(f);
  char b[16];
  size_t n = 99;
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
  return eok();
}

IO_TEST(io_read_errors) {
  char b[4];
  size_t n = 99;
  etry(expect_fail(io_read(NULL, b, sizeof b, &n), EINVAL));
  expect(n == 0);
  FILE *f = tmpfile();
  expect(f);
  n = 99;
  etry(expect_fail(io_read(f, NULL, 1, &n), EINVAL));
  expect(n == 0);
  fclose(f);
  FILE *wo = fopen("/dev/null", "w");
  expect(wo);
  n = 99;
  etry(expect_fail(io_read(wo, b, sizeof b, &n), EBADF));
  expect(n == 0);
  fclose(wo);
  return eok();
}

IO_TEST(io_flush_cases) {
  etry(expect_fail(io_flush(NULL), EINVAL));
  FILE *f = tmpfile();
  expect(f);
  errno = EBADF; // stale
  etry(io_flush(f));
  fclose(f);
  // a buffered write to a full device only fails at flush
  FILE *full = fopen("/dev/full", "w");
  expect(full);
  etry(io_write(full, "x", 1, NULL));
  etry(expect_fail(io_flush(full), ENOSPC));
  fclose(full);
  return eok();
}

IO_TEST(io_open_close_roundtrip) {
  char path[] = "/tmp/io_test_XXXXXX";
  int fd = mkstemp(path);
  expect(fd >= 0);
  close(fd);

  FILE *f = (FILE *)1;
  etry(io_open(&f, path, "w"));
  expect(f);
  etry(io_write(f, "hello", 5, NULL));
  etry(io_close(f));

  char b[8] = {0};
  size_t n;
  etry(io_open(&f, path, "r"));
  etry(io_read(f, b, sizeof b, &n));
  etry(io_close(f));
  remove(path);
  expect(n == 5 && !strcmp(b, "hello"));
  return eok();
}

IO_TEST(io_open_errors) {
  FILE *f = (FILE *)1;
  etry(expect_fail(io_open(&f, "/nonexistent/dir/file", "r"), ENOENT));
  expect(f == NULL);
  f = (FILE *)1;
  etry(expect_fail(io_open(&f, "/dev/null", "bogus"), EINVAL));
  expect(f == NULL);
  etry(expect_fail(io_open(&f, NULL, "r"), EINVAL));
  etry(expect_fail(io_open(&f, "/dev/null", NULL), EINVAL));
  etry(expect_fail(io_open(NULL, "/dev/null", "r"), EINVAL));
  return eok();
}

IO_TEST(io_open_error_names_path) {
  FILE *f;
  Error e = io_open(&f, "/nonexistent/x", "r");
  expect(!e.ok && strstr(e.msg, "/nonexistent/x"));
  efree(&e);
  return eok();
}

IO_TEST(io_close_cases) {
  etry(expect_fail(io_close(NULL), EINVAL));
  // buffered data that can't be written is reported at close
  FILE *full;
  etry(io_open(&full, "/dev/full", "w"));
  etry(io_write(full, "x", 1, NULL));
  etry(expect_fail(io_close(full), ENOSPC));
  return eok();
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
  };
  size_t n = sizeof tests / sizeof *tests, passed = 0;
  for (size_t i = 0; i < n; i++)
    passed += run_in_child(&tests[i]);
  printf("io: %zu/%zu passed\n", passed, n);
  return passed == n;
}

#endif // MTEST
