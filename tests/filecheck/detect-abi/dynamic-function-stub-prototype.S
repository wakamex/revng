#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` calls `counter_next` through its PLT stub, which rev.ng makes a
# function of its own that callers inline. The model knows `counter_next`'s
# prototype, here added to the model as a prototypes database would. The stub
# took and returned only what its callers happened to use, as raw registers,
# so a caller reading a register the others did not lost it; it now has the
# prototype of the function it reaches.
#
# RUN: printf 'int counter_next(int x) { return x + 1; }\n' > %t.dep.c
# RUN: clang --target=x86_64-unknown-linux-gnu -O1 -fPIC -c %t.dep.c -o %t.dep.o
# RUN: ld.lld -shared -soname libdep.so %t.dep.o -o %t.libdep.so
# RUN: clang --target=x86_64-unknown-linux-gnu -c %s -o %t.o
# RUN: ld.lld -pie -e entry --build-id=none %t.o %t.libdep.so -o %t
# RUN: rm -rf %t.p && mkdir -p %t.p
# RUN: cd %t.p && %root/bin/revng project --storage-provider 'local://?inline' init --no-initial-auto-analysis %t
# RUN: cd %t.p && %root/bin/revng project --storage-provider 'local://?inline' analyze parse-binary -o /dev/null
# RUN: python3 %S/dynamic-function-stub-prototype.py %t.p/revng.yml
# RUN: cd %t.p && %root/bin/revng project --storage-provider 'local://?inline' analyze detect-abi -o /dev/null
# RUN: cd %t.p && %root/bin/revng project --storage-provider 'local://?inline' artifact emit-c-as-single-file -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: entry(void) {
# CHECK-NEXT: return {{.*}}counter_next(41) + 1
# CHECK: int32_t {{function_0x[0-9a-f]+_Code_x86_64}}(int32_t argument_0) {
# CHECK-NEXT: return counter_next(argument_0);

    .text
    .globl entry
    .type entry, @function
entry:
    sub $8, %rsp
    mov $41, %edi
    call counter_next@PLT
    add $1, %eax
    add $8, %rsp
    ret
