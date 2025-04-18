/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * unart - definitely not a real UART
 * Copyright (c) 2025 Dominic Sacré <dominic@sacre.net>
 */
#ifndef _DSACRE_UNART_H
#define _DSACRE_UNART_H

#include <linux/hrtimer.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/tty_port.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define UNART_DEFAULT_RX_SKEW 30

#define UNART_RX_FIFO_SIZE 32
#define UNART_TX_FIFO_SIZE 1024

#define UNART_MAX_TTY_DEVICES 32


struct unart_module_params {
	char *gpiochip;
	int rx_gpio;
	int tx_gpio;
	unsigned int rx_skew;
	bool rx_debug;
};

extern struct unart_module_params unart_params;


struct unart;

struct unart_rx {
	struct gpio_desc *gpio;
	struct hrtimer timer;
	int irq;

	unsigned int skew_percent;
	ktime_t period;
	ktime_t skew;

	struct kfifo fifo;
	struct work_struct push_work;
	void (*push_callback)(struct unart *unart, const u8 *buf, size_t count);

	int bit_index;
	u8 payload;

	int debug_toggle;

	raw_spinlock_t lock;
};

struct unart_tx {
	struct gpio_desc *gpio;
	struct hrtimer timer;

	ktime_t period;

	struct kfifo fifo;
	wait_queue_head_t wait_queue;
	struct work_struct wakeup_work;
	void (*wakeup_callback)(struct unart *unart);

	int bit_index;
	u8 payload;

	raw_spinlock_t lock;
};

struct unart {
	struct unart_rx rx;
	struct unart_tx tx;

	unsigned int tty_index;
	struct device *tty_dev;
	struct tty_port tty_port;
};


int	unart_rx_setup(struct platform_device *pdev, struct unart_rx *rx);
void	unart_rx_set_baud_rate(struct unart_rx *rx, unsigned int baudrate);
int	unart_rx_activate(struct unart_rx *rx);
void	unart_rx_shutdown(struct unart_rx *rx);

int	unart_tx_setup(struct platform_device *pdev, struct unart_tx *tx);
void	unart_tx_set_baud_rate(struct unart_tx *tx, unsigned int baudrate);
ssize_t	unart_tx_write(struct unart_tx *tx, const u8 *buf, size_t count);
size_t	unart_tx_write_room(struct unart_tx *tx);
void	unart_tx_wait_until_sent(struct unart_tx *tx, int timeout);


int	unart_tty_device_setup(struct platform_device *pdev, struct unart *unart);

int	unart_tty_register_driver(void);
void	unart_tty_unregister_driver(void);


#endif /* _DSACRE_UNART_H */
