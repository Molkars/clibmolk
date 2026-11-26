// Formatting your own types: IO_FORMAT_TYPES has to be defined before io.h, so
// this can't live in io.h's own tests.
#include <assert.h>
#include <string.h>

typedef struct {
  int x, y;
} Vec2;
typedef struct {
  Vec2 a, b;
} Seg;
#define IO_FORMAT_TYPES(X) X(Vec2) X(Seg)

#define MOLKIE_ERR_IMPL
#define MOLKIE_IO_IMPL
#define MOLKIE_ARRAY_IMPL
#define MOLKIE_CSTR_IMPL
#define MOLKIE_STR_IMPL
#include "str.h" // brings its own String formatter along with the user's

IO_FORMAT_DEFINE(Vec2)
IO_FORMAT_DEFINE(Seg)

Error Vec2_format(Writer w, const Vec2 *v, FormatSpec spec) {
  (void)spec;
  return io_fprint(w, "({}, {})", v->x, v->y);
}

// formatters can nest: Seg uses Vec2's
Error Seg_format(Writer w, const Seg *v, FormatSpec spec) {
  (void)spec;
  return io_fprint(w, "{} -> {}", &v->a, &v->b);
}

#define CHECK(want, ...)                                                       \
  do {                                                                         \
    char b_[128];                                                              \
    Scratch s_ = Scratch_init(b_, sizeof b_);                                  \
    Error e_ = io_fprint(Scratch_writer(&s_), __VA_ARGS__);                    \
    assert(e_.ok && !strcmp(b_, want));                                        \
  } while (0)

int main(void) {
  Vec2 p = {1, -2};
  const Vec2 *cp = &p;
  Seg s = {{0, 0}, {3, 4}};
  CHECK("at (1, -2)", "at {}", &p);
  CHECK("at (1, -2)", "at {}", cp);
  CHECK("(0, 0) -> (3, 4)", "{}", &s);
  CHECK("n=7 (1, -2) x", "n={} {} {}", 7, &p, "x");
  String name = String_fromc("bob");
  CHECK("bob (1, -2)", "{} {}", &name, &p);
  String_free(&name);
  return 0;
}
