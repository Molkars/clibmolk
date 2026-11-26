#!/usr/bin/env python3
"""Build and run each module's tests with ONLY its declared dependencies.

For every header in deps.json this generates a tiny program that defines the
IMPL macro of the module and of its transitive deps, includes just those
headers, and runs `<module>_tests()`. A test that quietly needs some other
module fails to compile here, so the dependency list stays honest.
"""
import json, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = os.environ.get("CC", "gcc")
CFLAGS = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
          "-Wpedantic", "-Wformat=2", "-Werror", "-I" + os.path.join(ROOT, "inc")]
NO_TESTS = {"attr", "test"}  # no runnable tests of their own


def jump_flag():
    """-Wjump-misses-init where the compiler has it (gcc): a goto that skips an
    initialiser is a bug with etry."""
    r = subprocess.run([CC, "-Werror", "-Wjump-misses-init", "-x", "c",
                        "-fsyntax-only", "-"], input="", capture_output=True, text=True)
    return ["-Wjump-misses-init"] if r.returncode == 0 else []


def closure(deps, name):
    seen, todo = [], [name]
    while todo:
        n = todo.pop()
        if n not in seen:
            seen.append(n)
            todo += deps[n]
    return seen


def main():
    deps = json.load(open(os.path.join(ROOT, "deps.json")))
    only = sys.argv[1:] or sorted(deps)
    failed = []
    with tempfile.TemporaryDirectory() as tmp:
        for name in only:
            if name in NO_TESTS:
                continue
            mods = closure(deps, name)
            src = [f"#define MOLKIE_{name.upper()}_TEST"]
            src += [f"#define MOLKIE_{m.upper()}_IMPL" for m in mods if m != "test"]
            src += [f'#include "{m}.h"' for m in mods if m != "attr"]
            src += [f"int main(void) {{ return {name}_tests() ? 0 : 1; }}"]
            c = os.path.join(tmp, name + ".c")
            exe = os.path.join(tmp, name)
            open(c, "w").write("\n".join(src) + "\n")
            r = subprocess.run([CC, *CFLAGS, *jump_flag(), "-o", exe, c], capture_output=True, text=True)
            if r.returncode:
                print(f"BUILD FAIL {name} (needs: {', '.join(mods[1:]) or 'nothing'})")
                print(r.stderr)
                failed.append(name)
                continue
            r = subprocess.run([exe], capture_output=True, text=True)
            print(r.stdout.strip() or f"{name}: (no output)")
            if r.returncode:
                print(r.stderr)
                failed.append(name)
    if failed:
        print("FAILED:", ", ".join(failed))
        return 1
    print("standalone: all modules pass with only their declared deps")
    return 0


sys.exit(main())
