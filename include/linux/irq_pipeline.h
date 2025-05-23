/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2002 Philippe Gerum  <rpm@xenomai.org>.
 *               2006 Gilles Chanteperdrix.
 *               2007 Jan Kiszka.
 */
#ifndef _LINUX_IRQ_PIPELINE_H
#define _LINUX_IRQ_PIPELINE_H

struct cpuidle_device;
struct cpuidle_state;
struct irq_desc;

#ifdef CONFIG_IRQ_PIPELINE

#include <linux/compiler.h>
#include <linux/irqdomain.h>
#include <linux/percpu.h>
#include <linux/interrupt.h>
#include <linux/irqstage.h>
#include <linux/thread_info.h>
#include <asm/irqflags.h>

void irq_pipeline_init_early(void);

void irq_pipeline_init(void);

void arch_irq_pipeline_init(void);

int generic_pipeline_irq_desc(struct irq_desc *desc);

int irq_inject_pipeline(unsigned int irq);

void synchronize_pipeline(void);

static __always_inline void synchronize_pipeline_on_irq(void)
{
	/*
	 * Optimize if we preempted the high priority oob stage: we
	 * don't need to synchronize the pipeline unless there is a
	 * pending interrupt for it.
	 */
	if (running_inband() ||
	    stage_irqs_pending(this_oob_staged()))
		synchronize_pipeline();
}

bool handle_oob_irq(struct irq_desc *desc);

void arch_do_IRQ_pipelined(struct irq_desc *desc);

#ifdef CONFIG_SMP
void irq_send_oob_ipi(unsigned int ipi,
		const struct cpumask *cpumask);
#endif	/* CONFIG_SMP */

void irq_pipeline_oops(void);

bool irq_pipeline_can_idle(void);

bool irq_cpuidle_enter(struct cpuidle_device *dev,
		       struct cpuidle_state *state);

int run_oob_call(int (*fn)(void *arg), void *arg);

static inline bool inband_irq_pending(void)
{
	check_hard_irqs_disabled();

	return stage_irqs_pending(this_inband_staged());
}

struct irq_stage_data *
handle_irq_pipelined_prepare(struct pt_regs *regs);

int handle_irq_pipelined_finish(struct irq_stage_data *prevd,
				struct pt_regs *regs);

int handle_irq_pipelined(struct pt_regs *regs);

void sync_inband_irqs(void);

void kentry_enter_pipelined(struct pt_regs *regs);

void noinstr kentry_exit_pipelined(struct pt_regs *regs);

static inline void irq_pipeline_idling_checks(void)
{
	if (irq_pipeline_debug()) {
		WARN_ON_ONCE(!raw_irqs_disabled());
		WARN_ON_ONCE(!hard_irqs_disabled());
		WARN_ON_ONCE(stage_irqs_pending(this_inband_staged()));
	}
}

bool irq_cpuidle_control(struct cpuidle_device *dev,
			struct cpuidle_state *state);

extern struct irq_domain *synthetic_irq_domain;

#else /* !CONFIG_IRQ_PIPELINE */

#include <linux/irqstage.h>
#include <linux/hardirq.h>

static inline
void irq_pipeline_init_early(void) { }

static inline
void irq_pipeline_init(void) { }

static inline
void irq_pipeline_oops(void) { }

static inline int
generic_pipeline_irq_desc(struct irq_desc *desc)
{
	return 0;
}

static inline bool handle_oob_irq(struct irq_desc *desc)
{
	return false;
}

static inline bool irq_cpuidle_enter(struct cpuidle_device *dev,
				     struct cpuidle_state *state)
{
	return true;
}

static inline bool inband_irq_pending(void)
{
	return false;
}

static inline void sync_inband_irqs(void) { }

static inline bool irq_pipeline_can_idle(void)
{
	return true;
}

static inline void irq_pipeline_idling_checks(void) { }

static inline int handle_irq_pipelined(struct pt_regs *regs)
{
	return 1;
}

#endif /* !CONFIG_IRQ_PIPELINE */

#endif /* _LINUX_IRQ_PIPELINE_H */
