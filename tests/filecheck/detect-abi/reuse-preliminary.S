#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# detect-abi runs twice: the second run starts from the prototypes the first
# one wrote to the model and also analyzes `orphan`, which becomes a function
# only then, because its address is used as a literal and no CFG covers it.
# The second run reuses the preliminary analysis of the three functions the
# first one analyzed, since nothing they read changed, analyzes `orphan`, and
# produces the same model as when every function is analyzed again.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none -Wl,--strip-all -Wl,-T,%S/overlapping-indirect-branch.ld %s -o %t
# RUN: env REVNG_OPTIONS=--trace=%t.trace %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t > %t.yml
# RUN: env REVNG_OPTIONS=-detect-abi-reuse-preliminary=false %root/bin/revng quick analyze --analyses=parse-binary,detect-abi %t > %t.again.yml
# RUN: cmp %t.yml %t.again.yml
# RUN: FileCheck %s --check-prefix=MODEL < %t.yml
# RUN: sed -n '/"name": "Collect functions from unused addresses"/,$p' %t.trace > %t.second
# RUN: grep -c '"name": "Reuse the previous analysis", "cat": "task", "ph": "B"' %t.second | FileCheck %s --check-prefix=REUSED
# RUN: grep -c '"name": "Analyze function", "cat": "task", "ph": "B"' %t.second | FileCheck %s --check-prefix=ANALYZED
#
# MODEL: Functions:
# MODEL-COUNT-4: - Entry: "0x{{[0-9a-f]+}}:Code_x86_64"
# MODEL-NOT: - Entry:
# MODEL: TypeDefinitions:
#
# REUSED: {{^}}3{{$}}
# ANALYZED: {{^}}1{{$}}

.text
.globl entry
.type entry, @function
entry:
  call second
  call first
  lea orphan(%rip), %rax
  ret
second:
  call first
  ret
first:
  ret
  int3
orphan:
  mov $1, %eax
  ret
.section .note.GNU-stack,"",@progbits
