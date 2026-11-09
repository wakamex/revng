#!/usr/bin/env python3
#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
"""How well rev.ng finds the functions of a C++ binary with exceptions, and which of them never return.

  benchmark.py OUT [--revng REVNG] [--prototypes-db DB] [--count N] [--seed S] [--pie] [--repeat N] [--json FILE]
               [--check]

Generates C++ functions of random kinds that call each other: plain ones, ones that may throw, ones that always
throw, ones whose locals have destructors around a call that may throw (a cleanup landing pad ending in
`_Unwind_Resume`), ones that catch, ones that call a function that always throws on one path, and ones that
dispatch through a `switch` to helpers nothing else calls. About a third are called only through a table of function
pointers, as virtual functions and Rust trait objects are. They are compiled with clang for x86-64, as
position-independent code with --pie, and linked with `ld.lld` against a stub shared library providing the C++
runtime functions, so that, as in a real program, rev.ng sees `__cxa_throw` and `_Unwind_Resume` as imports. A copy
stripped of its symbol table, as hl-node is, is analyzed with `revng project init`, and the model's functions are
compared with the symbol table of the original:

- found: a function of the model starts where the symbol does;
- missing: none does, so the code is part of another function or of none;
- split: a function of the model starts inside the symbol's code, such as at a landing pad;
- false noreturn: the model says the function never returns, and it does;
- missed noreturn: the function always throws, and the model says it may return.

--prototypes-db links a prototypes database into the launcher's resource root for the run, as the hl-node runs and
the Rust corpora use one; without it rev.ng does not know that `__cxa_throw` never returns. --repeat analyzes the
binary that many times and reports how many different models came out; the verdicts are those of the first.
--check exits with status 1 on any missing function, split or false noreturn, and on models that differ.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import re
import subprocess
import sys

import yaml

KINDS = ["plain", "may_throw", "always_throws", "cleanup", "catches", "calls_thrower", "switches"]
# The helpers of a function that dispatches through a `switch`, each called from one case only
CASES = 6
# Where rev.ng places a position-independent binary
PIE_BASE = 0x400000

RUNTIME = """\
void *__cxa_allocate_exception(unsigned long size) { static char buffer[64]; return buffer; }
void __cxa_throw(void *exception, void *type, void (*destructor)(void *)) {}
void *__cxa_begin_catch(void *exception) { return exception; }
void __cxa_end_catch(void) {}
void _Unwind_Resume(void *exception) {}
int __gxx_personality_v0(void) { return 0; }
"""


def plan(count: int, seed: int) -> list:
    """The kind of each function and the earlier functions it calls"""
    generator = random.Random(seed)
    functions = []
    for index in range(count):
        kind = generator.choice(KINDS)
        # A function that must return calls only functions that may return, and one that calls a thrower calls one
        direct = [i for i, f in enumerate(functions) if not f["indirect"]]
        returning = [i for i in direct if functions[i]["kind"] != "always_throws"]
        throwers = [i for i in direct if functions[i]["kind"] == "always_throws"]
        if kind == "calls_thrower" and not throwers:
            kind = "plain"
        callees = generator.sample(returning, min(len(returning), generator.randint(0, 2)))
        thrower = generator.choice(throwers) if kind == "calls_thrower" else None
        functions.append({"kind": kind, "callees": callees, "thrower": thrower, "k": generator.randint(2, 9),
                          "indirect": generator.random() < 0.3})
    return functions


def source(functions: list) -> str:
    lines = ["struct Error { int value; };",
             "struct Guard { int *counter; ~Guard() { *counter += 1; } };",
             "volatile int input;", "int sink;"]
    for index, function in enumerate(functions):
        k = function["k"]
        calls = " + ".join(f"f{c}(x + {k})" for c in function["callees"]) or "0"
        kind = function["kind"]
        if kind == "always_throws":
            lines.append(f"__attribute__((noinline, noreturn)) int f{index}(int x) {{ throw Error{{x * {k}}}; }}")
            continue
        if kind == "switches":
            for case in range(CASES):
                lines.append(f"__attribute__((noinline)) int g{index}_{case}(int x) {{ if (x > {case + k}) "
                             f"throw Error{{x}}; return x * {case + 2}; }}")
        cases = " ".join(f"case {case}: return g{index}_{case}(x) + {case};" for case in range(CASES))
        body = {
            "switches": f"Guard guard{{&sink}}; switch (x & 7) {{ {cases} default: return x * {k} + {calls}; }}",
            "plain": f"return x * {k} + {calls};",
            "may_throw": f"if (x > {k}) throw Error{{x}}; return x * {k} + {calls};",
            "cleanup": f"Guard guard{{&sink}}; return x * {k} + {calls} + (x > {k} ? (throw Error{{x}}, 0) : 0);",
            "catches": f"try {{ return {calls} + (x > {k} ? (throw Error{{x}}, 0) : x); }} "
                       f"catch (Error &e) {{ return e.value + {k}; }}",
            "calls_thrower": f"if (x == {k}) f{function['thrower']}(x); return x + {k} + {calls};",
        }[kind]
        lines.append(f"__attribute__((noinline)) int f{index}(int x) {{ {body} }}")
    indirect = [i for i, f in enumerate(functions) if f["indirect"]]
    if indirect:
        lines.append(f"int (*table[])(int) = {{ {', '.join(f'f{i}' for i in indirect)} }};")
    lines.append('extern "C" __attribute__((noinline)) void run() {')
    for index, function in enumerate(functions):
        if not function["indirect"]:
            lines.append(f"  try {{ sink += f{index}(input); }} catch (...) {{ sink += 1; }}")
    if indirect:
        lines.append(f"  for (int i = 0; i < {len(indirect)}; ++i)")
        lines.append("    try { sink += table[i](input); } catch (...) { sink += 1; }")
    lines.append("}")
    lines.append('extern "C" void _start() { run(); for (;;) {} }')
    return "\n".join(lines) + "\n"


def build(directory: str, text: str, pie: bool = False) -> tuple:
    """The stripped binary and, as rev.ng addresses them, the address and size of each function of the original"""
    def path(name):
        return os.path.join(directory, name)

    with open(path("functions.cpp"), "w") as stream:
        stream.write(text)
    with open(path("runtime.c"), "w") as stream:
        stream.write(RUNTIME)
    with open(path("typeinfo.c"), "w") as stream:
        stream.write("void *_ZTVN10__cxxabiv117__class_type_infoE[4];\n")
    target = "--target=x86_64-unknown-linux-gnu"
    subprocess.run(["clang", target, "-O2", "-fPIC", "-c", path("runtime.c"), "-o", path("runtime.o")], check=True)
    subprocess.run(["ld.lld", "-shared", "-soname", "libruntime.so", path("runtime.o"), "-o",
                    path("libruntime.so")], check=True)
    model = "-fPIE" if pie else "-fno-pic"
    subprocess.run(["clang++", target, "-O2", model, "-c", path("functions.cpp"), "-o", path("functions.o")],
                   check=True)
    subprocess.run(["clang", target, "-O2", model, "-c", path("typeinfo.c"), "-o", path("typeinfo.o")], check=True)
    binary = path("functions")
    subprocess.run(["ld.lld", *(["-pie"] if pie else []), "--eh-frame-hdr", "-e", "_start", "--dynamic-linker",
                    "/lib64/ld-linux-x86-64.so.2", path("functions.o"), path("typeinfo.o"), path("libruntime.so"),
                    "-o", binary], check=True)
    output = subprocess.run(["llvm-nm", "--print-size", "--defined-only", binary], check=True, capture_output=True,
                            text=True).stdout
    stripped = binary + ".stripped"
    subprocess.run(["llvm-strip", "--strip-all", binary, "-o", stripped], check=True)
    symbols = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 4 and fields[2] in "Tt":
            symbols[fields[3]] = (int(fields[0], 16) + (PIE_BASE if pie else 0), int(fields[1], 16))
    return stripped, symbols


def resource_link(revng: str) -> str:
    """Where rev.ng looks for its prototypes database, as tests/rust-differential/check.sh links it"""
    return os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(revng))), "share", "revng",
                        "prototypes.sqlite")


def analyze(revng: str, binary: str, directory: str, database: str | None, run: int = 0) -> tuple:
    """The model of the binary and a hash that two runs share when their models are the same"""
    project = os.path.join(directory, "project" if run == 0 else f"project-{run}")
    os.makedirs(project, exist_ok=True)
    link = resource_link(revng)
    if database:
        if os.path.lexists(link):
            raise SystemExit(f"{link} exists already; another run may be using it")
        os.symlink(os.path.abspath(database), link)
    try:
        with open(os.path.join(project, "init.log"), "w") as log:
            subprocess.run([revng, "project", "--storage-provider", "local://?inline", "init",
                            os.path.abspath(binary)], cwd=project, check=True, stdout=log, stderr=subprocess.STDOUT)
    finally:
        if database:
            os.remove(link)
    with open(os.path.join(project, "revng.yml")) as stream:
        text = stream.read()
    return yaml.load(text, Loader=getattr(yaml, "CSafeLoader", yaml.SafeLoader)), identity(text)


def identity(model_text: str) -> str:
    """A hash of a model without the path of its binary, which names the project"""
    lines = [line for line in model_text.splitlines() if "CanonicalPath:" not in line]
    return hashlib.sha256("\n".join(lines).encode()).hexdigest()[:16]


def scored(functions: list) -> list:
    """The name, kind and whether it is called only through the table, of each generated function and helper"""
    result = []
    for index, function in enumerate(functions):
        result.append((f"f{index}", function["kind"], function["indirect"]))
        if function["kind"] == "switches":
            result += [(f"g{index}_{case}", "case_only", False) for case in range(CASES)]
    return result


def score(functions: list, symbols: dict, model: dict) -> list:
    entries = {}
    for function in model.get("Functions", []):
        attributes = function.get("Attributes", [])
        if "AlwaysInline" in attributes:
            continue
        entries[int(function["Entry"].split(":")[0], 16)] = "NoReturn" in attributes
    result = []
    for name, kind, indirect in scored(functions):
        begin, size = symbols[f"_Z{len(name)}{name}i"]
        noreturn = kind == "always_throws"
        splits = sorted(f"0x{a:x}" for a in entries if begin < a < begin + size)
        verdicts = []
        if begin not in entries:
            verdicts.append("missing")
        elif entries[begin] and not noreturn:
            verdicts.append("false noreturn")
        elif noreturn and not entries[begin]:
            verdicts.append("missed noreturn")
        if splits:
            verdicts.append("split")
        result.append({"function": name, "kind": kind, "indirect": indirect, "address": f"0x{begin:x}",
                       "verdicts": verdicts or ["found"], "splits": splits})
    return result


VERDICTS = ["found", "missing", "split", "false noreturn", "missed noreturn"]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out", help="directory for the sources, the binary and the project")
    parser.add_argument("--revng", default="revng")
    parser.add_argument("--prototypes-db", help="prototypes database to link into the launcher's resource root")
    parser.add_argument("--count", type=int, default=60)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--pie", action="store_true", help="build position-independent code")
    parser.add_argument("--repeat", type=int, default=1, help="analyze the binary this many times")
    parser.add_argument("--json", help="write every function's verdicts here")
    parser.add_argument("--check", action="store_true",
                        help="fail on any missing function, split or false noreturn, and on models that differ")
    args = parser.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    functions = plan(args.count, args.seed)
    binary, symbols = build(args.out, source(functions), args.pie)
    runs = [analyze(args.revng, binary, args.out, args.prototypes_db, run) for run in range(args.repeat)]
    result = score(functions, symbols, runs[0][0])
    models = sorted({model_identity for _, model_identity in runs})
    print("| kind | functions | " + " | ".join(VERDICTS) + " |")
    print("| --- | --- |" + " --- |" * len(VERDICTS))
    for kind in KINDS + ["case_only", "indirect", "all"]:
        entries = [e for e in result if kind in ("all", e["kind"]) or (kind == "indirect" and e["indirect"])]
        counts = [sum(1 for e in entries if v in e["verdicts"]) for v in VERDICTS]
        print(f"| {kind} | {len(entries)} | " + " | ".join(str(c) for c in counts) + " |")
    if args.repeat > 1:
        print(f"{len(models)} different models in {args.repeat} runs")
    if args.json:
        with open(args.json, "w") as stream:
            json.dump(result, stream, indent=1)
    if args.check:
        failed = [e for e in result if {"missing", "split", "false noreturn"} & set(e["verdicts"])]
        for entry in failed:
            print(f"{entry['function']} {entry['kind']} {entry['address']}: {', '.join(entry['verdicts'])}"
                  f"{' at ' + ', '.join(entry['splits']) if entry['splits'] else ''}")
        return 1 if failed or len(models) > 1 else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
