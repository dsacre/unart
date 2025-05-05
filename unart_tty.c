// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * unart - definitely not a real UART
 * Copyright (c) 2025 Dominic Sacré <dominic@sacre.net>
 */
#include "unart.h"

#include <linux/bitmap.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/tty_port.h>
#include <linux/version.h>


static struct tty_driver *unart_tty_driver;

static DECLARE_BITMAP(index_bitmap, UNART_MAX_TTY_DEVICES);
static DEFINE_MUTEX(index_bitmap_mutex);

static int find_free_device_index(void)
{
	mutex_lock(&index_bitmap_mutex);

	int index = find_first_zero_bit(index_bitmap, UNART_MAX_TTY_DEVICES);
	if (index < UNART_MAX_TTY_DEVICES)
		set_bit(index, index_bitmap);
	else
		index = -EBUSY;

	mutex_unlock(&index_bitmap_mutex);
	return index;
}

static void release_device_index(int index)
{
	mutex_lock(&index_bitmap_mutex);
	clear_bit(index, index_bitmap);
	mutex_unlock(&index_bitmap_mutex);
}


static int unart_tty_open(struct tty_struct *tty, struct file *filp)
{
	return tty_port_open(tty->port, tty, filp);
}

static void unart_tty_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(tty->port, tty, filp);
}

static void unart_tty_hangup(struct tty_struct *tty)
{
	tty_port_hangup(tty->port);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
static int unart_tty_write(struct tty_struct *tty, const u8 *buf, int count)
#else
static ssize_t unart_tty_write(struct tty_struct *tty, const u8 *buf, size_t count)
#endif
{
	struct unart *unart = tty->driver_data;
	return unart_tx_write(&unart->tx, buf, count);
}

static unsigned int unart_tty_write_room(struct tty_struct *tty)
{
	struct unart *unart = tty->driver_data;
	return unart_tx_write_room(&unart->tx);
}

static void unart_tty_wait_until_sent(struct tty_struct *tty, int timeout)
{
	struct unart *unart = tty->driver_data;
	unart_tx_wait_until_sent(&unart->tx, timeout);
}

static int unart_tty_tiocmget(struct tty_struct *tty)
{
	return 0;
}

static int unart_tty_tiocmset(struct tty_struct *tty, unsigned int set, unsigned int clear)
{
	return 0;
}

static void unart_tty_set_termios(struct tty_struct *tty, const struct ktermios *old)
{
	struct platform_device *pdev = to_platform_device(tty->dev->parent);
	struct unart *unart = tty->driver_data;

	if (C_CSIZE(tty) != CS8 || C_CSTOPB(tty) || C_PARENB(tty))
		dev_err(&pdev->dev, "Unsupported cflag, expected 8N1\n");

	speed_t baud_rate = tty_get_baud_rate(tty);

	unart_rx_set_baud_rate(&unart->rx, baud_rate);
	unart_tx_set_baud_rate(&unart->tx, baud_rate);
}


static void unart_tty_rx_push_callback(
		struct unart *unart, const u8 *buf, size_t count)
{
	tty_insert_flip_string(&unart->tty_port, buf, count);
	tty_flip_buffer_push(&unart->tty_port);
}

static void unart_tty_tx_wakeup_callback(struct unart *unart)
{
	tty_port_tty_wakeup(&unart->tty_port);
}

static int unart_tty_port_activate(struct tty_port *port, struct tty_struct *tty)
{
	struct platform_device *pdev = to_platform_device(tty->dev->parent);
	struct unart *unart = platform_get_drvdata(pdev);

	tty->driver_data = unart;

	unart_rx_activate(&unart->rx);

	return 0;
}

static void unart_tty_port_shutdown(struct tty_port *port)
{
	struct unart *unart = container_of(port, struct unart, tty_port);

	unart_rx_shutdown(&unart->rx);
}


static const struct tty_operations unart_tty_ops = {
	.open = unart_tty_open,
	.close = unart_tty_close,
	.hangup = unart_tty_hangup,
	.write = unart_tty_write,
	.write_room = unart_tty_write_room,
	.wait_until_sent = unart_tty_wait_until_sent,
	.tiocmget = unart_tty_tiocmget,
	.tiocmset = unart_tty_tiocmset,
	.set_termios = unart_tty_set_termios,
};

static const struct tty_port_operations unart_tty_port_ops = {
	.activate = unart_tty_port_activate,
	.shutdown = unart_tty_port_shutdown,
};

static ssize_t name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", dev_name(dev->parent));
}
static DEVICE_ATTR_RO(name);


static void unart_tty_device_cleanup(void *_unart)
{
	struct unart *unart = _unart;

	device_remove_file(unart->tty_dev, &dev_attr_name);
	tty_port_unregister_device(&unart->tty_port, unart_tty_driver, unart->tty_index);
	tty_port_destroy(&unart->tty_port);
	release_device_index(unart->tty_index);
}

int unart_tty_device_setup(struct platform_device *pdev, struct unart *unart)
{
	int index = find_free_device_index();
	if (index < 0)
		return index;
	unart->tty_index = (unsigned int)index;

	tty_port_init(&unart->tty_port);
	unart->tty_port.ops = &unart_tty_port_ops;

	unart->tty_dev = tty_port_register_device(&unart->tty_port,
				unart_tty_driver, unart->tty_index, &pdev->dev);
	if (IS_ERR(unart->tty_dev)) {
		tty_port_destroy(&unart->tty_port);
		release_device_index(unart->tty_index);
		return PTR_ERR(unart->tty_dev);
	}

	device_create_file(unart->tty_dev, &dev_attr_name);

	unart->rx.push_callback = unart_tty_rx_push_callback;
	unart->tx.wakeup_callback = unart_tty_tx_wakeup_callback;

	unart_rx_set_baud_rate(&unart->rx, unart_tty_driver->init_termios.c_ispeed);
	unart_tx_set_baud_rate(&unart->tx, unart_tty_driver->init_termios.c_ospeed);

	return devm_add_action_or_reset(&pdev->dev, unart_tty_device_cleanup, unart);
}


int unart_tty_register_driver(void)
{
	struct tty_driver *driver = tty_alloc_driver(
			UNART_MAX_TTY_DEVICES,
			TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(driver))
		return PTR_ERR(driver);

	driver->owner = THIS_MODULE;
	driver->driver_name = "unart";
	driver->name = "ttyunart";
	driver->major = 0;
	driver->minor_start = 0;
	driver->type = TTY_DRIVER_TYPE_SERIAL;
	driver->subtype = SERIAL_TYPE_NORMAL;
	driver->init_termios = tty_std_termios;
	driver->init_termios.c_ispeed = 9600;
	driver->init_termios.c_ospeed = 9600;
	driver->init_termios.c_cflag = B9600 | CREAD | CS8 | CLOCAL;
	driver->ops = &unart_tty_ops;

	int err = tty_register_driver(driver);
	if (err) {
		tty_driver_kref_put(driver);
		return err;
	}

	unart_tty_driver = driver;

	return 0;
}

void unart_tty_unregister_driver(void)
{
	tty_unregister_driver(unart_tty_driver);
	tty_driver_kref_put(unart_tty_driver);
}
