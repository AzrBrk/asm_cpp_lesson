	.file	"main.cpp"
	.text
	.p2align 4
	.globl	_Z8ret_testy
	.def	_Z8ret_testy;	.scl	2;	.type	32;	.endef
	.seh_proc	_Z8ret_testy
_Z8ret_testy:
.LFB163:
	pushq	%rbx
	.seh_pushreg	%rbx
	.seh_endprologue
/APP
 # 7 "lessons\lesson02\main.cpp" 1
	movq $1, %rbx
 # 0 "" 2
/NO_APP
	leaq	1(%rcx), %rax
	popq	%rbx
	ret
	.seh_endproc
	.p2align 4
	.globl	_Z9bad_storePi
	.def	_Z9bad_storePi;	.scl	2;	.type	32;	.endef
	.seh_proc	_Z9bad_storePi
_Z9bad_storePi:
.LFB164:
	.seh_endprologue
/APP
 # 18 "lessons\lesson02\main.cpp" 1
	movl $99, (%rcx)
 # 0 "" 2
/NO_APP
	movl	(%rcx), %eax
	addl	%eax, %eax
	ret
	.seh_endproc
	.p2align 4
	.globl	_Z10good_storePi
	.def	_Z10good_storePi;	.scl	2;	.type	32;	.endef
	.seh_proc	_Z10good_storePi
_Z10good_storePi:
.LFB165:
	.seh_endprologue
	movl	(%rcx), %eax
/APP
 # 28 "lessons\lesson02\main.cpp" 1
	movl $99, (%rcx)
 # 0 "" 2
/NO_APP
	addl	(%rcx), %eax
	ret
	.seh_endproc
	.p2align 4
	.globl	_Z13precise_storePi
	.def	_Z13precise_storePi;	.scl	2;	.type	32;	.endef
	.seh_proc	_Z13precise_storePi
_Z13precise_storePi:
.LFB166:
	.seh_endprologue
	movl	(%rcx), %eax
/APP
 # 39 "lessons\lesson02\main.cpp" 1
	movl $99, (%rcx)
 # 0 "" 2
/NO_APP
	addl	(%rcx), %eax
	ret
	.seh_endproc
	.p2align 4
	.globl	_Z15increment_storePi
	.def	_Z15increment_storePi;	.scl	2;	.type	32;	.endef
	.seh_proc	_Z15increment_storePi
_Z15increment_storePi:
.LFB167:
	.seh_endprologue
	movl	(%rcx), %eax
/APP
 # 48 "lessons\lesson02\main.cpp" 1
	addl $1, (%rcx)
 # 0 "" 2
/NO_APP
	addl	(%rcx), %eax
	ret
	.seh_endproc
	.p2align 4
	.globl	_Z9rdtsc_asmv
	.def	_Z9rdtsc_asmv;	.scl	2;	.type	32;	.endef
	.seh_proc	_Z9rdtsc_asmv
_Z9rdtsc_asmv:
.LFB168:
	.seh_endprologue
/APP
 # 65 "lessons\lesson02\main.cpp" 1
	rdtsc
 # 0 "" 2
/NO_APP
	salq	$32, %rdx
	movl	%eax, %eax
	orq	%rdx, %rax
	ret
	.seh_endproc
	.p2align 4
	.globl	_Z27loop_without_memory_barrieri
	.def	_Z27loop_without_memory_barrieri;	.scl	2;	.type	32;	.endef
	.seh_proc	_Z27loop_without_memory_barrieri
_Z27loop_without_memory_barrieri:
.LFB169:
	.seh_endprologue
	movl	g(%rip), %eax
	xorl	%edx, %edx
	imull	%ecx, %eax
	testl	%ecx, %ecx
	cmovle	%edx, %eax
	ret
	.seh_endproc
	.p2align 4
	.globl	_Z24loop_with_memory_barrieri
	.def	_Z24loop_with_memory_barrieri;	.scl	2;	.type	32;	.endef
	.seh_proc	_Z24loop_with_memory_barrieri
_Z24loop_with_memory_barrieri:
.LFB170:
	.seh_endprologue
	testl	%ecx, %ecx
	jle	.L14
	xorl	%eax, %eax
	xorl	%edx, %edx
	.p2align 4
	.p2align 3
.L13:
	addl	$1, %eax
	addl	g(%rip), %edx
	cmpl	%eax, %ecx
	jne	.L13
	movl	%edx, %eax
	ret
	.p2align 4,,10
	.p2align 3
.L14:
	xorl	%edx, %edx
	movl	%edx, %eax
	ret
	.seh_endproc
	.section .rdata,"dr"
.LC0:
	.ascii "bad_store output: %d\12\0"
.LC1:
	.ascii "good_store output: %d\12\0"
.LC2:
	.ascii "precise_store output: %d\12\0"
	.align 8
.LC3:
	.ascii "increment_store output: %d, d after increment: %d\12\0"
.LC4:
	.ascii "rdtsc_asm output: %016llX\12\0"
	.align 8
.LC5:
	.ascii "loop without memory barrier, with 5 times: %d\12\0"
	.align 8
.LC6:
	.ascii "loop with memory barrier, with 5 times: %d\12\0"
	.section	.text.startup,"x"
	.p2align 4
	.globl	main
	.def	main;	.scl	2;	.type	32;	.endef
	.seh_proc	main
main:
.LFB171:
	subq	$56, %rsp
	.seh_stackalloc	56
	.seh_endprologue
	call	__main
	leaq	32(%rsp), %rcx
	movl	$1, 32(%rsp)
	call	_Z9bad_storePi
	leaq	.LC0(%rip), %rcx
	movl	%eax, %edx
	call	__mingw_printf
	leaq	36(%rsp), %rcx
	movl	$1, 36(%rsp)
	call	_Z10good_storePi
	leaq	.LC1(%rip), %rcx
	movl	%eax, %edx
	call	__mingw_printf
	leaq	40(%rsp), %rcx
	movl	$1, 40(%rsp)
	call	_Z13precise_storePi
	leaq	.LC2(%rip), %rcx
	movl	%eax, %edx
	call	__mingw_printf
	leaq	44(%rsp), %rcx
	movl	$10, 44(%rsp)
	call	_Z15increment_storePi
	movl	44(%rsp), %r8d
	leaq	.LC3(%rip), %rcx
	movl	%eax, %edx
	call	__mingw_printf
	leaq	.LC4(%rip), %rcx
	call	_Z9rdtsc_asmv
	movq	%rax, %rdx
	call	__mingw_printf
	movl	$5, %ecx
	call	_Z27loop_without_memory_barrieri
	leaq	.LC5(%rip), %rcx
	movl	%eax, %edx
	call	__mingw_printf
	movl	$5, %ecx
	call	_Z24loop_with_memory_barrieri
	leaq	.LC6(%rip), %rcx
	movl	%eax, %edx
	call	__mingw_printf
	xorl	%eax, %eax
	addq	$56, %rsp
	ret
	.seh_endproc
	.globl	g
	.data
	.align 4
g:
	.long	9
	.def	__main;	.scl	2;	.type	32;	.endef
	.ident	"GCC: (MinGW-W64 x86_64-ucrt-posix-seh, built by Brecht Sanders, r1) 16.2.0"
