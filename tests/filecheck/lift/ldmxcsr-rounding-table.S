#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `ldmxcsr` lifts to a call to `helper_ldmxcsr`, whose body sets the rounding
# mode through a lookup table indexed with a value that is not constant.
# Inlining the helper left a getelementptr in the isolated function, which
# post-inline-helpers-verify forbids, and the pipeline aborted; a math routine
# of Warcraft III's client did so. The getelementptr is now lowered to address
# arithmetic once the helpers are inlined.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none %s -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: function_0x{{[0-9a-f]+}}_Code_x86_64({{.*}}) {

.text
.globl _start
_start:
  ldmxcsr 8(%rsp)
  ret
