/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * unart - definitely not a real UART
 * Copyright (c) 2025 Dominic Sacré <dominic@sacre.net>
 */
#ifndef _DSACRE_UNART_UTIL_H
#define _DSACRE_UNART_UTIL_H

#include <linux/device.h>
#include <linux/hrtimer.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/version.h>

/*
 * Scope-based wrapper around raw_spin_lock_irqsave().
 */
struct raw_spinlock_irqsave_scope {
	raw_spinlock_t *lock;
	unsigned long flags;
};

static inline void raw_spin_unlock_irqrestore_scoped(
		struct raw_spinlock_irqsave_scope *scope)
{
	raw_spin_unlock_irqrestore(scope->lock, scope->flags);
}

#define raw_spin_lock_irqsave_scoped(_lock) \
	__attribute__((__cleanup__(raw_spin_unlock_irqrestore_scoped))) \
		struct raw_spinlock_irqsave_scope __scope = { .lock = (_lock) }; \
	raw_spin_lock_irqsave((_lock), __scope.flags)


/*
 * Devres wrapper around kfifo_alloc().
 */
static void devm_kfifo_free(void *data)
{
	kfifo_free((struct kfifo *)data);
}

static inline int devm_kfifo_alloc(struct device *dev, struct kfifo *fifo,
				   unsigned int size, gfp_t gfp_mask)
{
	int err = kfifo_alloc(fifo, size, gfp_mask);
	if (err)
		return err;

	return devm_add_action_or_reset(dev, devm_kfifo_free, fifo);
}

/*
 * kfifo_is_empty_spinlocked(), but with a raw spinlock.
 */
#define kfifo_is_empty_rawspinlocked(fifo, lock) \
({ \
	unsigned long __flags; \
	raw_spin_lock_irqsave(lock, __flags); \
	bool __ret = kfifo_is_empty(fifo); \
	raw_spin_unlock_irqrestore(lock, __flags); \
	__ret; \
})


/*
 * hrtimer_setup() for older kernels.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
static inline void hrtimer_setup(struct hrtimer *timer,
		enum hrtimer_restart (*function)(struct hrtimer *),
		clockid_t clock_id, enum hrtimer_mode mode)
{
	hrtimer_init(timer, clock_id, mode);
	timer->function = function;
}
#endif

#endif /* _DSACRE_UNART_UTIL_H */
