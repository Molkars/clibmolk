#!/usr/bin/env python3
"""Check deps.json against the `#include "x.h"` lines in inc/*.h.

Includes inside the MTEST block don't count: tests may use `test.h` and
nothing else. Every other quoted include (declarations or IMPL) is a dep.
"""
import json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
deps = json.load(open(os.path.join(ROOT, "deps.json")))
bad = 0
for name in sorted(deps):
    text = open(os.path.join(ROOT, "inc", name + ".h")).read()
    text = re.split(r"^#if \(defined\(MTEST\)", text, flags=re.M)[0]
    found = sorted(set(re.findall(r'^#include "(\w+)\.h"', text, flags=re.M)))
    if found != sorted(deps[name]):
        print(f"{name}: includes {found} but deps.json says {sorted(deps[name])}")
        bad = 1
    # test.h is only for tests
    if "test" in found:
        print(f"{name}: non-test code includes test.h")
        bad = 1
for f in os.listdir(os.path.join(ROOT, "inc")):
    if f.endswith(".h") and f[:-2] not in deps:
        print(f"{f}: missing from deps.json")
        bad = 1
sys.exit(bad)
