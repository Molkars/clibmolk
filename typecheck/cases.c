// Compile-time checks for the typed array/btree API. Never linked or run.
//
// Everything outside a `BAD_*` block must compile cleanly. Each `BAD_*` block
// is a misuse that must FAIL to compile; `just typecheck` builds every one
// with -DBAD_<name> and complains if any of them is accepted.
#include "array.h"
#include "btree.h"
#include "err.h"

typedef array_t(int) Ints;
typedef array_t(double) Doubles;
typedef btree_str_t(int) Ages;
typedef btree_t(int, double) IntMap;

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
