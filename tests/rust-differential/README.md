# Rust differential corpora

These libraries test whether rev.ng's C behaves like the code it was lifted from. Each library in `src/` is a Rust `cdylib` whose exported functions take and return integers, mostly `u64 f(u64, u64, u64)`, and exercise common Rust code: formatting, sorting, B-trees, trait objects, closures, hash maps and floating point. `check.sh` builds a library, emits its C with a rev.ng launcher, compiles that C into a shared library, calls every exported function of both libraries with the same 200 random arguments, each call in a forked child, and counts the functions whose results agree on every call. Two results agree when both return the same value or both end in the same signal.

```
./check.sh LAUNCHER [NAME...]
PROTOTYPES_DB=path/to/prototypes.sqlite ./check.sh LAUNCHER corpus3 corpus4
```

`LAUNCHER` is a rev.ng `revng` executable whose resource root holds `share/revng` and the helper bitcode. The libraries are built with Rust 1.95.0 (`rustc +1.95.0`, or `$RUSTC`, by default the `rustc` on the path or in `~/.cargo/bin`) and stripped. The scripts take rev.ng's toolchain, its Python and LLVM tools, from `$ORCHESTRA`, by default `~/.local/share/revng/orchestra/root`. Set `PROTOTYPES_DB` to a rev.ng prototype database, such as the `share/revng/prototypes.sqlite` of rev.ng's public distribution, to reproduce the results cited in `FORK-CHANGES.md`. Without one, the C library's imports, such as `free`, get rev.ng's default prototype, and most functions crash: the formatting probe agrees in 1 of 6 functions without the database and in 6 of 6 with it.

## Libraries

| Library | Contents |
| --- | --- |
| `corpus1` | Functions with status codes, pointers and panics at the boundary, compared through their own signatures by `compare_corpus1.py` |
| `corpus2` | Collections, closures, enums, atomics, formatting and `Result` chains |
| `corpus3` | Sorting structs, heaps, trait objects, boxed closures, hash maps, deques, linked lists and windows |
| `corpus4` | Floating point arithmetic and formatting, conversions, B-tree ranges, iterator adapters and bit operations |
| `fmtprobe` | `core::fmt` paths: literals, integers, padding and hexadecimal |
| `sortprobe`, `sortprobe2` | Stable and unstable sorts, and the steps of corpus3's `sort_orders` and corpus4's `float_sort` |
| `f64probe` | Float formatting (`{:.3}`, `{:e}`, `{}`), parsing and rounding |
| `btreeprobe` | The steps of corpus4's `btree_ranges` |

## Support files

`generate.sh` runs `run_revng_control.py`, which emits the C and headers and compiles them with the files in `support/`: `revng_runtime_library.c` for the operations the C calls, `revng_intrinsic_wrappers.py` for LLVM intrinsics the C calls as helpers, `revng_runtime_traps.c`, which aborts where rev.ng could not resolve control flow or a value, and `tcg-with-data.o`, QEMU's helpers with their data. `segment_init.py` initializes the segments from the binary, `extra_helpers.sh` compiles helpers the C needs from the runtime's helper bitcode, and `imports.c` defines imports the C calls under rev.ng's hashed names, such as `abort`.
