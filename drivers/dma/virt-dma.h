/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Virtual DMA channel support for DMAengine
 *
 * Copyright (C) 2012 Russell King
 */
#ifndef VIRT_DMA_H
#define VIRT_DMA_H

#include <linux/dmaengine.h>
#include <linux/interrupt.h>

#include "dmaengine.h"

struct virt_dma_desc {
	struct dma_async_tx_descriptor tx;
	struct dmaengine_result tx_result;
	/* protected by vc.lock */
	struct list_head node;
};

struct virt_dma_lockops;

struct virt_dma_chan {
	struct dma_chan	chan;
	struct tasklet_struct task;
	void (*desc_free)(struct virt_dma_desc *);

#ifdef CONFIG_DMA_VIRTUAL_CHANNELS_OOB
	struct virt_dma_lockops *lock_ops;
	union {
		spinlock_t lock;
		hybrid_spinlock_t oob_lock;
	};
#else
	spinlock_t lock;
#endif

	/* protected by vc.lock */
	struct list_head desc_allocated;
	struct list_head desc_submitted;
	struct list_head desc_issued;
	struct list_head desc_completed;
	struct list_head desc_terminated;

	struct virt_dma_desc *cyclic;
};

static inline struct virt_dma_chan *to_virt_chan(struct dma_chan *chan)
{
	return container_of(chan, struct virt_dma_chan, chan);
}

#ifdef CONFIG_DMA_VIRTUAL_CHANNELS_OOB

struct virt_dma_lockops {
	void (*init)(struct virt_dma_chan *vc);
	void (*lock)(struct virt_dma_chan *vc);
	void (*unlock)(struct virt_dma_chan *vc);
	void (*lock_irq)(struct virt_dma_chan *vc);
	void (*unlock_irq)(struct virt_dma_chan *vc);
	unsigned long (*lock_irqsave)(struct virt_dma_chan *vc);
	void (*unlock_irqrestore)(struct virt_dma_chan *vc,
				unsigned long flags);
};

static inline void vchan_lock_init(struct virt_dma_chan *vc)
{
	vc->lock_ops->init(vc);
}

static inline void vchan_lock(struct virt_dma_chan *vc)
{
	vc->lock_ops->lock(vc);
}

static inline void vchan_unlock(struct virt_dma_chan *vc)
{
	vc->lock_ops->unlock(vc);
}

static inline void vchan_lock_irq(struct virt_dma_chan *vc)
{
	vc->lock_ops->lock_irq(vc);
}

static inline void vchan_unlock_irq(struct virt_dma_chan *vc)
{
	vc->lock_ops->unlock_irq(vc);
}

static inline
unsigned long __vchan_lock_irqsave(struct virt_dma_chan *vc)
{
	return vc->lock_ops->lock_irqsave(vc);
}

#define vchan_lock_irqsave(__vc, __flags)		\
	do {						\
		(__flags) = __vchan_lock_irqsave(__vc);	\
	} while (0)

static inline
void vchan_unlock_irqrestore(struct virt_dma_chan *vc,
			unsigned long flags)
{
	vc->lock_ops->unlock_irqrestore(vc, flags);
}

static inline bool vchan_oob_handled(struct virt_dma_desc *vd)
{
	return !!(vd->tx.flags & DMA_OOB_INTERRUPT);
}

static inline bool vchan_oob_pulsed(struct virt_dma_desc *vd)
{
	return !!(vd->tx.flags & DMA_OOB_PULSE);
}

#else

#define vchan_lock_init(__vc)				\
	spin_lock_init(&(__vc)->lock)

#define vchan_lock(__vc)				\
	spin_lock(&(__vc)->lock)

#define vchan_unlock(__vc)				\
	spin_unlock(&(__vc)->lock)

#define vchan_lock_irq(__vc)				\
	spin_lock_irq(&(__vc)->lock)

#define vchan_unlock_irq(__vc)				\
	spin_unlock_irq(&(__vc)->lock)

#define vchan_lock_irqsave(__vc, __flags)		\
	spin_lock_irqsave(&(__vc)->lock, __flags)

#define vchan_unlock_irqrestore(__vc, __flags)		\
	spin_unlock_irqrestore(&(__vc)->lock, __flags)

static inline bool vchan_oob_handled(struct virt_dma_desc *vd)
{
	return false;
}

static inline bool vchan_oob_pulsed(struct virt_dma_desc *vd)
{
	return false;
}

#endif	/* !CONFIG_DMA_VIRTUAL_CHANNELS_OOB */

void vchan_dma_desc_free_list(struct virt_dma_chan *vc, struct list_head *head);
void vchan_init(struct virt_dma_chan *vc, struct dma_device *dmadev);
struct virt_dma_desc *vchan_find_desc(struct virt_dma_chan *, dma_cookie_t);
extern dma_cookie_t vchan_tx_submit(struct dma_async_tx_descriptor *);
extern int vchan_tx_desc_free(struct dma_async_tx_descriptor *);

/**
 * vchan_tx_prep - prepare a descriptor
 * @vc: virtual channel allocating this descriptor
 * @vd: virtual descriptor to prepare
 * @tx_flags: flags argument passed in to prepare function
 */
static inline struct dma_async_tx_descriptor *vchan_tx_prep(struct virt_dma_chan *vc,
	struct virt_dma_desc *vd, unsigned long tx_flags)
{
	unsigned long flags;

	dma_async_tx_descriptor_init(&vd->tx, &vc->chan);
	vd->tx.flags = tx_flags;
	vd->tx.tx_submit = vchan_tx_submit;
	vd->tx.desc_free = vchan_tx_desc_free;

	vd->tx_result.result = DMA_TRANS_NOERROR;
	vd->tx_result.residue = 0;

	vchan_lock_irqsave(vc, flags);
	list_add_tail(&vd->node, &vc->desc_allocated);
	vchan_unlock_irqrestore(vc, flags);

	return &vd->tx;
}

/**
 * vchan_issue_pending - move submitted descriptors to issued list
 * @vc: virtual channel to update
 *
 * vc.lock must be held by caller
 */
static inline bool vchan_issue_pending(struct virt_dma_chan *vc)
{
	lockdep_assert_held(&vc->lock);

	list_splice_tail_init(&vc->desc_submitted, &vc->desc_issued);
	return !list_empty(&vc->desc_issued);
}

/**
 * vchan_cookie_complete - report completion of a descriptor
 * @vd: virtual descriptor to update
 *
 * vc.lock must be held by caller
 *
 * irq_pipeline: calling with hard irqs off is ok as long as we are
 * running in-band from an interrupt context (i.e. in_interrupt()
 * yields true), so that there is no attempt to wake up softirqd until
 * the interrupt frame unwinds.
 */
static inline void vchan_cookie_complete(struct virt_dma_desc *vd)
{
	struct virt_dma_chan *vc = to_virt_chan(vd->tx.chan);
	dma_cookie_t cookie;

	lockdep_assert_held(&vc->lock);

	cookie = vd->tx.cookie;
	dma_cookie_complete(&vd->tx);
	dev_vdbg(vc->chan.device->dev, "txd %p[%x]: marked complete\n",
		 vd, cookie);
	list_add_tail(&vd->node, &vc->desc_completed);

	tasklet_schedule(&vc->task);
}

/**
 * vchan_vdesc_fini - Free or reuse a descriptor
 * @vd: virtual descriptor to free/reuse
 */
static inline void vchan_vdesc_fini(struct virt_dma_desc *vd)
{
	struct virt_dma_chan *vc = to_virt_chan(vd->tx.chan);

	if (dmaengine_desc_test_reuse(&vd->tx)) {
		unsigned long flags;

		vchan_lock_irqsave(vc, flags);
		list_add(&vd->node, &vc->desc_allocated);
		vchan_unlock_irqrestore(vc, flags);
	} else {
		vc->desc_free(vd);
	}
}

/**
 * vchan_cyclic_callback - report the completion of a period
 * @vd: virtual descriptor
 */
static inline void vchan_cyclic_callback(struct virt_dma_desc *vd)
{
	struct virt_dma_chan *vc = to_virt_chan(vd->tx.chan);

	vc->cyclic = vd;
	tasklet_schedule(&vc->task);
}

/**
 * vchan_terminate_vdesc - Disable pending cyclic callback
 * @vd: virtual descriptor to be terminated
 *
 * vc.lock must be held by caller
 */
static inline void vchan_terminate_vdesc(struct virt_dma_desc *vd)
{
	struct virt_dma_chan *vc = to_virt_chan(vd->tx.chan);

	lockdep_assert_held(&vc->lock);

	list_add_tail(&vd->node, &vc->desc_terminated);

	if (vc->cyclic == vd)
		vc->cyclic = NULL;
}

/**
 * vchan_next_desc - peek at the next descriptor to be processed
 * @vc: virtual channel to obtain descriptor from
 *
 * vc.lock must be held by caller
 */
static inline struct virt_dma_desc *vchan_next_desc(struct virt_dma_chan *vc)
{
	lockdep_assert_held(&vc->lock);

	return list_first_entry_or_null(&vc->desc_issued,
					struct virt_dma_desc, node);
}

/**
 * vchan_get_all_descriptors - obtain all submitted and issued descriptors
 * @vc: virtual channel to get descriptors from
 * @head: list of descriptors found
 *
 * vc.lock must be held by caller
 *
 * Removes all submitted and issued descriptors from internal lists, and
 * provides a list of all descriptors found
 */
static inline void vchan_get_all_descriptors(struct virt_dma_chan *vc,
	struct list_head *head)
{
	lockdep_assert_held(&vc->lock);

	list_splice_tail_init(&vc->desc_allocated, head);
	list_splice_tail_init(&vc->desc_submitted, head);
	list_splice_tail_init(&vc->desc_issued, head);
	list_splice_tail_init(&vc->desc_completed, head);
	list_splice_tail_init(&vc->desc_terminated, head);
}

static inline void vchan_free_chan_resources(struct virt_dma_chan *vc)
{
	struct virt_dma_desc *vd;
	unsigned long flags;
	LIST_HEAD(head);

	vchan_lock_irqsave(vc, flags);
	vchan_get_all_descriptors(vc, &head);
	list_for_each_entry(vd, &head, node)
		dmaengine_desc_clear_reuse(&vd->tx);
	vchan_unlock_irqrestore(vc, flags);

	vchan_dma_desc_free_list(vc, &head);
}

/**
 * vchan_synchronize() - synchronize callback execution to the current context
 * @vc: virtual channel to synchronize
 *
 * Makes sure that all scheduled or active callbacks have finished running. For
 * proper operation the caller has to ensure that no new callbacks are scheduled
 * after the invocation of this function started.
 * Free up the terminated cyclic descriptor to prevent memory leakage.
 */
static inline void vchan_synchronize(struct virt_dma_chan *vc)
{
	LIST_HEAD(head);
	unsigned long flags;

	tasklet_kill(&vc->task);

	vchan_lock_irqsave(vc, flags);

	list_splice_tail_init(&vc->desc_terminated, &head);

	vchan_unlock_irqrestore(vc, flags);

	vchan_dma_desc_free_list(vc, &head);
}

#endif
