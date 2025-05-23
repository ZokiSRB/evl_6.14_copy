/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  arch/arm/include/asm/thread_info.h
 *
 *  Copyright (C) 2002 Russell King.
 */
#ifndef __ASM_ARM_THREAD_INFO_H
#define __ASM_ARM_THREAD_INFO_H

#ifdef __KERNEL__

#include <linux/compiler.h>
#include <asm/fpstate.h>
#include <asm/page.h>

#ifdef CONFIG_KASAN
/*
 * KASan uses a lot of extra stack space so the thread size order needs to
 * be increased.
 */
#define THREAD_SIZE_ORDER	2
#else
#define THREAD_SIZE_ORDER	1
#endif
#define THREAD_SIZE		(PAGE_SIZE << THREAD_SIZE_ORDER)
#define THREAD_START_SP		(THREAD_SIZE - 8)

#ifdef CONFIG_VMAP_STACK
#define THREAD_ALIGN		(2 * THREAD_SIZE)
#else
#define THREAD_ALIGN		THREAD_SIZE
#endif

#define OVERFLOW_STACK_SIZE	SZ_4K

#ifndef __ASSEMBLY__

struct task_struct;

DECLARE_PER_CPU(struct task_struct *, __entry_task);

#include <dovetail/thread_info.h>
#include <asm/types.h>
#include <asm/traps.h>

struct cpu_context_save {
	__u32	r4;
	__u32	r5;
	__u32	r6;
	__u32	r7;
	__u32	r8;
	__u32	r9;
	__u32	sl;
	__u32	fp;
	__u32	sp;
	__u32	pc;
	__u32	extra[2];		/* Xscale 'acc' register, etc */
};

/*
 * low level task data that entry.S needs immediate access to.
 * __switch_to() assumes cpu_context follows immediately after cpu_domain.
 */
struct thread_info {
	unsigned long		flags;		/* low level flags */
	__u32			local_flags;	/* local (synchronous) flags */
	int			preempt_count;	/* 0 => preemptable, <0 => bug */
	__u32			cpu;		/* cpu */
	__u32			cpu_domain;	/* cpu domain */
	struct cpu_context_save	cpu_context;	/* cpu context */
	__u32			abi_syscall;	/* ABI type and syscall nr */
	unsigned long		tp_value[2];	/* TLS registers */
	union fp_state		fpstate __attribute__((aligned(8)));
	union vfp_state		vfpstate;
#ifdef CONFIG_ARM_THUMBEE
	unsigned long		thumbee_state;	/* ThumbEE Handler Base register */
#endif
	struct oob_thread_state	oob_state; /* co-kernel thread state */
};

#define INIT_THREAD_INFO(tsk)						\
{									\
	.flags		= 0,						\
	.local_flags	= 0,						\
	.preempt_count	= INIT_PREEMPT_COUNT,				\
}

static inline struct task_struct *thread_task(struct thread_info* ti)
{
	return (struct task_struct *)ti;
}

#define ti_local_flags(__ti)	((__ti)->local_flags)

#define thread_saved_pc(tsk)						\
	((unsigned long)(task_thread_info(tsk)->cpu_context.pc))
#define thread_saved_sp(tsk)	\
	((unsigned long)(task_thread_info(tsk)->cpu_context.sp))

#ifndef CONFIG_THUMB2_KERNEL
#define thread_saved_fp(tsk)	\
	((unsigned long)(task_thread_info(tsk)->cpu_context.fp))
#else
#define thread_saved_fp(tsk)	\
	((unsigned long)(task_thread_info(tsk)->cpu_context.r7))
#endif

extern void iwmmxt_task_disable(struct thread_info *);
extern void iwmmxt_task_copy(struct thread_info *, void *);
extern void iwmmxt_task_restore(struct thread_info *, void *);
extern void iwmmxt_task_release(struct thread_info *);
extern void iwmmxt_task_switch(struct thread_info *);

extern int iwmmxt_undef_handler(struct pt_regs *, u32);

static inline void register_iwmmxt_undef_handler(void)
{
	static struct undef_hook iwmmxt_undef_hook = {
		.instr_mask	= 0x0c000e00,
		.instr_val	= 0x0c000000,
		.cpsr_mask	= MODE_MASK | PSR_T_BIT,
		.cpsr_val	= USR_MODE,
		.fn		= iwmmxt_undef_handler,
	};

	register_undef_hook(&iwmmxt_undef_hook);
}

extern void vfp_sync_hwstate(struct thread_info *);
extern void vfp_flush_hwstate(struct thread_info *);

struct user_vfp;
struct user_vfp_exc;

extern int vfp_preserve_user_clear_hwstate(struct user_vfp *,
					   struct user_vfp_exc *);
extern int vfp_restore_user_hwstate(struct user_vfp *,
				    struct user_vfp_exc *);
#endif

/*
 * thread information flags:
 *  TIF_USEDFPU		- FPU was used by this task this quantum (SMP)
 *  TIF_POLLING_NRFLAG	- true if poll_idle() is polling TIF_NEED_RESCHED
 *
 * Any bit in the range of 0..15 will cause do_work_pending() to be invoked.
 */
#define TIF_SIGPENDING		0	/* signal pending */
#define TIF_NEED_RESCHED	1	/* rescheduling necessary */
#define TIF_NOTIFY_RESUME	2	/* callback before returning to user */
#define TIF_UPROBE		3	/* breakpointed or singlestepping */
#define TIF_NOTIFY_SIGNAL	4	/* signal notifications exist */
#define TIF_RETUSER		5	/* INBAND_TASK_RETUSER is pending */

#define TIF_USING_IWMMXT	17
#define TIF_MEMDIE		18	/* is terminating due to OOM killer */
#define TIF_RESTORE_SIGMASK	19
#define TIF_SYSCALL_TRACE	20	/* syscall trace active */
#define TIF_SYSCALL_AUDIT	21	/* syscall auditing active */
#define TIF_SYSCALL_TRACEPOINT	22	/* syscall tracepoint instrumentation */
#define TIF_SECCOMP		23	/* seccomp syscall filtering active */

#define TIF_MAYDAY		24	/* emergency trap pending */

#define _TIF_SIGPENDING		(1 << TIF_SIGPENDING)
#define _TIF_NEED_RESCHED	(1 << TIF_NEED_RESCHED)
#define _TIF_NOTIFY_RESUME	(1 << TIF_NOTIFY_RESUME)
#define _TIF_UPROBE		(1 << TIF_UPROBE)
#define _TIF_SYSCALL_TRACE	(1 << TIF_SYSCALL_TRACE)
#define _TIF_SYSCALL_AUDIT	(1 << TIF_SYSCALL_AUDIT)
#define _TIF_SYSCALL_TRACEPOINT	(1 << TIF_SYSCALL_TRACEPOINT)
#define _TIF_SECCOMP		(1 << TIF_SECCOMP)
#define _TIF_NOTIFY_SIGNAL	(1 << TIF_NOTIFY_SIGNAL)
#define _TIF_RETUSER		(1 << TIF_RETUSER)
#define _TIF_USING_IWMMXT	(1 << TIF_USING_IWMMXT)
#define _TIF_MAYDAY		(1 << TIF_MAYDAY)

/*
 * Checks for any syscall work in entry-common.S.
 * CAUTION: Only bit0-bit15 are tested there.
 */
#define _TIF_SYSCALL_WORK (_TIF_SYSCALL_TRACE | _TIF_SYSCALL_AUDIT | \
			   _TIF_SYSCALL_TRACEPOINT | _TIF_SECCOMP)

/*
 * Change these and you break ASM code in entry-common.S
 */
#define _TIF_WORK_MASK		(_TIF_NEED_RESCHED | _TIF_SIGPENDING | \
				 _TIF_NOTIFY_RESUME | _TIF_UPROBE | \
				 _TIF_NOTIFY_SIGNAL | _TIF_RETUSER)

/*
 * Local (synchronous) thread flags.
 */
#define _TLF_OOB		0x0001
#define _TLF_DOVETAIL		0x0002
#define _TLF_OFFSTAGE		0x0004
#define _TLF_OOBTRAP		0x0008

#endif /* __KERNEL__ */
#endif /* __ASM_ARM_THREAD_INFO_H */
