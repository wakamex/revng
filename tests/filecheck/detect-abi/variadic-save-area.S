#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# `vfmt(const char *format, ...)` is variadic: its prologue stores rsi to r9,
# and xmm0 to xmm7 behind a test of al, to the register save area va_arg reads
# its variable arguments from. Those stores read no argument of vfmt: each call
# passes what its caller sets, so the call in `alloc`, which passes the size,
# has a prototype of its own taking two arguments. `alloc` and `user` take
# only rdi. The stores used to make all six general purpose and all eight
# vector argument registers arguments of vfmt, and of every caller that does
# not set them, up to `_start`: on hl-node, a variadic diagnostic function
# of the allocator gave 83,185 functions all eight vector registers.
#
# The code is clang -O2 output for:
#   static char buffer[64];
#   int vfmt(const char *format, ...)  sums an int argument per '%' in format
#   void *alloc(size_t size) { if (size > 4096) { vfmt("... % ...", (int) size); return 0; } return buffer; }
#   long user(long n) { char *p = alloc(n); return p ? p[0] : -1; }
#
# RUN: clang --target=x86_64-unknown-linux-gnu -nostdlib -static -Wl,--build-id=none %s -o %t
# RUN: %root/bin/revng quick artifact emit-c-as-single-file %t -o %t.ptml
# RUN: %root/bin/revng ptml --plain %t.ptml | FileCheck %s
#
# CHECK: void vfmt({{[^,]*}}) {
# CHECK: alloc(generic64_t argument_0) {
# CHECK: , argument_0);
# CHECK: user(generic64_t argument_0) {

	.text
	.globl	vfmt
	.p2align	4
	.type	vfmt,@function
vfmt:
	subq	$88, %rsp
	movq	%rsi, -88(%rsp)
	movq	%rdx, -80(%rsp)
	movq	%rcx, -72(%rsp)
	movq	%r8, -64(%rsp)
	movq	%r9, -56(%rsp)
	testb	%al, %al
	je	.LBB0_10
	movaps	%xmm0, -48(%rsp)
	movaps	%xmm1, -32(%rsp)
	movaps	%xmm2, -16(%rsp)
	movaps	%xmm3, (%rsp)
	movaps	%xmm4, 16(%rsp)
	movaps	%xmm5, 32(%rsp)
	movaps	%xmm6, 48(%rsp)
	movaps	%xmm7, 64(%rsp)
.LBB0_10:
	leaq	-96(%rsp), %rcx
	movq	%rcx, -112(%rsp)
	leaq	96(%rsp), %rsi
	movq	%rsi, -120(%rsp)
	movabsq	$206158430216, %rax
	movq	%rax, -128(%rsp)
	movl	$8, %edx
	xorl	%eax, %eax
	jmp	.LBB0_1
	.p2align	4
.LBB0_2:
	testl	%r8d, %r8d
	je	.LBB0_3
.LBB0_8:
	incq	%rdi
.LBB0_1:
	movzbl	(%rdi), %r8d
	cmpl	$37, %r8d
	jne	.LBB0_2
	cmpl	$40, %edx
	ja	.LBB0_6
	movq	%rsi, %r8
	movl	%edx, %esi
	addq	%rcx, %rsi
	addl	$8, %edx
	movl	%edx, -128(%rsp)
	jmp	.LBB0_7
	.p2align	4
.LBB0_6:
	leaq	8(%rsi), %r8
	movq	%r8, -120(%rsp)
.LBB0_7:
	addl	(%rsi), %eax
	movq	%r8, %rsi
	jmp	.LBB0_8
.LBB0_3:
	movb	%al, buffer(%rip)
	addq	$88, %rsp
	retq
.Lfunc_end0:
	.size	vfmt, .Lfunc_end0-vfmt
	.globl	alloc
	.p2align	4
	.type	alloc,@function
alloc:
	pushq	%rbx
	movl	$buffer, %ebx
	cmpq	$4097, %rdi
	jb	.LBB1_2
	movq	%rdi, %rsi
	xorl	%ebx, %ebx
	movl	$.L.str, %edi
	xorl	%eax, %eax
	callq	vfmt
.LBB1_2:
	movq	%rbx, %rax
	popq	%rbx
	retq
.Lfunc_end1:
	.size	alloc, .Lfunc_end1-alloc
	.globl	user
	.p2align	4
	.type	user,@function
user:
	pushq	%rax
	callq	alloc
	testq	%rax, %rax
	je	.LBB2_1
	movsbq	(%rax), %rax
	popq	%rcx
	retq
.LBB2_1:
	movq	$-1, %rax
	popq	%rcx
	retq
.Lfunc_end2:
	.size	user, .Lfunc_end2-user
	.globl	_start
	.p2align	4
	.type	_start,@function
_start:
	pushq	%rax
	movl	$10, %edi
	callq	user
	.p2align	4
.LBB3_1:
	jmp	.LBB3_1
.Lfunc_end3:
	.size	_start, .Lfunc_end3-_start
	.type	buffer,@object
	.local	buffer
	.comm	buffer,64,16
	.type	.L.str,@object
	.section	.rodata.str1.1,"aMS",@progbits,1
.L.str:
	.asciz	"allocation of % bytes failed"
	.size	.L.str, 29
