#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `scale` multiplies a double in xmm0 in a loop and returns it, and `run`
# converts what it returns. A vector register is a return value only if a
# caller reads it after a call and the callee may write it, and a write of
# the loop counted as read, by the next iteration, so scale returned nothing
# and run converted its own argument instead of scale's result. A write that
# reaches the return unread along some path now makes the register one the
# function may write, and a read the code makes after a call counts even when
# the callee already returns the register.
#
# The code is clang -O2 output for:
#   double scale(double value, double factor, int steps) { for (int i = 0; i < steps; i++) value *= factor; return value; }
#   long run(long n) { return (long) scale(1.5, 2.0, (int) n); }
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none %s -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: float64_t scale(
# CHECK: float64_t [[RESULT:var_[0-9]+]] = scale(
# CHECK-NEXT: float64_to_int64_round_to_zero(bit_cast(generic64_t, [[RESULT]]),

	.text
	.globl	scale
	.p2align	4
	.type	scale,@function
scale:
	testl	%edi, %edi
	jle	.LBB0_6
	movl	%edi, %eax
	andl	$7, %eax
	cmpl	$8, %edi
	jb	.LBB0_5
	andl	$2147483640, %edi
	.p2align	4
.LBB0_3:
	mulsd	%xmm1, %xmm0
	mulsd	%xmm1, %xmm0
	mulsd	%xmm1, %xmm0
	mulsd	%xmm1, %xmm0
	mulsd	%xmm1, %xmm0
	mulsd	%xmm1, %xmm0
	mulsd	%xmm1, %xmm0
	mulsd	%xmm1, %xmm0
	addl	$-8, %edi
	jne	.LBB0_3
	testl	%eax, %eax
	je	.LBB0_6
	.p2align	4
.LBB0_5:
	mulsd	%xmm1, %xmm0
	decl	%eax
	jne	.LBB0_5
.LBB0_6:
	retq
.Lfunc_end0:
	.size	scale, .Lfunc_end0-scale
	.section	.rodata.cst8,"aM",@progbits,8
	.p2align	3, 0x0
.LCPI1_0:
	.quad	0x3ff8000000000000
.LCPI1_1:
	.quad	0x4000000000000000
	.text
	.globl	run
	.p2align	4
	.type	run,@function
run:
	pushq	%rax
	movsd	.LCPI1_0(%rip), %xmm0
	movsd	.LCPI1_1(%rip), %xmm1
	callq	scale
	cvttsd2si	%xmm0, %rax
	popq	%rcx
	retq
.Lfunc_end1:
	.size	run, .Lfunc_end1-run
	.globl	_start
	.p2align	4
	.type	_start,@function
_start:
	pushq	%rax
	movl	$7, %edi
	callq	run
	movq	%rax, (%rsp)
	movq	(%rsp), %rax
	.p2align	4
.LBB2_1:
	jmp	.LBB2_1
.Lfunc_end2:
	.size	_start, .Lfunc_end2-_start
