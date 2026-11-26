#ifndef MOLKIE_TEST_H
#define MOLKIE_TEST_H

#include <string.h>

// run_in_child needs fileno/strsignal/fork: build with -D_POSIX_C_SOURCE=200809L
// (or a gnu11 mode)
#if !defined(_POSIX_C_SOURCE)
#error "test.h needs POSIX: compile with -D_POSIX_C_SOURCE=200809L"
#endif

#if defined(__GNUC__) || defined(__clang__)
#define MOLKIE_TEST_UNUSED __attribute__((unused))
#else
#define MOLKIE_TEST_UNUSED
#endif

/// seconds before a test that hangs is killed (reported as a CRASH by SIGALRM)
#ifndef MOLKIE_TEST_TIMEOUT
#define MOLKIE_TEST_TIMEOUT 30
#endif

typedef struct {
  const char *name;
  void (*fn)(void);
} TestCase;

#define T(n) {#n, n}

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

// Run one test in a child process. Returns 1 if it passed.
MOLKIE_TEST_UNUSED static int run_in_child(const TestCase *t) {
  fflush(NULL); // don't duplicate buffered output into the child
  // capture the child's stdout+stderr; replay it only if the test fails
  FILE *out = tmpfile();
  if (!out) {
    perror("tmpfile");
    return 0;
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    fclose(out);
    return 0;
  }
  if (pid == 0) {
    int fd = fileno(out);
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0)
      _exit(127);
    setvbuf(stdout, NULL, _IONBF, 0); // keep output printed before a crash
    alarm(MOLKIE_TEST_TIMEOUT);
    t->fn();
    fflush(NULL);
    _exit(0); // fn returned without a failed assert
  }

  int status;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    fclose(out);
    return 0;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    printf("ok    %s\n", t->name);
    fflush(stdout);
    fclose(out);
    return 1;
  }
  if (WIFSIGNALED(status))
    printf("CRASH %s (signal %d: %s)\n", t->name, WTERMSIG(status),
           strsignal(WTERMSIG(status)));
  else
    printf("FAIL  %s\n", t->name);
  fflush(stdout); // before the replay below goes to stderr

  // replay captured output, indented, to stderr
  rewind(out);
  char line[1024];
  int atStart = 1; // fgets can split a long line: prefix only real line starts
  while (fgets(line, sizeof line, out)) {
    fprintf(stderr, "%s%s", atStart ? "    | " : "", line);
    atStart = line[strlen(line) - 1] == '\n';
  }
  if (!atStart)
    fputc('\n', stderr);
  fclose(out);
  return 0;
}

/// integer-only comparison. Each argument is evaluated exactly once, a failure
/// stops the test even under -DNDEBUG, and it is a single statement (safe in
/// an un-braced if/else)
#define ASSERT_EQ(a, b)                                                        \
  do {                                                                         \
    long long _ae_a = (long long)(a), _ae_b = (long long)(b);                  \
    if (_ae_a != _ae_b) {                                                      \
      fprintf(stdout, "%s != %s\nleft  : %lld\nright : %lld\n", #a, #b,        \
              _ae_a, _ae_b);                                                   \
      fflush(stdout);                                                          \
      abort();                                                                 \
    }                                                                          \
  } while (0)

#endif // MOLKIE_TEST_H
