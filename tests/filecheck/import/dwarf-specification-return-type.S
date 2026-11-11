#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `Counter::next` is declared in its class and defined outside it. Its debug
# information has two entries: the declaration, with the return type, and the
# definition, with the code's address and a reference to the declaration
# (DW_AT_specification), but no return type of its own. The prototype was read
# from the definition alone, so every such C++ function returned void, and its
# callers dropped the result. GCC, which built the libraries of the prototypes
# database, splits namespace functions so too, and 94% of libstdc++'s functions
# there had no return type. `Counter::next` returns its int, and `entry` returns
# the result of calling it.
#
# RUN: printf 'struct Counter { static int next(int x); };\nint Counter::next(int x) { return x + 1; }\n' > %t.cpp
# RUN: clang++ --target=x86_64-unknown-linux-gnu -g -O1 -c %t.cpp -o %t.dep.o
# RUN: clang --target=x86_64-unknown-linux-gnu -c %s -o %t.o
# RUN: ld.lld -static -e entry %t.o %t.dep.o -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: entry(void) {
# CHECK-NEXT: return {{.*}}_ZN7Counter4nextEi(41);
# CHECK: int32_t _ZN7Counter4nextEi(int32_t x) {

    .text
    .globl entry
    .type entry, @function
entry:
    sub $8, %rsp
    mov $41, %edi
    call _ZN7Counter4nextEi
    add $8, %rsp
    ret
