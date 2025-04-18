// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * unart - definitely not a real UART
 * Copyright (c) 2025 Dominic Sacré <dominic@sacre.net>
 */
#include "unart.h"
#include "unart_util.h"

#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>


static enum hrtimer_restart unart_tx_timer_callback(struct hrtimer *timer)
{
	struct unart_tx *tx = container_of(timer, struct unart_tx, timer);

	raw_spin_lock_irqsave_scoped(&tx->lock);

	if (tx->bit_index == -1) {
		gpiod_set_raw_value(tx->gpio, 0); // Start bit
		tx->bit_index = 0;

	} else if (tx->bit_index < 8) {
		gpiod_set_raw_value(tx->gpio, tx->payload & 0b1);
		tx->payload >>= 1;
		++tx->bit_index;

	} else {
		gpiod_set_raw_value(tx->gpio, 1); // Stop bit
		tx->bit_index = -1;

		// Get next data byte from FIFO. Wake up waiting tasks and
		// stop timer if FIFO is empty.
		if (!kfifo_get(&tx->fifo, &tx->payload)) {
			schedule_work(&tx->wakeup_work);
			return HRTIMER_NORESTART;
		}
	}

	hrtimer_forward_now(timer, tx->period);
	return HRTIMER_RESTART;
}

static void unart_tx_wakeup_work(struct work_struct *wakeup_work)
{
	struct unart_tx *tx = container_of(wakeup_work, struct unart_tx, wakeup_work);
	struct unart *unart = container_of(tx, struct unart, tx);

	wake_up_interruptible(&tx->wait_queue);
	tx->wakeup_callback(unart);
}


static void unart_tx_cleanup(void *_tx)
{
	struct unart_tx *tx = _tx;

	hrtimer_cancel(&tx->timer);
	wait_event_interruptible(tx->wait_queue, !hrtimer_active(&tx->timer));
}

int unart_tx_setup(struct platform_device *pdev, struct unart_tx *tx)
{
	int err;

	raw_spin_lock_init(&tx->lock);
	init_waitqueue_head(&tx->wait_queue);
	INIT_WORK(&tx->wakeup_work, unart_tx_wakeup_work);

	err = devm_kfifo_alloc(&pdev->dev, &tx->fifo, UNART_TX_FIFO_SIZE, GFP_KERNEL);
	if (err)
		return err;

	tx->gpio = devm_gpiod_get(&pdev->dev, "tx", GPIOD_OUT_HIGH);
	if (IS_ERR(tx->gpio)) {
		dev_err(&pdev->dev, "Failed to get TX GPIO\n");
		return PTR_ERR(tx->gpio);
	}
	if (gpiod_cansleep(tx->gpio)) {
		dev_err(&pdev->dev, "TX GPIO can sleep\n");
		return -EINVAL;
	}

	hrtimer_setup(&tx->timer, &unart_tx_timer_callback,
		      CLOCK_MONOTONIC, HRTIMER_MODE_ABS_HARD);

	return devm_add_action_or_reset(&pdev->dev, unart_tx_cleanup, tx);
}


void unart_tx_set_baud_rate(struct unart_tx *tx, unsigned int baudrate)
{
	tx->period = ns_to_ktime(NSEC_PER_SEC / baudrate);
}

ssize_t unart_tx_write(struct unart_tx *tx, const u8 *buf, size_t count)
{
	// Disable TX entirely if RX debugging is enabled.
	if (unart_params.rx_debug)
		return count;

	ssize_t ret = kfifo_in(&tx->fifo, buf, count);

	raw_spin_lock_irqsave_scoped(&tx->lock);

	if (!hrtimer_active(&tx->timer) && kfifo_get(&tx->fifo, &tx->payload)) {
		tx->bit_index = -1;
		// Add one period so the first IRQ isn't automatically late.
		ktime_t target = ktime_get() + tx->period;
		hrtimer_start(&tx->timer, target, HRTIMER_MODE_ABS_HARD);
	}

	return ret;
}

size_t unart_tx_write_room(struct unart_tx *tx)
{
	raw_spin_lock_irqsave_scoped(&tx->lock);
	return kfifo_avail(&tx->fifo);
}

void unart_tx_wait_until_sent(struct unart_tx *tx, int timeout)
{
	wait_event_interruptible_timeout(
			tx->wait_queue,
			kfifo_is_empty_rawspinlocked(&tx->fifo, &tx->lock),
			timeout);
}
