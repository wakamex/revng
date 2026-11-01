"""Call each corpus1 function in the original library and in rev.ng's recovered library with the same arguments and
report every disagreement. Calls that may panic run in a forked child, so an abort is an outcome to compare rather
than the end of the harness. Usage: compare_corpus1.py ORIGINAL.so RECOVERED.so [--seed N] [--count N]"""
import argparse
import ctypes as c
import json
import os
import random
import signal
import sys

U64 = (1 << 64) - 1
EDGES = [0, 1, 2, 7, 9, 10, 99, 100, 101, 10**8, 10**8 + 1, (1 << 32) - 1, 1 << 32, (1 << 32) + 1, 1 << 63, U64]


def bind(path):
    lib = c.CDLL(os.path.abspath(path))
    sig = {
        "unit_convert": ([c.c_uint64, c.c_uint64, c.c_uint8, c.c_uint8, c.c_void_p], c.c_uint16),
        "dispatch": ([c.c_uint8, c.c_uint64, c.c_uint64], c.c_uint64),
        "checked_notional": ([c.c_uint64, c.c_uint64, c.c_uint64], c.c_uint64),
        "count_calls": ([c.c_uint64], c.c_uint64),
        "vec_checksum": ([c.c_uint32, c.c_uint64], c.c_uint64),
        "unwrap_lookup": ([c.c_void_p, c.c_size_t, c.c_size_t], c.c_uint64),
        "ratio": ([c.c_uint64, c.c_uint64], c.c_uint64),
    }
    functions = {}
    for name, (args, result) in sig.items():
        f = getattr(lib, name)
        f.argtypes, f.restype = args, result
        functions[name] = f
    return functions


def isolated(call):
    """Run call() in a forked child: ['ok', value], or ['signal', number, last stderr line], or ['exit', code]. A
    call still running after 10 seconds ends with SIGALRM (signal 14)."""
    read, write = os.pipe()
    err_read, err_write = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(read)
        os.close(err_read)
        os.dup2(err_write, 2)
        # A call that never returns ends as SIGALRM instead of hanging the harness
        signal.alarm(10)
        try:
            os.write(write, json.dumps(call()).encode())
        finally:
            os._exit(0)
    os.close(write)
    os.close(err_write)
    data = os.read(read, 1 << 16)
    err = b""
    while chunk := os.read(err_read, 1 << 16):
        err += chunk
    os.close(read)
    os.close(err_read)
    _, status = os.waitpid(pid, 0)
    if os.WIFSIGNALED(status):
        lines = [l for l in err.decode(errors="replace").splitlines() if l.strip()]
        return ["signal", os.WTERMSIG(status), lines[-1][:120] if lines else ""]
    if data:
        return ["ok", json.loads(data)]
    return ["exit", os.WEXITSTATUS(status)]


def unit_convert(f, a):
    out = (c.c_uint64 * 2)(0xA5A5, 0xA5A5)
    status = f(a[0], a[1], a[2], a[3], c.addressof(out))
    return [status, out[0], out[1]]


def cases(rng, count):
    pick = lambda: rng.choice(EDGES) if rng.random() < 0.5 else rng.getrandbits(rng.choice([8, 16, 32, 64]))
    for i in range(count):
        p = rng.choice([10**k * rng.randrange(1, 1000) for k in range(10)] + [pick()])
        yield "unit_convert", (p & U64, pick(), rng.randrange(12), rng.randrange(12)), False
        yield "dispatch", (rng.randrange(9), pick(), pick()), False
        yield "checked_notional", (pick(), pick(), rng.choice([0, 1, 5, 25, 10_000, pick()])), False
        yield "count_calls", (pick(),), False
        yield "vec_checksum", (rng.getrandbits(32), pick()), False
        yield "unwrap_lookup", (rng.randrange(6), rng.randrange(8)), True
        yield "ratio", (pick(), rng.choice([0, 1, 3, pick()])), True


def run(functions, name, args, may_abort, table):
    f = functions[name]
    if name == "unit_convert":
        call = lambda: unit_convert(f, args)
    elif name == "unwrap_lookup":
        call = lambda: f(c.addressof(table), args[0], args[1])
    else:
        call = lambda: f(*args)
    return isolated(call)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("original")
    p.add_argument("recovered")
    p.add_argument("--seed", type=int, default=20261028)
    p.add_argument("--count", type=int, default=300)
    a = p.parse_args()
    libs = [bind(a.original), bind(a.recovered)]
    table = (c.c_uint64 * 8)(*[3 * i + 1 for i in range(8)])
    totals, failures, kinds = {}, [], {}
    for name, args, may_abort in cases(random.Random(a.seed), a.count):
        results = [run(lib, name, args, may_abort, table) for lib in libs]
        t = totals.setdefault(name, [0, 0])
        t[0] += 1
        pair = (results[0][0] + ("" if results[0][0] != "signal" else str(results[0][1])),
                results[1][0] + ("" if results[1][0] != "signal" else str(results[1][1])))
        if results[0] == results[1]:
            pair = ("same", "same")
        kinds.setdefault(name, {}).setdefault(" vs ".join(pair), 0)
        kinds[name][" vs ".join(pair)] += 1
        if results[0] != results[1]:
            t[1] += 1
            if len(failures) < 40:
                failures.append({"function": name, "args": args, "original": results[0], "recovered": results[1]})
    print(json.dumps({"calls": {k: {"calls": v[0], "disagreements": v[1]} for k, v in totals.items()},
                      "outcomes": kinds,
                      "first_disagreements": failures}, indent=1))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
