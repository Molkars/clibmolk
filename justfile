cc := env("CC", "gcc")
cflags := "-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Wformat=2 -Wmissing-format-attribute -Werror -Iinc"

default: test

# everything: deps check, all modules at once, each module standalone, typecheck
test: deps all asan standalone fmt-user typecheck

# deps.json matches what the headers include
deps:
  python3 tools/check_deps.py

# all modules + extensions in one program
all:
  mkdir -p dist
  {{cc}} {{cflags}} -o dist/.all_test tests/all.c
  dist/.all_test

# everything under ASan + UBSan
asan:
  mkdir -p dist
  {{cc}} {{cflags}} -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o dist/.all_asan tests/all.c
  dist/.all_asan
  {{cc}} {{cflags}} -Wno-unused-function -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o dist/.leaks tests/leaks.c
  dist/.leaks

# each module with only its declared deps
standalone:
  python3 tools/standalone.py

# io.h formatting of user types (needs IO_FORMAT_TYPES before the include)
fmt-user:
  mkdir -p dist
  {{cc}} {{cflags}} -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -o dist/.fmt_user tests/fmt_user.c
  dist/.fmt_user
  @echo "fmt-user: ok"

# compile-time checks for the typed array/btree/format API: the good code must
# compile and every BAD_* case in typecheck/cases.c must be rejected
typecheck:
  #!/usr/bin/env bash
  set -u
  base="{{cc}} {{cflags}} -fsyntax-only"
  $base typecheck/cases.c || { echo "typecheck: good cases do not compile"; exit 1; }
  fail=0
  for bad in $(grep -o 'BAD_[A-Z][A-Z0-9_]*' typecheck/cases.c | sort -u); do
    if $base -D$bad typecheck/cases.c >/dev/null 2>&1; then
      echo "NOT REJECTED $bad"; fail=1
    else
      echo "rejected     $bad"
    fi
  done
  exit $fail

# release assets: every header plus deps.json, into dist/
dist:
  rm -rf dist && mkdir dist
  cp inc/*.h deps.json dist/

# publish dist/ as a GitHub release (run `just test` first). e.g. just release v0.1.0
release tag: test dist
  gh release create {{tag}} dist/* --repo molkars/clibmolk --title {{tag}} --generate-notes

# rebuild compile_commands.json for LSP help
bear:
  bear -- just all

clean:
  rm -rf dist compile_commands.json
