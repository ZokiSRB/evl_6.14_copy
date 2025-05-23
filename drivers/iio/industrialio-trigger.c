// SPDX-License-Identifier: GPL-2.0-only
/* The industrial I/O core, trigger handling functions
 *
 * Copyright (c) 2008 Jonathan Cameron
 */

#include <linux/cleanup.h>
#include <linux/kernel.h>
#include <linux/idr.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/slab.h>

#include <linux/iio/iio.h>
#include <linux/iio/iio-opaque.h>
#include <linux/iio/trigger.h>
#include "iio_core.h"
#include "iio_core_trigger.h"
#include <linux/iio/trigger_consumer.h>

/* RFC - Question of approach
 * Make the common case (single sensor single trigger)
 * simple by starting trigger capture from when first sensors
 * is added.
 *
 * Complex simultaneous start requires use of 'hold' functionality
 * of the trigger. (not implemented)
 *
 * Any other suggestions?
 */

static DEFINE_IDA(iio_trigger_ida);

/* Single list of all available triggers */
static LIST_HEAD(iio_trigger_list);
static DEFINE_MUTEX(iio_trigger_list_lock);

/**
 * name_show() - retrieve useful identifying name
 * @dev:	device associated with the iio_trigger
 * @attr:	pointer to the device_attribute structure that is
 *		being processed
 * @buf:	buffer to print the name into
 *
 * Return: a negative number on failure or the number of written
 *	   characters on success.
 */
static ssize_t name_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct iio_trigger *trig = to_iio_trigger(dev);

	return sysfs_emit(buf, "%s\n", trig->name);
}

static DEVICE_ATTR_RO(name);

static struct attribute *iio_trig_dev_attrs[] = {
	&dev_attr_name.attr,
	NULL,
};
ATTRIBUTE_GROUPS(iio_trig_dev);

static struct iio_trigger *__iio_trigger_find_by_name(const char *name);

int iio_trigger_register(struct iio_trigger *trig_info)
{
	int ret;

	trig_info->id = ida_alloc(&iio_trigger_ida, GFP_KERNEL);
	if (trig_info->id < 0)
		return trig_info->id;

	/* Set the name used for the sysfs directory etc */
	dev_set_name(&trig_info->dev, "trigger%d", trig_info->id);

	ret = device_add(&trig_info->dev);
	if (ret)
		goto error_unregister_id;

	/* Add to list of available triggers held by the IIO core */
	scoped_guard(mutex, &iio_trigger_list_lock) {
		if (__iio_trigger_find_by_name(trig_info->name)) {
			pr_err("Duplicate trigger name '%s'\n", trig_info->name);
			ret = -EEXIST;
			goto error_device_del;
		}
		list_add_tail(&trig_info->list, &iio_trigger_list);
	}

	return 0;

error_device_del:
	device_del(&trig_info->dev);
error_unregister_id:
	ida_free(&iio_trigger_ida, trig_info->id);
	return ret;
}
EXPORT_SYMBOL(iio_trigger_register);

void iio_trigger_unregister(struct iio_trigger *trig_info)
{
	scoped_guard(mutex, &iio_trigger_list_lock)
		list_del(&trig_info->list);

	ida_free(&iio_trigger_ida, trig_info->id);
	/* Possible issue in here */
	device_del(&trig_info->dev);
}
EXPORT_SYMBOL(iio_trigger_unregister);

int iio_trigger_set_immutable(struct iio_dev *indio_dev, struct iio_trigger *trig)
{
	struct iio_dev_opaque *iio_dev_opaque;

	if (!indio_dev || !trig)
		return -EINVAL;

	iio_dev_opaque = to_iio_dev_opaque(indio_dev);
	guard(mutex)(&iio_dev_opaque->mlock);
	WARN_ON(iio_dev_opaque->trig_readonly);

	indio_dev->trig = iio_trigger_get(trig);
	iio_dev_opaque->trig_readonly = true;

	return 0;
}
EXPORT_SYMBOL(iio_trigger_set_immutable);

/* Search for trigger by name, assuming iio_trigger_list_lock held */
static struct iio_trigger *__iio_trigger_find_by_name(const char *name)
{
	struct iio_trigger *iter;

	list_for_each_entry(iter, &iio_trigger_list, list)
		if (!strcmp(iter->name, name))
			return iter;

	return NULL;
}

static struct iio_trigger *iio_trigger_acquire_by_name(const char *name)
{
	struct iio_trigger *iter;

	guard(mutex)(&iio_trigger_list_lock);
	list_for_each_entry(iter, &iio_trigger_list, list)
		if (sysfs_streq(iter->name, name))
			return iio_trigger_get(iter);

	return NULL;
}

static void iio_reenable_work_fn(struct work_struct *work)
{
	struct iio_trigger *trig = container_of(work, struct iio_trigger,
						reenable_work);

	/*
	 * This 'might' occur after the trigger state is set to disabled -
	 * in that case the driver should skip reenabling.
	 */
	trig->ops->reenable(trig);
}

/*
 * In general, reenable callbacks may need to sleep and this path is
 * not performance sensitive, so just queue up a work item
 * to reneable the trigger for us.
 *
 * Races that can cause this.
 * 1) A handler occurs entirely in interrupt context so the counter
 *    the final decrement is still in this interrupt.
 * 2) The trigger has been removed, but one last interrupt gets through.
 *
 * For (1) we must call reenable, but not in atomic context.
 * For (2) it should be safe to call reenanble, if drivers never blindly
 * reenable after state is off.
 */
static void iio_trigger_notify_done_atomic(struct iio_trigger *trig)
{
	if (atomic_dec_and_test(&trig->use_count) && trig->ops &&
	    trig->ops->reenable)
		schedule_work(&trig->reenable_work);
}

/**
 * iio_trigger_poll() - Call the IRQ trigger handler of the consumers
 * @trig: trigger which occurred
 *
 * This function should only be called from a hard IRQ context.
 */
void iio_trigger_poll(struct iio_trigger *trig)
{
	int i;

	if (!atomic_read(&trig->use_count)) {
		atomic_set(&trig->use_count, CONFIG_IIO_CONSUMERS_PER_TRIGGER);

		for (i = 0; i < CONFIG_IIO_CONSUMERS_PER_TRIGGER; i++) {
			if (trig->subirqs[i].enabled)
				generic_handle_irq(trig->subirq_base + i);
			else
				iio_trigger_notify_done_atomic(trig);
		}
	}
}
EXPORT_SYMBOL(iio_trigger_poll);

irqreturn_t iio_trigger_generic_data_rdy_poll(int irq, void *private)
{
	iio_trigger_poll(private);
	return IRQ_HANDLED;
}
EXPORT_SYMBOL(iio_trigger_generic_data_rdy_poll);

/**
 * iio_trigger_poll_nested() - Call the threaded trigger handler of the
 * consumers
 * @trig: trigger which occurred
 *
 * This function should only be called from a kernel thread context.
 */
void iio_trigger_poll_nested(struct iio_trigger *trig)
{
	int i;

	if (!atomic_read(&trig->use_count)) {
		atomic_set(&trig->use_count, CONFIG_IIO_CONSUMERS_PER_TRIGGER);

		for (i = 0; i < CONFIG_IIO_CONSUMERS_PER_TRIGGER; i++) {
			if (trig->subirqs[i].enabled)
				handle_nested_irq(trig->subirq_base + i);
			else
				iio_trigger_notify_done(trig);
		}
	}
}
EXPORT_SYMBOL(iio_trigger_poll_nested);

void iio_trigger_notify_done(struct iio_trigger *trig)
{
	if (atomic_dec_and_test(&trig->use_count) && trig->ops &&
	    trig->ops->reenable)
		trig->ops->reenable(trig);
}
EXPORT_SYMBOL(iio_trigger_notify_done);

/* Trigger Consumer related functions */
static int iio_trigger_get_irq(struct iio_trigger *trig)
{
	int ret;

	scoped_guard(mutex, &trig->pool_lock) {
		ret = bitmap_find_free_region(trig->pool,
					      CONFIG_IIO_CONSUMERS_PER_TRIGGER,
					      ilog2(1));
		if (ret < 0)
			return ret;
	}

	return ret + trig->subirq_base;
}

static void iio_trigger_put_irq(struct iio_trigger *trig, int irq)
{
	guard(mutex)(&trig->pool_lock);
	clear_bit(irq - trig->subirq_base, trig->pool);
}

/* Complexity in here.  With certain triggers (datardy) an acknowledgement
 * may be needed if the pollfuncs do not include the data read for the
 * triggering device.
 * This is not currently handled.  Alternative of not enabling trigger unless
 * the relevant function is in there may be the best option.
 */
/* Worth protecting against double additions? */
int iio_trigger_attach_poll_func(struct iio_trigger *trig,
				 struct iio_poll_func *pf)
{
	struct iio_dev_opaque *iio_dev_opaque = to_iio_dev_opaque(pf->indio_dev);
	bool notinuse =
		bitmap_empty(trig->pool, CONFIG_IIO_CONSUMERS_PER_TRIGGER);
	int ret = 0;

	/* Prevent the module from being removed whilst attached to a trigger */
	__module_get(iio_dev_opaque->driver_module);

	/* Get irq number */
	pf->irq = iio_trigger_get_irq(trig);
	if (pf->irq < 0) {
		pr_err("Could not find an available irq for trigger %s, CONFIG_IIO_CONSUMERS_PER_TRIGGER=%d limit might be exceeded\n",
			trig->name, CONFIG_IIO_CONSUMERS_PER_TRIGGER);
		goto out_put_module;
	}

	/* Request irq */
	ret = request_threaded_irq(pf->irq, pf->h, pf->thread,
				   pf->type, pf->name,
				   pf);
	if (ret < 0)
		goto out_put_irq;

	/* Enable trigger in driver */
	if (trig->ops && trig->ops->set_trigger_state && notinuse) {
		ret = trig->ops->set_trigger_state(trig, true);
		if (ret)
			goto out_free_irq;
	}

	/*
	 * Check if we just registered to our own trigger: we determine that
	 * this is the case if the IIO device and the trigger device share the
	 * same parent device.
	 */
	if (!iio_validate_own_trigger(pf->indio_dev, trig))
		trig->attached_own_device = true;

	return ret;

out_free_irq:
	free_irq(pf->irq, pf);
out_put_irq:
	iio_trigger_put_irq(trig, pf->irq);
out_put_module:
	module_put(iio_dev_opaque->driver_module);
	return ret;
}

int iio_trigger_detach_poll_func(struct iio_trigger *trig,
				 struct iio_poll_func *pf)
{
	struct iio_dev_opaque *iio_dev_opaque = to_iio_dev_opaque(pf->indio_dev);
	bool no_other_users =
		bitmap_weight(trig->pool, CONFIG_IIO_CONSUMERS_PER_TRIGGER) == 1;
	int ret = 0;

	if (trig->ops && trig->ops->set_trigger_state && no_other_users) {
		ret = trig->ops->set_trigger_state(trig, false);
		if (ret)
			return ret;
	}
	if (pf->indio_dev->dev.parent == trig->dev.parent)
		trig->attached_own_device = false;
	iio_trigger_put_irq(trig, pf->irq);
	free_irq(pf->irq, pf);
	module_put(iio_dev_opaque->driver_module);
	pf->irq = 0;

	return ret;
}

irqreturn_t iio_pollfunc_store_time(int irq, void *p)
{
	struct iio_poll_func *pf = p;

	pf->timestamp = iio_get_time_ns(pf->indio_dev);
	return IRQ_WAKE_THREAD;
}
EXPORT_SYMBOL(iio_pollfunc_store_time);

struct iio_poll_func
*iio_alloc_pollfunc(irqreturn_t (*h)(int irq, void *p),
		    irqreturn_t (*thread)(int irq, void *p),
		    int type,
		    struct iio_dev *indio_dev,
		    const char *fmt,
		    ...)
{
	va_list vargs;
	struct iio_poll_func *pf;

	pf = kmalloc(sizeof(*pf), GFP_KERNEL);
	if (!pf)
		return NULL;
	va_start(vargs, fmt);
	pf->name = kvasprintf(GFP_KERNEL, fmt, vargs);
	va_end(vargs);
	if (pf->name == NULL) {
		kfree(pf);
		return NULL;
	}
	pf->h = h;
	pf->thread = thread;
	pf->type = type;
	pf->indio_dev = indio_dev;

	return pf;
}
EXPORT_SYMBOL_GPL(iio_alloc_pollfunc);

void iio_dealloc_pollfunc(struct iio_poll_func *pf)
{
	kfree(pf->name);
	kfree(pf);
}
EXPORT_SYMBOL_GPL(iio_dealloc_pollfunc);

/**
 * current_trigger_show() - trigger consumer sysfs query current trigger
 * @dev:	device associated with an industrial I/O device
 * @attr:	pointer to the device_attribute structure that
 *		is being processed
 * @buf:	buffer where the current trigger name will be printed into
 *
 * For trigger consumers the current_trigger interface allows the trigger
 * used by the device to be queried.
 *
 * Return: a negative number on failure, the number of characters written
 *	   on success or 0 if no trigger is available
 */
static ssize_t current_trigger_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);

	if (indio_dev->trig)
		return sysfs_emit(buf, "%s\n", indio_dev->trig->name);
	return 0;
}

/**
 * current_trigger_store() - trigger consumer sysfs set current trigger
 * @dev:	device associated with an industrial I/O device
 * @attr:	device attribute that is being processed
 * @buf:	string buffer that holds the name of the trigger
 * @len:	length of the trigger name held by buf
 *
 * For trigger consumers the current_trigger interface allows the trigger
 * used for this device to be specified at run time based on the trigger's
 * name.
 *
 * Return: negative error code on failure or length of the buffer
 *	   on success
 */
static ssize_t current_trigger_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct iio_dev_opaque *iio_dev_opaque = to_iio_dev_opaque(indio_dev);
	struct iio_trigger *oldtrig = indio_dev->trig;
	struct iio_trigger *trig;
	int ret;

	scoped_guard(mutex, &iio_dev_opaque->mlock) {
		if (iio_dev_opaque->currentmode == INDIO_BUFFER_TRIGGERED)
			return -EBUSY;
		if (iio_dev_opaque->trig_readonly)
			return -EPERM;
	}

	trig = iio_trigger_acquire_by_name(buf);
	if (oldtrig == trig) {
		ret = len;
		goto out_trigger_put;
	}

	if (trig && indio_dev->info->validate_trigger) {
		ret = indio_dev->info->validate_trigger(indio_dev, trig);
		if (ret)
			goto out_trigger_put;
	}

	if (trig && trig->ops && trig->ops->validate_device) {
		ret = trig->ops->validate_device(trig, indio_dev);
		if (ret)
			goto out_trigger_put;
	}

	indio_dev->trig = trig;

	if (oldtrig) {
		if (indio_dev->modes & INDIO_EVENT_TRIGGERED)
			iio_trigger_detach_poll_func(oldtrig,
						     indio_dev->pollfunc_event);
		iio_trigger_put(oldtrig);
	}
	if (indio_dev->trig) {
		if (indio_dev->modes & INDIO_EVENT_TRIGGERED)
			iio_trigger_attach_poll_func(indio_dev->trig,
						     indio_dev->pollfunc_event);
	}

	return len;

out_trigger_put:
	if (trig)
		iio_trigger_put(trig);
	return ret;
}

static DEVICE_ATTR_RW(current_trigger);

static struct attribute *iio_trigger_consumer_attrs[] = {
	&dev_attr_current_trigger.attr,
	NULL,
};

static const struct attribute_group iio_trigger_consumer_attr_group = {
	.name = "trigger",
	.attrs = iio_trigger_consumer_attrs,
};

static void iio_trig_release(struct device *device)
{
	struct iio_trigger *trig = to_iio_trigger(device);
	int i;

	if (trig->subirq_base) {
		for (i = 0; i < CONFIG_IIO_CONSUMERS_PER_TRIGGER; i++) {
			irq_modify_status(trig->subirq_base + i,
					  IRQ_NOAUTOEN,
					  IRQ_NOREQUEST | IRQ_NOPROBE);
			irq_set_chip(trig->subirq_base + i,
				     NULL);
			irq_set_handler(trig->subirq_base + i,
					NULL);
		}

		irq_free_descs(trig->subirq_base,
			       CONFIG_IIO_CONSUMERS_PER_TRIGGER);
	}
	kfree(trig->name);
	kfree(trig);
}

static const struct device_type iio_trig_type = {
	.release = iio_trig_release,
	.groups = iio_trig_dev_groups,
};

static void iio_trig_subirqmask(struct irq_data *d)
{
	struct irq_chip *chip = irq_data_get_irq_chip(d);
	struct iio_trigger *trig = container_of(chip, struct iio_trigger, subirq_chip);

	trig->subirqs[d->irq - trig->subirq_base].enabled = false;
}

static void iio_trig_subirqunmask(struct irq_data *d)
{
	struct irq_chip *chip = irq_data_get_irq_chip(d);
	struct iio_trigger *trig = container_of(chip, struct iio_trigger, subirq_chip);

	trig->subirqs[d->irq - trig->subirq_base].enabled = true;
}

static __printf(3, 0)
struct iio_trigger *viio_trigger_alloc(struct device *parent,
				       struct module *this_mod,
				       const char *fmt,
				       va_list vargs)
{
	struct iio_trigger *trig;
	int i;

	trig = kzalloc(sizeof(*trig), GFP_KERNEL);
	if (!trig)
		return NULL;

	trig->dev.parent = parent;
	trig->dev.type = &iio_trig_type;
	trig->dev.bus = &iio_bus_type;
	device_initialize(&trig->dev);
	INIT_WORK(&trig->reenable_work, iio_reenable_work_fn);

	mutex_init(&trig->pool_lock);
	trig->subirq_base = irq_alloc_descs(-1, 0,
					    CONFIG_IIO_CONSUMERS_PER_TRIGGER,
					    0);
	if (trig->subirq_base < 0)
		goto free_trig;

	trig->name = kvasprintf(GFP_KERNEL, fmt, vargs);
	if (trig->name == NULL)
		goto free_descs;

	INIT_LIST_HEAD(&trig->list);

	trig->owner = this_mod;

	trig->subirq_chip.name = trig->name;
	trig->subirq_chip.irq_mask = &iio_trig_subirqmask;
	trig->subirq_chip.irq_unmask = &iio_trig_subirqunmask;
	trig->subirq_chip.flags = IRQCHIP_PIPELINE_SAFE;
	for (i = 0; i < CONFIG_IIO_CONSUMERS_PER_TRIGGER; i++) {
		irq_set_chip(trig->subirq_base + i, &trig->subirq_chip);
		irq_set_handler(trig->subirq_base + i, &handle_simple_irq);
		irq_modify_status(trig->subirq_base + i,
				  IRQ_NOREQUEST | IRQ_NOAUTOEN, IRQ_NOPROBE);
	}

	return trig;

free_descs:
	irq_free_descs(trig->subirq_base, CONFIG_IIO_CONSUMERS_PER_TRIGGER);
free_trig:
	kfree(trig);
	return NULL;
}

/**
 * __iio_trigger_alloc - Allocate a trigger
 * @parent:		Device to allocate iio_trigger for
 * @this_mod:		module allocating the trigger
 * @fmt:		trigger name format. If it includes format
 *			specifiers, the additional arguments following
 *			format are formatted and inserted in the resulting
 *			string replacing their respective specifiers.
 * RETURNS:
 * Pointer to allocated iio_trigger on success, NULL on failure.
 */
struct iio_trigger *__iio_trigger_alloc(struct device *parent,
					struct module *this_mod,
					const char *fmt, ...)
{
	struct iio_trigger *trig;
	va_list vargs;

	va_start(vargs, fmt);
	trig = viio_trigger_alloc(parent, this_mod, fmt, vargs);
	va_end(vargs);

	return trig;
}
EXPORT_SYMBOL(__iio_trigger_alloc);

void iio_trigger_free(struct iio_trigger *trig)
{
	if (trig)
		put_device(&trig->dev);
}
EXPORT_SYMBOL(iio_trigger_free);

static void devm_iio_trigger_release(struct device *dev, void *res)
{
	iio_trigger_free(*(struct iio_trigger **)res);
}

/**
 * __devm_iio_trigger_alloc - Resource-managed iio_trigger_alloc()
 * Managed iio_trigger_alloc.  iio_trigger allocated with this function is
 * automatically freed on driver detach.
 * @parent:		Device to allocate iio_trigger for
 * @this_mod:		module allocating the trigger
 * @fmt:		trigger name format. If it includes format
 *			specifiers, the additional arguments following
 *			format are formatted and inserted in the resulting
 *			string replacing their respective specifiers.
 *
 *
 * RETURNS:
 * Pointer to allocated iio_trigger on success, NULL on failure.
 */
struct iio_trigger *__devm_iio_trigger_alloc(struct device *parent,
					     struct module *this_mod,
					     const char *fmt, ...)
{
	struct iio_trigger **ptr, *trig;
	va_list vargs;

	ptr = devres_alloc(devm_iio_trigger_release, sizeof(*ptr),
			   GFP_KERNEL);
	if (!ptr)
		return NULL;

	/* use raw alloc_dr for kmalloc caller tracing */
	va_start(vargs, fmt);
	trig = viio_trigger_alloc(parent, this_mod, fmt, vargs);
	va_end(vargs);
	if (trig) {
		*ptr = trig;
		devres_add(parent, ptr);
	} else {
		devres_free(ptr);
	}

	return trig;
}
EXPORT_SYMBOL_GPL(__devm_iio_trigger_alloc);

static void devm_iio_trigger_unreg(void *trigger_info)
{
	iio_trigger_unregister(trigger_info);
}

/**
 * devm_iio_trigger_register - Resource-managed iio_trigger_register()
 * @dev:	device this trigger was allocated for
 * @trig_info:	trigger to register
 *
 * Managed iio_trigger_register().  The IIO trigger registered with this
 * function is automatically unregistered on driver detach. This function
 * calls iio_trigger_register() internally. Refer to that function for more
 * information.
 *
 * RETURNS:
 * 0 on success, negative error number on failure.
 */
int devm_iio_trigger_register(struct device *dev,
			      struct iio_trigger *trig_info)
{
	int ret;

	ret = iio_trigger_register(trig_info);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, devm_iio_trigger_unreg, trig_info);
}
EXPORT_SYMBOL_GPL(devm_iio_trigger_register);

bool iio_trigger_using_own(struct iio_dev *indio_dev)
{
	return indio_dev->trig->attached_own_device;
}
EXPORT_SYMBOL(iio_trigger_using_own);

/**
 * iio_validate_own_trigger - Check if a trigger and IIO device belong to
 *  the same device
 * @idev: the IIO device to check
 * @trig: the IIO trigger to check
 *
 * This function can be used as the validate_trigger callback for triggers that
 * can only be attached to their own device.
 *
 * Return: 0 if both the trigger and the IIO device belong to the same
 * device, -EINVAL otherwise.
 */
int iio_validate_own_trigger(struct iio_dev *idev, struct iio_trigger *trig)
{
	if (idev->dev.parent != trig->dev.parent)
		return -EINVAL;
	return 0;
}
EXPORT_SYMBOL_GPL(iio_validate_own_trigger);

/**
 * iio_trigger_validate_own_device - Check if a trigger and IIO device belong to
 *  the same device
 * @trig: The IIO trigger to check
 * @indio_dev: the IIO device to check
 *
 * This function can be used as the validate_device callback for triggers that
 * can only be attached to their own device.
 *
 * Return: 0 if both the trigger and the IIO device belong to the same
 * device, -EINVAL otherwise.
 */
int iio_trigger_validate_own_device(struct iio_trigger *trig,
				    struct iio_dev *indio_dev)
{
	if (indio_dev->dev.parent != trig->dev.parent)
		return -EINVAL;
	return 0;
}
EXPORT_SYMBOL(iio_trigger_validate_own_device);

int iio_device_register_trigger_consumer(struct iio_dev *indio_dev)
{
	return iio_device_register_sysfs_group(indio_dev,
					       &iio_trigger_consumer_attr_group);
}

void iio_device_unregister_trigger_consumer(struct iio_dev *indio_dev)
{
	/* Clean up an associated but not attached trigger reference */
	if (indio_dev->trig)
		iio_trigger_put(indio_dev->trig);
}

int iio_device_suspend_triggering(struct iio_dev *indio_dev)
{
	struct iio_dev_opaque *iio_dev_opaque = to_iio_dev_opaque(indio_dev);

	guard(mutex)(&iio_dev_opaque->mlock);

	if ((indio_dev->pollfunc) && (indio_dev->pollfunc->irq > 0))
		disable_irq(indio_dev->pollfunc->irq);

	return 0;
}
EXPORT_SYMBOL(iio_device_suspend_triggering);

int iio_device_resume_triggering(struct iio_dev *indio_dev)
{
	struct iio_dev_opaque *iio_dev_opaque = to_iio_dev_opaque(indio_dev);

	guard(mutex)(&iio_dev_opaque->mlock);

	if ((indio_dev->pollfunc) && (indio_dev->pollfunc->irq > 0))
		enable_irq(indio_dev->pollfunc->irq);

	return 0;
}
EXPORT_SYMBOL(iio_device_resume_triggering);
