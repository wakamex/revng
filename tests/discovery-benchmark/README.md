# Discovery benchmark

`benchmark.py` measures how well rev.ng finds the functions of a C++ binary that uses exceptions, and which of them it marks as never returning. It generates functions of random kinds that call each other: plain ones, ones that may throw, ones that always throw, ones with a destructor around a call that may throw (a cleanup landing pad ending in `_Unwind_Resume`), ones that catch, ones that call a function that always throws on one path, and ones that dispatch through a `switch` to helpers nothing else calls. About a third are called only through a table of function pointers, as virtual functions and Rust trait objects are.

The functions are compiled with [clang](https://clang.llvm.org/) for x86-64, as position-independent code with `--pie`, and linked with `ld.lld` against a stub shared library that provides the C++ runtime functions, so that rev.ng sees `__cxa_throw` and `_Unwind_Resume` as imports, as it does in a real program. A copy without the symbol table is analyzed with `revng project init`, and the functions of the model are compared with the symbol table of the original. 150 functions take about 10 seconds.

```
python3 benchmark.py OUT --revng build/bin/revng --prototypes-db prototypes-v14.sqlite [--count 60] [--seed 1] [--pie] [--repeat 1] [--json verdicts.json] [--check]
```

| Verdict | Meaning |
| --- | --- |
| found | a function of the model starts where the symbol does |
| missing | none does: the code is part of another function or of none |
| split | a function of the model starts inside the symbol's code, such as at a landing pad |
| false noreturn | the model says the function never returns, and it does |
| missed noreturn | the function always throws, and the model says it may return |

`--prototypes-db` links a prototypes database into the launcher's resource root for the run and removes it afterwards, as `tests/rust-differential/check.sh` does. Without one, rev.ng does not know that `__cxa_throw` never returns, and the results do not describe a real configuration. `--repeat N` analyzes the binary N times and reports how many different models came out, since the order in which rev.ng analyzes functions varies between runs and a model must not depend on it. `--check` exits with status 1 on any missing function, split or false noreturn, and on models that differ.

`test_benchmark.py` tests the generator and the scoring without a compiler or rev.ng: `python3 -m unittest test_benchmark`.
