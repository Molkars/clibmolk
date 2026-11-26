// Compile-time checks for the typed array/btree API. Never linked or run.
//
// Everything outside a `BAD_*` block must compile cleanly. Each `BAD_*` block
// is a misuse that must FAIL to compile; `just typecheck` builds every one
// with -DBAD_<name> and complains if any of them is accepted.
#include "array.h"
#include "btree.h"
#include "err.h"
#include <stdlib.h>

#include "cstr.h"
#include "io.h"

typedef array_t(int) Ints;
typedef array_t(double) Doubles;
typedef btree_str_t(int) Ages;
typedef btree_t(int, double) IntMap;

static Error ret_error(int a, void *p) {
  (void)a, (void)p;
  return eok();
}
static void ret_void(int a, const char *s) { (void)a, (void)s; }

// edefer: the call is checked like a normal call
Error good_defer(void) {
  Error eret = eok();
  EDEFERS(4);
  char *buf = malloc(8);
  FILE *f = NULL;
  edefer_void(free, buf);
  edefer(ret_error, 1, buf);
  edefer_void(ret_void, 1, "x");
  edefer(io_close, f);
  etry(eok());
end:
  erundefers();
  return eret;
}

void good(void) {
  Ints a = array_empty(int);
  int i = 1;
  array_push(&a, &i);
  int *p = array_at(&a, 0);
  const int *cp = &i;
  array_push(&a, cp);
  int more[] = {1, 2, 3};
  array_extend2(&a, more, 3);
  Ints b = array_new(8, int);
  array_extend(&a, &b);
  array_grow(&a, 4);
  size_t n = a.len + (size_t)*p;
  (void)n;
  array_free(&a);
  array_free(&b);

  Ages ages = btree_str_empty(int);
  const char *name = "dillon";
  int age = 20, old = 0;
  btree_insert(&ages, &name, &age);
  btree_insert_ex(&ages, &name, &age, &old);
  btree_insert_ex(&ages, &name, &age, NULL);
  int *found = btree_get(&ages, &name);
  (void)found;
  int has = btree_contains(&ages, &name);
  btree_remove_ex(&ages, &name, &old);
  has += btree_remove(&ages, &name);
  size_t sz = btree_size(&ages) + (size_t)has;
  (void)sz;

  Ages other = btree_str_empty(int);
  btree_extend(&ages, &other);
  btree_iter_t(const char *, int) it;
  btree_iter(&it, &ages);
  while (btree_next(&it)) {
    const char *k = *it.item;
    int v = *it.value;
    (void)k, (void)v;
  }
  btree_free(&ages);
  btree_free(&other);

  Error e = io_fprint(stdoutW, "{} {:>4} {}", 1, 2.5, "x");
  e = io_fprint(stdoutW, "plain");
  int parsed;
  double d;
  e = cstr_to("1", &parsed);
  e = cstr_to("1", &d);
  (void)e;
}

#ifdef BAD_ARRAY_PUSH_WRONG_TYPE
void bad(void) {
  Ints a = array_empty(int);
  double d = 1;
  array_push(&a, &d);
}
#endif

#ifdef BAD_ARRAY_PUSH_BY_VALUE
void bad(void) {
  Ints a = array_empty(int);
  array_push(&a, 5);
}
#endif

#ifdef BAD_ARRAY_PUSH_POINTER_TO_POINTER
void bad(void) {
  Ints a = array_empty(int);
  int i = 1;
  int *p = &i;
  array_push(&a, &p);
}
#endif

#ifdef BAD_ARRAY_AT_WRONG_TYPE
void bad(void) {
  Ints a = array_empty(int);
  double *p = array_at(&a, 0);
  (void)p;
}
#endif

#ifdef BAD_ARRAY_EXTEND_MISMATCH
void bad(void) {
  Ints a = array_empty(int);
  Doubles b = array_empty(double);
  array_extend(&a, &b);
}
#endif

#ifdef BAD_ARRAY_EXTEND2_WRONG_TYPE
void bad(void) {
  Ints a = array_empty(int);
  double ds[] = {1, 2};
  array_extend2(&a, ds, 2);
}
#endif

#ifdef BAD_ARRAY_NOT_AN_ARRAY
void bad(void) {
  Ages t = btree_str_empty(int);
  array_free(&t);
}
#endif

#ifdef BAD_BTREE_STRING_KEY_NOT_ADDRESS
void bad(void) {
  Ages t = btree_str_empty(int);
  const char *name = "x";
  btree_contains(&t, name); // needs &name
}
#endif

#ifdef BAD_BTREE_STRING_LITERAL_KEY
void bad(void) {
  Ages t = btree_str_empty(int);
  int v = 1;
  btree_insert(&t, "x", &v);
}
#endif

#ifdef BAD_BTREE_KEY_WRONG_TYPE
void bad(void) {
  IntMap t = btree_empty(int, double, 0);
  long k = 1;
  btree_get(&t, &k);
}
#endif

#ifdef BAD_BTREE_VALUE_WRONG_TYPE
void bad(void) {
  IntMap t = btree_empty(int, double, 0);
  int k = 1, v = 2;
  btree_insert(&t, &k, &v); // value must be a double
}
#endif

#ifdef BAD_BTREE_OLD_WRONG_TYPE
void bad(void) {
  IntMap t = btree_empty(int, double, 0);
  int k = 1, old = 0;
  double v = 2;
  btree_insert_ex(&t, &k, &v, &old);
}
#endif

#ifdef BAD_BTREE_GET_WRONG_TYPE
void bad(void) {
  Ages t = btree_str_empty(int);
  const char *name = "x";
  char *p = btree_get(&t, &name);
  (void)p;
}
#endif

#ifdef BAD_BTREE_EXTEND_MISMATCH
void bad(void) {
  Ages a = btree_str_empty(int);
  IntMap b = btree_empty(int, double, 0);
  btree_extend(&a, &b);
}
#endif

#ifdef BAD_BTREE_ITER_MISMATCH
void bad(void) {
  Ages t = btree_str_empty(int);
  btree_iter_t(int, int) it;
  btree_iter(&it, &t);
}
#endif

#ifdef BAD_BTREE_NOT_A_TREE
void bad(void) {
  Ints a = array_empty(int);
  int k = 0;
  btree_get(&a, &k);
}
#endif

#ifdef BAD_FMT_STRUCT_BY_VALUE
void bad(void) {
  Ints a = array_empty(int);
  io_fprint(stdoutW, "{}", a); // user types go by address, and must be listed
}
#endif

#ifdef BAD_FMT_UNLISTED_POINTER
void bad(void) {
  int i = 0;
  io_fprint(stdoutW, "{}", &i); // `int *` is not a formattable type
}
#endif

#ifdef BAD_FMT_WRITER_BY_POINTER
void bad(void) {
  Writer w = stdoutW;
  io_fprint(&w, "x"); // writers are passed by value
}
#endif

#ifdef BAD_CSTR_TO_UNSUPPORTED_TYPE
void bad(void) {
  char c;
  cstr_to("x", &c); // no parser for plain char
}
#endif

#ifdef BAD_CSTR_TO_BY_VALUE
void bad(void) {
  int n = 0;
  cstr_to("1", n); // the destination is a pointer
}
#endif

#define TC_DEFER_CASE(call)                                                   \
  Error bad(void) {                                                            \
    Error eret = eok();                                                        \
    EDEFERS(1);                                                                \
    double d = 1;                                                              \
    Ints ints = array_empty(int);                                              \
    (void)ints;                                                                \
    Big big = {0};                                                             \
    (void)d, (void)big;                                                        \
    call;                                                                      \
  end:                                                                         \
    erundefers();                                                              \
    return eret;                                                               \
  }

typedef struct {
  int a, b;
} Big;

#ifdef BAD_DEFER_TOO_MANY_ARGS
TC_DEFER_CASE(edefer_void(ret_void, 1, "x", 3))
#endif

#ifdef BAD_DEFER_TOO_FEW_ARGS
TC_DEFER_CASE(edefer_void(ret_void, 1))
#endif

#ifdef BAD_DEFER_WRONG_ARG_TYPE
TC_DEFER_CASE(edefer_void(free, 5)) // an int is not a pointer
#endif

#ifdef BAD_DEFER_DOUBLE_ARG
TC_DEFER_CASE(edefer_void(ret_void, d, "x"))
#endif

#ifdef BAD_DEFER_STRUCT_ARG
TC_DEFER_CASE(edefer_void(ret_void, big, "x"))
#endif

#ifdef BAD_DEFER_VOID_FN_IN_EDEFER
TC_DEFER_CASE(edefer(ret_void, 1, "x")) // edefer needs an Error-returning fn
#endif

#ifdef BAD_DEFER_MACRO_CALLEE
TC_DEFER_CASE(edefer_void(array_free0, &ints, free)) // a macro, not a function
#endif

#ifdef BAD_DEFER_SEVEN_ARGS
static void seven(int a, int b, int c, int d, int e, int f, int g) {
  (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)g;
}
TC_DEFER_CASE(edefer_void(seven, 1, 2, 3, 4, 5, 6, 7))
#endif
