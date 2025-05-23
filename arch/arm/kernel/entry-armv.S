/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  linux/arch/arm/kernel/entry-armv.S
 *
 *  Copyright (C) 1996,1997,1998 Russell King.
 *  ARM700 fix by Matthew Godbolt (linux-user@willothewisp.demon.co.uk)
 *  nommu support by Hyok S. Choi (hyok.choi@samsung.com)
 *  Copyright (C) 2005 Stelian Pop.
 *
 *  Low-level vector interface routines
 *
 *  Note:  there is a StrongARM bug in the STMIA rn, {regs}^ instruction
 *  that causes it to save wrong values...  Be aware!
 */

#include <linux/init.h>

#include <asm/assembler.h>
#include <asm/page.h>
#include <asm/glue-df.h>
#include <asm/glue-pf.h>
#include <asm/vfpmacros.h>
#include <asm/thread_notify.h>
#include <asm/unwind.h>
#include <asm/unistd.h>
#include <asm/tls.h>
#include <asm/system_info.h>
#include <asm/uaccess-asm.h>
#include <asm/kasan_def.h>

#include "entry-header.S"
#include <asm/probes.h>
#include <asm/dovetail.h>

#ifdef CONFIG_HAVE_LD_DEAD_CODE_DATA_ELIMINATION
#define RELOC_TEXT_NONE .reloc  .text, R_ARM_NONE, .
#else
#define RELOC_TEXT_NONE
#endif

/*
 * Interrupt handling.
 */

#ifdef CONFIG_IRQ_PIPELINE
#define irq_service_handler	handle_arch_irq_pipelined
#else
#define irq_service_handler	generic_handle_arch_irq
#endif

	.macro	irq_handler, from_user:req
#ifdef CONFIG_DOVETAIL
	mov	r0, sp
	bl	irq_service_handler
#else
	mov	r1, sp
	ldr_this_cpu r2, irq_stack_ptr, r2, r3
	.if	\from_user == 0
	@
	@ If we took the interrupt while running in the kernel, we may already
	@ be using the IRQ stack, so revert to the original value in that case.
	@
	subs	r3, r2, r1		@ SP above bottom of IRQ stack?
	rsbscs	r3, r3, #THREAD_SIZE	@ ... and below the top?
#ifdef CONFIG_VMAP_STACK
	ldr_va	r3, high_memory, cc	@ End of the linear region
	cmpcc	r3, r1			@ Stack pointer was below it?
#endif
	bcc	0f			@ If not, switch to the IRQ stack
	mov	r0, r1
	bl	irq_service_handler
	b	1f
0:
	.endif

	mov_l	r0, irq_service_handler
	bl	call_with_stack
1:
#endif /* !CONFIG_DOVETAIL */
	.endm

	.macro	pabt_helper
	@ PABORT handler takes pt_regs in r2, fault address in r4 and psr in r5
#ifdef MULTI_PABORT
	ldr_va	ip, processor, offset=PROCESSOR_PABT_FUNC
	bl_r	ip
#else
	bl	CPU_PABORT_HANDLER
#endif
	.endm

	.macro	dabt_helper

	@
	@ Call the processor-specific abort handler:
	@
	@  r2 - pt_regs
	@  r4 - aborted context pc
	@  r5 - aborted context psr
	@
	@ The abort handler must return the aborted address in r0, and
	@ the fault status register in r1.  r9 must be preserved.
	@
#ifdef MULTI_DABORT
	ldr_va	ip, processor, offset=PROCESSOR_DABT_FUNC
	bl_r	ip
#else
	bl	CPU_DABORT_HANDLER
#endif
	.endm

	.section	.entry.text,"ax",%progbits

/*
 * Invalid mode handlers
 */
	.macro	inv_entry, reason
	sub	sp, sp, #PT_REGS_SIZE
 ARM(	stmib	sp, {r1 - lr}		)
 THUMB(	stmia	sp, {r0 - r12}		)
 THUMB(	str	sp, [sp, #S_SP]		)
 THUMB(	str	lr, [sp, #S_LR]		)
	mov	r1, #\reason
	.endm

__pabt_invalid:
	inv_entry BAD_PREFETCH
	b	common_invalid
ENDPROC(__pabt_invalid)

__dabt_invalid:
	inv_entry BAD_DATA
	b	common_invalid
ENDPROC(__dabt_invalid)

__irq_invalid:
	inv_entry BAD_IRQ
	b	common_invalid
ENDPROC(__irq_invalid)

__und_invalid:
	inv_entry BAD_UNDEFINSTR

	@
	@ XXX fall through to common_invalid
	@

@
@ common_invalid - generic code for failed exception (re-entrant version of handlers)
@
common_invalid:
	zero_fp

	ldmia	r0, {r4 - r6}
	add	r0, sp, #S_PC		@ here for interlock avoidance
	mov	r7, #-1			@  ""   ""    ""        ""
	str	r4, [sp]		@ save preserved r0
	stmia	r0, {r5 - r7}		@ lr_<exception>,
					@ cpsr_<exception>, "old_r0"

	mov	r0, sp
	b	bad_mode
ENDPROC(__und_invalid)

/*
 * SVC mode handlers
 */

#if defined(CONFIG_AEABI) && (__LINUX_ARM_ARCH__ >= 5)
#define SPFIX(code...) code
#else
#define SPFIX(code...)
#endif

	.macro	svc_entry, stack_hole=0, trace=1, uaccess=1, overflow_check=1
 UNWIND(.fnstart		)
	sub	sp, sp, #(SVC_REGS_SIZE + \stack_hole)
 THUMB(	add	sp, r1		)	@ get SP in a GPR without
 THUMB(	sub	r1, sp, r1	)	@ using a temp register

	.if	\overflow_check
 UNWIND(.save	{r0 - pc}	)
	do_overflow_check (SVC_REGS_SIZE + \stack_hole)
	.endif

#ifdef CONFIG_THUMB2_KERNEL
	tst	r1, #4			@ test stack pointer alignment
	sub	r1, sp, r1		@ restore original R1
	sub	sp, r1			@ restore original SP
#else
 SPFIX(	tst	sp, #4		)
#endif
 SPFIX(	subne	sp, sp, #4	)

 ARM(	stmib	sp, {r1 - r12}	)
 THUMB(	stmia	sp, {r0 - r12}	)	@ No STMIB in Thumb-2

	ldmia	r0, {r3 - r5}
	add	r7, sp, #S_SP		@ here for interlock avoidance
	mov	r6, #-1			@  ""  ""      ""       ""
	add	r2, sp, #(SVC_REGS_SIZE + \stack_hole)
 SPFIX(	addne	r2, r2, #4	)
	str	r3, [sp]		@ save the "real" r0 copied
					@ from the exception stack

	mov	r3, lr

	@
	@ We are now ready to fill in the remaining blanks on the stack:
	@
	@  r2 - sp_svc
	@  r3 - lr_svc
	@  r4 - lr_<exception>, already fixed up for correct return/restart
	@  r5 - spsr_<exception>
	@  r6 - orig_r0 (see pt_regs definition in ptrace.h)
	@
	stmia	r7, {r2 - r6}

	get_thread_info tsk
	uaccess_entry tsk, r0, r1, r2, \uaccess

	.if \trace
#ifdef CONFIG_IRQ_PIPELINE
	mov	r0, sp
	bl	kentry_enter_pipelined
#elif defined(CONFIG_TRACE_IRQFLAGS)
	bl	trace_hardirqs_off
#endif
	.endif
	.endm

	.align	5
__dabt_svc:
	svc_entry uaccess=0
	mov	r2, sp
	dabt_helper
 THUMB(	ldr	r5, [sp, #S_PSR]	)	@ potentially updated CPSR
	svc_exit r5				@ return from exception
 UNWIND(.fnend		)
ENDPROC(__dabt_svc)

	.align	5
__irq_svc:
	svc_entry
	irq_handler from_user=0
#ifdef CONFIG_IRQ_PIPELINE
	tst	r0, r0				@ skip epilogue if oob or in-band stalled
	beq	1f
#endif

#ifdef CONFIG_PREEMPTION
	ldr	r8, [tsk, #TI_PREEMPT]		@ get preempt count
	ldr	r0, [tsk, #TI_FLAGS]		@ get flags
	teq	r8, #0				@ if preempt count != 0
	movne	r0, #0				@ force flags to 0
	tst	r0, #_TIF_NEED_RESCHED
	blne	svc_preempt
#endif

1:
	svc_exit r5, irq = 1			@ return from exception
 UNWIND(.fnend		)
ENDPROC(__irq_svc)

	.ltorg

#ifdef CONFIG_PREEMPTION
svc_preempt:
	mov	r8, lr
1:	bl	arm_preempt_schedule_irq	@ irq en/disable is done inside
	ldr	r0, [tsk, #TI_FLAGS]		@ get new tasks TI_FLAGS
	tst	r0, #_TIF_NEED_RESCHED
	reteq	r8				@ go again
	b	1b
#endif

__und_fault:
	@ Correct the PC such that it is pointing at the instruction
	@ which caused the fault.  If the faulting instruction was ARM
	@ the PC will be pointing at the next instruction, and have to
	@ subtract 4.  Otherwise, it is Thumb, and the PC will be
	@ pointing at the second half of the Thumb instruction.  We
	@ have to subtract 2.
	ldr	r2, [r0, #S_PC]
	sub	r2, r2, r1
	str	r2, [r0, #S_PC]
	b	do_undefinstr
ENDPROC(__und_fault)

	.align	5
__und_svc:
#ifdef CONFIG_KPROBES
	@ If a kprobe is about to simulate a "stmdb sp..." instruction,
	@ it obviously needs free stack space which then will belong to
	@ the saved context.
	svc_entry MAX_STACK_SIZE
#else
	svc_entry
#endif
#ifdef CONFIG_DOVETAIL
	get_thread_info tsk
	ldr	r0, [tsk, #TI_PREEMPT]		@ get preempt count
	tst	r0, #TI_OOB_MASK		@ oob stage?
	beq	1f
	mov	r0, #ARM_TRAP_UNDEFINSTR
	mov	r1, sp				@ r1 = &regs
	bl	__oob_trap_notify
1:
#endif

	mov	r1, #4				@ PC correction to apply
 THUMB(	tst	r5, #PSR_T_BIT		)	@ exception taken in Thumb mode?
 THUMB(	movne	r1, #2			)	@ if so, fix up PC correction
	mov	r0, sp				@ struct pt_regs *regs
	bl	__und_fault

__und_svc_finish:
	get_thread_info tsk
#ifdef CONFIG_DOVETAIL
	ldr	r0, [tsk, #TI_PREEMPT]		@ get preempt count
	tst	r0, #TI_OOB_MASK		@ oob stage?
	beq	1f
	mov	r0, #ARM_TRAP_UNDEFINSTR
	mov	r1, sp				@ r1 = &regs
	bl	__oob_trap_unwind
1:
#endif
	ldr	r5, [sp, #S_PSR]		@ Get SVC cpsr
	svc_exit r5				@ return from exception
 UNWIND(.fnend		)
ENDPROC(__und_svc)

	.align	5
__pabt_svc:
	svc_entry
	mov	r2, sp				@ regs
	pabt_helper
	svc_exit r5				@ return from exception
 UNWIND(.fnend		)
ENDPROC(__pabt_svc)

	.align	5
__fiq_svc:
	svc_entry trace=0
	mov	r0, sp				@ struct pt_regs *regs
	bl	handle_fiq_as_nmi
	svc_exit_via_fiq
 UNWIND(.fnend		)
ENDPROC(__fiq_svc)

/*
 * Abort mode handlers
 */

@
@ Taking a FIQ in abort mode is similar to taking a FIQ in SVC mode
@ and reuses the same macros. However in abort mode we must also
@ save/restore lr_abt and spsr_abt to make nested aborts safe.
@
	.align 5
__fiq_abt:
	svc_entry trace=0

 ARM(	msr	cpsr_c, #ABT_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( mov	r0, #ABT_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( msr	cpsr_c, r0 )
	mov	r1, lr		@ Save lr_abt
	mrs	r2, spsr	@ Save spsr_abt, abort is now safe
 ARM(	msr	cpsr_c, #SVC_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( mov	r0, #SVC_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( msr	cpsr_c, r0 )
	stmfd	sp!, {r1 - r2}

	add	r0, sp, #8			@ struct pt_regs *regs
	bl	handle_fiq_as_nmi

	ldmfd	sp!, {r1 - r2}
 ARM(	msr	cpsr_c, #ABT_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( mov	r0, #ABT_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( msr	cpsr_c, r0 )
	mov	lr, r1		@ Restore lr_abt, abort is unsafe
	msr	spsr_cxsf, r2	@ Restore spsr_abt
 ARM(	msr	cpsr_c, #SVC_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( mov	r0, #SVC_MODE | PSR_I_BIT | PSR_F_BIT )
 THUMB( msr	cpsr_c, r0 )

	svc_exit_via_fiq
 UNWIND(.fnend		)
ENDPROC(__fiq_abt)

/*
 * User mode handlers
 *
 * EABI note: sp_svc is always 64-bit aligned here, so should PT_REGS_SIZE
 */

#if defined(CONFIG_AEABI) && (__LINUX_ARM_ARCH__ >= 5) && (PT_REGS_SIZE & 7)
#error "sizeof(struct pt_regs) must be a multiple of 8"
#endif

	.macro	usr_entry, trace=1, uaccess=1
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)	@ don't unwind the user space
	sub	sp, sp, #PT_REGS_SIZE
 ARM(	stmib	sp, {r1 - r12}	)
 THUMB(	stmia	sp, {r0 - r12}	)

 ATRAP(	mrc	p15, 0, r7, c1, c0, 0)
 ATRAP(	ldr_va	r8, cr_alignment)

	ldmia	r0, {r3 - r5}
	add	r0, sp, #S_PC		@ here for interlock avoidance
	mov	r6, #-1			@  ""  ""     ""        ""

	str	r3, [sp]		@ save the "real" r0 copied
					@ from the exception stack

	@
	@ We are now ready to fill in the remaining blanks on the stack:
	@
	@  r4 - lr_<exception>, already fixed up for correct return/restart
	@  r5 - spsr_<exception>
	@  r6 - orig_r0 (see pt_regs definition in ptrace.h)
	@
	@ Also, separately save sp_usr and lr_usr
	@
	stmia	r0, {r4 - r6}
 ARM(	stmdb	r0, {sp, lr}^			)
 THUMB(	store_user_sp_lr r0, r1, S_SP - S_PC	)

	.if \uaccess
	uaccess_disable ip
	.endif

	@ Enable the alignment trap while in kernel mode
 ATRAP(	teq	r8, r7)
 ATRAP( mcrne	p15, 0, r8, c1, c0, 0)

	reload_current r7, r8

	@
	@ Clear FP to mark the first stack frame
	@
	zero_fp

	.if	\trace
#ifdef CONFIG_TRACE_IRQFLAGS
	bl	trace_hardirqs_off_pipelined
#endif
	ct_user_exit save = 0
	.endif
	.endm

	.macro	kuser_cmpxchg_check
#if !defined(CONFIG_CPU_32v6K) && defined(CONFIG_KUSER_HELPERS)
#ifndef CONFIG_MMU
#warning "NPTL on non MMU needs fixing"
#else
	@ Make sure our user space atomic helper is restarted
	@ if it was interrupted in a critical region.  Here we
	@ perform a quick test inline since it should be false
	@ 99.9999% of the time.  The rest is done out of line.
	ldr	r0, =TASK_SIZE
	cmp	r4, r0
	blhs	kuser_cmpxchg64_fixup
#endif
#endif
	.endm

	.align	5
__dabt_usr:
	usr_entry uaccess=0
	kuser_cmpxchg_check
	mov	r2, sp
	dabt_helper
	b	ret_from_exception
 UNWIND(.fnend		)
ENDPROC(__dabt_usr)

	.align	5
__irq_usr:
	usr_entry
	kuser_cmpxchg_check
	irq_handler from_user=1
	get_thread_info tsk
	mov	why, #0
#ifdef CONFIG_IRQ_PIPELINE
	tst	r0, r0
	beq	fast_ret_to_user	@ skip epilogue if oob (in-band cannot be stalled)
#endif
	b	ret_to_user_from_irq
 UNWIND(.fnend		)
ENDPROC(__irq_usr)

	.ltorg

	.align	5
__und_usr:
	usr_entry uaccess=0

	@ IRQs must be enabled before attempting to read the instruction from
	@ user space since that could cause a page/translation fault if the
	@ page table was modified by another CPU.
	enable_irq

	tst	r5, #PSR_T_BIT			@ Thumb mode?
	mov	r1, #2				@ set insn size to 2 for Thumb
	bne	0f				@ handle as Thumb undef exception
#ifdef CONFIG_FPE_NWFPE
	adr	r9, ret_from_exception
	bl	call_fpe			@ returns via R9 on success
#endif
	mov	r1, #4				@ set insn size to 4 for ARM
0:	mov	r0, sp
	uaccess_disable ip
	bl	__und_fault
	b	ret_from_exception
 UNWIND(.fnend)
ENDPROC(__und_usr)

	.align	5
__pabt_usr:
	usr_entry
	mov	r2, sp				@ regs
	pabt_helper
 UNWIND(.fnend		)
	/* fall through */
/*
 * This is the return code to user mode for abort handlers
 */
ENTRY(ret_from_exception)
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)
	get_thread_info tsk
	mov	why, #0
	ret_to_user_pipelined r1
 UNWIND(.fnend		)
ENDPROC(__pabt_usr)
ENDPROC(ret_from_exception)

	.align	5
__fiq_usr:
	usr_entry trace=0
	kuser_cmpxchg_check
	mov	r0, sp				@ struct pt_regs *regs
	bl	handle_fiq_as_nmi
	get_thread_info tsk
	restore_user_regs fast = 0, offset = 0
 UNWIND(.fnend		)
ENDPROC(__fiq_usr)

/*
 * Register switch for ARMv3 and ARMv4 processors
 * r0 = previous task_struct, r1 = previous thread_info, r2 = next thread_info
 * previous and next are guaranteed not to be the same.
 */
ENTRY(__switch_to)
 UNWIND(.fnstart	)
 UNWIND(.cantunwind	)
	add	ip, r1, #TI_CPU_SAVE
 ARM(	stmia	ip!, {r4 - sl, fp, sp, lr} )	@ Store most regs on stack
 THUMB(	stmia	ip!, {r4 - sl, fp}	   )	@ Store most regs on stack
 THUMB(	str	sp, [ip], #4		   )
 THUMB(	str	lr, [ip], #4		   )
	ldr	r4, [r2, #TI_TP_VALUE]
	ldr	r5, [r2, #TI_TP_VALUE + 4]
#ifdef CONFIG_CPU_USE_DOMAINS
	mrc	p15, 0, r6, c3, c0, 0		@ Get domain register
	str	r6, [r1, #TI_CPU_DOMAIN]	@ Save old domain register
	ldr	r6, [r2, #TI_CPU_DOMAIN]
#endif
	switch_tls r1, r4, r5, r3, r7
#if defined(CONFIG_STACKPROTECTOR) && !defined(CONFIG_SMP) && \
    !defined(CONFIG_STACKPROTECTOR_PER_TASK)
	ldr	r8, =__stack_chk_guard
	.if (TSK_STACK_CANARY > IMM12_MASK)
	add	r9, r2, #TSK_STACK_CANARY & ~IMM12_MASK
	ldr	r9, [r9, #TSK_STACK_CANARY & IMM12_MASK]
	.else
	ldr	r9, [r2, #TSK_STACK_CANARY & IMM12_MASK]
	.endif
#endif
	mov	r7, r2				@ Preserve 'next'
#ifdef CONFIG_CPU_USE_DOMAINS
	mcr	p15, 0, r6, c3, c0, 0		@ Set domain register
#endif
	mov	r5, r0
	add	r4, r2, #TI_CPU_SAVE
	ldr	r0, =thread_notify_head
	mov	r1, #THREAD_NOTIFY_SWITCH
	bl	atomic_notifier_call_chain
#if defined(CONFIG_STACKPROTECTOR) && !defined(CONFIG_SMP) && \
    !defined(CONFIG_STACKPROTECTOR_PER_TASK)
	str	r9, [r8]
#endif
	mov	r0, r5
#if !defined(CONFIG_THUMB2_KERNEL) && !defined(CONFIG_VMAP_STACK)
	set_current r7, r8
	ldmia	r4, {r4 - sl, fp, sp, pc}	@ Load all regs saved previously
#else
	mov	r1, r7
	ldmia	r4, {r4 - sl, fp, ip, lr}	@ Load all regs saved previously
#ifdef CONFIG_VMAP_STACK
	@
	@ Do a dummy read from the new stack while running from the old one so
	@ that we can rely on do_translation_fault() to fix up any stale PMD
	@ entries covering the vmalloc region.
	@
	ldr	r2, [ip]
#ifdef CONFIG_KASAN_VMALLOC
	@ Also dummy read from the KASAN shadow memory for the new stack if we
	@ are using KASAN
	mov_l	r2, KASAN_SHADOW_OFFSET
	add	r2, r2, ip, lsr #KASAN_SHADOW_SCALE_SHIFT
	ldr	r2, [r2]
#endif
#endif

	@ When CONFIG_THREAD_INFO_IN_TASK=n, the update of SP itself is what
	@ effectuates the task switch, as that is what causes the observable
	@ values of current and current_thread_info to change. When
	@ CONFIG_THREAD_INFO_IN_TASK=y, setting current (and therefore
	@ current_thread_info) is done explicitly, and the update of SP just
	@ switches us to another stack, with few other side effects. In order
	@ to prevent this distinction from causing any inconsistencies, let's
	@ keep the 'set_current' call as close as we can to the update of SP.
	set_current r1, r2
	mov	sp, ip
	ret	lr
#endif
 UNWIND(.fnend		)
ENDPROC(__switch_to)

#ifdef CONFIG_VMAP_STACK
	.text
	.align	2
__bad_stack:
	@
	@ We've just detected an overflow. We need to load the address of this
	@ CPU's overflow stack into the stack pointer register. We have only one
	@ scratch register so let's use a sequence of ADDs including one
	@ involving the PC, and decorate them with PC-relative group
	@ relocations. As these are ARM only, switch to ARM mode first.
	@
	@ We enter here with IP clobbered and its value stashed on the mode
	@ stack.
	@
THUMB(	bx	pc		)
THUMB(	nop			)
THUMB(	.arm			)
	ldr_this_cpu_armv6 ip, overflow_stack_ptr

	str	sp, [ip, #-4]!			@ Preserve original SP value
	mov	sp, ip				@ Switch to overflow stack
	pop	{ip}				@ Original SP in IP

#if defined(CONFIG_UNWINDER_FRAME_POINTER) && defined(CONFIG_CC_IS_GCC)
	mov	ip, ip				@ mov expected by unwinder
	push	{fp, ip, lr, pc}		@ GCC flavor frame record
#else
	str	ip, [sp, #-8]!			@ store original SP
	push	{fpreg, lr}			@ Clang flavor frame record
#endif
UNWIND( ldr	ip, [r0, #4]	)		@ load exception LR
UNWIND( str	ip, [sp, #12]	)		@ store in the frame record
	ldr	ip, [r0, #12]			@ reload IP

	@ Store the original GPRs to the new stack.
	svc_entry uaccess=0, overflow_check=0

UNWIND( .save   {sp, pc}	)
UNWIND( .save   {fpreg, lr}	)
UNWIND( .setfp  fpreg, sp	)

	ldr	fpreg, [sp, #S_SP]		@ Add our frame record
						@ to the linked list
#if defined(CONFIG_UNWINDER_FRAME_POINTER) && defined(CONFIG_CC_IS_GCC)
	ldr	r1, [fp, #4]			@ reload SP at entry
	add	fp, fp, #12
#else
	ldr	r1, [fpreg, #8]
#endif
	str	r1, [sp, #S_SP]			@ store in pt_regs

	@ Stash the regs for handle_bad_stack
	mov	r0, sp

	@ Time to die
	bl	handle_bad_stack
	nop
UNWIND( .fnend			)
ENDPROC(__bad_stack)
#endif

	__INIT

/*
 * User helpers.
 *
 * Each segment is 32-byte aligned and will be moved to the top of the high
 * vector page.  New segments (if ever needed) must be added in front of
 * existing ones.  This mechanism should be used only for things that are
 * really small and justified, and not be abused freely.
 *
 * See Documentation/arch/arm/kernel_user_helpers.rst for formal definitions.
 */
 THUMB(	.arm	)

	.macro	usr_ret, reg
#ifdef CONFIG_ARM_THUMB
	bx	\reg
#else
	ret	\reg
#endif
	.endm

	.macro	kuser_pad, sym, size
	.if	(. - \sym) & 3
	.rept	4 - (. - \sym) & 3
	.byte	0
	.endr
	.endif
	.rept	(\size - (. - \sym)) / 4
	.word	0xe7fddef1
	.endr
	.endm

#ifdef CONFIG_KUSER_HELPERS
	.align	5
	.globl	__kuser_helper_start
__kuser_helper_start:

/*
 * Due to the length of some sequences, __kuser_cmpxchg64 spans 2 regular
 * kuser "slots", therefore 0xffff0f80 is not used as a valid entry point.
 */

__kuser_cmpxchg64:				@ 0xffff0f60

#if defined(CONFIG_CPU_32v6K)

	stmfd	sp!, {r4, r5, r6, r7}
	ldrd	r4, r5, [r0]			@ load old val
	ldrd	r6, r7, [r1]			@ load new val
	smp_dmb	arm
1:	ldrexd	r0, r1, [r2]			@ load current val
	eors	r3, r0, r4			@ compare with oldval (1)
	eorseq	r3, r1, r5			@ compare with oldval (2)
	strexdeq r3, r6, r7, [r2]		@ store newval if eq
	teqeq	r3, #1				@ success?
	beq	1b				@ if no then retry
	smp_dmb	arm
	rsbs	r0, r3, #0			@ set returned val and C flag
	ldmfd	sp!, {r4, r5, r6, r7}
	usr_ret	lr

#elif !defined(CONFIG_SMP)

#ifdef CONFIG_MMU

	/*
	 * The only thing that can break atomicity in this cmpxchg64
	 * implementation is either an IRQ or a data abort exception
	 * causing another process/thread to be scheduled in the middle of
	 * the critical sequence.  The same strategy as for cmpxchg is used.
	 */
	stmfd	sp!, {r4, r5, r6, lr}
	ldmia	r0, {r4, r5}			@ load old val
	ldmia	r1, {r6, lr}			@ load new val
1:	ldmia	r2, {r0, r1}			@ load current val
	eors	r3, r0, r4			@ compare with oldval (1)
	eorseq	r3, r1, r5			@ compare with oldval (2)
2:	stmiaeq	r2, {r6, lr}			@ store newval if eq
	rsbs	r0, r3, #0			@ set return val and C flag
	ldmfd	sp!, {r4, r5, r6, pc}

	.text
kuser_cmpxchg64_fixup:
	@ Called from kuser_cmpxchg_fixup.
	@ r4 = address of interrupted insn (must be preserved).
	@ sp = saved regs. r7 and r8 are clobbered.
	@ 1b = first critical insn, 2b = last critical insn.
	@ If r4 >= 1b and r4 <= 2b then saved pc_usr is set to 1b.
	mov	r7, #0xffff0fff
	sub	r7, r7, #(0xffff0fff - (0xffff0f60 + (1b - __kuser_cmpxchg64)))
	subs	r8, r4, r7
	rsbscs	r8, r8, #(2b - 1b)
	strcs	r7, [sp, #S_PC]
#if __LINUX_ARM_ARCH__ < 6
	bcc	kuser_cmpxchg32_fixup
#endif
	ret	lr
	.previous

#else
#warning "NPTL on non MMU needs fixing"
	mov	r0, #-1
	adds	r0, r0, #0
	usr_ret	lr
#endif

#else
#error "incoherent kernel configuration"
#endif

	kuser_pad __kuser_cmpxchg64, 64

__kuser_memory_barrier:				@ 0xffff0fa0
	smp_dmb	arm
	usr_ret	lr

	kuser_pad __kuser_memory_barrier, 32

__kuser_cmpxchg:				@ 0xffff0fc0

#if __LINUX_ARM_ARCH__ < 6

#ifdef CONFIG_MMU

	/*
	 * The only thing that can break atomicity in this cmpxchg
	 * implementation is either an IRQ or a data abort exception
	 * causing another process/thread to be scheduled in the middle
	 * of the critical sequence.  To prevent this, code is added to
	 * the IRQ and data abort exception handlers to set the pc back
	 * to the beginning of the critical section if it is found to be
	 * within that critical section (see kuser_cmpxchg_fixup).
	 */
1:	ldr	r3, [r2]			@ load current val
	subs	r3, r3, r0			@ compare with oldval
2:	streq	r1, [r2]			@ store newval if eq
	rsbs	r0, r3, #0			@ set return val and C flag
	usr_ret	lr

	.text
kuser_cmpxchg32_fixup:
	@ Called from kuser_cmpxchg_check macro.
	@ r4 = address of interrupted insn (must be preserved).
	@ sp = saved regs. r7 and r8 are clobbered.
	@ 1b = first critical insn, 2b = last critical insn.
	@ If r4 >= 1b and r4 <= 2b then saved pc_usr is set to 1b.
	mov	r7, #0xffff0fff
	sub	r7, r7, #(0xffff0fff - (0xffff0fc0 + (1b - __kuser_cmpxchg)))
	subs	r8, r4, r7
	rsbscs	r8, r8, #(2b - 1b)
	strcs	r7, [sp, #S_PC]
	ret	lr
	.previous

#else
#warning "NPTL on non MMU needs fixing"
	mov	r0, #-1
	adds	r0, r0, #0
	usr_ret	lr
#endif

#else

	smp_dmb	arm
1:	ldrex	r3, [r2]
	subs	r3, r3, r0
	strexeq	r3, r1, [r2]
	teqeq	r3, #1
	beq	1b
	rsbs	r0, r3, #0
	/* beware -- each __kuser slot must be 8 instructions max */
	ALT_SMP(b	__kuser_memory_barrier)
	ALT_UP(usr_ret	lr)

#endif

	kuser_pad __kuser_cmpxchg, 32

__kuser_get_tls:				@ 0xffff0fe0
	ldr	r0, [pc, #(16 - 8)]	@ read TLS, set in kuser_get_tls_init
	usr_ret	lr
	mrc	p15, 0, r0, c13, c0, 3	@ 0xffff0fe8 hardware TLS code
	kuser_pad __kuser_get_tls, 16
	.rep	3
	.word	0			@ 0xffff0ff0 software TLS value, then
	.endr				@ pad up to __kuser_helper_version

__kuser_helper_version:				@ 0xffff0ffc
	.word	((__kuser_helper_end - __kuser_helper_start) >> 5)

	.globl	__kuser_helper_end
__kuser_helper_end:

#endif

 THUMB(	.thumb	)

/*
 * Vector stubs.
 *
 * This code is copied to 0xffff1000 so we can use branches in the
 * vectors, rather than ldr's.  Note that this code must not exceed
 * a page size.
 *
 * Common stub entry macro:
 *   Enter in IRQ mode, spsr = SVC/USR CPSR, lr = SVC/USR PC
 *
 * SP points to a minimal amount of processor-private memory, the address
 * of which is copied into r0 for the mode specific abort handler.
 */
	.macro	vector_stub, name, mode, correction=0
	.align	5
#ifdef CONFIG_HARDEN_BRANCH_HISTORY
vector_bhb_bpiall_\name:
	mcr	p15, 0, r0, c7, c5, 6	@ BPIALL
	@ isb not needed due to "movs pc, lr" in the vector stub
	@ which gives a "context synchronisation".
#endif

vector_\name:
	.if \correction
	sub	lr, lr, #\correction
	.endif

	@ Save r0, lr_<exception> (parent PC)
	stmia	sp, {r0, lr}		@ save r0, lr

	@ Save spsr_<exception> (parent CPSR)
.Lvec_\name:
	mrs	lr, spsr
	str	lr, [sp, #8]		@ save spsr

	@
	@ Prepare for SVC32 mode.  IRQs remain disabled.
	@
	mrs	r0, cpsr
	eor	r0, r0, #(\mode ^ SVC_MODE | PSR_ISETSTATE)
	msr	spsr_cxsf, r0

	@
	@ the branch table must immediately follow this code
	@
	and	lr, lr, #0x0f
 THUMB(	adr	r0, 1f			)
 THUMB(	ldr	lr, [r0, lr, lsl #2]	)
	mov	r0, sp
 ARM(	ldr	lr, [pc, lr, lsl #2]	)
	movs	pc, lr			@ branch to handler in SVC mode
ENDPROC(vector_\name)

#ifdef CONFIG_HARDEN_BRANCH_HISTORY
	.subsection 1
	.align 5
vector_bhb_loop8_\name:
	.if \correction
	sub	lr, lr, #\correction
	.endif

	@ Save r0, lr_<exception> (parent PC)
	stmia	sp, {r0, lr}

	@ bhb workaround
	mov	r0, #8
3:	W(b)	. + 4
	subs	r0, r0, #1
	bne	3b
	dsb	nsh
	@ isb not needed due to "movs pc, lr" in the vector stub
	@ which gives a "context synchronisation".
	b	.Lvec_\name
ENDPROC(vector_bhb_loop8_\name)
	.previous
#endif

	.align	2
	@ handler addresses follow this label
1:
	.endm

	.section .stubs, "ax", %progbits
	@ These need to remain at the start of the section so that
	@ they are in range of the 'SWI' entries in the vector tables
	@ located 4k down.
.L__vector_swi:
	.word	vector_swi
#ifdef CONFIG_HARDEN_BRANCH_HISTORY
.L__vector_bhb_loop8_swi:
	.word	vector_bhb_loop8_swi
.L__vector_bhb_bpiall_swi:
	.word	vector_bhb_bpiall_swi
#endif

vector_rst:
 ARM(	swi	SYS_ERROR0	)
 THUMB(	svc	#0		)
 THUMB(	nop			)
	b	vector_und

/*
 * Interrupt dispatcher
 */
	vector_stub	irq, IRQ_MODE, 4

	.long	__irq_usr			@  0  (USR_26 / USR_32)
	.long	__irq_invalid			@  1  (FIQ_26 / FIQ_32)
	.long	__irq_invalid			@  2  (IRQ_26 / IRQ_32)
	.long	__irq_svc			@  3  (SVC_26 / SVC_32)
	.long	__irq_invalid			@  4
	.long	__irq_invalid			@  5
	.long	__irq_invalid			@  6
	.long	__irq_invalid			@  7
	.long	__irq_invalid			@  8
	.long	__irq_invalid			@  9
	.long	__irq_invalid			@  a
	.long	__irq_invalid			@  b
	.long	__irq_invalid			@  c
	.long	__irq_invalid			@  d
	.long	__irq_invalid			@  e
	.long	__irq_invalid			@  f

/*
 * Data abort dispatcher
 * Enter in ABT mode, spsr = USR CPSR, lr = USR PC
 */
	vector_stub	dabt, ABT_MODE, 8

	.long	__dabt_usr			@  0  (USR_26 / USR_32)
	.long	__dabt_invalid			@  1  (FIQ_26 / FIQ_32)
	.long	__dabt_invalid			@  2  (IRQ_26 / IRQ_32)
	.long	__dabt_svc			@  3  (SVC_26 / SVC_32)
	.long	__dabt_invalid			@  4
	.long	__dabt_invalid			@  5
	.long	__dabt_invalid			@  6
	.long	__dabt_invalid			@  7
	.long	__dabt_invalid			@  8
	.long	__dabt_invalid			@  9
	.long	__dabt_invalid			@  a
	.long	__dabt_invalid			@  b
	.long	__dabt_invalid			@  c
	.long	__dabt_invalid			@  d
	.long	__dabt_invalid			@  e
	.long	__dabt_invalid			@  f

/*
 * Prefetch abort dispatcher
 * Enter in ABT mode, spsr = USR CPSR, lr = USR PC
 */
	vector_stub	pabt, ABT_MODE, 4

	.long	__pabt_usr			@  0 (USR_26 / USR_32)
	.long	__pabt_invalid			@  1 (FIQ_26 / FIQ_32)
	.long	__pabt_invalid			@  2 (IRQ_26 / IRQ_32)
	.long	__pabt_svc			@  3 (SVC_26 / SVC_32)
	.long	__pabt_invalid			@  4
	.long	__pabt_invalid			@  5
	.long	__pabt_invalid			@  6
	.long	__pabt_invalid			@  7
	.long	__pabt_invalid			@  8
	.long	__pabt_invalid			@  9
	.long	__pabt_invalid			@  a
	.long	__pabt_invalid			@  b
	.long	__pabt_invalid			@  c
	.long	__pabt_invalid			@  d
	.long	__pabt_invalid			@  e
	.long	__pabt_invalid			@  f

/*
 * Undef instr entry dispatcher
 * Enter in UND mode, spsr = SVC/USR CPSR, lr = SVC/USR PC
 */
	vector_stub	und, UND_MODE

	.long	__und_usr			@  0 (USR_26 / USR_32)
	.long	__und_invalid			@  1 (FIQ_26 / FIQ_32)
	.long	__und_invalid			@  2 (IRQ_26 / IRQ_32)
	.long	__und_svc			@  3 (SVC_26 / SVC_32)
	.long	__und_invalid			@  4
	.long	__und_invalid			@  5
	.long	__und_invalid			@  6
	.long	__und_invalid			@  7
	.long	__und_invalid			@  8
	.long	__und_invalid			@  9
	.long	__und_invalid			@  a
	.long	__und_invalid			@  b
	.long	__und_invalid			@  c
	.long	__und_invalid			@  d
	.long	__und_invalid			@  e
	.long	__und_invalid			@  f

	.align	5

/*=============================================================================
 * Address exception handler
 *-----------------------------------------------------------------------------
 * These aren't too critical.
 * (they're not supposed to happen, and won't happen in 32-bit data mode).
 */

vector_addrexcptn:
	b	vector_addrexcptn

/*=============================================================================
 * FIQ "NMI" handler
 *-----------------------------------------------------------------------------
 * Handle a FIQ using the SVC stack allowing FIQ act like NMI on x86
 * systems. This must be the last vector stub, so lets place it in its own
 * subsection.
 */
	.subsection 2
	vector_stub	fiq, FIQ_MODE, 4

	.long	__fiq_usr			@  0  (USR_26 / USR_32)
	.long	__fiq_svc			@  1  (FIQ_26 / FIQ_32)
	.long	__fiq_svc			@  2  (IRQ_26 / IRQ_32)
	.long	__fiq_svc			@  3  (SVC_26 / SVC_32)
	.long	__fiq_svc			@  4
	.long	__fiq_svc			@  5
	.long	__fiq_svc			@  6
	.long	__fiq_abt			@  7
	.long	__fiq_svc			@  8
	.long	__fiq_svc			@  9
	.long	__fiq_svc			@  a
	.long	__fiq_svc			@  b
	.long	__fiq_svc			@  c
	.long	__fiq_svc			@  d
	.long	__fiq_svc			@  e
	.long	__fiq_svc			@  f

	.globl	vector_fiq

	.section .vectors, "ax", %progbits
	RELOC_TEXT_NONE
	W(b)	vector_rst
	W(b)	vector_und
ARM(	.reloc	., R_ARM_LDR_PC_G0, .L__vector_swi		)
THUMB(	.reloc	., R_ARM_THM_PC12, .L__vector_swi		)
	W(ldr)	pc, .
	W(b)	vector_pabt
	W(b)	vector_dabt
	W(b)	vector_addrexcptn
	W(b)	vector_irq
	W(b)	vector_fiq

#ifdef CONFIG_HARDEN_BRANCH_HISTORY
	.section .vectors.bhb.loop8, "ax", %progbits
	RELOC_TEXT_NONE
	W(b)	vector_rst
	W(b)	vector_bhb_loop8_und
ARM(	.reloc	., R_ARM_LDR_PC_G0, .L__vector_bhb_loop8_swi	)
THUMB(	.reloc	., R_ARM_THM_PC12, .L__vector_bhb_loop8_swi	)
	W(ldr)	pc, .
	W(b)	vector_bhb_loop8_pabt
	W(b)	vector_bhb_loop8_dabt
	W(b)	vector_addrexcptn
	W(b)	vector_bhb_loop8_irq
	W(b)	vector_bhb_loop8_fiq

	.section .vectors.bhb.bpiall, "ax", %progbits
	RELOC_TEXT_NONE
	W(b)	vector_rst
	W(b)	vector_bhb_bpiall_und
ARM(	.reloc	., R_ARM_LDR_PC_G0, .L__vector_bhb_bpiall_swi	)
THUMB(	.reloc	., R_ARM_THM_PC12, .L__vector_bhb_bpiall_swi	)
	W(ldr)	pc, .
	W(b)	vector_bhb_bpiall_pabt
	W(b)	vector_bhb_bpiall_dabt
	W(b)	vector_addrexcptn
	W(b)	vector_bhb_bpiall_irq
	W(b)	vector_bhb_bpiall_fiq
#endif

	.data
	.align	2

	.globl	cr_alignment
cr_alignment:
	.space	4
