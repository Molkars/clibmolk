#ifndef MOLKIE_ARRAY_H
#define MOLKIE_ARRAY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "err.h"

typedef struct {
  void *elements;
  size_t len, cap, esz;
} _array_t;

/// A typed array. `elements` is a `T *`, so the compiler knows the element
/// type; `raw` is the same storage seen as an untyped `_array_t`, which is what
/// the `*_raw` functions take. Declare function parameters through a typedef
/// (`typedef array_t(Foo) Foos;`): two `array_t(Foo)` spelled separately are
/// distinct types.
#define array_t(T)                                                             \
  struct {                                                                     \
    union {                                                                    \
      _array_t raw;                                                            \
      struct {                                                                 \
        T *elements;                                                           \
        size_t len, cap, esz;                                                  \
      };                                                                       \
    };                                                                         \
  }

/// brace initializers (declarations only): `Foos a = array_empty(Foo);`
#define array_new(cap, T)                                                      \
  {                                                                            \
    .raw = {NULL, 0, cap, sizeof(T)}                                           \
  }
#define array_empty(T)                                                         \
  {                                                                            \
    .raw = {NULL, 0, 0, sizeof(T)}                                             \
  }

/// ensure that [a->len + extra] items can fit in [a].
/// returns an error if the array can't grow; [a] is unchanged
NODISCARD Error array_grow_raw(_array_t *a, size_t extra);
/// add the element [elem] to [a]. returns an error
NODISCARD Error array_push_raw(_array_t *a, const void *elem);
/// add [elems] to the end of [a]. returns an error (also if the element sizes
/// differ)
NODISCARD Error array_extend_raw(_array_t *a, const _array_t *elems);
/// add [n] elements from [elems] to the end of [a]. returns an error
NODISCARD Error array_extend2_raw(_array_t *a, const void *elems, size_t n);
/// pointer to element [i], or NULL if [i] is out of bounds
void *array_at_raw(_array_t *a, size_t i);
/// free the array [a], without calling [free] on elements
void array_free_raw(_array_t *a);
/// free the array [a], calling [subfree] on each item.
void array_free0_raw(_array_t *a, void (*subfree)(void *));

// --- type-checked API ------------------------------------------------------
// Every macro takes a pointer to an `array_t(T)` and rejects, at compile time,
// anything that is not a `T *` (or `const T *`) where an element is expected.
// Arguments are evaluated once, except `a`, which is only used in unevaluated
// (type) contexts besides the call itself.

/// the element type `T` of the array pointed to by [a]
#define _array_elem(a) __typeof__(*(a)->elements)
/// yields [p] if it is a `T *` / `const T *` for [a]'s `T`; compile error else
#define _array_ck(a, p)                                                        \
  _Generic((p), _array_elem(a) *: (p), const _array_elem(a) *: (p))

/// ensure that [a->len + n] items can fit in [a]. returns an `Error`
#define array_grow(a, n) array_grow_raw(&(a)->raw, n)
/// add the element pointed to by [elem] to [a]. returns an `Error`
#define array_push(a, elem) array_push_raw(&(a)->raw, _array_ck(a, elem))
/// add [b]'s elements to the end of [a]; both must hold the same `T`.
/// returns an `Error`
#define array_extend(a, b)                                                  \
  ((void)_Generic((b)->elements, _array_elem(a) *: 0),                         \
   array_extend_raw(&(a)->raw, &(b)->raw))
/// add [n] elements from [elems] (a `T *`) to the end of [a]. returns an `Error`
#define array_extend2(a, elems, n)                                             \
  array_extend2_raw(&(a)->raw, _array_ck(a, elems), n)
/// pointer to element [i] as a `T *`, or NULL if out of bounds
#define array_at(a, i) ((_array_elem(a) *)array_at_raw(&(a)->raw, i))
/// free the array [a], without calling [free] on elements
#define array_free(a) array_free_raw(&(a)->raw)
/// free the array [a], calling [subfree] (`void (*)(void *)`) on each item
#define array_free0(a, subfree) array_free0_raw(&(a)->raw, subfree)

#endif // MOLKIE_ARRAY_H

#if defined(MOLKIE_ARRAY_IMPL) && !defined(MOLKIE_ARRAY_IMPL_DONE)
#define MOLKIE_ARRAY_IMPL_DONE
#include <assert.h>
#include <errno.h>
#include <stdlib.h>

// offset of [p] within [a]'s storage, or SIZE_MAX if [p] is elsewhere. Used so
// pushing an element of the array itself survives the realloc in grow
static size_t _array_offset_of(const _array_t *a, const void *p) {
  if (!a->elements || !a->esz)
    return SIZE_MAX;
  uintptr_t lo = (uintptr_t)a->elements, x = (uintptr_t)p;
  if (x >= lo && x - lo < a->cap * a->esz)
    return x - lo;
  return SIZE_MAX;
}

Error array_grow_raw(_array_t *a, size_t n) {
  assert(a && "array is null");
  if (!a->esz)
    return enew(EINVAL, "array has no element size (use array_empty/array_new)");
  if (n > SIZE_MAX - a->len)
    return enew(ENOMEM, "array length overflow (%zu + %zu)", a->len, n);

  size_t cap = a->cap;
  if (!cap)
    cap = 10;
  while (cap < a->len + n) {
    if (cap > SIZE_MAX / 2)
      return enew(ENOMEM, "array capacity overflow (%zu)", cap);
    cap = cap * 2;
  }
  // `array_new(cap, T)` sets cap without allocating, so also allocate then
  if (cap > a->cap || !a->elements) {
    if (a->esz && cap > SIZE_MAX / a->esz)
      return enew(ENOMEM, "array size overflow (%zu x %zu bytes)", cap,
                        a->esz);
    void *p = realloc(a->elements, a->esz * cap);
    if (!p)
      return enew(ENOMEM, "unable to allocate %zu x %zu bytes for array",
                        cap, a->esz);

    a->elements = p;
    a->cap = cap;
  }
  return eok();
}

Error array_push_raw(_array_t *a, const void *elem) {
  assert(a && "array is null");
  assert(elem && "elem is null");
  size_t off = _array_offset_of(a, elem); // elem may live inside [a]
  Error _e = array_grow_raw(a, 1);
  if (!_e.ok)
    return _e;
  uint8_t *p = a->elements;
  if (off != SIZE_MAX)
    elem = p + off;
  memcpy(p + a->esz * a->len, elem, a->esz);
  a->len += 1;
  return eok();
}

Error array_extend_raw(_array_t *a, const _array_t *elems) {
  assert(a && "array is null");
  assert(elems && "elems is null");
  if (a->esz != elems->esz)
    return enew(EINVAL, "element sizes differ (%zu vs %zu)", a->esz,
                      elems->esz);
  if (!elems->len)
    return eok();
  Error _e = array_grow_raw(a, elems->len);
  if (!_e.ok)
    return _e;
  uint8_t *p = a->elements;
  memcpy(p + a->len * a->esz, elems->elements, elems->len * elems->esz);
  a->len += elems->len;
  return eok();
}

Error array_extend2_raw(_array_t *a, const void *elems, size_t n) {
  assert(a && "array is null");
  assert(elems && "elems is null");
  if (!n)
    return eok();
  size_t off = _array_offset_of(a, elems); // elems may live inside [a]
  Error _e = array_grow_raw(a, n);
  if (!_e.ok)
    return _e;
  uint8_t *p = a->elements;
  if (off != SIZE_MAX)
    elems = p + off;
  memmove(p + a->len * a->esz, elems, n * a->esz);
  a->len += n;
  return eok();
}

void *array_at_raw(_array_t *a, size_t i) {
  assert(a && "array is null");
  if (i >= a->len)
    return NULL;

  return ((uint8_t *)a->elements) + i * a->esz;
}

void array_free_raw(_array_t *a) {
  if (!a)
    return;
  free(a->elements);
  a->elements = NULL;
  a->cap = 0;
  a->len = 0;
}

void array_free0_raw(_array_t *a, void (*subfree)(void *)) {
  if (!a)
    return;
  for (size_t i = 0; subfree && i < a->len; i++)
    subfree(array_at_raw(a, i));
  free(a->elements);
  a->elements = NULL;
  a->cap = 0;
  a->len = 0;
}
#endif // MOLKIE_ARRAY_IMPL

// Tests: compiled where MTEST (all modules) or MOLKIE_ARRAY_TEST (just this one)
// is defined. Needs MOLKIE_ARRAY_IMPL and
// MOLKIE_ERR_IMPL in the same program.
#if (defined(MTEST) || defined(MOLKIE_ARRAY_TEST)) && !defined(MOLKIE_ARRAY_TESTS)
#define MOLKIE_ARRAY_TESTS
#include "test.h"

typedef array_t(int) _TestInts;

static void array_push_at(void) {
  _TestInts a = array_empty(int);
  for (int i = 0; i < 100; i++)
    assert(array_push(&a, &i).ok);
  ASSERT_EQ(a.len, 100);
  assert(a.cap >= 100);
  for (int i = 0; i < 100; i++)
    ASSERT_EQ(*array_at(&a, (size_t)i), i);
  assert(array_at(&a, 100) == NULL); // out of bounds
  array_free(&a);
  assert(a.elements == NULL && a.len == 0 && a.cap == 0);
}

static void array_extend_grow(void) {
  _TestInts a = array_empty(int), b = array_new(4, int);
  int more[] = {1, 2, 3};
  assert(array_extend2(&a, more, 3).ok);
  assert(array_extend2(&b, more, 3).ok);
  assert(array_extend(&a, &b).ok);
  ASSERT_EQ(a.len, 6);
  ASSERT_EQ(*array_at(&a, 5), 3);
  assert(array_grow(&a, 1000).ok);
  assert(a.cap >= 1006);
  ASSERT_EQ(a.len, 6); // grow doesn't change len
  array_free(&a);
  array_free(&b);
}

static int array_freed_count;
static void array_count_free(void *p) {
  (void)p;
  array_freed_count++;
}

static void array_free0_calls(void) {
  _TestInts a = array_empty(int);
  for (int i = 0; i < 5; i++)
    assert(array_push(&a, &i).ok);
  array_freed_count = 0;
  array_free0(&a, array_count_free);
  ASSERT_EQ(array_freed_count, 5);
  assert(a.elements == NULL && a.len == 0);
}

static void array_self_element(void) {
  _TestInts a = array_empty(int);
  for (int i = 0; i < 10; i++) // len == cap == 10: the next push reallocs
    assert(array_push(&a, &i).ok);
  assert(array_push(&a, &a.elements[0]).ok);
  ASSERT_EQ(a.len, 11);
  ASSERT_EQ(*array_at(&a, 10), 0);
  array_free(&a);

  _TestInts b = array_empty(int);
  for (int i = 0; i < 10; i++)
    assert(array_push(&b, &i).ok);
  assert(array_extend2(&b, b.elements, 3).ok);
  ASSERT_EQ(b.len, 13);
  ASSERT_EQ(*array_at(&b, 12), 2);
  array_free(&b);
}

static void array_uninitialised(void) {
  _TestInts z = {0}; // no element size: rejected, not silently ignored
  int x = 7;
  Error e = array_push(&z, &x);
  assert(!e.ok);
  ASSERT_EQ(e.code, EINVAL);
  ASSERT_EQ(z.len, 0);
}

static void array_extend_size_mismatch(void) {
  _TestInts a = array_empty(int);
  array_t(char) c = array_empty(char); // empty, but the wrong element size
  assert(!array_extend_raw(&a.raw, &c.raw).ok);
  array_free(&a);
}

static void array_free0_null(void) {
  _TestInts a = array_empty(int);
  int x = 1;
  assert(array_push(&a, &x).ok);
  array_free0(&a, NULL); // NULL subfree == plain free
  assert(a.elements == NULL);
}

static void array_overflow(void) {
  _TestInts a = array_empty(int);
  assert(!array_grow(&a, SIZE_MAX).ok); // can't grow; a is unchanged
  assert(a.elements == NULL && a.len == 0);
}

static int array_tests(void) {
  static const TestCase tests[] = {
      T(array_push_at),
      T(array_extend_grow),
      T(array_free0_calls),
      T(array_overflow),
      T(array_self_element),
      T(array_uninitialised),
      T(array_extend_size_mismatch),
      T(array_free0_null),
  };
  size_t n = sizeof tests / sizeof *tests, passed = 0;
  for (size_t i = 0; i < n; i++)
    passed += run_in_child(&tests[i]);
  printf("array: %zu/%zu passed\n", passed, n);
  return passed == n;
}

#endif // MTEST
