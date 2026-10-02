#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# The C library, the Itanium C++ ABI and its unwinding ABI define abort and
# _Unwind_Resume to never return, so importing the binary marks them NoReturn
# even without debug information for the library defining them. c0 calls abort
# through its GOT slot, c1 through the PLT and c2 calls _Unwind_Resume through
# the PLT: none of them returns, and none falls into the vN that follows it.
# puts returns, so c3 falls into v3.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -shared -DSTUB -Wl,--build-id=none -Wl,-soname,libstub.so %s -o %t.so
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -Wl,--build-id=none -Wl,-e,entry %s %t.so -o %t
# RUN: %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t | FileCheck %s
#
# CHECK: ImportedDynamicFunctions:
# CHECK-NEXT: - Name: _Unwind_Resume
# CHECK-NEXT: Attributes:
# CHECK-NEXT: - NoReturn
# CHECK: - Name: abort
# CHECK-NEXT: Attributes:
# CHECK-NEXT: - NoReturn
# CHECK: - Name: puts
# CHECK-NOT: NoReturn
# CHECK: Functions:
# CHECK: Name: c0
# CHECK-NOT: Name:
# CHECK: - NoReturn
# CHECK: Name: c1
# CHECK-NOT: Name:
# CHECK: - NoReturn
# CHECK: Name: c2
# CHECK-NOT: Name:
# CHECK: - NoReturn
# CHECK: Name: c3
# CHECK-NOT: NoReturn
# CHECK: Name: v3

#ifdef STUB
.text
.globl abort
.type abort, @function
abort:
  ret
.globl _Unwind_Resume
.type _Unwind_Resume, @function
_Unwind_Resume:
  ret
.globl puts
.type puts, @function
puts:
  ret
#else
.macro case n, insn:vararg
.globl c\n
.type c\n, @function
c\n:
  \insn
.size c\n, .-c\n
.globl v\n
.type v\n, @function
v\n:
  mov $0x11, %eax
  ret
.size v\n, .-v\n
.endm

.text
.globl entry
.type entry, @function
entry:
  ret
.size entry, .-entry

case 0, call *abort@GOTPCREL(%rip)
case 1, call abort@PLT
case 2, call _Unwind_Resume@PLT
case 3, call puts@PLT
#endif
.section .note.GNU-stack,"",@progbits
