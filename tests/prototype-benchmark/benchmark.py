#!/usr/bin/env python3
#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
"""How often rev.ng recovers the C prototypes of functions whose signatures are known.

  benchmark.py OUT [--revng REVNG] [--abi sysv|microsoft ...] [--count N] [--seed S] [--json FILE] [--check]

Generates C functions with random signatures of integers, pointers, floats and doubles, up to eight arguments, so
that some go on the stack, and a caller that calls each with values the compiler cannot see. They are compiled with
clang for x86-64 under the System V ABI (a static ELF) and the Microsoft ABI (a PE linked with lld-link), each
binary is analyzed with `revng project init`, and the prototype rev.ng gives each function is compared with its
source signature:

- c-exact: a C prototype the ABI lays out as the source signature: a value of the same class in each register, and
  as many stack arguments;
- c-wrong: a C prototype laid out otherwise;
- raw-right: a raw prototype with the registers the ABI puts the source's arguments and return value in, and a stack
  area of the same 8-byte slots, so only the conversion to C is missing;
- raw-wrong: a raw prototype with other registers or another stack area;
- missing: no function at the address of the source's.

With --check it exits with status 1 if any function is c-wrong, raw-wrong or missing: raw-right is a function the
conversion to C declined, which is safe.

The comparison is by layout because that is all the machine code shows. The classes are integer, which includes
pointers, and floating point, since a register does not tell an integer from a pointer, and rev.ng tracks vector
registers in 8-byte parts, which do not tell a float from a double. A stack argument has no class, and under System V
the order of integer arguments relative to floating point ones does not change the layout.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
import subprocess
import sys

import yaml

INTEGER = {"i32": "int", "i64": "long long", "ptr": "char *"}
FLOATING = {"f32": "float", "f64": "double"}
C_TYPES = {**INTEGER, **FLOATING}
RETURNS = ["void", "i32", "i64", "ptr", "f32", "f64"]
SYSV_INTEGER = ["rdi", "rsi", "rdx", "rcx", "r8", "r9"]
MICROSOFT_INTEGER = ["rcx", "rdx", "r8", "r9"]


def class_of(kind: str) -> str:
    """The class a register shows: int for integers and pointers, float for float and double"""
    return "float" if kind in FLOATING else "int"


def placement(classes: list, returned: str, microsoft: bool) -> dict:
    """Where the ABI puts arguments and a return value of the given classes (int, float, or other, which is passed
    like an integer): (register, class) for each register argument, and the number of 8-byte stack slots"""
    registers, slots = [], 0
    if microsoft:
        for position, class_ in enumerate(classes):
            if position >= 4:
                slots += 1
            elif class_ == "float":
                registers.append((f"zmm{position}", class_))
            else:
                registers.append((MICROSOFT_INTEGER[position], class_))
    else:
        integers = floats = 0
        for class_ in classes:
            if class_ == "float" and floats < 8:
                registers.append((f"zmm{floats}", class_))
                floats += 1
            elif class_ != "float" and integers < 6:
                registers.append((SYSV_INTEGER[integers], class_))
                integers += 1
            else:
                slots += 1
    return {"arguments": sorted(registers), "slots": slots, "returns": returned}


def signatures(count: int, seed: int) -> list:
    """Random signatures: a return kind and a list of argument kinds"""
    generator = random.Random(seed)
    result = []
    for _ in range(count):
        arguments = [generator.choice(list(C_TYPES)) for _ in range(generator.randint(0, 8))]
        result.append((generator.choice(RETURNS), arguments))
    return result


def source(signatures_: list, microsoft: bool) -> str:
    """C for the functions and a caller passing each values read from volatile globals"""
    lines = ["volatile long long sink_i;", "volatile double sink_d;"]
    lines += [f"{c} volatile in_{k};" for k, c in C_TYPES.items()]
    if microsoft:
        lines.append("int _fltused;")
    for index, (returns, arguments) in enumerate(signatures_):
        parameters = ", ".join(f"{C_TYPES[k]} a{i}" for i, k in enumerate(arguments)) or "void"
        result = "void" if returns == "void" else C_TYPES[returns]
        # Starting from volatile globals keeps the compiler from computing a result
        # at compile time and dropping the call.
        body = ["  long long i = in_i64;", "  double d = in_f64;"]
        for i, kind in enumerate(arguments):
            if kind in FLOATING:
                body.append(f"  d = d * 1.5 + a{i};")
            elif kind == "ptr":
                body.append(f"  i = i * 31 ^ (long long) a{i};")
            else:
                body.append(f"  i = i * 31 + a{i};")
        if returns == "void":
            body += ["  sink_i = i;", "  sink_d = d;"]
        elif returns == "ptr":
            body.append("  return (char *) (i + (long long) d);")
        elif returns in FLOATING:
            body.append(f"  return ({result}) (d + i);")
        else:
            body.append(f"  return ({result}) (i + (long long) d);")
        lines.append(f"__attribute__((noinline)) {result} f{index}({parameters}) {{")
        lines += body
        lines.append("}")
    lines.append("__attribute__((noinline)) void run(void) {")
    for index, (returns, arguments) in enumerate(signatures_):
        call = f"f{index}({', '.join(f'in_{k}' for k in arguments)})"
        if returns == "void":
            lines.append(f"  {call};")
        elif returns in FLOATING:
            lines.append(f"  sink_d = {call};")
        else:
            lines.append(f"  sink_i = (long long) {call};")
    lines.append("}")
    lines.append("void start(void) { run(); for (;;) {} }" if microsoft else
                 "void _start(void) { run(); for (;;) {} }")
    return "\n".join(lines) + "\n"


def expected_locations(returns: str, arguments: list, microsoft: bool) -> dict:
    """The registers the ABI passes the arguments and the return value in, and the 8-byte stack slots they take"""
    laid = placement([class_of(k) for k in arguments], "void" if returns == "void" else class_of(returns), microsoft)
    returned = {"void": [], "int": ["rax"], "float": ["zmm0"]}[laid["returns"]]
    return {"arguments": sorted(r for r, _ in laid["arguments"]), "returns": returned, "slots": laid["slots"]}


def compile_binary(directory: str, name: str, text: str, microsoft: bool) -> tuple:
    """The binary and the model address of each function"""
    c_path = os.path.join(directory, name + ".c")
    with open(c_path, "w") as stream:
        stream.write(text)
    flags = ["-O2", "-fno-builtin", "-fno-stack-protector"]
    if microsoft:
        objects = os.path.join(directory, name + ".obj")
        binary = os.path.join(directory, name + ".exe")
        mapping = os.path.join(directory, name + ".map")
        subprocess.run(["clang", "--target=x86_64-pc-windows-msvc", *flags, "-c", c_path, "-o", objects], check=True)
        subprocess.run(["lld-link", "/entry:start", "/subsystem:console", "/nodefaultlib", f"/map:{mapping}",
                        f"/out:{binary}", objects], check=True, capture_output=True)
        addresses = {}
        with open(mapping) as stream:
            for line in stream:
                fields = line.split()
                if len(fields) >= 4 and re.fullmatch(r"[0-9a-f]{16}", fields[2]):
                    addresses[fields[1]] = int(fields[2], 16)
    else:
        binary = os.path.join(directory, name)
        objects = binary + ".o"
        subprocess.run(["clang", "--target=x86_64-unknown-linux-gnu", *flags, "-fno-pic", "-c", c_path, "-o",
                        objects], check=True)
        subprocess.run(["ld.lld", "-static", "-e", "_start", "-o", binary, objects], check=True)
        output = subprocess.run(["llvm-nm", binary], check=True, capture_output=True, text=True).stdout
        addresses = {fields[2]: int(fields[0], 16) for fields in (line.split() for line in output.splitlines())
                     if len(fields) == 3}
    return binary, addresses


def analyze(revng: str, binary: str, directory: str) -> dict:
    """The model `revng project init` leaves for \\p binary"""
    project = os.path.join(directory, os.path.basename(binary) + ".project")
    os.makedirs(project, exist_ok=True)
    with open(os.path.join(project, "init.log"), "w") as log:
        subprocess.run([revng, "project", "--storage-provider", "local://?inline", "init", os.path.abspath(binary)],
                       cwd=project, check=True, stdout=log, stderr=subprocess.STDOUT)
    with open(os.path.join(project, "revng.yml")) as stream:
        return yaml.load(stream, Loader=getattr(yaml, "CSafeLoader", yaml.SafeLoader))


class Model:
    def __init__(self, model: dict):
        self.definitions = {d["ID"]: d for d in model.get("TypeDefinitions", [])}
        self.functions = {int(f["Entry"].split(":")[0], 16): f for f in model.get("Functions", [])}

    def definition(self, reference: str) -> dict:
        return self.definitions[int(re.match(r"/TypeDefinitions/(\d+)-", reference).group(1))]

    def size(self, type_: dict) -> int:
        kind = type_["Kind"]
        if kind == "PrimitiveType":
            return type_.get("Size", 0)
        if kind == "PointerType":
            return type_["PointerSize"]
        if kind == "ArrayType":
            return type_["ElementCount"] * self.size(type_["ElementType"])
        definition = self.definition(type_["Definition"])
        if definition["Kind"] in ("StructDefinition", "UnionDefinition") and "Size" in definition:
            return definition["Size"]
        if definition["Kind"] == "UnionDefinition":
            return max(self.size(f["Type"]) for f in definition["Fields"])
        if "UnderlyingType" in definition:
            return self.size(definition["UnderlyingType"])
        return 0

    def c_class(self, type_: dict | None) -> str:
        """int, float, void, or other for a C argument or return type"""
        if not type_:
            return "void"
        if type_["Kind"] == "DefinedType":
            definition = self.definition(type_["Definition"])
            if definition["Kind"] == "TypedefDefinition":
                return self.c_class(definition["UnderlyingType"])
            if definition["Kind"] == "EnumDefinition":
                return "int"
            return "other"
        if type_["Kind"] == "PointerType":
            return "int"
        if type_["Kind"] == "PrimitiveType":
            if type_["PrimitiveKind"] == "Void":
                return "void"
            if type_["PrimitiveKind"] == "Float":
                return "float" if type_["Size"] in (4, 8) else "other"
            return "int" if type_.get("Size", 0) <= 8 else "other"
        return "other"


def register_name(location: str) -> str:
    return location.rsplit("_x86_64", 1)[0]


def score(model: Model, address: int, returns: str, arguments: list, microsoft: bool) -> dict:
    """The verdict on the prototype of the function at \\p address, with what was recovered"""
    function = model.functions.get(address)
    if function is None or "Prototype" not in function:
        return {"verdict": "missing"}
    prototype = model.definition(function["Prototype"]["Definition"])
    if prototype["Kind"] == "CABIFunctionDefinition":
        found = [model.c_class(a["Type"]) for a in sorted(prototype.get("Arguments", []), key=lambda a: a["Index"])]
        found_return = model.c_class(prototype.get("ReturnType"))
        laid = placement(found, found_return, microsoft)
        wanted = placement([class_of(k) for k in arguments], "void" if returns == "void" else class_of(returns),
                           microsoft)
        verdict = "c-exact" if laid == wanted else "c-wrong"
        return {"verdict": verdict, "found": f"{found_return} ({', '.join(found)})"}
    slots = 0
    if "StackArgumentsType" in prototype:
        reserved = 32 if microsoft else 0
        slots = max(0, (model.size(prototype["StackArgumentsType"]) - reserved + 7) // 8)
    found = {"arguments": sorted(register_name(a["Location"]) for a in prototype.get("Arguments", [])),
             "returns": sorted(register_name(r["Location"]) for r in prototype.get("ReturnValues", [])),
             "slots": slots}
    expected = expected_locations(returns, arguments, microsoft)
    verdict = "raw-right" if found == expected else "raw-wrong"
    return {"verdict": verdict, "found": found, "expected": expected}


VERDICTS = ["c-exact", "c-wrong", "raw-right", "raw-wrong", "missing"]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out", help="directory for the sources, binaries and projects")
    parser.add_argument("--revng", default="revng")
    parser.add_argument("--abi", action="append", choices=["sysv", "microsoft"])
    parser.add_argument("--count", type=int, default=60)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--json", help="write every function's verdict here")
    parser.add_argument("--check", action="store_true", help="fail on any c-wrong, raw-wrong or missing function")
    args = parser.parse_args(argv)
    os.makedirs(args.out, exist_ok=True)
    signatures_ = signatures(args.count, args.seed)
    results = {}
    for abi in args.abi or ["sysv", "microsoft"]:
        microsoft = abi == "microsoft"
        binary, addresses = compile_binary(args.out, f"prototypes-{abi}", source(signatures_, microsoft), microsoft)
        model = Model(analyze(args.revng, binary, args.out))
        results[abi] = []
        for index, (returns, arguments) in enumerate(signatures_):
            entry = {"function": f"f{index}", "signature": f"{returns} ({', '.join(arguments)})"}
            entry.update(score(model, addresses[f"f{index}"], returns, arguments, microsoft))
            results[abi].append(entry)
    print("| ABI | " + " | ".join(VERDICTS) + " |")
    print("| --- |" + " --- |" * len(VERDICTS))
    for abi, entries in results.items():
        counts = [sum(1 for e in entries if e["verdict"] == v) for v in VERDICTS]
        print(f"| {abi} | " + " | ".join(str(c) for c in counts) + " |")
    if args.json:
        with open(args.json, "w") as stream:
            json.dump(results, stream, indent=1)
    if args.check:
        failed = [(abi, e) for abi, entries in results.items() for e in entries
                  if e["verdict"] in ("c-wrong", "raw-wrong", "missing")]
        for abi, entry in failed:
            print(f"{abi} {entry['function']} {entry['signature']}: {entry['verdict']} {entry.get('found', '')}")
        return 1 if failed else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
