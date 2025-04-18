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
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>


static inline void unart_rx_debug_toggle(struct unart_rx *rx)
{
	struct unart *unart = container_of(rx, struct unart, rx);

	rx->debug_toggle ^= 1;
	gpiod_set_raw_value(unart->tx.gpio, rx->debug_toggle);
}

static irqreturn_t unart_rx_irq_handler(int irq, void *_rx)
{
	struct unart_rx *rx = _rx;
	ktime_t now = ktime_get();

	raw_spin_lock_irqsave_scoped(&rx->lock);

	// Ignore falling edges while a byte is being read.
	// It would be better if we could mask the IRQ somehow...
	if (rx->bit_index != -1 || hrtimer_active(&rx->timer))
		return IRQ_HANDLED;

	rx->payload = 0;

	hrtimer_start(&rx->timer, now + rx->skew, HRTIMER_MODE_ABS_HARD);

	if (unlikely(unart_params.rx_debug))
		unart_rx_debug_toggle(rx);

	return IRQ_HANDLED;
}

static enum hrtimer_restart unart_rx_timer_callback(struct hrtimer *timer)
{
	struct unart_rx *rx = container_of(timer, struct unart_rx, timer);

	raw_spin_lock_irqsave_scoped(&rx->lock);

	int bit = gpiod_get_raw_value(rx->gpio);

	if (unlikely(unart_params.rx_debug))
		unart_rx_debug_toggle(rx);

	if (rx->bit_index == -1) {
		if (bit != 0)
			// Start bit is invalid.
			return HRTIMER_NORESTART;
		++rx->bit_index;

	} else if (rx->bit_index < 8) {
		rx->payload = (bit << 7) | (rx->payload >> 1);
		++rx->bit_index;

	} else {
		if (bit == 1) {
			// Stop bit is valid. Add payload to FIFO and
			// schedule pushing it to TTY buffer.
			kfifo_put(&rx->fifo, rx->payload);
			schedule_work(&rx->push_work);
		}
		rx->bit_index = -1;
		return HRTIMER_NORESTART;
	}

	hrtimer_forward_now(timer, rx->period);
	return HRTIMER_RESTART;
}

static void unart_rx_push_work(struct work_struct *push_work)
{
	struct unart_rx *rx = container_of(push_work, struct unart_rx, push_work);
	struct unart *unart = container_of(rx, struct unart, rx);

	u8 buf[UNART_RX_FIFO_SIZE];
	size_t n = kfifo_out(&rx->fifo, buf, sizeof(buf));
	rx->push_callback(unart, buf, n);
}


static void unart_rx_cleanup(void *_rx)
{
	struct unart_rx *rx = _rx;

	disable_irq(rx->irq);
	hrtimer_cancel(&rx->timer);
	while (hrtimer_active(&rx->timer))
		cond_resched();
}

int unart_rx_setup(struct platform_device *pdev, struct unart_rx *rx)
{
	int err;

	raw_spin_lock_init(&rx->lock);
	INIT_WORK(&rx->push_work, unart_rx_push_work);

	rx->bit_index = -1;
	rx->debug_toggle = 0;

	err = devm_kfifo_alloc(&pdev->dev, &rx->fifo, UNART_RX_FIFO_SIZE, GFP_KERNEL);
	if (err)
		return err;

	rx->gpio = devm_gpiod_get(&pdev->dev, "rx", GPIOD_IN);
	if (IS_ERR(rx->gpio)) {
		dev_err(&pdev->dev, "Failed to get RX GPIO\n");
		return PTR_ERR(rx->gpio);
	}
	if (gpiod_cansleep(rx->gpio)) {
		dev_err(&pdev->dev, "RX GPIO can sleep\n");
		return -EINVAL;
	}

	err = device_property_read_u32(&pdev->dev, "rx-skew", &rx->skew_percent);
	if (err)
		rx->skew_percent = unart_params.rx_skew;
	rx->skew_percent = clamp(rx->skew_percent, 0u, 100u);

	rx->irq = gpiod_to_irq(rx->gpio);
	err = devm_request_irq(
			&pdev->dev, rx->irq, unart_rx_irq_handler,
			IRQF_TRIGGER_FALLING | IRQF_NO_THREAD | IRQF_NO_AUTOEN,
			"unart-rx", rx);
	if (err) {
		dev_err(&pdev->dev, "Failed to request RX IRQ\n");
		return err;
	}

	hrtimer_setup(&rx->timer, &unart_rx_timer_callback,
		      CLOCK_MONOTONIC, HRTIMER_MODE_ABS_HARD);

	return devm_add_action_or_reset(&pdev->dev, unart_rx_cleanup, rx);
}


void unart_rx_set_baud_rate(struct unart_rx *rx, unsigned int baudrate)
{
	rx->period = ns_to_ktime(NSEC_PER_SEC / baudrate);
	rx->skew = rx->period * rx->skew_percent / 100;
}

int unart_rx_activate(struct unart_rx *rx)
{
	enable_irq(rx->irq);
	return 0;
}

void unart_rx_shutdown(struct unart_rx *rx)
{
	disable_irq(rx->irq);
}
