# About this fork

This fork of [rev.ng](https://rev.ng/) aims to turn a large binary into a complete, analyzable and executable whole-program model. Upstream rev.ng targets decompilation and treats lift-and-recompile as a regression-testing aid for simple binaries; this fork makes the whole-program model and its recompilation supported uses on large binaries.

## Goal

The model has three properties, in dependency order:

1. Complete: the whole image lifts and its functions get detected prototypes (detect-abi), at a cost that scales to binaries with over 100,000 functions.
2. Analyzable: the lifted LLVM IR represents memory objects and pointers explicitly. Functions take arguments and return values instead of exchanging them through global CPU-state variables; vtables, RTTI and other static objects are their own globals; address formation stays a pointer instead of integer arithmetic. Off-the-shelf whole-program analyses such as [SVF](https://github.com/SVF-tools/SVF) and [cclyzer++](https://github.com/GaloisInc/cclyzerpp) should then work on the module directly.
3. Executable: the emitted C compiles and links with the runtime pieces rev.ng's output needs, including partial builds in which some functions are recompiled and the rest stay native code in the loaded original image.

The fork produces the model. Pointer analysis, class recovery, source matching and semantic reconstruction are consumers and live outside it; at most the fork adds a thin export of the module in a form those tools accept.

## Scaling priorities

Memory is the primary scaling dimension: a stage that does not fit in memory blocks every later property. Speed, including multithreading and running independent work concurrently, is secondary. A change that saves time by holding more memory needs its peak-memory cost measured and justified.

Measure both on a ladder of binaries of increasing size, recording peak memory and wall time per pipeline stage, and extrapolate only from completed runs.

## What belongs here

A change belongs in this fork when it makes rev.ng correct, complete, analyzable, executable or cheaper on input it accepts, and needs no knowledge of a particular target binary, of how a consumer slices or delegates code, or of what a consumer does with the output. Target facts, slicing and delegation policy, harnesses, comparisons against original behavior, and progress dashboards belong to the consuming project.

## Current state

The whole pipeline completes on a 90 MB binary: lifting, ABI detection, data layout analysis and C emission of all 109,919 of its functions, the last in 24.85 hours at a peak of 68 GB. The analyzable and executable properties are goals: compiling and running emitted C is so far exercised only on small inputs and on individual functions with externally supplied runtime support.

[FORK-CHANGES.md](FORK-CHANGES.md) lists every change so far, with its commits, tests and upstream status.
