// Everything at once: every module's IMPL and tests in one program. Catches
// clashes between modules and exercises the cross-module extensions (the
// order writer.h is included in matters for them).
#define MTEST
#define MOLKIE_ERR_IMPL
#define MOLKIE_WRITER_IMPL
#define MOLKIE_ARRAY_IMPL
#define MOLKIE_BTREE_IMPL
#define MOLKIE_IO_IMPL
#define MOLKIE_CSTR_IMPL
#define MOLKIE_STR_IMPL

#include "writer.h" // before io.h: io's tests then print full error chains
#include "err.h"
#include "array.h"
#include "btree.h"
#include "io.h"
#include "cstr.h"
#include "str.h"

int main(void) {
  int ok = 1;
  ok &= array_tests();
  ok &= btree_tests();
  ok &= cstr_tests();
  ok &= err_tests();
  ok &= io_tests();
  ok &= str_tests();
  ok &= writer_tests();
  return ok ? 0 : 1;
}
