# Prototype benchmark

`benchmark.py` measures how often rev.ng gives a function the C prototype its source had. It generates C functions with random signatures of integers, pointers, floats and doubles, with up to eight arguments so that some go on the stack, and a caller that passes each of them values the compiler cannot see. It compiles them with [clang](https://clang.llvm.org/) for x86-64 under the System V ABI, as a static ELF linked with `ld.lld`, and under the Microsoft x64 ABI, as a PE linked with `lld-link`. It analyzes each binary with `revng project init` and compares every function's prototype in the resulting model with its source signature. Both binaries take about 20 seconds.

```
python3 benchmark.py OUT --revng build/bin/revng [--count 60] [--seed 1] [--json verdicts.json] [--check]
```

Each function gets one verdict:

| Verdict | Meaning |
| --- | --- |
| c-exact | a C prototype that the ABI lays out as the source signature |
| c-wrong | a C prototype laid out otherwise: callers and the function disagree on where values are |
| raw-right | a raw prototype with the source's registers and stack slots: only the conversion to C is missing |
| raw-wrong | a raw prototype with other registers or stack slots: ABI detection is wrong |
| missing | no function where the source's is |

Prototypes are compared by layout, the register each value is in and its class, because that is all the machine code shows. Integers and pointers are one class. Floats and doubles are another, since rev.ng tracks vector registers in 8-byte parts. A stack argument has no class. Under System V, moving an integer argument before or after a floating point one does not change the layout.

`--check` exits with status 1 if any function is c-wrong, raw-wrong or missing. A raw-right function is one the conversion declined, which costs readability but not correctness.

`test_benchmark.py` tests the layout rules without a compiler or rev.ng: `python3 -m unittest test_benchmark`.
