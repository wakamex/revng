#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# The lifted module links the early-linked and support modules, whose debug
# information names the files they were compiled from. Those names must not
# depend on where rev.ng was built: the lifted module may name neither the build
# directory nor the source directory.
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none %s -o %t
# RUN: %root/bin/revng quick artifact lift --analyses=parse-binary %t -o %t.bc
# RUN: llvm-dis %t.bc -o %t.ll
# RUN: sh -c '! grep -F "$(realpath %root)" %t.ll'
# RUN: sh -c '! grep -F "$(realpath %S/../../..)" %t.ll'
# RUN: FileCheck %s < %t.ll
#
# CHECK: !DIFile(filename: "/revng-source/share/revng/early-linked.c", directory: "/revng-build"

.text
.globl _start
.type _start, @function
_start:
    ret
