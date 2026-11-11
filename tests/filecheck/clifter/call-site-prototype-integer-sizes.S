#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `entry` calls `counter_next`, whose prototype takes and returns an int,
# through a register loaded from its GOT slot. In hl-node a call like this
# kept the call-site prototype detect-abi guessed while the callee was unknown,
# with 64-bit generic values, after the CFG found the callee: the call passed
# the int its callee takes where the prototype has a 64-bit value, and the
# bitcast between the two, of different sizes, failed verification and
# stopped the emission of all the C. The test gives the call such a prototype.
# An integer of another size than the prototype's is now extended or
# truncated, and the C calls `counter_next` through the guessed prototype.
#
# RUN: printf 'int counter_next(int x) { return x + 1; }\n' > %t.dep.c
# RUN: clang --target=x86_64-unknown-linux-gnu -O1 -fPIC -c %t.dep.c -o %t.dep.o
# RUN: ld.lld -shared -soname libdep.so %t.dep.o -o %t.libdep.so
# RUN: clang --target=x86_64-unknown-linux-gnu -c -fPIC %s -o %t.o
# RUN: ld.lld -shared --build-id=none --no-relax -z relro %t.o %t.libdep.so -o %t
# RUN: rm -rf %t.p && mkdir -p %t.p
# RUN: cd %t.p && %root/bin/revng project --storage-provider 'local://?inline' init --no-initial-auto-analysis %t
# RUN: cd %t.p && %root/bin/revng project --storage-provider 'local://?inline' analyze parse-binary -o /dev/null
# RUN: cd %t.p && %root/bin/revng project --storage-provider 'local://?inline' analyze detect-abi -o /dev/null
# RUN: python3 %S/call-site-prototype-integer-sizes.py %t.p/revng.yml \
# RUN:   $(printf '%%x' $((0x400000 + 0x$(llvm-nm %t | awk '$3 == "entry" { print $1 }')))) \
# RUN:   $(printf '%%x' $((0x400000 + 0x$(llvm-nm %t | awk '$3 == "call_site" { print $1 }'))))
# RUN: cd %t.p && %root/bin/revng project --storage-provider 'local://?inline' artifact emit-c-as-single-file -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: entry(
# CHECK: = ((cabifunction_{{[0-9]+}} *) counter_next)(var_{{[0-9]+}});

.text
.globl entry
.type entry, @function
entry:
  .cfi_startproc
  push %rbx
  .cfi_def_cfa_offset 16
  push %r12
  .cfi_def_cfa_offset 24
  push %r13
  .cfi_def_cfa_offset 32
  mov %esi, %r13d
  mov counter_next@GOTPCREL(%rip), %r12
  mov %edi, %ebx
call_site:
  mov %ebx, %edi
  call *%r12
  mov %eax, %ebx
  dec %r13d
  jne call_site
  mov %ebx, %eax
  pop %r13
  .cfi_def_cfa_offset 24
  pop %r12
  .cfi_def_cfa_offset 16
  pop %rbx
  .cfi_def_cfa_offset 8
  ret
  .cfi_endproc
.size entry, .-entry
