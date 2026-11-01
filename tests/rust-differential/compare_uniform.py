"""Call every function the original library exports, each `u64 f(u64, u64, u64)`, in the original and in rev.ng's
recovered library with the same arguments, each call in a forked child, and report the outcomes per function: two
results agree when both return the same value or both die from the same signal. Usage:
compare_uniform.py ORIGINAL.so RECOVERED.so [--seed N] [--count N]"""
import argparse
import ctypes as c
import json
import os
import random
import subprocess
import sys

from compare_corpus1 import EDGES, isolated


def exports(path):
    out = subprocess.run(["nm", "-D", "--defined-only", path], capture_output=True, text=True, check=True).stdout
    return sorted(line.split()[2] for line in out.splitlines() if line.split()[1] == "T")


def bind(path, names):
    lib = c.CDLL(os.path.abspath(path))
    functions = {}
    for name in names:
        f = getattr(lib, name)
        f.argtypes, f.restype = [c.c_uint64] * 3, c.c_uint64
        functions[name] = f
    return functions


def outcome(result):
    return result[:2] if result[0] == "signal" else result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("original")
    p.add_argument("recovered")
    p.add_argument("--seed", type=int, default=20261029)
    p.add_argument("--count", type=int, default=200)
    a = p.parse_args()
    names = exports(a.original)
    libs = [bind(a.original, names), bind(a.recovered, names)]
    rng = random.Random(a.seed)
    pick = lambda: rng.choice(EDGES) if rng.random() < 0.4 else rng.getrandbits(rng.choice([4, 8, 16, 32, 64]))
    summary, failures = {}, []
    for _ in range(a.count):
        for name in names:
            args = (pick(), pick(), pick())
            results = [isolated(lambda: lib[name](*args)) for lib in libs]
            key = "agree" if outcome(results[0]) == outcome(results[1]) else f"{results[0][0]} vs {results[1][0]}"
            counts = summary.setdefault(name, {})
            counts[key] = counts.get(key, 0) + 1
            if key != "agree" and sum(1 for f in failures if f["function"] == name) < 5:
                failures.append({"function": name, "args": args, "original": results[0], "recovered": results[1]})
    print(json.dumps({"outcomes": summary, "first_disagreements": failures}, indent=1))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
