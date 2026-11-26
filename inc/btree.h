#ifndef MOLKIE_BTREE_H
#define MOLKIE_BTREE_H

#include <stdio.h>
#include <string.h>

#include <stddef.h>

#include "err.h"

typedef struct {
  size_t keySize;
  size_t valueSize;
  /// number of branches per node
  size_t arity;
  /// [a] and [b] point at keys (not the keys themselves; for string keys
  /// they are [const char **]).
  /// returns: 0 if a == b, <0 if a < b, >0 if a > b
  int (*cmp)(const void *a, const void *b);
  /// Optional key ownership. A key lives in a heap slot of [keySize] bytes that
  /// holds the key BY VALUE; for strings that is the `char *`. By default the
  /// slot is a plain byte copy of the caller's key, so keys that point at
  /// other memory (like strings) are only borrowed and must outlive the tree.
  ///
  /// If [key_clone] is set, it is called instead of the byte copy: it must
  /// deep-copy the key in slot [src] into the (uninitialised) slot [dst], or
  /// return an error and leave nothing allocated. [key_free] then releases
  /// whatever the key in [slot] owns (not the slot itself). Set both or
  /// neither. The tree clones every key it stores, including the copies that
  /// internal nodes keep as separators, and frees them on remove, on
  /// [btree_free_raw] and when an insert fails, so the caller can free its own
  /// key right after the insert. Values are always plain byte copies.
  Error (*key_clone)(void *dst, const void *src);
  void (*key_free)(void *slot);
  size_t size;
  void *root;
} _btree_t;

typedef struct _btree_node {
  /// array of length [arity]
  void **keys;
  /// current length of [keys]
  size_t nkeys;
  /// array of length [arity]
  /// holds children or values
  void **values;
  struct _btree_node *sibling;
  unsigned leaf : 1;
} _btree_node_t;

/// Iterates a tree in ascending key order. Do not modify the tree while
/// iterating.
/// ```
/// _btree_iter_t it = btree_iter_raw(&tree);
/// while (btree_next_raw(&it)) { use(it.item, it.value); }
/// ```
typedef struct {
  /// pointer to the current key (same convention as the [key] passed to
  /// [btree_insert_raw]); valid after [btree_next_raw] returns 1
  void *item;
  /// pointer to the current value
  void *value;
  _btree_node_t *node;
  size_t idx;
} _btree_iter_t;

/// raw (untyped) initializers; prefer the typed `btree_empty` below
#define _btree_empty_own(k, v, cmpfn, clonefn, freefn)                         \
  (_btree_t) {                                                                 \
    .keySize = sizeof(k), .valueSize = sizeof(v), .arity = 7, .cmp = cmpfn,    \
    .key_clone = clonefn, .key_free = freefn, .size = 0, .root = NULL          \
  }
#define _btree_empty(k, v, cmpfn) _btree_empty_own(k, v, cmpfn, NULL, NULL)
/// string keys are owned: the tree keeps its own copy of every string
#define _btree_str_empty(v)                                                    \
  _btree_empty_own(const char *, v, btree_strcmp, btree_str_clone,             \
                   btree_str_free)

/// A typed tree from key type [K] to value type [V]. `raw` is the untyped
/// tree the `*_raw` functions take; `_key`/`_value` are never used, they only
/// carry [K] and [V] in the type. Declare function parameters through a
/// typedef (`typedef btree_t(char *, int) Ages;`): two `btree_t(K, V)` spelled
/// separately are distinct types.
#define btree_t(K, V)                                                          \
  struct {                                                                     \
    _btree_t raw;                                                              \
    K *_key;                                                                   \
    V *_value;                                                                 \
  }
/// a tree keyed by C strings. Keys are passed as `const char **`.
#define btree_str_t(V) btree_t(const char *, V)

/// brace initializers (declarations only): `Ages t = btree_empty(K, V, cmp);`
#define btree_empty(K, V, cmpfn) {.raw = _btree_empty(K, V, cmpfn)}
#define btree_str_empty(V) {.raw = _btree_str_empty(V)}
/// like [btree_empty], with key ownership hooks (see `_btree_t.key_clone`)
#define btree_empty_own(K, V, cmpfn, clonefn, freefn)                          \
  {.raw = _btree_empty_own(K, V, cmpfn, clonefn, freefn)}

/// A typed iterator over a `btree_t(K, V)`; see [btree_iter].
#define btree_iter_t(K, V)                                                     \
  struct {                                                                     \
    _btree_iter_t raw;                                                         \
    K *item;                                                                   \
    V *value;                                                                  \
  }

void *btree_get_raw(const _btree_t *bt, const void *key);
/// insert [key] -> [value], overwriting an existing entry (its old value is
/// copied to [old] if non-NULL). returns an error if memory runs out; the tree is left exactly as it was
Error btree_insert_ex_raw(_btree_t *bt, const void *key, const void *value,
                         void *old);
Error btree_insert_raw(_btree_t *bt, const void *key, const void *value);

/// returns 1 if [key] is in [bt], 0 otherwise
int btree_contains_raw(const _btree_t *bt, const void *key);
/// remove [key] from [bt]. returns 1 if it was removed, 0 if not present
int btree_remove_raw(_btree_t *bt, const void *key);
/// like [btree_remove_raw], copying the removed value into [old] if non-NULL
int btree_remove_ex_raw(_btree_t *bt, const void *key, void *old);
/// insert every entry of [other] into [bt], overwriting existing keys.
/// returns an error if the key or value sizes differ (nothing is
/// inserted) or if memory runs out (the entries inserted so far stay)
Error btree_extend_raw(_btree_t *bt, const _btree_t *other);

/// create an iterator positioned before the first entry of [bt]
_btree_iter_t btree_iter_raw(const _btree_t *bt);
/// advance [it] to the next entry. returns 1 and sets [it->item] and
/// [it->value], or returns 0 when there are no more entries.
int btree_next_raw(_btree_iter_t *it);

void btree_free_raw(_btree_t *bt);

int btree_strcmp(const void *a, const void *b);
/// key hooks for `const char *` keys: [btree_str_clone] `strdup`s the string
/// in slot [src] into slot [dst] (ENOMEM on failure), [btree_str_free] frees it
Error btree_str_clone(void *dst, const void *src);
void btree_str_free(void *slot);
// --- type-checked API ------------------------------------------------------
// Every macro takes a pointer to a `btree_t(K, V)`. Keys and values are passed
// by pointer, like the raw API, but the pointer must be a `K *` / `V *` (or
// the `const` versions). For `btree_str_t(V)`, `K` is `const char *`, so a key
// is a `const char **`: pass `&name`, not `name` and not a string literal.
//
// Key ownership: `btree_str_t` trees own their strings. Insert copies the
// string, so you may free or reuse yours immediately; remove and `btree_free`
// free the tree's copy. `it.item` / the key slot points at the tree's copy and
// is valid until that key is removed. Other `K` are plain byte copies unless
// the tree was made with `btree_empty_own` (see `_btree_t.key_clone`).

#define _btree_key(bt) __typeof__(*(bt)->_key)
#define _btree_val(bt) __typeof__(*(bt)->_value)
#define _btree_ck(p, T) _Generic((p), T *: (p), const T *: (p))
#define _btree_ckk(bt, p) _btree_ck(p, _btree_key(bt))
#define _btree_ckv(bt, p) _btree_ck(p, _btree_val(bt))
/// like [_btree_ckv], but also allows a NULL `void *` (for optional outputs)
#define _btree_ckv_opt(bt, p)                                                  \
  _Generic((p), _btree_val(bt) *: (p), void *: (p))

/// number of entries in [bt]
#define btree_size(bt) ((bt)->raw.size)
/// the value for [key] as a `V *`, or NULL if absent
#define btree_get(bt, key)                                                     \
  ((_btree_val(bt) *)btree_get_raw(&(bt)->raw, _btree_ckk(bt, key)))
/// insert [key] -> [value], overwriting an existing entry. returns an `Error`; if it failed the tree is
/// unchanged
#define btree_insert(bt, key, value)                                           \
  btree_insert_raw(&(bt)->raw, _btree_ckk(bt, key), _btree_ckv(bt, value))
/// like [btree_insert], copying the replaced value into [old] (`V *` or NULL)
#define btree_insert_ex(bt, key, value, old)                                   \
  btree_insert_ex_raw(&(bt)->raw, _btree_ckk(bt, key), _btree_ckv(bt, value),  \
                      _btree_ckv_opt(bt, old))
/// returns 1 if [key] is in [bt], 0 otherwise
#define btree_contains(bt, key)                                                \
  btree_contains_raw(&(bt)->raw, _btree_ckk(bt, key))
/// remove [key] from [bt]. returns 1 if it was removed, 0 if not present
#define btree_remove(bt, key) btree_remove_raw(&(bt)->raw, _btree_ckk(bt, key))
/// like [btree_remove], copying the removed value into [old] (`V *` or NULL)
#define btree_remove_ex(bt, key, old)                                          \
  btree_remove_ex_raw(&(bt)->raw, _btree_ckk(bt, key),                         \
                      _btree_ckv_opt(bt, old))
/// insert every entry of [other] into [bt]; both must have the same `K`, `V`.
/// returns an `Error`
#define btree_extend(bt, other)                                             \
  ((void)_Generic((other)->_key, __typeof__((bt)->_key): 0),                   \
   (void)_Generic((other)->_value, __typeof__((bt)->_value): 0),               \
   btree_extend_raw(&(bt)->raw, &(other)->raw))
/// position [it] (a `btree_iter_t(K, V)` of the tree's own `K`, `V`) before
/// the first entry of [bt]. `it` must be a plain lvalue.
/// ```
/// btree_iter_t(const char *, int) it;
/// btree_iter(&it, &tree);
/// while (btree_next(&it)) { use(*it.item, *it.value); }
/// ```
#define btree_iter(it, bt)                                                     \
  ((void)_Generic((it)->item, __typeof__((bt)->_key): 0),                      \
   (void)_Generic((it)->value, __typeof__((bt)->_value): 0),                   \
   (void)((it)->raw = btree_iter_raw(&(bt)->raw)))
/// advance [it]. returns 1 and sets `it->item`/`it->value` (typed pointers),
/// or returns 0 when there are no more entries. `it` must be a plain lvalue.
#define btree_next(it)                                                         \
  (btree_next_raw(&(it)->raw)                                                  \
       ? ((it)->item = (it)->raw.item, (it)->value = (it)->raw.value, 1)       \
       : 0)
/// free every entry of [bt]
#define btree_free(bt) btree_free_raw(&(bt)->raw)

#endif // MOLKIE_BTREE_H

#if defined(MOLKIE_BTREE_IMPL) && !defined(MOLKIE_BTREE_IMPL_DONE)
#define MOLKIE_BTREE_IMPL_DONE
#include <assert.h>
#include <errno.h>
#include <stdlib.h>

#if defined(MTEST) || defined(MOLKIE_BTREE_TEST)
/// test-only out-of-memory injection: when >= 0, that many more allocations
/// succeed and every one after that fails. -1 turns it off.
static long _btree_fail_in = -1;
static void *_btree_malloc(size_t n) {
  if (_btree_fail_in == 0)
    return NULL;
  if (_btree_fail_in > 0)
    _btree_fail_in--;
  return malloc(n);
}
#else
#define _btree_malloc malloc
#endif

Error btree_str_clone(void *dst, const void *src) {
  const char *s = *(const char *const *)src;
  size_t n = strlen(s) + 1;
  char *copy = _btree_malloc(n);
  if (!copy)
    return enew(ENOMEM, "out of memory copying a %zu byte key", n);
  memcpy(copy, s, n);
  *(const char **)dst = copy;
  return eok();
}

void btree_str_free(void *slot) { free((void *)*(const char **)slot); }

/// copy the key in slot [src] to the uninitialised slot [dst]
static Error _btree_key_copy(const _btree_t *bt, void *dst, const void *src) {
  if (bt->key_clone)
    return bt->key_clone(dst, src);
  memcpy(dst, src, bt->keySize);
  return eok();
}

/// free key slot [slot] and (if it was filled) whatever its key owns
static void _btree_key_drop(const _btree_t *bt, void *slot) {
  if (!slot)
    return;
  if (bt->key_free)
    bt->key_free(slot);
  free(slot);
}

static void _btree_free_node(const _btree_t *bt, _btree_node_t *node) {
  for (size_t i = 0; i < node->nkeys; i++) {
    _btree_key_drop(bt, node->keys[i]);
    if (node->leaf)
      free(node->values[i]);
    else
      _btree_free_node(bt, node->values[i]);
  }
  if (!node->leaf)
    _btree_free_node(bt, node->values[node->nkeys]);
  free(node->keys);
  free(node->values);
  free(node);
}

void btree_free_raw(_btree_t *bt) {
  if (!bt)
    return;
  if (bt->root)
    _btree_free_node(bt, bt->root);
  bt->root = NULL;
  bt->size = 0;
}

typedef struct {
  int present;
  _btree_node_t *node;
  size_t idx;
} _btree_search;

static inline size_t _btree_ceildiv(size_t l, size_t r) {
  return (l + r - 1) / r;
}

/// a new empty leaf, or NULL if out of memory
static _btree_node_t *_btree_mk_node(const _btree_t *bt) {
  _btree_node_t *node = _btree_malloc(sizeof(_btree_node_t));
  if (!node)
    return NULL;

  // A node holds at most [arity] keys. One extra key slot (and one extra
  // child slot, since internal nodes have nkeys + 1 children) is reserved so
  // a node can overflow by one entry before being split.
  node->keys = _btree_malloc(sizeof(void *) * (bt->arity + 1));
  node->values = _btree_malloc(sizeof(void *) * (bt->arity + 2));
  if (!node->keys || !node->values) {
    free(node->keys);
    free(node->values);
    free(node);
    return NULL;
  }
  node->nkeys = 0;
  node->sibling = NULL;
  node->leaf = 1;

  return node;
}

_btree_search _btree_get(_btree_t *bt, _btree_node_t *node, const void *key) {
  if (node->leaf) {
    size_t i;
    for (i = 0; i < node->nkeys; i++) {
      int cmp = bt->cmp(key, node->keys[i]);
      if (cmp < 0)
        break;
      if (cmp == 0)
        return (_btree_search){1, node, i};
    }
    return (_btree_search){0, node, i};
  }

  for (size_t i = 0; i < node->nkeys; i++) {
    int cmp = bt->cmp(key, node->keys[i]);

    if (cmp <= 0)
      return _btree_get(bt, node->values[i], key);
  }

  return _btree_get(bt, node->values[node->nkeys], key);
}

void *btree_get_raw(const _btree_t *bt, const void *key) {
  if (!bt || !bt->size)
    return NULL;
  _btree_search search = _btree_get((_btree_t *)bt, bt->root, key);
  if (!search.present)
    return NULL;
  return search.node->values[search.idx];
}

Error btree_insert_raw(_btree_t *bt, const void *key, const void *value) {
  return btree_insert_ex_raw(bt, key, value, NULL);
}

/// Everything an insert may need to allocate, gathered BEFORE the tree is
/// touched, so that running out of memory can't leave a half-split tree.
typedef struct {
  /// owned copies of the new entry
  void *key, *value;
  /// owned copy of the key that separates a split leaf from its new sibling
  void *sep;
  /// the slots above hold a cloned key (so [key_free] must run on release)
  int key_live, sep_live;
  /// spare nodes, one per split (plus one for a new root); `used` taken so far
  _btree_node_t *nodes[64];
  size_t nnodes, used;
} _btree_reserve;

static void _btree_reserve_release(const _btree_t *bt, _btree_reserve *r) {
  if (r->key_live)
    _btree_key_drop(bt, r->key);
  else
    free(r->key);
  free(r->value);
  if (r->sep_live)
    _btree_key_drop(bt, r->sep);
  else
    free(r->sep);
  for (size_t i = r->used; i < r->nnodes; i++) {
    free(r->nodes[i]->keys);
    free(r->nodes[i]->values);
    free(r->nodes[i]);
  }
  *r = (_btree_reserve){0};
}

static _btree_node_t *_btree_take_node(_btree_reserve *r) {
  assert(r->used < r->nnodes && "insert needed more nodes than reserved");
  return r->nodes[r->used++];
}

/// Reserve what inserting [key] -> [value] needs. Nothing is reserved if [key]
/// is already present (the insert just overwrites).
///
/// A node splits exactly when it is full before the insert, and a split of a
/// node adds one entry to its parent. So the number of splits is the number of
/// consecutive full nodes from the leaf upwards, plus a new root if that run
/// reaches the root.
static Error _btree_reserve_for(const _btree_t *bt, const void *key,
                               const void *value, _btree_reserve *r) {
  *r = (_btree_reserve){0};
  assert(!bt->key_clone == !bt->key_free && "set both key hooks or neither");
  size_t need = 0;
  int sep = 0;
  const void *sepsrc = NULL; // the key the split separator will be a copy of

  if (!bt->root) {
    need = 1;
  } else {
    size_t levels = 0, run = 0;
    _btree_node_t *node = bt->root;
    for (;;) {
      levels++;
      run = node->nkeys == bt->arity ? run + 1 : 0;
      if (node->leaf)
        break;
      size_t i = 0;
      while (i < node->nkeys && bt->cmp(key, node->keys[i]) > 0)
        i++;
      node = node->values[i];
    }
    for (size_t i = 0; i < node->nkeys; i++) {
      if (bt->cmp(key, node->keys[i]) == 0)
        return eok(); // overwrite: nothing to allocate
    }
    need = run + (run == levels ? 1 : 0);
    sep = run > 0;
    if (sep) {
      // the leaf is full, so it splits: after the insert it holds nkeys + 1
      // keys and the separator is a copy of the max key of the lower half
      size_t i = 0;
      while (i < node->nkeys && bt->cmp(key, node->keys[i]) > 0)
        i++;
      size_t j = (node->nkeys + 2) / 2 - 1;
      sepsrc = j < i ? node->keys[j] : j == i ? key : node->keys[j - 1];
    }
  }
  assert(need <= sizeof r->nodes / sizeof *r->nodes);

  r->key = _btree_malloc(bt->keySize);
  r->value = _btree_malloc(bt->valueSize);
  if (sep)
    r->sep = _btree_malloc(bt->keySize);
  int ok = r->key && r->value && (!sep || r->sep);
  while (ok && r->nnodes < need) {
    r->nodes[r->nnodes] = _btree_mk_node(bt);
    ok = r->nodes[r->nnodes] != NULL;
    r->nnodes += ok;
  }
  if (!ok) {
    _btree_reserve_release(bt, r);
    return enew(ENOMEM, "out of memory inserting into btree (%zu entries)",
                bt->size);
  }
  Error e = _btree_key_copy(bt, r->key, key);
  if (e.ok) {
    r->key_live = 1;
    if (sep) {
      e = _btree_key_copy(bt, r->sep, sepsrc);
      r->sep_live = e.ok;
    }
  }
  if (!e.ok) {
    _btree_reserve_release(bt, r);
    ectx(&e, "inserting into btree (%zu entries)", bt->size);
    return e;
  }
  memcpy(r->value, value, bt->valueSize);
  return eok();
}

/// Puts the reserved copies of the new key and value into slot [i] of [node].
/// Does not touch nkeys or shift anything.
static inline void _btree_insert(_btree_node_t *node, size_t i,
                                 _btree_reserve *r) {
  node->keys[i] = r->key;
  node->values[i] = r->value;
  r->key = r->value = NULL;
  r->key_live = 0;
}

/// Result of inserting into a subtree.
typedef struct {
  /// the key already existed and its value was overwritten
  int replaced;
  /// the child split; [sep] and [right] must be inserted into the parent
  int split;
  /// owned copy of the max key of the (left) node that split
  void *sep;
  /// the new right sibling produced by the split
  _btree_node_t *right;
} _btree_ins;

/// Split an overflowing leaf (nkeys == arity + 1) in place. [node] keeps the
/// lower half; the upper half moves to a new right sibling. Keys stay in the
/// leaves (B+ tree), so the separator is a fresh copy of the left max.
static _btree_ins _btree_split_leaf(_btree_t *bt, _btree_node_t *node,
                                    _btree_reserve *rsv) {
  _btree_node_t *right = _btree_take_node(rsv);
  size_t keep = (node->nkeys + 1) / 2;
  size_t move = node->nkeys - keep;

  memcpy(right->keys, &node->keys[keep], sizeof(void *) * move);
  memcpy(right->values, &node->values[keep], sizeof(void *) * move);
  right->nkeys = move;
  node->nkeys = keep;

  right->sibling = node->sibling;
  node->sibling = right;

  // the separator was cloned from this very key by _btree_reserve_for
  void *sep = rsv->sep;
  rsv->sep = NULL;
  rsv->sep_live = 0;
  assert(bt->cmp(sep, node->keys[keep - 1]) == 0);

  return (_btree_ins){.split = 1, .sep = sep, .right = right};
}

/// Split an overflowing internal node (nkeys == arity + 1, nkeys + 1
/// children) in place. The middle key is promoted (moved, not copied) into
/// the parent; it belongs to neither half.
static _btree_ins _btree_split_internal(_btree_node_t *node,
                                        _btree_reserve *rsv) {
  _btree_node_t *right = _btree_take_node(rsv);
  right->leaf = 0;

  size_t mid = node->nkeys / 2;
  size_t rkeys = node->nkeys - mid - 1;
  void *sep = node->keys[mid];

  memcpy(right->keys, &node->keys[mid + 1], sizeof(void *) * rkeys);
  memcpy(right->values, &node->values[mid + 1], sizeof(void *) * (rkeys + 1));
  right->nkeys = rkeys;
  node->nkeys = mid;

  return (_btree_ins){.split = 1, .sep = sep, .right = right};
}

static _btree_ins _btree_insert_rec(_btree_t *bt, _btree_node_t *node,
                                    const void *key, const void *value,
                                    void *old, _btree_reserve *rsv) {
  _btree_ins none = {0};

  if (node->leaf) {
    size_t i;
    for (i = 0; i < node->nkeys; i++) {
      int cmp = bt->cmp(key, node->keys[i]);
      if (cmp < 0)
        break;
      if (cmp == 0) {
        void *slot = node->values[i];
        if (old)
          memcpy(old, slot, bt->valueSize);
        memcpy(slot, value, bt->valueSize);
        none.replaced = 1;
        return none;
      }
    }

    memmove(&node->keys[i + 1], &node->keys[i],
            (node->nkeys - i) * sizeof(void *));
    memmove(&node->values[i + 1], &node->values[i],
            (node->nkeys - i) * sizeof(void *));
    _btree_insert(node, i, rsv);
    node->nkeys += 1;

    if (node->nkeys <= bt->arity)
      return none;
    return _btree_split_leaf(bt, node, rsv);
  }

  // Child i holds keys <= keys[i]; the last child holds everything greater.
  size_t i = 0;
  while (i < node->nkeys && bt->cmp(key, node->keys[i]) > 0)
    i++;

  _btree_ins res =
      _btree_insert_rec(bt, (_btree_node_t *)node->values[i], key, value, old,
                        rsv);
  if (!res.split)
    return res;

  // Insert the separator at keys[i] and the new right child at values[i + 1].
  memmove(&node->keys[i + 1], &node->keys[i],
          (node->nkeys - i) * sizeof(void *));
  memmove(&node->values[i + 2], &node->values[i + 1],
          (node->nkeys - i) * sizeof(void *));
  node->keys[i] = res.sep;
  node->values[i + 1] = res.right;
  node->nkeys += 1;

  if (node->nkeys <= bt->arity)
    return none;
  return _btree_split_internal(node, rsv);
}

Error btree_insert_ex_raw(_btree_t *bt, const void *key, const void *value,
                         void *old) {
  assert(bt);

  _btree_reserve rsv;
  Error e = _btree_reserve_for(bt, key, value, &rsv);
  if (!e.ok)
    return e;

  if (!bt->root) {
    _btree_node_t *node = _btree_take_node(&rsv);
    _btree_insert(node, 0, &rsv);
    node->nkeys = 1;
    bt->root = node;
    bt->size += 1;
    _btree_reserve_release(bt, &rsv);
    return eok();
  }

  _btree_ins res = _btree_insert_rec(bt, bt->root, key, value, old, &rsv);
  if (!res.replaced) {
    bt->size += 1;

    if (res.split) {
      // The root split: grow the tree by one level.
      _btree_node_t *root = _btree_take_node(&rsv);
      root->leaf = 0;
      root->keys[0] = res.sep;
      root->values[0] = bt->root;
      root->values[1] = res.right;
      root->nkeys = 1;
      bt->root = root;
    }
  }
  _btree_reserve_release(bt, &rsv);
  return eok();
}

int btree_contains_raw(const _btree_t *bt, const void *key) {
  return btree_get_raw(bt, key) != NULL;
}

/// min keys in a non-root node. a leaf split yields >= this many keys per
/// half, and merging a min-1 node with a min node always fits in [arity].
static inline size_t _btree_min(const _btree_t *bt) { return bt->arity / 2; }

/// free [node] and its arrays, but not the keys/values/children they hold
static void _btree_free_shell(_btree_node_t *node) {
  free(node->keys);
  free(node->values);
  free(node);
}

/// merge child [j + 1] of [parent] into child [j], dropping separator [j]
static void _btree_merge(const _btree_t *bt, _btree_node_t *parent, size_t j) {
  _btree_node_t *l = parent->values[j];
  _btree_node_t *r = parent->values[j + 1];

  if (l->leaf) {
    _btree_key_drop(bt, parent->keys[j]); // leaf separators are copies
    memcpy(&l->keys[l->nkeys], r->keys, sizeof(void *) * r->nkeys);
    memcpy(&l->values[l->nkeys], r->values, sizeof(void *) * r->nkeys);
    l->nkeys += r->nkeys;
    l->sibling = r->sibling;
  } else {
    // the separator moves down between the two halves
    l->keys[l->nkeys] = parent->keys[j];
    memcpy(&l->keys[l->nkeys + 1], r->keys, sizeof(void *) * r->nkeys);
    memcpy(&l->values[l->nkeys + 1], r->values,
           sizeof(void *) * (r->nkeys + 1));
    l->nkeys += r->nkeys + 1;
  }

  memmove(&parent->keys[j], &parent->keys[j + 1],
          (parent->nkeys - j - 1) * sizeof(void *));
  memmove(&parent->values[j + 1], &parent->values[j + 2],
          (parent->nkeys - j - 1) * sizeof(void *));
  parent->nkeys -= 1;
  _btree_free_shell(r);
}

/// Replace the leaf separator [idx] of [parent] with a copy of the key in slot
/// [src]. Returns 0, with the separator unchanged, if the copy can't be made.
static int _btree_sep_set(_btree_t *bt, _btree_node_t *parent, size_t idx,
                          const void *src) {
  if (!bt->key_clone) {
    memcpy(parent->keys[idx], src, bt->keySize);
    return 1;
  }
  void *slot = _btree_malloc(bt->keySize);
  if (!slot)
    return 0;
  Error e = bt->key_clone(slot, src);
  if (!e.ok) {
    efree(&e);
    free(slot);
    return 0;
  }
  _btree_key_drop(bt, parent->keys[idx]);
  parent->keys[idx] = slot;
  return 1;
}

/// move one entry from child [i - 1] into child [i] of [parent]. Returns 0
/// (changing nothing) if the new separator can't be allocated
static int _btree_borrow_left(_btree_t *bt, _btree_node_t *parent, size_t i) {
  _btree_node_t *l = parent->values[i - 1];
  _btree_node_t *c = parent->values[i];

  if (c->leaf) {
    // separator is the max key of the left child once its last key has moved
    if (!_btree_sep_set(bt, parent, i - 1, l->keys[l->nkeys - 2]))
      return 0;
    memmove(&c->keys[1], &c->keys[0], c->nkeys * sizeof(void *));
    memmove(&c->values[1], &c->values[0], c->nkeys * sizeof(void *));
    c->keys[0] = l->keys[l->nkeys - 1];
    c->values[0] = l->values[l->nkeys - 1];
    l->nkeys -= 1;
  } else {
    memmove(&c->keys[1], &c->keys[0], c->nkeys * sizeof(void *));
    memmove(&c->values[1], &c->values[0], (c->nkeys + 1) * sizeof(void *));
    c->keys[0] = parent->keys[i - 1];
    c->values[0] = l->values[l->nkeys];
    parent->keys[i - 1] = l->keys[l->nkeys - 1];
    l->nkeys -= 1;
  }
  c->nkeys += 1;
  return 1;
}

/// move one entry from child [i + 1] into child [i] of [parent]. Returns 0
/// (changing nothing) if the new separator can't be allocated
static int _btree_borrow_right(_btree_t *bt, _btree_node_t *parent, size_t i) {
  _btree_node_t *c = parent->values[i];
  _btree_node_t *r = parent->values[i + 1];

  if (c->leaf) {
    // separator is the max key of [c] once r's first key has moved in
    if (!_btree_sep_set(bt, parent, i, r->keys[0]))
      return 0;
    c->keys[c->nkeys] = r->keys[0];
    c->values[c->nkeys] = r->values[0];
    memmove(&r->keys[0], &r->keys[1], (r->nkeys - 1) * sizeof(void *));
    memmove(&r->values[0], &r->values[1], (r->nkeys - 1) * sizeof(void *));
  } else {
    c->keys[c->nkeys] = parent->keys[i];
    c->values[c->nkeys + 1] = r->values[0];
    parent->keys[i] = r->keys[0];
    memmove(&r->keys[0], &r->keys[1], (r->nkeys - 1) * sizeof(void *));
    memmove(&r->values[0], &r->values[1], r->nkeys * sizeof(void *));
  }
  r->nkeys -= 1;
  c->nkeys += 1;
  return 1;
}

/// restore the minimum occupancy of child [i] of [parent] after a removal.
///
/// Only a leaf borrow allocates (a new separator, when keys are cloned). Remove
/// can't report an error, so if that fails the leaf is left under-full: the
/// tree stays correct, only less balanced, until a later remove fixes it.
static void _btree_rebalance(_btree_t *bt, _btree_node_t *parent, size_t i) {
  size_t min = _btree_min(bt);
  _btree_node_t *c = parent->values[i];
  if (c->nkeys >= min)
    return;

  if (i > 0 && ((_btree_node_t *)parent->values[i - 1])->nkeys > min &&
      _btree_borrow_left(bt, parent, i))
    return;
  if (i < parent->nkeys &&
      ((_btree_node_t *)parent->values[i + 1])->nkeys > min &&
      _btree_borrow_right(bt, parent, i))
    return;

  size_t j = i > 0 ? i - 1 : i;
  _btree_node_t *l = parent->values[j], *r = parent->values[j + 1];
  if (l->nkeys + r->nkeys + (l->leaf ? 0 : 1) > bt->arity)
    return; // only after a failed borrow: can't merge, leave it under-full
  _btree_merge(bt, parent, j);
}

static int _btree_remove_rec(_btree_t *bt, _btree_node_t *node,
                             const void *key, void *old) {
  if (node->leaf) {
    for (size_t i = 0; i < node->nkeys; i++) {
      int cmp = bt->cmp(key, node->keys[i]);
      if (cmp < 0)
        break;
      if (cmp == 0) {
        if (old)
          memcpy(old, node->values[i], bt->valueSize);
        _btree_key_drop(bt, node->keys[i]);
        free(node->values[i]);
        memmove(&node->keys[i], &node->keys[i + 1],
                (node->nkeys - i - 1) * sizeof(void *));
        memmove(&node->values[i], &node->values[i + 1],
                (node->nkeys - i - 1) * sizeof(void *));
        node->nkeys -= 1;
        return 1;
      }
    }
    return 0;
  }

  size_t i = 0;
  while (i < node->nkeys && bt->cmp(key, node->keys[i]) > 0)
    i++;

  if (!_btree_remove_rec(bt, node->values[i], key, old))
    return 0;
  _btree_rebalance(bt, node, i);
  return 1;
}

int btree_remove_raw(_btree_t *bt, const void *key) {
  return btree_remove_ex_raw(bt, key, NULL);
}

int btree_remove_ex_raw(_btree_t *bt, const void *key, void *old) {
  assert(bt);
  if (!bt->root || !_btree_remove_rec(bt, bt->root, key, old))
    return 0;
  bt->size -= 1;

  // the root may be left empty: drop a level (or the whole tree)
  _btree_node_t *root = bt->root;
  if (root->nkeys == 0) {
    bt->root = root->leaf ? NULL : root->values[0];
    _btree_free_shell(root);
  }
  return 1;
}

_btree_iter_t btree_iter_raw(const _btree_t *bt) {
  assert(bt);
  _btree_node_t *node = bt->root;
  while (node && !node->leaf)
    node = node->values[0];
  return (_btree_iter_t){.node = node};
}

int btree_next_raw(_btree_iter_t *it) {
  assert(it);
  while (it->node && it->idx >= it->node->nkeys) {
    it->node = it->node->sibling;
    it->idx = 0;
  }
  if (!it->node) {
    it->item = NULL;
    it->value = NULL;
    return 0;
  }
  it->item = it->node->keys[it->idx];
  it->value = it->node->values[it->idx];
  it->idx += 1;
  return 1;
}

Error btree_extend_raw(_btree_t *bt, const _btree_t *other) {
  assert(bt && "btree is null");
  assert(other && "other is null");
  if (bt == other || !other->size)
    return eok();
  if (bt->keySize != other->keySize || bt->valueSize != other->valueSize)
    return enew(EINVAL,
                "btree sizes differ (key %zu vs %zu, value %zu vs %zu)",
                bt->keySize, other->keySize, bt->valueSize, other->valueSize);
  if (bt->key_clone != other->key_clone || bt->key_free != other->key_free)
    return enew(EINVAL, "btrees own their keys differently");
  _btree_iter_t it = btree_iter_raw(other);
  size_t done = 0;
  while (btree_next_raw(&it)) {
    Error e = btree_insert_raw(bt, it.item, it.value);
    if (!e.ok)
      return enew(e.code, "btree extend stopped after %zu of %zu entries: %s",
                  done, other->size, e.msg);
    done++;
  }
  return eok();
}

int btree_strcmp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

#endif // MOLKIE_BTREE_IMPL

// Tests: compiled where MTEST (all modules) or MOLKIE_BTREE_TEST (just this one)
// is defined. Needs MOLKIE_BTREE_IMPL and
// MOLKIE_ERR_IMPL in the same program.
#if (defined(MTEST) || defined(MOLKIE_BTREE_TEST)) && !defined(MOLKIE_BTREE_TESTS)
#define MOLKIE_BTREE_TESTS
#include "test.h"

static int btree_intcmp_test(const void *a, const void *b) {
  return *(const int *)a - *(const int *)b;
}

/// insert, failing the test if it reports an error
static void bt_ins(_btree_t *t, const void *k, const void *v) {
  Error e = btree_insert_raw(t, k, v);
  if (!e.ok) {
    fprintf(stderr, "error: %s\n", e.msg);
    assert(0 && "btree insert failed");
  }
}

static void btree_insert_empty(void) {
  _btree_t tree = _btree_str_empty(int);

  char *name = "dillon";
  int value = 7;
  bt_ins(&tree, &name, &value);
  ASSERT_EQ(tree.size, 1);
  ASSERT_EQ(((_btree_node_t *)tree.root)->nkeys, 1);

  const char *names[] = {"mike",   "nolan", "redempta", "matt",
                         "carson", "foo",   "bar",      "baz"};
  const int values[] = {1, 2, 3, 4, 5, 6, 7, 8};
  for (size_t i = 0; i < 6; i++) {
    bt_ins(&tree, &names[i], &values[i]);
    ASSERT_EQ(tree.size, 2 + i);
    ASSERT_EQ(((_btree_node_t *)tree.root)->nkeys, 2 + i);
  }
}

// Insert [n] keys in the order given by key(i), then read all back.
static void btree_check_order(size_t n, size_t (*order)(size_t, size_t)) {
  _btree_t tree = _btree_empty(int, int, btree_intcmp_test);
  for (size_t i = 0; i < n; i++) {
    int k = (int)order(i, n), v = (int)order(i, n) * 10;
    bt_ins(&tree, &k, &v);
  }
  ASSERT_EQ(tree.size, n);
  for (size_t i = 0; i < n; i++) {
    int k = (int)i;
    int *v = btree_get_raw(&tree, &k);
    assert(v && "key missing after insert");
    ASSERT_EQ(*v, k * 10);
  }
  int missing = (int)n;
  assert(!btree_get_raw(&tree, &missing));

  // overwrite must not change size
  int k = 3, v = 99, old = 0;
  assert(btree_insert_ex_raw(&tree, &k, &v, &old).ok);
  ASSERT_EQ(tree.size, n);
  ASSERT_EQ(old, 30);
  ASSERT_EQ(*(int *)btree_get_raw(&tree, &k), 99);

  btree_free_raw(&tree);
  ASSERT_EQ(tree.size, 0);
  assert(!tree.root);
}

static size_t ord_asc(size_t i, size_t n) { (void)n; return i; }
static size_t ord_desc(size_t i, size_t n) { return n - 1 - i; }
// i * 7919 mod n is a permutation when gcd(7919, n) == 1 (7919 is prime)
static size_t ord_perm(size_t i, size_t n) { return (i * 7919) % n; }

static void btree_insert_asc(void) { btree_check_order(1000, ord_asc); }
static void btree_insert_desc(void) { btree_check_order(1000, ord_desc); }
static void btree_insert_perm(void) { btree_check_order(1000, ord_perm); }

/// check structural invariants of the subtree at [n]; returns its height.
/// keys in [n] must be in (lo, hi]; NULL means unbounded.
static int btree_validate_lenient; // allow under-full nodes (best-effort remove)
static size_t btree_validate_node(const _btree_t *bt, const _btree_node_t *n,
                                  int isroot, const void *lo, const void *hi) {
  assert(n->nkeys <= bt->arity);
  if (!btree_validate_lenient)
    assert(n->nkeys >= (isroot ? 1 : bt->arity / 2));
  for (size_t i = 1; i < n->nkeys; i++)
    assert(bt->cmp(n->keys[i - 1], n->keys[i]) < 0);
  if (lo)
    assert(bt->cmp(lo, n->keys[0]) < 0 || !n->leaf);
  if (hi)
    assert(bt->cmp(n->keys[n->nkeys - 1], hi) <= 0);
  if (n->leaf)
    return 1;

  size_t height = 0;
  for (size_t i = 0; i <= n->nkeys; i++) {
    size_t h = btree_validate_node(bt, n->values[i], 0,
                                   i ? n->keys[i - 1] : lo,
                                   i < n->nkeys ? n->keys[i] : hi);
    if (i == 0)
      height = h;
    assert(h == height && "leaves at different depths");
  }
  return height + 1;
}

/// validate structure, then check the leaf chain yields [size] ascending keys
static void btree_validate(const _btree_t *bt) {
  if (!bt->root) {
    ASSERT_EQ(bt->size, 0);
    return;
  }
  btree_validate_node(bt, bt->root, 1, NULL, NULL);
  size_t count = 0;
  const int *prev = NULL;
  _btree_iter_t it = btree_iter_raw(bt);
  while (btree_next_raw(&it)) {
    if (prev)
      assert(bt->cmp(prev, it.item) < 0);
    prev = it.item;
    count += 1;
  }
  ASSERT_EQ(count, bt->size);
}

static void btree_check_remove(size_t n, size_t (*ins)(size_t, size_t),
                               size_t (*rem)(size_t, size_t)) {
  _btree_t tree = _btree_empty(int, int, btree_intcmp_test);
  for (size_t i = 0; i < n; i++) {
    int k = (int)ins(i, n), v = k * 10;
    bt_ins(&tree, &k, &v);
  }
  btree_validate(&tree);

  for (size_t i = 0; i < n; i++) {
    int k = (int)rem(i, n), old = -1;
    ASSERT_EQ(btree_contains_raw(&tree, &k), 1);
    int r = btree_remove_ex_raw(&tree, &k, &old);
    ASSERT_EQ(r, 1);
    ASSERT_EQ(old, k * 10);
    ASSERT_EQ(tree.size, n - 1 - i);
    ASSERT_EQ(btree_contains_raw(&tree, &k), 0);
    ASSERT_EQ(btree_remove_raw(&tree, &k), 0);
    btree_validate(&tree);
  }
  assert(!tree.root);
  btree_free_raw(&tree);
}

static void btree_remove_asc_asc(void) {
  btree_check_remove(1000, ord_asc, ord_asc);
}
static void btree_remove_asc_desc(void) {
  btree_check_remove(1000, ord_asc, ord_desc);
}
static void btree_remove_perm_asc(void) {
  btree_check_remove(1000, ord_perm, ord_asc);
}
static void btree_remove_desc_perm(void) {
  btree_check_remove(1000, ord_desc, ord_perm);
}

static void btree_remove_half(void) {
  _btree_t tree = _btree_empty(int, int, btree_intcmp_test);
  const size_t n = 1000;
  for (size_t i = 0; i < n; i++) {
    int k = (int)ord_perm(i, n), v = k * 10;
    bt_ins(&tree, &k, &v);
  }
  for (int k = 0; k < (int)n; k += 2)
    ASSERT_EQ(btree_remove_raw(&tree, &k), 1);
  btree_validate(&tree);
  ASSERT_EQ(tree.size, n / 2);
  for (int k = 0; k < (int)n; k++) {
    int *v = btree_get_raw(&tree, &k);
    if (k % 2) {
      assert(v && "odd key was lost");
      ASSERT_EQ(*v, k * 10);
    } else {
      assert(!v && "even key still present");
    }
  }
  // re-insert after removals still works
  for (int k = 0; k < (int)n; k += 2)
    bt_ins(&tree, &k, &k);
  btree_validate(&tree);
  ASSERT_EQ(tree.size, n);
  btree_free_raw(&tree);
}

static void btree_iter_order(void) {
  _btree_t tree = _btree_empty(int, int, btree_intcmp_test);
  _btree_iter_t empty = btree_iter_raw(&tree);
  ASSERT_EQ(btree_next_raw(&empty), 0);

  const size_t n = 500;
  for (size_t i = 0; i < n; i++) {
    int k = (int)ord_perm(i, n), v = k * 10;
    bt_ins(&tree, &k, &v);
  }
  int expect = 0;
  _btree_iter_t it = btree_iter_raw(&tree);
  while (btree_next_raw(&it)) {
    int key = *(int *)it.item;
    int value = *(int *)it.value;
    ASSERT_EQ(key, expect);
    ASSERT_EQ(value, expect * 10);
    expect += 1;
  }
  ASSERT_EQ(expect, (int)n);
  ASSERT_EQ(btree_next_raw(&it), 0); // stays exhausted
  btree_free_raw(&tree);
}

static void btree_extend_overlap(void) {
  _btree_t a = _btree_empty(int, int, btree_intcmp_test);
  _btree_t b = _btree_empty(int, int, btree_intcmp_test);
  for (int k = 0; k < 300; k++) {
    int v = 1;
    bt_ins(&a, &k, &v);
  }
  for (int k = 200; k < 600; k++) {
    int v = 2;
    bt_ins(&b, &k, &v);
  }
  assert(btree_extend_raw(&a, &b).ok);
  btree_validate(&a);
  ASSERT_EQ(a.size, 600);
  ASSERT_EQ(b.size, 400);
  for (int k = 0; k < 600; k++) {
    int *v = btree_get_raw(&a, &k);
    assert(v);
    int want = k < 200 ? 1 : 2;
    ASSERT_EQ(*v, want);
  }
  assert(btree_extend_raw(&a, &a).ok); // no-op
  ASSERT_EQ(a.size, 600);

  _btree_t s = _btree_str_empty(int);
  assert(btree_extend_raw(&a, &s).ok); // empty source: no-op
  ASSERT_EQ(a.size, 600);
  const char *name = "x";
  int one = 1;
  bt_ins(&s, &name, &one);
  Error err = btree_extend_raw(&a, &s); // mismatched key size is rejected
  assert(!err.ok);
  ASSERT_EQ(err.code, EINVAL);
  assert(strstr(err.msg, "btree sizes differ"));
  ASSERT_EQ(a.size, 600);
  btree_free_raw(&a);
  btree_free_raw(&b);
  btree_free_raw(&s);
}

// An insert that runs out of memory must fail cleanly: report ENOMEM, leave the
// tree valid and unchanged, and succeed when retried with memory available.
// Walks every allocation point of every insert (leaf, split, root growth).
static void btree_insert_oom(void) {
  _btree_t tree = _btree_empty(int, int, btree_intcmp_test);
  for (int k = 0; k < 200; k++) {
    int v = k * 10;
    for (long fail_in = 0;; fail_in++) {
      size_t before = tree.size;
      _btree_fail_in = fail_in;
      Error e = btree_insert_raw(&tree, &k, &v);
      _btree_fail_in = -1;
      if (e.ok)
        break; // enough allocations succeeded: the insert went through
      ASSERT_EQ(e.code, ENOMEM);
      ASSERT_EQ(tree.size, before);
      assert(!btree_get_raw(&tree, &k));
      btree_validate(&tree);
    }
    assert(btree_get_raw(&tree, &k));
    ASSERT_EQ(tree.size, (size_t)k + 1);
    btree_validate(&tree);

    // overwriting needs no memory, so it can't fail even with none left
    _btree_fail_in = 0;
    int v2 = v + 1;
    assert(btree_insert_raw(&tree, &k, &v2).ok);
    _btree_fail_in = -1;
    ASSERT_EQ(*(int *)btree_get_raw(&tree, &k), v2);
  }
  btree_free_raw(&tree);
}

typedef btree_str_t(int) StrMap;

/// the bug this exists for: the caller frees its key strings while the tree
/// (leaf keys AND separators) is still in use
static void btree_str_owned_keys(void) {
  StrMap t = btree_str_empty(int);
  enum { N = 200 };
  for (int i = 0; i < N; i++) {
    char *name = malloc(16);
    assert(name);
    snprintf(name, 16, "k%03d", i);
    const char *k = name;
    assert(btree_insert(&t, &k, &i).ok);
    free(name); // the tree has its own copy
  }
  btree_validate(&t.raw);
  for (int i = 0; i < N; i++) {
    char name[16];
    snprintf(name, sizeof name, "k%03d", i);
    const char *k = name;
    int *v = btree_get(&t, &k);
    assert(v && *v == i);
  }
  // remove a quarter of the keys, freeing nothing: separators that equal a
  // removed key must stay valid on their own
  for (int i = 3; i < N; i += 4) {
    char name[16];
    snprintf(name, sizeof name, "k%03d", i);
    const char *k = name;
    assert(btree_remove(&t, &k));
  }
  btree_validate(&t.raw);
  for (int i = 0; i < N; i++) {
    char name[16];
    snprintf(name, sizeof name, "k%03d", i);
    const char *k = name;
    assert(btree_contains(&t, &k) == (i % 4 != 3));
  }
  // overwrite an existing key with a different (but equal) string
  char other[16] = "k001";
  const char *k = other;
  int nv = 77;
  assert(btree_insert(&t, &k, &nv).ok);
  memset(other, 'z', sizeof other - 1); // clobber the caller's copy
  char want[16] = "k001";
  k = want;
  assert(*btree_get(&t, &k) == 77);
  btree_free(&t); // frees every copy: LeakSanitizer checks this
}

/// an insert that runs out of memory (at any allocation, incl. the key clones)
/// fails cleanly: tree unchanged, nothing leaked
static void btree_str_insert_oom(void) {
  StrMap t = btree_str_empty(int);
  for (int i = 0; i < 100; i++) {
    char name[16];
    snprintf(name, sizeof name, "k%03d", (i * 37) % 100);
    const char *k = name;
    for (long fail_in = 0;; fail_in++) {
      size_t before = t.raw.size;
      _btree_fail_in = fail_in;
      Error e = btree_insert(&t, &k, &i);
      _btree_fail_in = -1;
      if (e.ok)
        break;
      ASSERT_EQ(e.code, ENOMEM);
      ASSERT_EQ(t.raw.size, before);
      assert(!btree_contains(&t, &k));
      btree_validate(&t.raw);
      efree(&e);
    }
    assert(btree_contains(&t, &k));
    btree_validate(&t.raw);
  }
  btree_free(&t);
}

/// remove can't fail, so with no memory for new separators it may leave a leaf
/// under-full: the tree must stay correct and leak nothing
static void btree_str_remove_oom(void) {
  StrMap t = btree_str_empty(int);
  enum { N = 300 };
  for (int i = 0; i < N; i++) {
    char name[16];
    snprintf(name, sizeof name, "k%03d", i);
    const char *k = name;
    assert(btree_insert(&t, &k, &i).ok);
  }
  btree_validate_lenient = 1;
  for (int i = 0; i < N; i++) {
    int j = (i * 113) % N;
    char name[16];
    snprintf(name, sizeof name, "k%03d", j);
    const char *k = name;
    _btree_fail_in = i % 3 == 0 ? 0 : -1; // starve a third of the removes
    int old = -1;
    assert(btree_remove_ex(&t, &k, &old));
    _btree_fail_in = -1;
    ASSERT_EQ(old, j);
    assert(!btree_contains(&t, &k));
    ASSERT_EQ(t.raw.size, (size_t)(N - 1 - i));
    if (i % 25 == 0)
      btree_validate(&t.raw);
  }
  btree_validate_lenient = 0;
  btree_free(&t);
}

static Error btree_test_clone_fail(void *dst, const void *src) {
  (void)dst, (void)src;
  return enew(EIO, "clone refused");
}

static void btree_clone_error(void) {
  StrMap t = btree_str_empty(int);
  t.raw.key_clone = btree_test_clone_fail; // a hook that always fails
  const char *k = "a";
  int v = 1;
  Error e = btree_insert(&t, &k, &v);
  assert(!e.ok);
  ASSERT_EQ(e.code, EIO);
  ASSERT_EQ(t.raw.size, 0);
  efree(&e);
  btree_free(&t);
}

static void btree_extend_owned(void) {
  StrMap a = btree_str_empty(int), b = btree_str_empty(int);
  for (int i = 0; i < 50; i++) {
    char name[16];
    snprintf(name, sizeof name, "k%02d", i);
    const char *k = name;
    assert(btree_insert(&b, &k, &i).ok);
  }
  assert(btree_extend(&a, &b).ok);
  btree_free(&b); // `a` has its own copies
  ASSERT_EQ(a.raw.size, 50);
  const char *k = "k49";
  assert(*btree_get(&a, &k) == 49);
  btree_free(&a);

  // trees that own their keys differently can't be mixed
  StrMap s = btree_str_empty(int);
  StrMap plain = btree_empty(const char *, int, btree_strcmp);
  const char *x = "x";
  int one = 1;
  assert(btree_insert(&s, &x, &one).ok);
  Error e = btree_extend(&plain, &s);
  assert(!e.ok);
  ASSERT_EQ(e.code, EINVAL);
  efree(&e);
  btree_free(&s);
}

static int btree_tests(void) {
  static const TestCase tests[] = {
      T(btree_str_owned_keys),
      T(btree_str_insert_oom),
      T(btree_str_remove_oom),
      T(btree_clone_error),
      T(btree_extend_owned),
      T(btree_insert_empty),
      T(btree_insert_asc),
      T(btree_insert_desc),
      T(btree_insert_perm),
      T(btree_remove_asc_asc),
      T(btree_remove_asc_desc),
      T(btree_remove_perm_asc),
      T(btree_remove_desc_perm),
      T(btree_remove_half),
      T(btree_iter_order),
      T(btree_extend_overlap),
      T(btree_insert_oom),
  };
  size_t n = sizeof tests / sizeof *tests, passed = 0;
  for (size_t i = 0; i < n; i++)
    passed += run_in_child(&tests[i]);
  printf("btree: %zu/%zu passed\n", passed, n);
  return passed == n;
}

#endif // MOLKIE_BTREE_TESTS
