// The forked test runner exits children with _exit(), so LeakSanitizer never
// reports from inside it. Run the allocation-heavy tests directly, in one
// process, so `just asan` catches leaks and use-after-free. Needs -fsanitize.
#define MOLKIE_ERR_IMPL
#define MOLKIE_BTREE_IMPL
#define MOLKIE_BTREE_TEST
#include "btree.h"

#define MOLKIE_ARRAY_IMPL
#define MOLKIE_CSTR_IMPL
#define MOLKIE_IO_IMPL
#define MOLKIE_STR_IMPL
#define MOLKIE_PARSER_IMPL
#define MOLKIE_PARSER_TEST
#include "parser.h"

int main(void) {
  btree_str_owned_keys();
  btree_str_insert_oom();
  btree_str_remove_oom();
  btree_clone_error();
  btree_extend_owned();
  btree_insert_oom();
  parser_numbers();
  parser_number_errors();
  parser_group();
  parser_group_errors();
  parser_record();
  parser_stream();
  parser_read_error();
  parser_fail();
  parser_lines();
  parser_p_line();
  parser_list();
  puts("leaks: ok");
  return 0;
}
