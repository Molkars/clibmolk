// Everything at once: every module's IMPL and tests in one program. Catches
// clashes between modules.
#define MTEST
#define MOLKIE_ERR_IMPL
#define MOLKIE_ARRAY_IMPL
#define MOLKIE_BTREE_IMPL
#define MOLKIE_IO_IMPL
#define MOLKIE_CSTR_IMPL
#define MOLKIE_STR_IMPL

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
  return ok ? 0 : 1;
}
