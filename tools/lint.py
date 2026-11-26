#!/usr/bin/env python3
"""Lint the `etry` / `edefer` conventions (see err.h and GUIDE.md).

A function that uses `etry`, `etry2`, `edefer`, `edefer_void` or `erundefers`
is a "single exit" function. For every one of them this checks:

  eret-decl     `Error eret` is declared before the first etry/edefer
  end-label     an `end:` label exists
  early-return  no `return` between the first etry/edefer and `end:`: it would
                skip the cleanup (returns BEFORE the first one are fine, they
                are guard clauses)
  end-return    the function finishes with `return eret;` after `end:`
  late-init     no initialised variable is declared after the first etry/
                edefer at function level (the goto skips the initialiser;
                gcc's -Wjump-misses-init says the same, this works on clang too)
  defer-setup   edefer needs EDEFERS(n) before it and erundefers() after `end:`;
                erundefers() needs EDEFERS(n)

Usage: lint.py [files...]   (default: inc/*.h tests/*.c typecheck/*.c)
       lint.py --selftest   (checks the checks)
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
USES = re.compile(r"\b(etry2?|edefer|edefer_void|erundefers|EDEFERS)\s*\(")
FIRST = re.compile(r"\b(etry2?|edefer|edefer_void)\s*\(")
KEYWORDS = {"return", "goto", "if", "else", "for", "while", "do", "switch",
            "case", "break", "continue", "sizeof", "typedef", "default"}
DECL = re.compile(
    r"^\s*(?:(?:const|static|unsigned|signed|struct|enum|volatile)\s+)*"
    r"([A-Za-z_]\w*)(?:\s+|\s*\*+\s*)\**([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*=")


def strip(text):
    """Blank out comments, string/char literals and preprocessor lines, keeping
    every newline so line numbers survive."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            while i < n and text[i] != "\n":
                i += 1
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c in "\"'":
            q, j = c, i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            out.append(q + q)
            i = j + 1
        elif c == "#" and re.match(r"\s*$", text[text.rfind("\n", 0, i) + 1:i]):
            j = i
            while j < n:  # to end of line, following backslash continuations
                e = text.find("\n", j)
                e = n if e < 0 else e
                cont = text[j:e].rstrip().endswith("\\")
                out.append("\n" if e < n else "")
                j = e + 1
                if not cont:
                    break
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def functions(code):
    """Yield (start_line, body) for every top-level `...) { body }`."""
    depth, i, n, line = 0, 0, len(code), 1
    while i < n:
        c = code[i]
        if c == "\n":
            line += 1
        elif c == "{":
            if depth == 0:
                head = code[:i].rstrip()
                if head.endswith(")"):
                    j, d = i, 0
                    start = line
                    while j < n:
                        if code[j] == "{":
                            d += 1
                        elif code[j] == "}":
                            d -= 1
                            if d == 0:
                                break
                        j += 1
                    yield start, code[i + 1:j]
                    line += code.count("\n", i, j)
                    i = j
                    continue
            depth += 1
        elif c == "}":
            depth = max(0, depth - 1)
        i += 1


def line_of(body, pos):
    return body.count("\n", 0, pos)


def depth_map(body):
    """depth (0 = function level) at the start of each character."""
    d, out = 0, []
    for c in body:
        out.append(d)
        if c == "{":
            d += 1
        elif c == "}":
            d -= 1
    return out


def check(body):
    """Return a list of (line_offset, rule, message) for one function body."""
    if not USES.search(body):
        return []
    found = []
    first = FIRST.search(body)
    end = re.search(r"^[ \t]*end[ \t]*:", body, re.M)
    first_pos = first.start() if first else None

    decl = re.search(r"\bError\s+eret\b", body)
    if first and (not decl or decl.start() > first_pos):
        found.append((line_of(body, first_pos), "eret-decl",
                      "`Error eret` must be declared before the first etry/edefer"))
    if not end:
        found.append((0, "end-label", "uses etry/edefer but has no `end:` label"))
        return found

    returns = [m.start() for m in re.finditer(r"\breturn\b", body)]
    if first:
        for r in returns:
            if first_pos < r < end.start():
                found.append((line_of(body, r), "early-return",
                              "return before `end:` skips the cleanup (use etry or goto end)"))
    after = [r for r in returns if r > end.start()]
    tail = body[end.end():]
    if not re.search(r"\breturn\s+eret\s*;\s*$", tail):
        found.append((line_of(body, end.start()), "end-return",
                      "the function must finish with `return eret;` after `end:`"))
    elif len(after) != 1:
        found.append((line_of(body, end.start()), "end-return",
                      "exactly one `return eret;` is expected after `end:`"))

    if first:
        depths = depth_map(body)
        pos = 0
        for ln in body.split("\n"):
            if first_pos < pos < end.start() and depths[pos] == 0:
                m = DECL.match(ln)
                if m and m.group(1) not in KEYWORDS and m.group(2) not in KEYWORDS:
                    found.append((line_of(body, pos), "late-init",
                                  "initialised declaration after the first etry/edefer: "
                                  "the goto skips it; declare it above"))
            pos += len(ln) + 1

    has_defers = re.search(r"\bEDEFERS\s*\(", body)
    uses_defer = re.search(r"\bedefer(_void)?\s*\(", body)
    runs = re.search(r"\berundefers\s*\(", body)
    if (uses_defer or runs) and not has_defers:
        found.append((0, "defer-setup", "edefer/erundefers without EDEFERS(n)"))
    if uses_defer and has_defers and has_defers.start() > uses_defer.start():
        found.append((line_of(body, uses_defer.start()), "defer-setup",
                      "EDEFERS(n) must come before the first edefer"))
    if uses_defer and not (runs and runs.start() > end.start()):
        found.append((line_of(body, uses_defer.start()), "defer-setup",
                      "edefer without `erundefers()` after `end:`"))
    if runs and runs.start() < end.start():
        found.append((line_of(body, runs.start()), "defer-setup",
                      "erundefers() belongs after `end:`"))
    return found


def lint_text(text):
    out = []
    for start, body in functions(strip(text)):
        for off, rule, msg in check(body):
            out.append((start + off, rule, msg))
    return out


def main():
    args = sys.argv[1:]
    if args == ["--selftest"]:
        return selftest()
    files = args or sorted(
        glob.glob(os.path.join(ROOT, "inc", "*.h"))
        + glob.glob(os.path.join(ROOT, "tests", "*.c"))
        + glob.glob(os.path.join(ROOT, "typecheck", "*.c")))
    bad = 0
    for f in files:
        for line, rule, msg in lint_text(open(f).read()):
            print(f"{os.path.relpath(f, ROOT)}:{line}: {rule}: {msg}")
            bad = 1
    if not bad:
        print(f"lint: {len(files)} files ok")
    return bad


GOOD = """
Error ok1(void) {
  Error eret = eok();
  FILE *f = NULL;
  etry(io_open(&f, "x", "r"));
end:
  if (f) ejoin(&eret, io_close(f));
  return eret;
}
Error guard(const char *p) {
  if (!p)
    return enew(EINVAL, "null");   // before any etry: fine
  Error eret = eok();
  etry(thing(p));
end:
  return eret;
}
Error deferred(void) {
  Error eret = eok();
  EDEFERS(2);
  FILE *f;
  etry(io_open(&f, "x", "r"));
  edefer(io_closeV, f);
end:
  erundefers();
  return eret;
}
Error noetry(void) { return eok(); }
#define MACRO etry(x); return 1;
"""

BAD = {
    "end-label": "Error f(void) {\n Error eret = eok();\n etry(g());\n return eret;\n}",
    "eret-decl": "Error f(void) {\n etry(g());\n Error eret = eok();\nend:\n return eret;\n}",
    "early-return": "Error f(void) {\n Error eret = eok();\n etry(g());\n if (x)\n  return eok();\nend:\n return eret;\n}",
    "end-return": "Error f(void) {\n Error eret = eok();\n etry(g());\nend:\n return eok();\n}",
    "late-init": "Error f(void) {\n Error eret = eok();\n etry(g());\n int n = 5;\nend:\n return eret;\n}",
    "defer-setup": "Error f(void) {\n Error eret = eok();\n EDEFERS(1);\n edefer(c, p);\nend:\n return eret;\n}",
}


def selftest():
    bad = 0
    got = lint_text(GOOD)
    if got:
        print("selftest: false positives:", got)
        bad = 1
    for rule, src in BAD.items():
        rules = {r for _, r, _ in lint_text(src)}
        if rule not in rules:
            print(f"selftest: {rule} not detected (got {sorted(rules)})")
            bad = 1
    if not bad:
        print(f"lint selftest: ok ({len(BAD)} rules)")
    return bad


if __name__ == "__main__":
    sys.exit(main())
