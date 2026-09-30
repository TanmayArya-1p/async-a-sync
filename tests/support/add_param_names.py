#!/usr/bin/env python3
"""Stands in for the patched clang in the tests that run the FilAsync pass on
IR from a stock clang: attaches !filc_async.params, the parameter names of
each annotated function, as the clang patch does.

Usage: add_param_names.py SOURCE.c IR.ll   (rewrites IR.ll in place)

Each annotated function's names come from the first prototype in SOURCE.c
that names all its parameters.
"""
import re
import sys

TYPE_WORDS = {"void", "char", "short", "int", "long", "unsigned", "signed",
              "float", "double", "const", "volatile", "size_t"}

source = open(sys.argv[1]).read()
ir_path = sys.argv[2]
ir = open(ir_path).read()


def param_names(name):
    for m in re.finditer(r"\b%s\s*\(([^()]*)\)" % re.escape(name), source):
        names = []
        for param in m.group(1).split(","):
            param = param.strip()
            if param in ("", "void"):
                continue
            last = re.search(r"([A-Za-z_]\w*)\s*$", param)
            names.append(last.group(1) if last and last.group(1) not in TYPE_WORDS else "")
        if names and all(names):
            return names
    return None


# The functions the annotations name: the first field of each entry of
# llvm.global.annotations.
table = re.search(r"^@llvm\.global\.annotations = .*$", ir, re.M)
functions = set(re.findall(r"\{ ptr @([\w.$]+),", table.group(0))) if table else set()

next_id = max((int(n) for n in re.findall(r"^!(\d+) = ", ir, re.M)), default=-1) + 1
nodes = []
lines = ir.split("\n")
for i, line in enumerate(lines):
    m = re.match(r"(declare|define)\b.*?@([\w.$]+)\(", line)
    if not m or m.group(2) not in functions:
        continue
    names = param_names(m.group(2))
    if names is None:
        continue
    attach = " !filc_async.params !%d" % next_id
    nodes.append("!%d = !{%s}" % (next_id, ", ".join('!"%s"' % n for n in names)))
    next_id += 1
    # A definition's attachments follow its attributes; a declaration's
    # follow the keyword.
    if m.group(1) == "define":
        lines[i] = line[:-2] + attach + " {"
    else:
        lines[i] = "declare" + attach + line[len("declare"):]

open(ir_path, "w").write("\n".join(lines).rstrip("\n") + "\n" + "\n".join(nodes) + "\n")
