# clibmolk

Small single-header C modules (C11, POSIX). Copy the files you want, or fetch
them from a release with the bundled script.

See [GUIDE.md](GUIDE.md) for how to use each module.

## Using a module

In exactly one `.c` file per module, define its IMPL macro before including:

```c
#define MOLKIE_ERR_IMPL
#define MOLKIE_ARRAY_IMPL
#include "array.h"   // pulls in err.h
```

Everywhere else just `#include` the header. The IMPL of a module's
dependencies must be defined somewhere in the program too.

## Modules and dependencies

| header     | needs                    | notes                                   |
|------------|--------------------------|-----------------------------------------|
| `attr.h`   | –                        | `PRINTF_LIKE`                           |
| `test.h`   | –                        | tiny fork-per-test runner, `ASSERT_EQ`  |
| `err.h`    | attr                     | `Error`, `enew`, `etry`, `efprint`, ... |
| `io.h`     | attr, err                | EINTR-safe `FILE*` helpers, `Writer`, `Reader`, `{}` formatting (`io_fprint`) |
| `array.h`  | err                      | typed growable array                    |
| `btree.h`  | err                      | typed B-tree map; `btree_str_t` owns its keys |
| `cstr.h`   | attr, err                | C-string helpers                        |
| `str.h`    | array, attr, cstr, err, io | growable `String`                       |

`deps.json` is the source of truth (`just deps` checks it against the
includes). `err` has no idea `Writer` exists: `efprint` (plain `FILE *`) is in `err.h`, `eprint` (`Writer`) is in `io.h`.

## Fetching

Get the script (Python 3 only):

```sh
curl -fsSLO https://raw.githubusercontent.com/Molkars/clibmolk/main/clibmolk
chmod +x clibmolk
```

Then:

```sh
./clibmolk download array.h v0.1.0            # + err.h, attr.h
./clibmolk download str.h io.h v0.1.0 --dest vendor
./clibmolk download err.h latest --no-deps
./clibmolk list v0.1.0
```

Dependencies come from the `deps.json` of the same release. Existing files
that differ are left alone unless `--force`. Needs only Python 3.

## Tests

Tests live in each header, behind `MTEST` (all modules) or
`MOLKIE_<NAME>_TEST` (one module), and need the module's IMPL in the same
program. A module's tests may only use its declared dependencies plus
`test.h`. Anything needing another module would be an *extension* guarded on
that module's include guard.

```sh
just test         # deps check, everything at once, each module standalone, user-type formatting, typecheck
just standalone   # each module with only its declared deps
```

## Releasing

```sh
just release v0.1.0   # tests, then `gh release create` with inc/*.h + deps.json
```
